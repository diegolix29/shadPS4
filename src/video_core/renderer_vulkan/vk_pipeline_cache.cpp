// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <type_traits>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include <boost/container/static_vector.hpp>
#include "common/config.h"

#include "common/assert.h"
#include "common/elf_info.h"
#include "common/hash.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_depth_stencil_state.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

using Shader::LogicalStage;
using Shader::Output;
using Shader::Stage;

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

void ResolvedStageResources::Bind(Shader::Info& info) const noexcept {
    info.resolved_buffers = std::span<const AmdGpu::Buffer>{buffers.data(), buffers.size()};
    info.resolved_images = std::span<const AmdGpu::Image>{images.data(), images.size()};
    info.resolved_samplers = std::span<const AmdGpu::Sampler>{samplers.data(), samplers.size()};
    info.resolved_fmasks = std::span<const AmdGpu::Image>{fmasks.data(), fmasks.size()};
    info.resolved_vertex_buffers =
        std::span<const AmdGpu::Buffer>{vertex_buffers.data(), vertex_buffers.size()};
}

struct ShaderCompileResult {
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    std::vector<u32> code;
    std::vector<u32> geometry_copy_code;
    Shader::Info info;
    Shader::RuntimeInfo runtime_info{};
    Shader::Backend::Bindings binding_start{};
    Shader::Backend::Bindings bindings{};
    ResolvedStageResources resolved_resources{};
    Shader::StageSpecialization specialization{};
    std::vector<u32> debug_spv;
    std::vector<u32> debug_patch;
    vk::ShaderModule module{};
    std::chrono::steady_clock::time_point started_at{};
    size_t permutation_index{};
    u64 permutation_hash{};
    bool initial_program{};
    bool collect_shader{};
    bool is_patched{};
};

namespace {

constexpr std::array<u8, 8> NativePipelineCacheMagic{'S', 'H', 'A', 'D', 'V', 'K', 'P', 'C'};
constexpr u32 NativePipelineCacheVersion = 1;
constexpr u64 MaxNativePipelineCacheSize = 512ULL * 1024 * 1024;

struct NativePipelineCacheHeader {
    std::array<u8, 8> magic;
    u32 version;
    u32 pipeline_key_version;
    u32 driver_version;
    u64 payload_size;
    Shader::Profile profile;
};

struct VulkanPipelineCacheHeader {
    u32 header_size;
    u32 header_version;
    u32 vendor_id;
    u32 device_id;
    std::array<u8, VK_UUID_SIZE> uuid;
};

static_assert(std::is_trivially_copyable_v<NativePipelineCacheHeader>);
static_assert(sizeof(VulkanPipelineCacheHeader) == 32);

struct GraphicsPipelineBuild {
    GraphicsPipelineKey key;
    std::array<std::optional<Shader::Info>, MaxShaderStages> compile_stage_storage;
    std::array<ResolvedStageResources, MaxShaderStages> compile_resources;
    std::array<const Shader::Info*, MaxShaderStages> runtime_stages{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader;

    [[nodiscard]] std::array<const Shader::Info*, MaxShaderStages> BindCompileStages() {
        std::array<const Shader::Info*, MaxShaderStages> compile_stages{};
        for (u32 stage = 0; stage < MaxShaderStages; ++stage) {
            auto& info = compile_stage_storage[stage];
            if (!info) {
                continue;
            }
            compile_resources[stage].Bind(*info);
            compile_stages[stage] = &*info;
        }
        return compile_stages;
    }
};

constexpr std::string_view NativePipelineCacheName = "pipeline_cache";

/// A shader compilation burst ends once no shader has been queued for this long.
constexpr auto ShaderBurstGap = std::chrono::milliseconds{500};

/// Command processor stalls on shader guest data captures, summed per burst so that short stalls
/// are accounted for without logging each of them.
struct ShaderStallBurst {
    std::chrono::steady_clock::time_point first_queued{};
    std::chrono::steady_clock::time_point last_captured{};
    std::chrono::nanoseconds stall{};
    std::chrono::nanoseconds queue{};
    std::chrono::nanoseconds max_stall{};
    u64 max_stall_hash{};
    u32 shaders{};
};

/// Guarded by the compiler task mutex of the pipeline cache.
ShaderStallBurst shader_stall_burst;

void LogShaderStallBurst(const ShaderStallBurst& burst) {
    using Milliseconds = std::chrono::duration<double, std::milli>;
    const auto stall = Milliseconds{burst.stall}.count();
    const auto queue = Milliseconds{burst.queue}.count();
    LOG_INFO(Render_Vulkan,
             "Shader burst: {} shaders in {:.0f} ms stalled the command processor {:.2f} ms "
             "(queue {:.2f} ms, translation {:.2f} ms, longest {:.2f} ms on {:#x})",
             burst.shaders, Milliseconds{burst.last_captured - burst.first_queued}.count(), stall,
             queue, stall - queue, Milliseconds{burst.max_stall}.count(), burst.max_stall_hash);
}

/// Translated IR lives in these until a pipeline worker emits its SPIR-V, so they are recycled
/// here instead of being thread local.
constexpr size_t MaxFreeShaderPools = 16;
std::mutex free_shader_pools_mutex;
std::vector<std::unique_ptr<Shader::Pools>> free_shader_pools;

[[nodiscard]] std::unique_ptr<Shader::Pools> AcquireShaderPools() {
    {
        std::scoped_lock lock{free_shader_pools_mutex};
        if (!free_shader_pools.empty()) {
            auto pools = std::move(free_shader_pools.back());
            free_shader_pools.pop_back();
            return pools;
        }
    }
    return std::make_unique<Shader::Pools>();
}

void RecycleShaderPools(std::unique_ptr<Shader::Pools> pools) {
    pools->ReleaseContents();
    std::scoped_lock lock{free_shader_pools_mutex};
    if (free_shader_pools.size() < MaxFreeShaderPools) {
        free_shader_pools.emplace_back(std::move(pools));
    }
}

/// Where the native cache was kept before it moved into the game's cache.
[[nodiscard]] std::filesystem::path GetLegacyNativePipelineCacheDir() {
    return Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / "vulkan" /
           Common::ElfInfo::Instance().GameSerial();
}

[[nodiscard]] std::vector<u8> ReadLegacyNativePipelineCache() {
    using namespace Common::FS;
    const IOFile file{GetLegacyNativePipelineCacheDir() / "pipeline_cache.bin",
                      FileAccessMode::Read};
    if (!file.IsOpen()) {
        return {};
    }
    std::vector<u8> blob(file.GetSize());
    if (blob.size() > sizeof(NativePipelineCacheHeader) + MaxNativePipelineCacheSize ||
        file.Read(blob) != blob.size()) {
        return {};
    }
    return blob;
}

void RemoveLegacyNativePipelineCache() {
    std::error_code ec;
    const auto dir = GetLegacyNativePipelineCacheDir();
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove(dir.parent_path(), ec); // Only when empty.
}

[[nodiscard]] bool ValidateNativePipelineCacheData(std::span<const u8> data,
                                                   const Instance& instance) {
    if (data.size() < sizeof(VulkanPipelineCacheHeader)) {
        return false;
    }
    VulkanPipelineCacheHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));
    return header.header_size >= sizeof(header) && header.header_size <= data.size() &&
           header.header_version == static_cast<u32>(VK_PIPELINE_CACHE_HEADER_VERSION_ONE) &&
           header.vendor_id == instance.GetVendorID() &&
           header.device_id == instance.GetDeviceID() &&
           header.uuid == instance.GetPipelineCacheUUID();
}

template <typename DescriptorList, typename ResourceList>
void ResolveResourceList(const DescriptorList& descriptors, ResourceList& resources,
                         const Shader::Info& info) {
    resources.resize(descriptors.size(), boost::container::default_init);
    for (size_t index = 0; index < descriptors.size(); ++index) {
        resources[index] = descriptors[index].GetSharp(info);
    }
}

void ResolveStageResources(Shader::Info& info,
                           const std::optional<Shader::Gcn::FetchShaderData>* fetch_shader,
                           ResolvedStageResources& resolved) {
    ResolveResourceList(info.buffers, resolved.buffers, info);
    ResolveResourceList(info.images, resolved.images, info);
    ResolveResourceList(info.samplers, resolved.samplers, info);
    ResolveResourceList(info.fmasks, resolved.fmasks, info);
    if (fetch_shader && fetch_shader->has_value()) {
        ResolveResourceList((*fetch_shader)->attributes, resolved.vertex_buffers, info);
    } else {
        resolved.vertex_buffers.clear();
    }
    resolved.Bind(info);
}

/// Why the specialization of a stage cannot be keyed by its user data alone.
enum class StageUncacheableReason : u32 {
    SrtWalker = 1U << 0,
    TessellationControl = 1U << 1,
    TessellationEvaluation = 1U << 2,
    DescriptorOutsideUserData = 1U << 3,
    FetchPointerOutsideUserData = 1U << 4,
};

bool BuildSpecializationPlan(Program& program) {
    if (program.specialization_plan_ready) {
        return program.specialization_plan_cacheable;
    }
    program.specialization_plan_ready = true;
    const auto& info = program.info;
    using Reason = StageUncacheableReason;
    u32 reasons{};
    if (info.srt_info.walker_func) {
        reasons |= static_cast<u32>(Reason::SrtWalker);
    }
    if (info.l_stage == LogicalStage::TessellationControl) {
        reasons |= static_cast<u32>(Reason::TessellationControl);
    }
    if (info.l_stage == LogicalStage::TessellationEval) {
        reasons |= static_cast<u32>(Reason::TessellationEvaluation);
    }
    const auto valid_range = [](u32 first, u32 dwords) {
        return first + dwords <= Shader::ShaderParams::NumShaderUserData;
    };
    for (const auto& resource : info.buffers) {
        if (!resource.inline_cbuf &&
            !valid_range(resource.sharp_idx, sizeof(AmdGpu::Buffer) / sizeof(u32))) {
            reasons |= static_cast<u32>(Reason::DescriptorOutsideUserData);
        }
    }
    for (const auto& resource : info.images) {
        if (!valid_range(resource.sharp_idx, sizeof(AmdGpu::Image) / sizeof(u32))) {
            reasons |= static_cast<u32>(Reason::DescriptorOutsideUserData);
        }
    }
    for (const auto& resource : info.samplers) {
        if (!resource.is_inline_sampler &&
            !valid_range(resource.sharp_idx, sizeof(AmdGpu::Sampler) / sizeof(u32))) {
            reasons |= static_cast<u32>(Reason::DescriptorOutsideUserData);
        }
    }
    for (const auto& resource : info.fmasks) {
        if (!valid_range(resource.sharp_idx, sizeof(AmdGpu::Image) / sizeof(u32))) {
            reasons |= static_cast<u32>(Reason::DescriptorOutsideUserData);
        }
    }
    if (info.has_fetch_shader && !valid_range(info.fetch_shader_sgpr_base, 2)) {
        reasons |= static_cast<u32>(Reason::FetchPointerOutsideUserData);
    }
    program.specialization_plan_reasons = static_cast<u8>(reasons);
    program.specialization_plan_cacheable = reasons == 0;
    return program.specialization_plan_cacheable;
}

/// Whether the specialization of a program is a function of its freshly resolved resources,
/// runtime info, fetch shader and bindings, which MatchesCurrentSpecialization compares.
/// Tessellation stages also fold constants read from guest memory into their runtime info.
[[nodiscard]] bool IsSpecializationMatchable(const Program& program) noexcept {
    using Reason = StageUncacheableReason;
    constexpr u32 TessellationReasons = static_cast<u32>(Reason::TessellationControl) |
                                        static_cast<u32>(Reason::TessellationEvaluation);
    return program.specialization_plan_ready &&
           (program.specialization_plan_reasons & TessellationReasons) == 0;
}

void RefreshDynamicProgramData(
    Shader::Info& info, VAddr program_base,
    std::span<const u32, Shader::ShaderParams::NumShaderUserData> user_data) {
    info.pgm_base = program_base;
    info.user_data = user_data;
    info.RefreshFlatBuf();
}

void RefreshDynamicProgramData(Shader::Info& info, const Shader::ShaderParams& params) {
    RefreshDynamicProgramData(info, params.Base(), params.user_data);
}

struct CachedFetchShader {
    const std::optional<Shader::Gcn::FetchShaderData>* parsed{};
    u64 revision{};

    [[nodiscard]] bool IsUsable(const Shader::Info& info) const noexcept {
        return parsed && (!info.has_fetch_shader || parsed->has_value());
    }
};

constinit const std::optional<Shader::Gcn::FetchShaderData> EmptyFetchShader{};

#if defined(_MSC_VER)
#define SHAD_FETCH_SHADER_FORCE_INLINE __forceinline
#else
#define SHAD_FETCH_SHADER_FORCE_INLINE __attribute__((always_inline)) inline
#endif

#if defined(__AVX2__)
template <size_t WordCount>
[[nodiscard]] SHAD_FETCH_SHADER_FORCE_INLINE bool EqualFixedFetchShaderCode(
    const u32* cached, const u32* current) noexcept {
    static_assert(WordCount >= 4 && WordCount <= 28);
    constexpr size_t YmmCount = WordCount / 8;
    constexpr size_t VectorTail = WordCount % 8;
    constexpr size_t ScalarOffset = YmmCount * 8 + (VectorTail >= 4 ? 4 : 0);
    constexpr size_t ScalarWords = VectorTail - (VectorTail >= 4 ? 4 : 0);

    __m256i difference = _mm256_setzero_si256();
    if constexpr (YmmCount != 0) {
        difference =
            _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(cached)),
                             _mm256_loadu_si256(reinterpret_cast<const __m256i*>(current)));
    }
    if constexpr (YmmCount >= 2) {
        const __m256i block =
            _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(cached + 8)),
                             _mm256_loadu_si256(reinterpret_cast<const __m256i*>(current + 8)));
        difference = _mm256_or_si256(difference, block);
    }
    if constexpr (YmmCount >= 3) {
        const __m256i block =
            _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(cached + 16)),
                             _mm256_loadu_si256(reinterpret_cast<const __m256i*>(current + 16)));
        difference = _mm256_or_si256(difference, block);
    }
    if constexpr (VectorTail >= 4) {
        const __m128i block = _mm_xor_si128(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(cached + YmmCount * 8)),
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(current + YmmCount * 8)));
        if constexpr (YmmCount == 0) {
            difference = _mm256_zextsi128_si256(block);
        } else {
            difference = _mm256_or_si256(difference, _mm256_zextsi128_si256(block));
        }
    }

    u64 tail_difference{};
    if constexpr (ScalarWords >= 2) {
        u64 cached_tail;
        u64 current_tail;
        std::memcpy(&cached_tail, cached + ScalarOffset, sizeof(cached_tail));
        std::memcpy(&current_tail, current + ScalarOffset, sizeof(current_tail));
        tail_difference = cached_tail ^ current_tail;
    }
    if constexpr ((ScalarWords & 1) != 0) {
        tail_difference |=
            cached[ScalarOffset + (ScalarWords & 2)] ^ current[ScalarOffset + (ScalarWords & 2)];
    }
    if constexpr (ScalarWords == 0) {
        return _mm256_testz_si256(difference, difference) != 0;
    } else {
        return tail_difference == 0 && _mm256_testz_si256(difference, difference) != 0;
    }
}
#endif

[[nodiscard]] SHAD_FETCH_SHADER_FORCE_INLINE bool EqualFetchShaderCode(const u32* cached,
                                                                       const u32* current,
                                                                       size_t word_count) noexcept {
#if defined(__AVX2__)
    switch (word_count) {
    case 6:
        return EqualFixedFetchShaderCode<6>(cached, current);
    case 9:
        return EqualFixedFetchShaderCode<9>(cached, current);
    case 10:
        return EqualFixedFetchShaderCode<10>(cached, current);
    case 12:
        return EqualFixedFetchShaderCode<12>(cached, current);
    case 15:
        return EqualFixedFetchShaderCode<15>(cached, current);
    case 16:
        return EqualFixedFetchShaderCode<16>(cached, current);
    case 17:
        return EqualFixedFetchShaderCode<17>(cached, current);
    case 18:
        return EqualFixedFetchShaderCode<18>(cached, current);
    case 19:
        return EqualFixedFetchShaderCode<19>(cached, current);
    case 20:
        return EqualFixedFetchShaderCode<20>(cached, current);
    case 21:
        return EqualFixedFetchShaderCode<21>(cached, current);
    case 24:
        return EqualFixedFetchShaderCode<24>(cached, current);
    case 25:
        return EqualFixedFetchShaderCode<25>(cached, current);
    case 28:
        return EqualFixedFetchShaderCode<28>(cached, current);
    default:
        break;
    }
#endif
    return std::memcmp(cached, current, word_count * sizeof(u32)) == 0;
}

#undef SHAD_FETCH_SHADER_FORCE_INLINE

SHAD_NO_INLINE void ReportInvalidFetchShaderSize() {
    ASSERT(false);
}

[[nodiscard]] CachedFetchShader GetCachedFetchShader(Program& program);

[[nodiscard]] SHAD_NO_INLINE CachedFetchShader CacheFetchShaderMiss(Program& program,
                                                                    const u32* code);

[[nodiscard]] CachedFetchShader GetCachedFetchShader(Program& program) {
    auto& info = program.info;
    if (!info.has_fetch_shader) {
        return {.parsed = &EmptyFetchShader, .revision = 0};
    }

    const u32* code = Shader::Gcn::GetFetchShaderCode(info, info.fetch_shader_sgpr_base);
    if (!code) {
        return {.parsed = &EmptyFetchShader, .revision = 0};
    }

    for (auto& entry : program.fetch_shader_cache) {
        if (entry.address != code || !entry.parsed) {
            continue;
        }
        if (entry.parsed->size % sizeof(u32) != 0) [[unlikely]] {
            ReportInvalidFetchShaderSize();
        }
        const size_t word_count = entry.parsed->size / sizeof(u32);
        if (entry.code.size() == word_count &&
            EqualFetchShaderCode(entry.code.data(), code, word_count)) {
            return {.parsed = &entry.parsed, .revision = entry.revision};
        }
    }

    return CacheFetchShaderMiss(program, code);
}

[[nodiscard]] SHAD_NO_INLINE CachedFetchShader CacheFetchShaderMiss(Program& program,
                                                                    const u32* code) {
    auto& info = program.info;
    auto parsed = Shader::Gcn::ParseFetchShader(info);
    if (!parsed) {
        return {.parsed = &EmptyFetchShader, .revision = 0};
    }
    ASSERT(parsed->size % sizeof(u32) == 0);
    const size_t word_count = parsed->size / sizeof(u32);

    u8 slot{};
    if (program.fetch_shader_cache.size() < Program::MaxFetchShaderCacheEntries) {
        slot = static_cast<u8>(program.fetch_shader_cache.size());
        program.fetch_shader_cache.emplace_back();
    } else {
        slot = program.next_fetch_shader_slot;
        program.next_fetch_shader_slot = static_cast<u8>((program.next_fetch_shader_slot + 1) %
                                                         Program::MaxFetchShaderCacheEntries);
    }

    auto& entry = program.fetch_shader_cache[slot];
    entry.address = code;
    entry.code.assign(code, code + word_count);
    entry.parsed = std::move(parsed);
    entry.revision = program.next_fetch_shader_revision++;
    if (program.next_fetch_shader_revision == 0) {
        program.next_fetch_shader_revision = 1;
        program.fetch_shader_cache.clear();
        return GetCachedFetchShader(program);
    }
    return {.parsed = &entry.parsed, .revision = entry.revision};
}

[[nodiscard]] constexpr u64 MixSpecializationFingerprint(u64 value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

class SpecializationFingerprintBuilder {
public:
    void Add(u64 value) noexcept {
        state = std::rotl(state, 17) ^ MixSpecializationFingerprint(value + state);
    }

    [[nodiscard]] u64 Finish() const noexcept {
        const u64 fingerprint = MixSpecializationFingerprint(state);
        return fingerprint != 0 ? fingerprint : 1;
    }

private:
    u64 state{0x6a09e667f3bcc909ULL};
};

[[nodiscard]] u32 PackCompMapping(const AmdGpu::CompMapping& mapping) noexcept {
    return std::bit_cast<u32>(mapping.array);
}

void AddFetchShaderFingerprint(
    SpecializationFingerprintBuilder& builder,
    const std::optional<Shader::Gcn::FetchShaderData>* fetch_shader) noexcept {
    const bool has_fetch_shader = fetch_shader && fetch_shader->has_value();
    builder.Add(has_fetch_shader);
    if (!has_fetch_shader) {
        return;
    }

    const auto& fetch = fetch_shader->value();
    builder.Add(fetch.attributes.size());
    for (const auto& attribute : fetch.attributes) {
        u64 packed{};
        std::memcpy(&packed, &attribute, sizeof(packed));
        builder.Add(packed & 0x0000ffffffffffffULL);
    }
    builder.Add(static_cast<u8>(fetch.vertex_offset_sgpr) |
                (static_cast<u64>(static_cast<u8>(fetch.instance_offset_sgpr)) << 8));
}

void AddVsAttribFingerprint(SpecializationFingerprintBuilder& builder,
                            const Shader::VsAttribSpecialization& spec) noexcept {
    builder.Add(spec.divisor | (static_cast<u64>(spec.num_class) << 32));
    builder.Add(PackCompMapping(spec.dst_select));
}

void AddBufferFingerprint(SpecializationFingerprintBuilder& builder,
                          const Shader::BufferSpecialization& spec) noexcept {
    builder.Add(spec.stride | (static_cast<u64>(spec.is_formatted) << 14) |
                (static_cast<u64>(spec.swizzle_enable) << 15));
    if (spec.is_formatted) {
        builder.Add(spec.data_format | (static_cast<u64>(spec.num_format) << 8) |
                    (static_cast<u64>(spec.num_conversion) << 16));
        builder.Add(PackCompMapping(spec.dst_select));
    }
    if (spec.swizzle_enable) {
        builder.Add(spec.index_stride | (static_cast<u64>(spec.element_size) << 8));
    }
}

void AddImageFingerprint(SpecializationFingerprintBuilder& builder,
                         const Shader::ImageSpecialization& spec) noexcept {
    builder.Add(static_cast<u64>(spec.type) | (static_cast<u64>(spec.is_integer) << 32) |
                (static_cast<u64>(spec.is_storage) << 33) | (static_cast<u64>(spec.is_cube) << 34) |
                (static_cast<u64>(spec.is_srgb) << 35));
    builder.Add(PackCompMapping(spec.dst_select) | (static_cast<u64>(spec.num_conversion) << 32));
    builder.Add(spec.num_bindings);
}

struct SpecializationBindingMask {
    u64 low{};
    u64 high{};

    [[nodiscard]] bool Any() const noexcept {
        return (low | high) != 0;
    }

    bool operator==(const SpecializationBindingMask&) const noexcept = default;
};

void SetSpecializationBinding(SpecializationBindingMask& mask, size_t binding) noexcept {
    if (binding < 64) {
        mask.low |= 1ULL << binding;
    } else {
        mask.high |= 1ULL << (binding - 64);
    }
}

[[nodiscard]] SpecializationBindingMask GetCurrentBindingMask(const Shader::Info& info) noexcept {
    SpecializationBindingMask mask{};
    size_t binding{};
    for (const auto sharp : info.resolved_buffers) {
        if (sharp) {
            SetSpecializationBinding(mask, binding);
        }
        ++binding;
    }
    for (const auto sharp : info.resolved_images) {
        if (sharp) {
            SetSpecializationBinding(mask, binding);
        }
        ++binding;
    }
    for (const auto sharp : info.resolved_fmasks) {
        if (sharp) {
            SetSpecializationBinding(mask, binding);
        }
        ++binding;
    }
    return mask;
}

[[nodiscard]] SpecializationBindingMask GetStoredBindingMask(
    const Shader::StageSpecialization& specialization) noexcept {
    SpecializationBindingMask mask{};
    for (size_t binding = 0; binding < Shader::StageSpecialization::MaxStageResources; ++binding) {
        if (specialization.bitset.test(binding)) {
            SetSpecializationBinding(mask, binding);
        }
    }
    return mask;
}

void AddBindingMask(SpecializationFingerprintBuilder& builder,
                    SpecializationBindingMask mask) noexcept {
    builder.Add(mask.low);
    builder.Add(mask.high);
}

void AddBindingStart(SpecializationFingerprintBuilder& builder,
                     const Shader::Backend::Bindings& start) noexcept {
    builder.Add(start.unified | (static_cast<u64>(start.buffer) << 32));
    builder.Add(start.user_data);
}

[[nodiscard]] SHAD_NO_INLINE u64
BuildStoredSpecializationFingerprint(const Shader::StageSpecialization& specialization) noexcept {
    SpecializationFingerprintBuilder builder{};
    builder.Add(specialization.vs_attribs.size());
    builder.Add(specialization.buffers.size());
    builder.Add(specialization.images.size());
    builder.Add(specialization.fmasks.size());
    builder.Add(specialization.samplers.size());

    const auto binding_mask = GetStoredBindingMask(specialization);
    AddBindingMask(builder, binding_mask);
    AddFetchShaderFingerprint(builder, &specialization.fetch_shader_data);
    for (const auto& spec : specialization.vs_attribs) {
        AddVsAttribFingerprint(builder, spec);
    }
    for (const auto& spec : specialization.fmasks) {
        builder.Add(std::bit_cast<u64>(spec));
    }
    if (binding_mask.Any()) {
        AddBindingStart(builder, specialization.start);
        for (const auto& spec : specialization.buffers) {
            AddBufferFingerprint(builder, spec);
        }
        for (const auto& spec : specialization.images) {
            AddImageFingerprint(builder, spec);
        }
        for (const auto& spec : specialization.samplers) {
            builder.Add(spec.force_unnormalized | (static_cast<u64>(spec.force_degamma) << 1));
        }
    }
    return builder.Finish();
}

[[nodiscard]] SHAD_NO_INLINE u64 BuildCurrentSpecializationFingerprint(
    const Shader::Info& info, const Shader::RuntimeInfo& runtime_info,
    const Shader::Backend::Bindings& start,
    const std::optional<Shader::Gcn::FetchShaderData>* fetch_shader) noexcept {
    SpecializationFingerprintBuilder builder{};
    const bool has_vertex_attributes =
        info.stage == Stage::Vertex && fetch_shader && fetch_shader->has_value();
    const size_t vertex_attribute_count =
        has_vertex_attributes ? fetch_shader->value().attributes.size() : 0;
    builder.Add(vertex_attribute_count);
    builder.Add(info.buffers.size());
    builder.Add(info.images.size());
    builder.Add(info.fmasks.size());
    builder.Add(info.samplers.size());

    const auto binding_mask = GetCurrentBindingMask(info);
    AddBindingMask(builder, binding_mask);
    AddFetchShaderFingerprint(builder, fetch_shader);
    if (has_vertex_attributes) {
        const auto& attributes = fetch_shader->value().attributes;
        for (size_t index = 0; index < attributes.size(); ++index) {
            Shader::VsAttribSpecialization spec{};
            const auto sharp = info.resolved_vertex_buffers[index];
            if (sharp) {
                spec = Shader::MakeVsAttribSpecialization(attributes[index], sharp, runtime_info);
            }
            AddVsAttribFingerprint(builder, spec);
        }
    }
    for (size_t index = 0; index < info.fmasks.size(); ++index) {
        Shader::FMaskSpecialization spec{};
        const auto sharp = info.resolved_fmasks[index];
        if (sharp) {
            spec = Shader::MakeFMaskSpecialization(sharp);
        }
        builder.Add(std::bit_cast<u64>(spec));
    }
    if (binding_mask.Any()) {
        AddBindingStart(builder, start);
        for (size_t index = 0; index < info.buffers.size(); ++index) {
            Shader::BufferSpecialization spec{};
            const auto sharp = info.resolved_buffers[index];
            if (sharp) {
                spec = Shader::MakeBufferSpecialization(info.buffers[index], sharp);
            }
            AddBufferFingerprint(builder, spec);
        }
        for (size_t index = 0; index < info.images.size(); ++index) {
            Shader::ImageSpecialization spec{};
            const auto sharp = info.resolved_images[index];
            if (sharp) {
                spec = Shader::MakeImageSpecialization(info.images[index], sharp);
            }
            AddImageFingerprint(builder, spec);
        }
        for (size_t index = 0; index < info.samplers.size(); ++index) {
            Shader::SamplerSpecialization spec{};
            const auto sharp = info.resolved_samplers[index];
            if (sharp) {
                spec = Shader::MakeSamplerSpecialization(sharp);
            }
            builder.Add(spec.force_unnormalized | (static_cast<u64>(spec.force_degamma) << 1));
        }
    }
    return builder.Finish();
}

[[nodiscard]] SHAD_NO_INLINE bool MatchesCurrentSpecialization(
    const Shader::StageSpecialization& candidate, const Shader::Info& info,
    const Shader::RuntimeInfo& runtime_info, const Shader::Backend::Bindings& start,
    const std::optional<Shader::Gcn::FetchShaderData>* fetch_shader) noexcept {
    if (!candidate.Valid() || candidate.runtime_info != runtime_info || !fetch_shader) {
        return false;
    }

    const bool has_vertex_attributes = info.stage == Stage::Vertex && fetch_shader->has_value();
    const size_t vertex_attribute_count =
        has_vertex_attributes ? fetch_shader->value().attributes.size() : 0;
    if (candidate.vs_attribs.size() != vertex_attribute_count ||
        candidate.buffers.size() != info.buffers.size() ||
        candidate.images.size() != info.images.size() ||
        candidate.fmasks.size() != info.fmasks.size() ||
        candidate.samplers.size() != info.samplers.size() ||
        candidate.fetch_shader_data != *fetch_shader) {
        return false;
    }

    const auto binding_mask = GetCurrentBindingMask(info);
    if (binding_mask != GetStoredBindingMask(candidate)) {
        return false;
    }

    if (has_vertex_attributes) {
        const auto& attributes = fetch_shader->value().attributes;
        if (info.resolved_vertex_buffers.size() != attributes.size()) {
            return false;
        }
        for (size_t index = 0; index < attributes.size(); ++index) {
            Shader::VsAttribSpecialization current{};
            const auto sharp = info.resolved_vertex_buffers[index];
            if (sharp) {
                current =
                    Shader::MakeVsAttribSpecialization(attributes[index], sharp, runtime_info);
            }
            if (candidate.vs_attribs[index] != current) {
                return false;
            }
        }
    }

    for (size_t index = 0; index < info.fmasks.size(); ++index) {
        Shader::FMaskSpecialization current{};
        const auto sharp = info.resolved_fmasks[index];
        if (sharp) {
            current = Shader::MakeFMaskSpecialization(sharp);
        }
        if (candidate.fmasks[index] != current) {
            return false;
        }
    }

    if (!binding_mask.Any()) {
        return true;
    }
    if (candidate.start != start) {
        return false;
    }

    for (size_t index = 0; index < info.buffers.size(); ++index) {
        Shader::BufferSpecialization current{};
        const auto sharp = info.resolved_buffers[index];
        if (sharp) {
            current = Shader::MakeBufferSpecialization(info.buffers[index], sharp);
        }
        if (candidate.buffers[index] != current) {
            return false;
        }
    }
    for (size_t index = 0; index < info.images.size(); ++index) {
        Shader::ImageSpecialization current{};
        const auto sharp = info.resolved_images[index];
        if (sharp) {
            current = Shader::MakeImageSpecialization(info.images[index], sharp);
        }
        if (candidate.images[index] != current) {
            return false;
        }
    }
    for (size_t index = 0; index < info.samplers.size(); ++index) {
        Shader::SamplerSpecialization current{};
        const auto sharp = info.resolved_samplers[index];
        if (sharp) {
            current = Shader::MakeSamplerSpecialization(sharp);
        }
        if (candidate.samplers[index] != current) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] SHAD_NO_INLINE size_t FindCachedPermutation(
    Program& program, const Shader::Info& info, const Shader::RuntimeInfo& runtime_info,
    const Shader::Backend::Bindings& start,
    const std::optional<Shader::Gcn::FetchShaderData>* fetch_shader,
    size_t excluded_permutation) noexcept {
    const u64 fingerprint =
        BuildCurrentSpecializationFingerprint(info, runtime_info, start, fetch_shader);
    for (size_t permutation = 0; permutation < program.modules.size(); ++permutation) {
        if (permutation == excluded_permutation) {
            continue;
        }
        auto& module = program.modules[permutation];
        if (module.spec.runtime_info != runtime_info) {
            continue;
        }
        if (module.specialization_fingerprint == 0) {
            module.specialization_fingerprint = BuildStoredSpecializationFingerprint(module.spec);
        }
        if (module.specialization_fingerprint != fingerprint) {
            continue;
        }
        if (MatchesCurrentSpecialization(module.spec, info, runtime_info, start, fetch_shader)) {
            return permutation;
        }
    }
    return Program::InvalidPermutation;
}

// The shape keys below keep exactly the descriptor fields the Make*Specialization functions in
// shader_recompiler/specialization.h read, plus whether the descriptor binds at all. Addresses,
// sizes and image dimensions change from draw to draw without changing the shader. A field
// read by a new specialization member must be added here as well.

/// MakeBufferSpecialization and MakeVsAttribSpecialization read the stride, swizzle, formats and
/// swizzle element fields. A V# binds when num_records is nonzero.
[[nodiscard]] u64 BufferShapeKey(const AmdGpu::Buffer& buffer) noexcept {
    return u64{buffer.stride} | (u64{buffer.swizzle_enable} << 14) | (u64{buffer.dst_sel_x} << 15) |
           (u64{buffer.dst_sel_y} << 18) | (u64{buffer.dst_sel_z} << 21) |
           (u64{buffer.dst_sel_w} << 24) | (u64{buffer.num_format} << 27) |
           (u64{buffer.data_format} << 30) | (u64{buffer.element_size} << 34) |
           (u64{buffer.index_stride} << 36) | (u64{buffer.num_records != 0} << 38);
}

/// MakeImageSpecialization reads the type, formats, swizzle and mip range. A T# binds when its
/// address is nonzero.
[[nodiscard]] u64 ImageShapeKey(const AmdGpu::Image& image) noexcept {
    return u64{image.type} | (u64{image.data_format} << 4) | (u64{image.num_format} << 10) |
           (u64{image.dst_sel_x} << 14) | (u64{image.dst_sel_y} << 17) |
           (u64{image.dst_sel_z} << 20) | (u64{image.dst_sel_w} << 23) |
           (u64{image.base_level} << 26) | (u64{image.last_level} << 30) |
           (u64{image.base_address != 0} << 34);
}

/// MakeFMaskSpecialization reads the dimensions.
[[nodiscard]] u64 FMaskShapeKey(const AmdGpu::Image& image) noexcept {
    return u64{image.width} | (u64{image.height} << 14) | (u64{image.base_address != 0} << 28);
}

/// MakeSamplerSpecialization reads the unnormalized and degamma bits.
[[nodiscard]] u64 SamplerShapeKey(const AmdGpu::Sampler& sampler) noexcept {
    return sampler.force_unnormalized.Value() | (sampler.force_degamma.Value() << 1) |
           (u64{static_cast<bool>(sampler)} << 2);
}

/// Collects the shape keys of the resources MatchesCurrentSpecialization compares, in the
/// same order.
void BuildSpecializationShapeKeys(const Shader::Info& info,
                                  const std::optional<Shader::Gcn::FetchShaderData>* fetch_shader,
                                  Program::SpecializationShape::Keys& keys) {
    keys.clear();
    if (info.stage == Stage::Vertex && fetch_shader && fetch_shader->has_value()) {
        for (const auto& sharp : info.resolved_vertex_buffers) {
            keys.push_back(BufferShapeKey(sharp));
        }
    }
    for (const auto& sharp : info.resolved_fmasks) {
        keys.push_back(FMaskShapeKey(sharp));
    }
    for (const auto& sharp : info.resolved_buffers) {
        keys.push_back(BufferShapeKey(sharp));
    }
    for (const auto& sharp : info.resolved_images) {
        keys.push_back(ImageShapeKey(sharp));
    }
    for (const auto& sharp : info.resolved_samplers) {
        keys.push_back(SamplerShapeKey(sharp));
    }
}

[[nodiscard]] bool MatchesSpecializationShape(const Program& program, size_t permutation,
                                              u64 fetch_shader_revision,
                                              const Shader::Backend::Bindings& start,
                                              const Shader::RuntimeInfo& runtime_info,
                                              const Program::SpecializationShape::Keys& keys) {
    const auto& shape = program.specialization_shape;
    return shape.permutation == permutation &&
           shape.modules_generation == program.modules_generation &&
           shape.fetch_shader_revision == fetch_shader_revision && shape.start == start &&
           shape.keys.size() == keys.size() &&
           std::memcmp(shape.keys.data(), keys.data(), keys.size() * sizeof(u64)) == 0 &&
           shape.runtime_info == runtime_info;
}

void RecordSpecializationShape(Program& program, size_t permutation, u64 fetch_shader_revision,
                               const Shader::Backend::Bindings& start,
                               const Shader::RuntimeInfo& runtime_info,
                               const Program::SpecializationShape::Keys& keys) {
    auto& shape = program.specialization_shape;
    shape.permutation = permutation;
    shape.modules_generation = program.modules_generation;
    shape.fetch_shader_revision = fetch_shader_revision;
    shape.start = start;
    shape.runtime_info = runtime_info;
    shape.keys.assign(keys.begin(), keys.end());
}

/// SHADPS4_VERIFY_STAGE_SHAPE=1 also runs the full comparison on every shape match and logs
/// any disagreement.
[[nodiscard]] bool VerifySpecializationShapes() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_VERIFY_STAGE_SHAPE");
        return env != nullptr && env[0] == '1';
    }();
    return enabled;
}

/// Confirms a shape match with the full comparison. A disagreement means a shape key misses a
/// field the specialization reads.
[[nodiscard]] SHAD_NO_INLINE bool VerifyShapeMatch(
    const Shader::StageSpecialization& candidate, const Shader::Info& info,
    const Shader::RuntimeInfo& runtime_info, const Shader::Backend::Bindings& start,
    const std::optional<Shader::Gcn::FetchShaderData>* fetch_shader) {
    if (MatchesCurrentSpecialization(candidate, info, runtime_info, start, fetch_shader)) {
        return true;
    }
    static std::atomic<u32> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 32) {
        LOG_ERROR(Render_Vulkan,
                  "Specialization shape matched a permutation the full comparison rejects: "
                  "program {:#x}, stage {}",
                  info.pgm_hash, static_cast<u32>(info.l_stage));
    }
    return false;
}

struct StageRawDependencyKey {
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    u64 fetch_shader_revision{};
};

[[nodiscard]] bool MatchesUserData(
    const StageRawDependencyKey& expected,
    std::span<const u32, Shader::ShaderParams::NumShaderUserData> user_data) noexcept {
#if defined(__AVX2__)
    const auto* expected_words = expected.user_data.data();
    const auto* current_words = user_data.data();
    __m256i difference =
        _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(expected_words)),
                         _mm256_loadu_si256(reinterpret_cast<const __m256i*>(current_words)));
    difference = _mm256_or_si256(
        difference,
        _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(expected_words + 8)),
                         _mm256_loadu_si256(reinterpret_cast<const __m256i*>(current_words + 8))));
    return _mm256_testz_si256(difference, difference) != 0;
#else
    return std::ranges::equal(expected.user_data, user_data);
#endif
}

struct GraphicsDependencyKey {
    u64 fixed_generation{};
    std::array<StageRawDependencyKey, MaxShaderStages> stage_keys{};
    std::array<VAddr, MaxShaderStages> stage_bases{};
    u32 active_mask{};
    // The key's depth and stencil formats follow the effective depth-stencil state, whose
    // DB_DEPTH_CONTROL, DB_RENDER_CONTROL and stencil registers do not advance the pipeline
    // generation.
    bool depth_stencil_attachment{};
};

struct StageCurrentEntry {
    Program* program{};
    VAddr program_base{};
    Stage stage{};
};

} // namespace

struct PipelineCache::OptimizationState {
    GraphicsDependencyKey graphics_dependency{};
    const GraphicsPipeline* graphics_pipeline{};
    bool graphics_valid{};
    bool graphics_cacheable{};
    bool shader_compile_pending{};
    std::array<StageCurrentEntry, MaxShaderStages> current_stages{};

    [[nodiscard]] bool MatchesGraphicsDependency(PipelineCache& cache);
    [[nodiscard]] bool CaptureGraphicsDependency(PipelineCache& cache);
};

SHAD_NO_INLINE bool PipelineCache::OptimizationState::MatchesGraphicsDependency(
    PipelineCache& cache) {
    const u32 expected_active_mask = graphics_dependency.active_mask;
    for (u32 logical_index = 0; logical_index < MaxShaderStages; ++logical_index) {
        const bool active = cache.infos[logical_index] != nullptr;
        if (active != ((expected_active_mask >> logical_index) & 1U)) {
            return false;
        }
        if (!active) {
            continue;
        }
        const auto& stage_cache = current_stages[logical_index];
        if (!stage_cache.program || cache.infos[logical_index] != &stage_cache.program->info) {
            return false;
        }
        const auto* shader_program =
            cache.liverpool->regs.ProgramForStage(static_cast<u32>(stage_cache.stage));
        if (!shader_program || !shader_program->Address<u32*>()) {
            return false;
        }
        const VAddr program_base = shader_program->Address<VAddr>();
        auto& program = *stage_cache.program;
        if (!program.specialization_plan_ready || !program.specialization_plan_cacheable ||
            program_base != stage_cache.program_base ||
            program_base != graphics_dependency.stage_bases[logical_index] ||
            !MatchesUserData(graphics_dependency.stage_keys[logical_index],
                             shader_program->user_data)) {
            return false;
        }

        program.info.pgm_base = program_base;
        program.info.user_data = shader_program->user_data;
        const auto cached_fetch_shader = GetCachedFetchShader(program);
        if (!cached_fetch_shader.IsUsable(program.info) ||
            cached_fetch_shader.revision !=
                graphics_dependency.stage_keys[logical_index].fetch_shader_revision) {
            return false;
        }
    }
    return true;
}

SHAD_NO_INLINE bool PipelineCache::OptimizationState::CaptureGraphicsDependency(
    PipelineCache& cache) {
    graphics_dependency = {};
    graphics_dependency.fixed_generation = cache.liverpool->GraphicsPipelineGeneration();
    graphics_dependency.depth_stencil_attachment =
        GetEffectiveDepthStencilState(cache.liverpool->regs).needs_attachment;
    for (u32 logical_index = 0; logical_index < MaxShaderStages; ++logical_index) {
        if (!cache.infos[logical_index]) {
            continue;
        }
        const auto& stage_cache = current_stages[logical_index];
        if (!stage_cache.program || cache.infos[logical_index] != &stage_cache.program->info) {
            return false;
        }
        const auto* shader_program =
            cache.liverpool->regs.ProgramForStage(static_cast<u32>(stage_cache.stage));
        if (!shader_program || !shader_program->Address<u32*>()) {
            return false;
        }
        const VAddr program_base = shader_program->Address<VAddr>();
        auto& program = *stage_cache.program;
        if (!BuildSpecializationPlan(program) || program_base != stage_cache.program_base) {
            return false;
        }

        program.info.pgm_base = program_base;
        program.info.user_data = shader_program->user_data;
        const auto cached_fetch_shader = GetCachedFetchShader(program);
        if (!cached_fetch_shader.IsUsable(program.info)) {
            return false;
        }

        graphics_dependency.active_mask |= 1U << logical_index;
        graphics_dependency.stage_bases[logical_index] = program_base;
        auto& stage_dependency = graphics_dependency.stage_keys[logical_index];
        std::ranges::copy(shader_program->user_data, stage_dependency.user_data.begin());
        stage_dependency.fetch_shader_revision = cached_fetch_shader.revision;
    }
    return true;
}

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

template <typename Program>
static void BuildCommonRuntimeInfo(Shader::RuntimeInfo& info, const Program& program) {
    info.num_user_data = program.settings.num_user_regs;
    info.num_input_vgprs = program.settings.vgpr_comp_cnt;
    info.num_allocated_vgprs = program.NumVgprs();
    info.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
    info.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
    info.fp_round_mode32 = program.settings.fp_round_mode32;
    info.fp_round_mode16_64 = program.settings.fp_round_mode64;
}

SHAD_NO_INLINE void PipelineCache::BuildGeometryRuntimeInfo(Shader::RuntimeInfo& info) {
    const auto& regs = liverpool->regs;
    BuildCommonRuntimeInfo(info, regs.gs_program);
    auto& gs_info = info.gs_info;
    gs_info.num_outputs = MapOutputs(gs_info.outputs, regs.vs_output_control);
    gs_info.output_vertices = regs.vgt_gs_max_vert_out;
    gs_info.num_invocations =
        regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
    if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
        gs_info.in_primitive = [&]() {
            switch (regs.tess_config.topology) {
            case AmdGpu::TessellationTopology::Point:
                return AmdGpu::PrimitiveType::PointList;
            case AmdGpu::TessellationTopology::Line:
                return AmdGpu::PrimitiveType::LineList;
            case AmdGpu::TessellationTopology::TriangleCw:
            case AmdGpu::TessellationTopology::TriangleCcw:
                return AmdGpu::PrimitiveType::TriangleList;
            default:
                UNREACHABLE();
            }
        }();
    } else {
        gs_info.in_primitive = regs.primitive_type;
    }
    for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
        gs_info.out_primitive[stream_id] = regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
    }
    gs_info.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
    gs_info.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
    gs_info.mode = regs.vgt_gs_mode.mode;
    const auto params_vc = AmdGpu::GetParams(regs.vs_program);
    gs_info.vs_copy = params_vc.code;
    gs_info.vs_copy_hash = params_vc.hash;
    DumpShader(gs_info.vs_copy, gs_info.vs_copy_hash, Shader::Stage::Vertex, 0, "copy.bin");
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(Stage stage, LogicalStage l_stage) {
    auto& info = runtime_infos[u32(l_stage)];
    const auto& regs = liverpool->regs;
    info.Initialize(stage);
    switch (stage) {
    case Stage::Local: {
        BuildCommonRuntimeInfo(info, regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = infos[u32(Shader::LogicalStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.ls_info.ls_stride = tess_constants.ls_stride;
        break;
    }
    case Stage::Hull: {
        BuildCommonRuntimeInfo(info, regs.hs_program);
        info.hs_info.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.hs_info.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.hs_info.tess_type = regs.tess_config.type;
        info.hs_info.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;

        // We need to initialize most hs_info fields after finding the V# with tess constants
        break;
    }
    case Stage::Export: {
        BuildCommonRuntimeInfo(info, regs.es_program);
        info.es_info.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        if (l_stage == LogicalStage::TessellationEval) {
            info.es_vs_info.tess_type = regs.tess_config.type;
            info.es_vs_info.tess_topology = regs.tess_config.topology;
            info.es_vs_info.tess_partitioning = regs.tess_config.partitioning;
        }
        break;
    }
    case Stage::Vertex: {
        BuildCommonRuntimeInfo(info, regs.vs_program);
        info.vs_info.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.vs_info.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.vs_info.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.vs_info.num_outputs = MapOutputs(info.vs_info.outputs, regs.vs_output_control);
        info.vs_info.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.vs_info.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        info.vs_info.clip_disable = regs.IsClipDisabled();
        if (l_stage == LogicalStage::TessellationEval) {
            info.es_vs_info.tess_type = regs.tess_config.type;
            info.es_vs_info.tess_topology = regs.tess_config.topology;
            info.es_vs_info.tess_partitioning = regs.tess_config.partitioning;
        }
        break;
    }
    case Stage::Geometry: {
        BuildGeometryRuntimeInfo(info);
        break;
    }
    case Stage::Fragment: {
        BuildCommonRuntimeInfo(info, regs.ps_program);
        info.fs_info.en_flags = regs.ps_input_ena;
        info.fs_info.addr_flags = regs.ps_input_addr;
        info.fs_info.num_inputs = regs.num_interp;
        info.fs_info.z_export_format = regs.z_export_format;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.fs_info.mrtz_mask = regs.depth_shader_control.z_export_enable |
                                 (stencil_ref_export_enable << 1) |
                                 (regs.depth_shader_control.mask_export_enable << 2) |
                                 (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.fs_info.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.fs_info.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.fs_info.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.fs_info.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.fs_info.color_buffers[i] = graphics_key.color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(Stage::Geometry));
        info.fs_info.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(Stage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        break;
    }
    case Stage::Compute: {
        const auto& cs_pgm = liverpool->GetCsRegs();
        info.num_user_data = cs_pgm.settings.num_user_regs;
        info.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
        info.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
        info.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
        info.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
        info.cs_info.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                       cs_pgm.num_thread_z.full};
        info.cs_info.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                    cs_pgm.IsTgidEnabled(2)};
        info.cs_info.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    return info;
}

std::vector<u8> PipelineCache::LoadNativePipelineCache() {
    if (!Config::isPipelineCacheEnabled()) {
        return {};
    }
    auto& database = Storage::DataBase::Instance();
    database.Open();
    std::vector<u8> blob;
    database.Load(Storage::BlobType::NativePipelineCache, std::string{NativePipelineCacheName},
                  blob);
    if (blob.empty()) {
        blob = ReadLegacyNativePipelineCache();
        if (blob.empty()) {
            return {};
        }
        // Moves the legacy copy into the game's cache on close.
        native_pipeline_cache_dirty.store(true, std::memory_order_release);
    }

    if (blob.size() < sizeof(NativePipelineCacheHeader) ||
        blob.size() > sizeof(NativePipelineCacheHeader) + MaxNativePipelineCacheSize) {
        LOG_WARNING(Render_Vulkan, "Ignoring invalid native Vulkan pipeline cache");
        return {};
    }

    NativePipelineCacheHeader header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    if (header.magic != NativePipelineCacheMagic || header.version != NativePipelineCacheVersion ||
        header.pipeline_key_version != Serialization::PipelineKeyVersion ||
        header.driver_version != instance.GetDriverVersion() || header.profile != profile ||
        header.payload_size != blob.size() - sizeof(header)) {
        LOG_INFO(Render_Vulkan, "Native Vulkan pipeline cache is stale; rebuilding it");
        return {};
    }

    std::vector<u8> data(blob.begin() + sizeof(header), blob.end());
    if (!ValidateNativePipelineCacheData(data, instance)) {
        LOG_WARNING(Render_Vulkan, "Ignoring incompatible native Vulkan pipeline cache");
        return {};
    }
    LOG_INFO(Render_Vulkan, "Loaded {} KiB native Vulkan pipeline cache", data.size() / 1024);
    return data;
}

void PipelineCache::Sync() {
    SaveNativePipelineCache();
    const bool persisted = !native_pipeline_cache_dirty.load(std::memory_order_acquire);
    Storage::DataBase::Instance().Close();
    if (persisted) {
        RemoveLegacyNativePipelineCache();
    }
}

void PipelineCache::SaveNativePipelineCacheCheckpoint() {
    // Saving to an archive before it is published only grows it.
    if (!Storage::DataBase::Instance().IsArchived()) {
        SaveNativePipelineCache();
    }
}

void PipelineCache::SaveNativePipelineCache() {
    auto& database = Storage::DataBase::Instance();
    if (!pipeline_cache || !database.IsOpened() ||
        !native_pipeline_cache_dirty.load(std::memory_order_acquire)) {
        return;
    }

    std::scoped_lock lock{native_pipeline_cache_mutex};
    if (!native_pipeline_cache_dirty.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    auto [result, data] = instance.GetDevice().getPipelineCacheData(*pipeline_cache);
    if (result != vk::Result::eSuccess || data.empty() ||
        data.size() > MaxNativePipelineCacheSize ||
        !ValidateNativePipelineCacheData(data, instance)) {
        native_pipeline_cache_dirty.store(true, std::memory_order_release);
        LOG_WARNING(Render_Vulkan, "Failed to retrieve native Vulkan pipeline cache: {}",
                    vk::to_string(result));
        return;
    }

    const NativePipelineCacheHeader header{
        .magic = NativePipelineCacheMagic,
        .version = NativePipelineCacheVersion,
        .pipeline_key_version = Serialization::PipelineKeyVersion,
        .driver_version = instance.GetDriverVersion(),
        .payload_size = data.size(),
        .profile = profile,
    };
    std::vector<u8> blob(sizeof(header) + data.size());
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), data.data(), data.size());
    if (!database.Save(Storage::BlobType::NativePipelineCache, std::string{NativePipelineCacheName},
                       std::move(blob))) {
        native_pipeline_cache_dirty.store(true, std::memory_order_release);
        LOG_WARNING(Render_Vulkan, "Failed to persist native Vulkan pipeline cache");
    }
}

void PipelineCache::StartGraphicsPipelineCompiler() {
    for (u32 index = 0; index < shader_module_workers.size(); ++index) {
        shader_module_workers[index] = std::jthread{[this, index] { CompilerThread(true, index); }};
    }
    for (u32 index = 0; index < graphics_pipeline_workers.size(); ++index) {
        graphics_pipeline_workers[index] =
            std::jthread{[this, index] { CompilerThread(false, index); }};
    }
}

void PipelineCache::StopGraphicsPipelineCompiler() {
    {
        std::scoped_lock lock{graphics_pipeline_tasks_mutex};
        graphics_pipeline_compiler_stopping = true;
    }
    shader_module_tasks_cv.notify_all();
    graphics_pipeline_tasks_cv.notify_all();
    for (auto& worker : shader_module_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    for (auto& worker : graphics_pipeline_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    // Shader module tasks hand their SPIR-V emission to the pipeline workers, which may have
    // stopped before the last hand-off arrived.
    for (auto& task : graphics_pipeline_tasks) {
        task();
    }
    graphics_pipeline_tasks.clear();
    if (shader_stall_burst.shaders != 0) {
        LogShaderStallBurst(std::exchange(shader_stall_burst, {}));
    }
}

void PipelineCache::WaitForGraphicsPipelineCompiler() {
    std::unique_lock lock{graphics_pipeline_tasks_mutex};
    compiler_idle_cv.wait(lock, [this] {
        return shader_module_tasks.empty() && graphics_pipeline_tasks.empty() &&
               graphics_pipeline_tasks_in_flight == 0;
    });
}

void PipelineCache::CompilerThread(bool shader_module_lane, u32 worker_index) {
    const auto name = shader_module_lane ? fmt::format("shadPS4:ShaderModule{}", worker_index)
                                         : fmt::format("shadPS4:ShaderCompiler{}", worker_index);
    Common::SetCurrentThreadName(name.c_str());
    // The command processor waits for shader module tasks, so they must not queue behind guest
    // threads. Pipeline compiles run off its critical path and yield to everything else.
    Common::SetCurrentThreadPriority(shader_module_lane ? Common::ThreadPriority::High
                                                        : Common::ThreadPriority::Low);
    auto& tasks = shader_module_lane ? shader_module_tasks : graphics_pipeline_tasks;
    auto& tasks_cv = shader_module_lane ? shader_module_tasks_cv : graphics_pipeline_tasks_cv;

    while (true) {
        std::packaged_task<void()> task;
        {
            std::unique_lock lock{graphics_pipeline_tasks_mutex};
            while (!graphics_pipeline_compiler_stopping && tasks.empty()) {
                // The shader module lane also closes the stall burst once shaders stop coming.
                if (!shader_module_lane || shader_stall_burst.shaders == 0) {
                    tasks_cv.wait(lock);
                } else if (const auto burst_end = shader_stall_burst.last_captured + ShaderBurstGap;
                           std::chrono::steady_clock::now() < burst_end) {
                    tasks_cv.wait_until(lock, burst_end);
                } else {
                    LogShaderStallBurst(std::exchange(shader_stall_burst, {}));
                }
            }
            if (tasks.empty()) {
                return;
            }
            task = std::move(tasks.front());
            tasks.pop_front();
            ++graphics_pipeline_tasks_in_flight;
        }
        task();
        // Saving the native cache can take a while, so it stays off the shader module lane.
        if (!shader_module_lane &&
            native_pipeline_cache_save_requested.exchange(false, std::memory_order_acq_rel)) {
            SaveNativePipelineCacheCheckpoint();
        }
        {
            std::scoped_lock lock{graphics_pipeline_tasks_mutex};
            ASSERT(graphics_pipeline_tasks_in_flight != 0);
            --graphics_pipeline_tasks_in_flight;
        }
        compiler_idle_cv.notify_all();
    }
}

void PipelineCache::QueueGraphicsPipelineTask(std::packaged_task<void()>&& task) {
    {
        std::scoped_lock lock{graphics_pipeline_tasks_mutex};
        ASSERT(!graphics_pipeline_compiler_stopping);
        graphics_pipeline_tasks.emplace_back(std::move(task));
    }
    graphics_pipeline_tasks_cv.notify_one();
}

void PipelineCache::QueueShaderModuleTask(std::packaged_task<void()>&& task) {
    {
        std::scoped_lock lock{graphics_pipeline_tasks_mutex};
        ASSERT(!graphics_pipeline_compiler_stopping);
        shader_module_tasks.emplace_back(std::move(task));
    }
    shader_module_tasks_cv.notify_one();
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetMasterSemaphore(), DescriptorHeapSizes},
      optimization{std::make_unique<OptimizationState>()},
      async_shader_recompiling{false} {
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
    };
    const auto initial_data = LoadNativePipelineCache();
    const vk::PipelineCacheCreateInfo cache_info{
        .initialDataSize = initial_data.size(),
        .pInitialData = initial_data.empty() ? nullptr : initial_data.data(),
    };
    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique(cache_info);
    if (cache_result != vk::Result::eSuccess && !initial_data.empty()) {
        LOG_WARNING(Render_Vulkan, "Driver rejected native Vulkan pipeline cache: {}",
                    vk::to_string(cache_result));
        auto fallback = instance.GetDevice().createPipelineCacheUnique({});
        cache_result = fallback.result;
        cache = std::move(fallback.value);
    }
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
    Shader::InitializeSrtWalker();
    WarmUp();
    SaveNativePipelineCacheCheckpoint();
    StartGraphicsPipelineCompiler();
}

PipelineCache::~PipelineCache() {
    StopGraphicsPipelineCompiler();
    PublishPendingProgramCompilations();
    SaveNativePipelineCache();
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline() {
    auto& opt = *optimization;

    if (opt.graphics_valid && opt.graphics_cacheable && opt.graphics_pipeline &&
        liverpool->GraphicsPipelineGeneration() == opt.graphics_dependency.fixed_generation &&
        GetEffectiveDepthStencilState(liverpool->regs).needs_attachment ==
            opt.graphics_dependency.depth_stencil_attachment) {
        if (opt.MatchesGraphicsDependency(*this)) {
            return opt.graphics_pipeline;
        }
    }

    const auto* pipeline = ResolveGraphicsPipelineSlow();
    opt.graphics_pipeline = pipeline;
    opt.graphics_valid = pipeline != nullptr;
    opt.graphics_cacheable = pipeline && CanReuseGraphicsPipeline();
    if (opt.graphics_cacheable && !opt.CaptureGraphicsDependency(*this)) {
        opt.graphics_cacheable = false;
    }
    return pipeline;
}

const GraphicsPipeline* PipelineCache::ResolveGraphicsPipelineSlow() {
    optimization->shader_compile_pending = false;
    if (!RefreshGraphicsKey()) {
        return nullptr;
    }
    const auto it = graphics_pipelines.find(graphics_key);
    if (it != graphics_pipelines.end()) [[likely]] {
        return it.value().get();
    }
    const auto pending_it = pending_graphics_pipelines.find(graphics_key);
    if (pending_it != pending_graphics_pipelines.end()) {
        if (pending_it.value().wait_for(std::chrono::seconds::zero()) !=
            std::future_status::ready) {
            return nullptr;
        }
        return PublishGraphicsPipeline(pending_it);
    }
    return CreateGraphicsPipeline();
}

const GraphicsPipeline* PipelineCache::PublishGraphicsPipeline(
    tsl::robin_map<GraphicsPipelineKey, std::future<std::unique_ptr<GraphicsPipeline>>>::iterator
        pending_it) {
    const auto key = pending_it.key();
    auto pipeline = pending_it.value().get();
    pending_graphics_pipelines.erase(pending_it);
    ASSERT(pipeline != nullptr);
    const auto [it, is_new] = graphics_pipelines.try_emplace(key, std::move(pipeline));
    ASSERT(is_new);
    return it.value().get();
}

SHAD_NO_INLINE const GraphicsPipeline* PipelineCache::CreateGraphicsPipeline() {
    const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(graphics_key);
    auto build = std::make_unique<GraphicsPipelineBuild>();
    build->key = graphics_key;
    std::ranges::copy(runtime_infos, build->runtime_infos.begin());
    std::ranges::copy(modules, build->modules.begin());
    if (fetch_shader && fetch_shader->has_value()) {
        build->fetch_shader.emplace(fetch_shader->value());
    }
    for (u32 stage = 0; stage < MaxShaderStages; ++stage) {
        const auto* info = infos[stage];
        build->runtime_stages[stage] = info;
        if (!info) {
            continue;
        }
        build->compile_stage_storage[stage].emplace(*info);
        auto& resources = build->compile_resources[stage];
        resources.buffers.assign(info->resolved_buffers.begin(), info->resolved_buffers.end());
        resources.images.assign(info->resolved_images.begin(), info->resolved_images.end());
        resources.samplers.assign(info->resolved_samplers.begin(), info->resolved_samplers.end());
        resources.fmasks.assign(info->resolved_fmasks.begin(), info->resolved_fmasks.end());
        resources.vertex_buffers.assign(info->resolved_vertex_buffers.begin(),
                                        info->resolved_vertex_buffers.end());
    }

    const bool compile_async = async_shader_recompiling;
    std::packaged_task<std::unique_ptr<GraphicsPipeline>()> build_task{
        [this, build = std::move(build), pipeline_hash, compile_async]() mutable {
            if (compile_async) {
                LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x} asynchronously",
                         pipeline_hash);
            } else {
                LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
            }
            auto compile_stages = build->BindCompileStages();
            GraphicsPipeline::SerializationSupport sdata{};
            auto pipeline = std::make_unique<GraphicsPipeline>(
                instance, scheduler, desc_heap, profile, build->key, *pipeline_cache,
                compile_stages, build->runtime_stages, build->runtime_infos,
                std::move(build->fetch_shader), build->modules, sdata, false);
            RegisterPipelineData(build->key, pipeline_hash, sdata);
            native_pipeline_cache_dirty.store(true, std::memory_order_release);
            const u32 updates =
                native_pipeline_cache_updates.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (updates % NativePipelineCacheSaveBatch == 0) {
                native_pipeline_cache_save_requested.store(true, std::memory_order_release);
            }
            return pipeline;
        }};
    auto future = build_task.get_future();
    ++num_new_pipelines;

    if (Config::collectShadersForDebug()) {
        for (auto stage = 0; stage < MaxShaderStages; ++stage) {
            if (infos[stage]) {
                auto& m = modules[stage];
                module_related_pipelines[m].emplace_back(graphics_key);
            }
        }
    }
    fetch_shader = nullptr;

    if (!compile_async) {
        build_task();
        auto pipeline = future.get();
        const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key, std::move(pipeline));
        ASSERT(is_new);
        return it.value().get();
    }

    const bool is_new =
        pending_graphics_pipelines.try_emplace(graphics_key, std::move(future)).second;
    ASSERT(is_new);
    QueueGraphicsPipelineTask(
        std::packaged_task<void()>{[task = std::move(build_task)]() mutable { task(); }});
    return nullptr;
}

bool PipelineCache::CanReuseGraphicsPipeline() const {
    for (u32 index = 0; index < MaxShaderStages; ++index) {
        const auto* info = infos[index];
        if (!info) {
            continue;
        }
        const auto* program = optimization->current_stages[index].program;
        if (!program || &program->info != info || !program->specialization_plan_ready ||
            !program->specialization_plan_cacheable) {
            return false;
        }
    }
    return true;
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);

        ComputePipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *infos[0],
                                                       modules[0], sdata, false);
        RegisterPipelineData(compute_key, sdata);
        native_pipeline_cache_dirty.store(true, std::memory_order_release);
        const u32 updates =
            native_pipeline_cache_updates.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (updates % NativePipelineCacheSaveBatch == 0) {
            native_pipeline_cache_save_requested.store(true, std::memory_order_release);
            QueueGraphicsPipelineTask(std::packaged_task<void()>{[] {}});
        }
        ++num_new_pipelines;

        if (Config::collectShadersForDebug()) {
            auto& m = modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    }
    return it->second.get();
}

bool PipelineCache::RefreshGraphicsKey() {
    std::memset(&graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = liverpool->regs;
    auto& key = graphics_key;

    const auto depth_stencil = GetEffectiveDepthStencilState(regs);
    const bool db_enabled = depth_stencil.needs_attachment;

    key.z_format = db_enabled && regs.depth_buffer.DepthValid()
                       ? regs.depth_buffer.z_info.format
                       : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = db_enabled && regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();
    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages()) {
        return false;
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages() {
    const auto& regs = liverpool->regs;
    auto& key = graphics_key;
    fetch_shader = nullptr;

    enum class BindResult {
        Inactive,
        Ready,
        Pending,
    };

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](Shader::Stage stage_in,
                                Shader::LogicalStage stage_out) -> BindResult {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            modules[stage_out_idx] = nullptr;
            return BindResult::Inactive;
        }

const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            modules[stage_out_idx] = nullptr;
            return BindResult::Inactive;
        }

        const auto params = AmdGpu::GetParams(*pgm);

        if (Config::getShaderSkipsEnabled()) {
            if (Config::ShouldSkipShader(params.hash)) {
                LOG_WARNING(Render_Vulkan, "Skipped graphics shader hash {:#x}.", params.hash);
                key.stage_hashes[stage_out_idx] = 0;
                infos[stage_out_idx] = nullptr;
                modules[stage_out_idx] = nullptr;
                return BindResult::Pending;
            }
        }

        const auto result = GetProgram(stage_in, stage_out, params, binding);
        if (!result) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            modules[stage_out_idx] = nullptr;
            optimization->shader_compile_pending = true;
            return BindResult::Pending;
        }
        const FetchShader* fetch_shader_{};
        std::tie(infos[stage_out_idx], modules[stage_out_idx], fetch_shader_,
                 key.stage_hashes[stage_out_idx]) = *result;
        if (fetch_shader_ && fetch_shader_->has_value()) {
            fetch_shader = fetch_shader_;
        }
        return BindResult::Ready;
    };

    infos.fill(nullptr);
    modules.fill(nullptr);

    if (bind_stage(Stage::Fragment, LogicalStage::Fragment) == BindResult::Pending) {
        return false;
    }

    const auto* fs_info = infos[static_cast<u32>(LogicalStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (bind_stage(Stage::Export, LogicalStage::Vertex) != BindResult::Ready) {
            return false;
        }
        if (bind_stage(Stage::Geometry, LogicalStage::Geometry) != BindResult::Ready) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (bind_stage(Stage::Hull, LogicalStage::TessellationControl) != BindResult::Ready) {
            return false;
        }
        if (bind_stage(Stage::Vertex, LogicalStage::TessellationEval) != BindResult::Ready) {
            return false;
        }
        if (bind_stage(Stage::Local, LogicalStage::Vertex) != BindResult::Ready) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (bind_stage(Stage::Hull, LogicalStage::TessellationControl) != BindResult::Ready) {
            return false;
        }
        if (bind_stage(Stage::Export, LogicalStage::TessellationEval) != BindResult::Ready) {
            return false;
        }
        if (bind_stage(Stage::Local, LogicalStage::Vertex) != BindResult::Ready) {
            return false;
        }
        if (bind_stage(Stage::Geometry, LogicalStage::Geometry) != BindResult::Ready) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs: {
        if (bind_stage(Stage::Vertex, LogicalStage::Vertex) == BindResult::Pending) {
            return false;
        }
        break;
    }
    default:
        UNREACHABLE_MSG("unhandled stage_en: {}", (u32)regs.stage_enable.raw);
    }

    const auto* vs_info = infos[static_cast<u32>(Shader::LogicalStage::Vertex)];
    if (vs_info && fetch_shader && fetch_shader->has_value() &&
        !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        ASSERT_MSG(vs_info->resolved_vertex_buffers.size() == (*fetch_shader)->attributes.size(),
                   "Resolved vertex buffer count does not match fetch shader attributes: {} != {}",
                   vs_info->resolved_vertex_buffers.size(), (*fetch_shader)->attributes.size());
        u32 vertex_binding = 0;
        for (const auto& attrib : (*fetch_shader)->attributes) {
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            const auto buffer = vs_info->resolved_vertex_buffers[vertex_binding];
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);

    if (Config::getShaderSkipsEnabled()) {
        if (Config::ShouldSkipShader(cs_params.hash)) {
            LOG_WARNING(Render_Vulkan, "Skipped compute shader hash {:#x}.", cs_params.hash);
            return false;
        }
    }

    const auto result =
        GetProgram(Shader::Stage::Compute, LogicalStage::Compute, cs_params, binding);
    ASSERT(result.has_value());
    std::tie(infos[0], modules[0], fetch_shader, compute_key.value) = *result;
    return true;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding,
                                              ShaderCompileResult* async_result,
                                              const std::function<void()>& on_guest_data_captured) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.stage, perm_idx, "bin");

    thread_local Shader::Pools pools;
    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    if (on_guest_data_captured) {
        on_guest_data_captured();
    }
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    DumpShader(spv, info.pgm_hash, info.stage, perm_idx, "spv");

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.stage, perm_idx, "spv");
    const bool is_patched = patch && Config::patchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    const auto name = GetShaderName(info.stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    const bool collect_shader =
        async_result ? async_result->collect_shader : Config::collectShadersForDebug();
    if (collect_shader && async_result) {
        async_result->debug_spv = spv;
        if (patch) {
            async_result->debug_patch = *patch;
        }
        async_result->is_patched = is_patched;
    } else if (collect_shader) {
        DebugState.CollectShader(name, info.l_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);
    return module;
}

void PipelineCache::QueueProgramCompilation(
    Program& program, Stage stage, LogicalStage l_stage, const Shader::ShaderParams& params,
    const Shader::RuntimeInfo& runtime_info, Shader::Backend::Bindings binding,
    std::optional<Shader::StageSpecialization> specialization, size_t permutation_index,
    u64 permutation_hash, bool initial_program) {
    ASSERT(stage != Stage::Compute);
    ASSERT(!program.pending_compilation);
    ASSERT(initial_program != specialization.has_value());

    auto result = std::make_shared<ShaderCompileResult>();
    std::ranges::copy(params.user_data, result->user_data.begin());
    result->code.assign(params.code.begin(), params.code.end());
    result->info = Shader::Info(stage, l_stage, params);
    result->info.user_data = result->user_data;
    result->runtime_info = runtime_info;
    if (stage == Stage::Geometry && !runtime_info.gs_info.vs_copy.empty()) {
        result->geometry_copy_code.assign(runtime_info.gs_info.vs_copy.begin(),
                                          runtime_info.gs_info.vs_copy.end());
        result->runtime_info.gs_info.vs_copy = result->geometry_copy_code;
    }
    result->binding_start = binding;
    result->bindings = binding;
    if (specialization) {
        result->specialization = std::move(*specialization);
    }
    result->permutation_index = permutation_index;
    result->permutation_hash = permutation_hash;
    result->initial_program = initial_program;
    result->collect_shader = Config::collectShadersForDebug();

    std::promise<void> guest_data_promise;
    auto guest_data_captured = guest_data_promise.get_future();
    std::promise<void> completion_promise;
    auto completion = completion_promise.get_future();
    std::packaged_task<void()> compile_task{
        [this, result, guest_data_promise = std::move(guest_data_promise),
         completion_promise = std::move(completion_promise)]() mutable {
            result->started_at = std::chrono::steady_clock::now();
            auto& info = result->info;
            const auto perm_idx = result->permutation_index;
            auto pools = AcquireShaderPools();
            std::optional<Shader::IR::Program> ir_program;
            try {
                LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.stage, info.pgm_hash,
                         perm_idx != 0 ? "(permutation)" : "");
                DumpShader(result->code, info.pgm_hash, info.stage, perm_idx, "bin");
                ir_program.emplace(Shader::TranslateProgram(result->code, *pools, info,
                                                            result->runtime_info, profile));
                if (result->initial_program) {
                    FetchShader compiled_fetch_shader{};
                    if (info.has_fetch_shader) {
                        compiled_fetch_shader = Shader::Gcn::ParseFetchShader(info);
                    }
                    ResolveStageResources(info, &compiled_fetch_shader, result->resolved_resources);
                    result->specialization =
                        Shader::StageSpecialization(info, result->runtime_info, profile,
                                                    result->binding_start, &compiled_fetch_shader);
                }
            } catch (...) {
                guest_data_promise.set_exception(std::current_exception());
                completion_promise.set_exception(std::current_exception());
                return;
            }
            guest_data_promise.set_value();

            // Only translation reads guest data, so the rest runs on a pipeline worker ahead of its
            // pipeline compiles and this lane is free for the next shader the command processor
            // waits for.
            std::packaged_task<void()> emit_task{
                [this, result, pools = std::move(pools), ir_program = std::move(ir_program),
                 completion_promise = std::move(completion_promise)]() mutable {
                    try {
                        auto& info = result->info;
                        const auto perm_idx = result->permutation_index;
                        auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, result->runtime_info,
                                                                     *ir_program, result->bindings);
                        DumpShader(spv, info.pgm_hash, info.stage, perm_idx, "spv");

                        auto patch = GetShaderPatch(info.pgm_hash, info.stage, perm_idx, "spv");
                        const bool is_patched = patch && Config::patchShaders();
                        if (is_patched) {
                            LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.stage,
                                     info.pgm_hash);
                        }
                        result->module =
                            CompileSPV(is_patched ? *patch : spv, instance.GetDevice());
                        const auto name = GetShaderName(info.stage, info.pgm_hash, perm_idx);
                        Vulkan::SetObjectName(instance.GetDevice(), result->module, name);
                        if (result->collect_shader) {
                            result->debug_spv = spv;
                            if (patch) {
                                result->debug_patch = *patch;
                            }
                            result->is_patched = is_patched;
                        }
                        RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);
                        completion_promise.set_value();
                    } catch (...) {
                        completion_promise.set_exception(std::current_exception());
                    }
                    ir_program.reset();
                    RecycleShaderPools(std::move(pools));
                }};
            {
                std::scoped_lock lock{graphics_pipeline_tasks_mutex};
                graphics_pipeline_tasks.emplace_front(std::move(emit_task));
            }
            graphics_pipeline_tasks_cv.notify_one();
        }};
    program.pending_compilation.emplace(
        Program::PendingCompilation{.result = result, .completion = std::move(completion)});
    const auto queued_at = std::chrono::steady_clock::now();
    QueueShaderModuleTask(std::move(compile_task));
    guest_data_captured.get();

    // The capture happened-before the promise was satisfied, so started_at is visible here.
    const auto captured_at = std::chrono::steady_clock::now();
    const std::chrono::nanoseconds stall = captured_at - queued_at;
    bool burst_opened{};
    {
        std::scoped_lock lock{graphics_pipeline_tasks_mutex};
        auto& burst = shader_stall_burst;
        if (burst.shaders != 0 && queued_at - burst.last_captured >= ShaderBurstGap) {
            LogShaderStallBurst(std::exchange(burst, {}));
        }
        if (burst.shaders++ == 0) {
            burst.first_queued = queued_at;
            burst_opened = true;
        }
        burst.last_captured = captured_at;
        burst.stall += stall;
        burst.queue += result->started_at - queued_at;
        if (stall > burst.max_stall) {
            burst.max_stall = stall;
            burst.max_stall_hash = params.hash;
        }
    }
    if (burst_opened) {
        // Wakes a shader module worker to close the burst once shaders stop coming.
        shader_module_tasks_cv.notify_one();
    }
}

bool PipelineCache::PublishProgramCompilation(Program& program) {
    if (!program.pending_compilation) [[likely]] {
        return true;
    }
    return PublishPendingCompilation(program);
}

SHAD_NO_INLINE bool PipelineCache::PublishPendingCompilation(Program& program) {
    auto& pending = *program.pending_compilation;
    if (pending.completion.wait_for(std::chrono::seconds::zero()) != std::future_status::ready) {
        return false;
    }

    auto result = std::move(pending.result);
    auto completion = std::move(pending.completion);
    program.pending_compilation.reset();
    completion.get();
    ASSERT(result->module);
    ASSERT(program.modules.size() == result->permutation_index);

    if (result->initial_program) {
        ASSERT(program.modules.empty());
        program.info = std::move(result->info);
    }
    result->specialization.info = &program.info;
    RegisterShaderMeta(program.info, result->specialization.fetch_shader_data,
                       result->specialization, result->permutation_hash, result->permutation_index);
    program.AddPermut(result->module, std::move(result->specialization));
    result->module = nullptr;
    program.modules.back().specialization_fingerprint =
        BuildStoredSpecializationFingerprint(program.modules.back().spec);

    if (result->collect_shader) {
        const auto name =
            GetShaderName(program.info.stage, program.info.pgm_hash, result->permutation_index);
        DebugState.CollectShader(name, program.info.l_stage, program.modules.back().module,
                                 result->debug_spv, result->code, result->debug_patch,
                                 result->is_patched);
    }

    if (result->initial_program) {
        program.info.user_data = {};
        program.info.resolved_buffers = {};
        program.info.resolved_images = {};
        program.info.resolved_samplers = {};
        program.info.resolved_fmasks = {};
        program.info.resolved_vertex_buffers = {};
    }
    return true;
}

void PipelineCache::PublishPendingProgramCompilations() {
    for (auto& [_, program] : program_cache) {
        if (program->pending_compilation) {
            const bool published = PublishProgramCompilation(*program);
            ASSERT(published);
        }
    }
}

std::optional<PipelineCache::Result> PipelineCache::GetProgram(Stage stage, LogicalStage l_stage,
                                                               const Shader::ShaderParams& params,
                                                               Shader::Backend::Bindings& binding) {
    const auto& runtime_info = BuildRuntimeInfo(stage, l_stage);
    auto& opt = *optimization;
    const u32 stage_index = static_cast<u32>(l_stage);
    auto& current_stage = opt.current_stages[stage_index];
    const auto start = binding;

    auto program_it = program_cache.find(params.hash);
    if (program_it == program_cache.end()) [[unlikely]] {
        const auto result = CreateProgram(stage, l_stage, params, runtime_info, binding);
        auto& program = *program_cache.find(params.hash).value();
        current_stage = result ? StageCurrentEntry{.program = &program,
                                                   .program_base = params.Base(),
                                                   .stage = stage}
                               : StageCurrentEntry{};
        return result;
    }

    auto& program = *program_it.value();
    const bool compilation_ready = PublishProgramCompilation(program);
    if (!compilation_ready && program.modules.empty()) {
        current_stage = {};
        return std::nullopt;
    }
    auto& info = program.info;
    RefreshDynamicProgramData(info, params);
    const auto cached_fetch_shader = GetCachedFetchShader(program);
    ResolveStageResources(info, cached_fetch_shader.parsed, program.resolved_resources);

    BuildSpecializationPlan(program);
    const bool fetch_shader_usable = cached_fetch_shader.IsUsable(info);
    // The resources were just resolved from guest memory, so the current specialization can be
    // compared even when descriptors come from an SRT walk. Only the graphics dependency key,
    // which skips resolution, needs a cacheable plan.
    if (fetch_shader_usable && IsSpecializationMatchable(program)) {
        const size_t current_permutation = program.current_permutation;
        auto& shape_keys = specialization_shape_keys;
        BuildSpecializationShapeKeys(info, cached_fetch_shader.parsed, shape_keys);
        if (current_permutation < program.modules.size()) [[likely]] {
            auto& module = program.modules[current_permutation];
            bool current_matches;
            {
                current_matches = MatchesSpecializationShape(program, current_permutation,
                                                             cached_fetch_shader.revision, start,
                                                             runtime_info, shape_keys);
                if (current_matches) [[likely]] {
                    if (VerifySpecializationShapes()) [[unlikely]] {
                        current_matches = VerifyShapeMatch(module.spec, info, runtime_info, start,
                                                           cached_fetch_shader.parsed);
                    }
                } else {
                    current_matches = MatchesCurrentSpecialization(
                        module.spec, info, runtime_info, start, cached_fetch_shader.parsed);
                    if (current_matches) {
                        RecordSpecializationShape(program, current_permutation,
                                                  cached_fetch_shader.revision, start, runtime_info,
                                                  shape_keys);
                    }
                }
            }
            if (current_matches) [[likely]] {
                info.AddBindings(binding);
                current_stage = {
                    .program = &program,
                    .program_base = params.Base(),
                    .stage = stage,
                };
                return Result{&info, module.module, cached_fetch_shader.parsed,
                              HashCombine(params.hash, current_permutation)};
            }
        }

        size_t permutation;
        {
            permutation = FindCachedPermutation(program, info, runtime_info, start,
                                                cached_fetch_shader.parsed, current_permutation);
        }
        if (permutation != Program::InvalidPermutation) {
            auto& module = program.modules[permutation];
            program.current_permutation = permutation;
            RecordSpecializationShape(program, permutation, cached_fetch_shader.revision, start,
                                      runtime_info, shape_keys);
            info.AddBindings(binding);
            current_stage = {
                .program = &program,
                .program_base = params.Base(),
                .stage = stage,
            };
            return Result{&info, module.module, cached_fetch_shader.parsed,
                          HashCombine(params.hash, permutation)};
        }
    }

    if (!compilation_ready) {
        current_stage = {};
        return std::nullopt;
    }

    const auto result = GetProgramSlow(program, stage, l_stage, params, runtime_info, binding,
                                       cached_fetch_shader.parsed);
    current_stage =
        result
            ? StageCurrentEntry{.program = &program, .program_base = params.Base(), .stage = stage}
            : StageCurrentEntry{};
    return result;
}

SHAD_NO_INLINE std::optional<PipelineCache::Result> PipelineCache::GetProgramSlow(
    Program& program, Stage stage, LogicalStage l_stage, const Shader::ShaderParams& params,
    const Shader::RuntimeInfo& runtime_info, Shader::Backend::Bindings& binding,
    const FetchShader* fetch_shader_) {
    auto& info = program.info;
    auto spec = [&] {
        return Shader::StageSpecialization(info, runtime_info, profile, binding, fetch_shader_);
    }();

    size_t perm_idx = program.modules.size();
    u64 perm_hash = HashCombine(params.hash, perm_idx);

    vk::ShaderModule module{};

    const auto it = std::ranges::find(program.modules, spec, &Program::Module::spec);
    if (it == program.modules.end()) [[unlikely]] {
        if (stage != Stage::Compute && async_shader_recompiling) {
            QueueProgramCompilation(program, stage, l_stage, params, runtime_info, binding,
                                    std::move(spec), perm_idx, perm_hash, false);
            return std::nullopt;
        }
        module = CompilePermutation(program, stage, l_stage, params, runtime_info, binding,
                                    std::move(spec), perm_idx, perm_hash);
    } else {
        info.AddBindings(binding);
        module = it->module;
        perm_idx = std::distance(program.modules.begin(), it);
        perm_hash = HashCombine(params.hash, perm_idx);
    }
    program.current_permutation = perm_idx;
    return Result{&program.info, module, fetch_shader_, perm_hash};
}

SHAD_NO_INLINE std::optional<PipelineCache::Result> PipelineCache::CreateProgram(
    Stage stage, LogicalStage l_stage, const Shader::ShaderParams& params,
    const Shader::RuntimeInfo& runtime_info, Shader::Backend::Bindings& binding) {
    auto [it_pgm, new_program] = program_cache.try_emplace(params.hash);
    ASSERT(new_program);
    it_pgm.value() = std::make_unique<Program>(stage, l_stage, params);
    auto& program = *it_pgm.value();
    const auto perm_hash = HashCombine(params.hash, 0);
    if (stage != Stage::Compute && async_shader_recompiling) {
        QueueProgramCompilation(program, stage, l_stage, params, runtime_info, binding,
                                std::nullopt, 0, perm_hash, true);
        return std::nullopt;
    }

    auto start = binding;
    auto compile_runtime_info = runtime_info;
    const auto module = CompileModule(program.info, compile_runtime_info, params.code, 0, binding);
    RefreshDynamicProgramData(program.info, params);
    const auto cached_fetch_shader = GetCachedFetchShader(program);
    ResolveStageResources(program.info, cached_fetch_shader.parsed, program.resolved_resources);
    auto spec = [&] {
        return Shader::StageSpecialization(program.info, compile_runtime_info, profile, start,
                                           cached_fetch_shader.parsed);
    }();
    RegisterShaderMeta(program.info, spec.fetch_shader_data, spec, perm_hash, 0);
    program.AddPermut(module, std::move(spec));
    program.modules[0].specialization_fingerprint =
        BuildStoredSpecializationFingerprint(program.modules[0].spec);
    program.current_permutation = 0;
    return Result{&program.info, module, cached_fetch_shader.parsed, perm_hash};
}

SHAD_NO_INLINE vk::ShaderModule PipelineCache::CompilePermutation(
    Program& program, Stage stage, LogicalStage l_stage, const Shader::ShaderParams& params,
    const Shader::RuntimeInfo& runtime_info, Shader::Backend::Bindings& binding,
    Shader::StageSpecialization&& specialization, size_t permutation_index, u64 permutation_hash) {
    auto compile_runtime_info = runtime_info;
    auto new_info = Shader::Info(stage, l_stage, params);
    const auto module =
        CompileModule(new_info, compile_runtime_info, params.code, permutation_index, binding);

    RegisterShaderMeta(program.info, specialization.fetch_shader_data, specialization,
                       permutation_hash, permutation_index);
    program.AddPermut(module, std::move(specialization));
    program.modules.back().specialization_fingerprint =
        BuildStoredSpecializationFingerprint(program.modules.back().spec);
    return module;
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    WaitForGraphicsPipelineCompiler();
    PublishPendingProgramCompilations();
    for (auto& [_, future] : pending_graphics_pipelines) {
        future.wait();
    }
    optimization->graphics_valid = false;
    optimization->graphics_pipeline = nullptr;
    for (auto& current_stage : optimization->current_stages) {
        current_stage = {};
    }
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
                pending_graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::Stage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::Stage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!Config::dumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::Stage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}

bool RunSpecializationShapeSelfTest(u32 rounds) {
    // Pairs of random descriptors that share only their shape keys must specialize identically;
    // otherwise a key misses a field the specialization reads.
    std::mt19937_64 rng{0x5eed5a9e5eed5a9eULL};
    const auto randomize = [&](auto& value) {
        std::array<u64, (sizeof(value) + sizeof(u64) - 1) / sizeof(u64)> words;
        for (auto& word : words) {
            word = rng();
        }
        std::memcpy(&value, words.data(), sizeof(value));
    };
    u64 checks = 0;
    u64 failures = 0;
    const auto check = [&](bool same, std::string_view what, u32 round) {
        ++checks;
        if (!same && failures++ < 16) {
            LOG_ERROR(Render_Vulkan, "Stage shape self-test: {} differs in round {}", what, round);
        }
    };
    constexpr u64 ImageAddressMask = (1ULL << 38) - 1;
    // MapNumberConversion only accepts 8, 16 and 32-bit data formats with SnormNz, like the
    // descriptors games produce.
    const auto sanitize_formats = [](auto& descriptor) {
        if (static_cast<AmdGpu::NumberFormat>(descriptor.num_format) ==
            AmdGpu::NumberFormat::SnormNz) {
            descriptor.data_format = static_cast<u32>(AmdGpu::DataFormat::Format32);
        }
    };

    Shader::RuntimeInfo runtime_info{};
    runtime_info.Initialize(Shader::Stage::Vertex);
    for (u32 round = 0; round < rounds; ++round) {
        AmdGpu::Buffer buffer;
        AmdGpu::Buffer buffer_twin;
        randomize(buffer);
        randomize(buffer_twin);
        if (rng() % 4 == 0) {
            buffer.num_records = 0;
        }
        sanitize_formats(buffer);
        buffer_twin.stride = buffer.stride;
        buffer_twin.swizzle_enable = buffer.swizzle_enable;
        buffer_twin.dst_sel_x = buffer.dst_sel_x;
        buffer_twin.dst_sel_y = buffer.dst_sel_y;
        buffer_twin.dst_sel_z = buffer.dst_sel_z;
        buffer_twin.dst_sel_w = buffer.dst_sel_w;
        buffer_twin.num_format = buffer.num_format;
        buffer_twin.data_format = buffer.data_format;
        buffer_twin.element_size = buffer.element_size;
        buffer_twin.index_stride = buffer.index_stride;
        buffer_twin.num_records = buffer.num_records != 0 ? static_cast<u32>(rng()) | 1U : 0U;
        check(BufferShapeKey(buffer) == BufferShapeKey(buffer_twin), "buffer key", round);
        check(static_cast<bool>(buffer) == static_cast<bool>(buffer_twin), "buffer binding", round);
        for (const bool formatted : {false, true}) {
            Shader::BufferResource resource{};
            resource.is_formatted = formatted;
            check(Shader::MakeBufferSpecialization(resource, buffer) ==
                      Shader::MakeBufferSpecialization(resource, buffer_twin),
                  "buffer specialization", round);
        }
        runtime_info.vs_info.step_rate_0 = static_cast<u32>(rng());
        runtime_info.vs_info.step_rate_1 = static_cast<u32>(rng());
        for (u8 step_rate = 0; step_rate < 4; ++step_rate) {
            Shader::Gcn::VertexAttribute attribute{};
            attribute.instance_data = step_rate;
            check(Shader::MakeVsAttribSpecialization(attribute, buffer, runtime_info) ==
                      Shader::MakeVsAttribSpecialization(attribute, buffer_twin, runtime_info),
                  "vertex attribute specialization", round);
        }

        AmdGpu::Image image;
        AmdGpu::Image image_twin;
        randomize(image);
        randomize(image_twin);
        if (rng() % 4 == 0) {
            image.base_address = 0;
        }
        sanitize_formats(image);
        image_twin.type = image.type;
        image_twin.data_format = image.data_format;
        image_twin.num_format = image.num_format;
        image_twin.dst_sel_x = image.dst_sel_x;
        image_twin.dst_sel_y = image.dst_sel_y;
        image_twin.dst_sel_z = image.dst_sel_z;
        image_twin.dst_sel_w = image.dst_sel_w;
        image_twin.base_level = image.base_level;
        image_twin.last_level = image.last_level;
        image_twin.base_address = image.base_address != 0 ? (rng() & ImageAddressMask) | 1 : 0;
        check(ImageShapeKey(image) == ImageShapeKey(image_twin), "image key", round);
        check(static_cast<bool>(image) == static_cast<bool>(image_twin), "image binding", round);
        for (u32 variant = 0; variant < 12; ++variant) {
            Shader::ImageResource resource{};
            resource.is_array = (variant & 1) != 0;
            resource.is_written = (variant & 2) != 0;
            resource.mip_fallback_mode = static_cast<Shader::MipStorageFallbackMode>(variant / 4);
            check(Shader::MakeImageSpecialization(resource, image) ==
                      Shader::MakeImageSpecialization(resource, image_twin),
                  "image specialization", round);
        }

        AmdGpu::Image fmask;
        AmdGpu::Image fmask_twin;
        randomize(fmask);
        randomize(fmask_twin);
        if (rng() % 4 == 0) {
            fmask.base_address = 0;
        }
        fmask_twin.width = fmask.width;
        fmask_twin.height = fmask.height;
        fmask_twin.base_address = fmask.base_address != 0 ? (rng() & ImageAddressMask) | 1 : 0;
        check(FMaskShapeKey(fmask) == FMaskShapeKey(fmask_twin), "fmask key", round);
        check(static_cast<bool>(fmask) == static_cast<bool>(fmask_twin), "fmask binding", round);
        check(Shader::MakeFMaskSpecialization(fmask) == Shader::MakeFMaskSpecialization(fmask_twin),
              "fmask specialization", round);

        AmdGpu::Sampler sampler;
        AmdGpu::Sampler sampler_twin;
        randomize(sampler);
        randomize(sampler_twin);
        if (rng() % 4 == 0) {
            sampler.raw0 = 0;
            sampler.raw1 = 0;
            sampler_twin.raw0 = 0;
            sampler_twin.raw1 = 0;
        } else {
            // Bit 0 of raw1 is the LOD bias, which the specialization does not read.
            sampler.raw1 |= 1;
            sampler_twin.raw1 |= 1;
            sampler_twin.force_unnormalized.Assign(sampler.force_unnormalized.Value());
            sampler_twin.force_degamma.Assign(sampler.force_degamma.Value());
        }
        check(SamplerShapeKey(sampler) == SamplerShapeKey(sampler_twin), "sampler key", round);
        check(Shader::MakeSamplerSpecialization(sampler) ==
                  Shader::MakeSamplerSpecialization(sampler_twin),
              "sampler specialization", round);
    }
    if (failures != 0) {
        LOG_ERROR(Render_Vulkan, "Stage shape self-test FAILED: {} of {} checks", failures, checks);
        return false;
    }
    LOG_INFO(Render_Vulkan, "Stage shape self-test passed: {} rounds, {} checks", rounds, checks);
    return true;
}
} // namespace Vulkan
