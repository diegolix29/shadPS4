// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/alignment.h"
#include "common/arch.h"
#include "common/assert.h"
#include "common/elf_info.h"
#include "common/logging/formatter.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/string_util.h"
#include "common/thread.h"
#include "core/aerolib/aerolib.h"
#include "core/aerolib/stubs.h"
#include "core/devtools/widget/module_list.h"
#include "core/emulator_settings.h"
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/fex_guest_cpu.h"
#include "core/guest_cpu/fex_hle_bridge.h"
#include "core/guest_cpu/guest_memory_validation_cache.h"
#include "core/guest_cpu/hle_call_adapter.h"
#endif
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/threads.h"
#include "core/libraries/sysmodule/sysmodule.h"
#include "core/linker.h"
#include "core/memory.h"
#include "core/tls.h"
#include "ipc/ipc.h"

#ifndef _WIN32
#include <signal.h>
#endif

namespace Core {

static PS4_SYSV_ABI void ProgramExitFunc() {
    LOG_ERROR(Core_Linker, "Exit function called");
}

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
static bool ValidateGuestMemory(void* context, std::uintptr_t address, std::size_t size,
                                bool writable) {
    if (context == nullptr || address == 0 || size == 0 || address > UINTPTR_MAX - size) {
        return false;
    }
    auto* memory = static_cast<MemoryManager*>(context);
    const auto required = static_cast<u32>(MemoryProt::CpuRead) |
                          (writable ? static_cast<u32>(MemoryProt::CpuWrite) : 0);
    static thread_local GuestCpu::GuestMemoryValidationCache validation_cache;
    const auto generation = memory->MappingGeneration();
    if (validation_cache.Contains(memory, generation, address, size, required)) {
        return true;
    }
    const auto end = address + size;
    auto cursor = address;
    while (cursor < end) {
        void* range_start{};
        void* range_end{};
        u32 protection{};
        u64 range_generation{};
        if (memory->QueryProtection(cursor, &range_start, &range_end, &protection,
                                    &range_generation) != ORBIS_OK) {
            return false;
        }
        const auto mapped_begin = reinterpret_cast<std::uintptr_t>(range_start);
        const auto mapped_end = reinterpret_cast<std::uintptr_t>(range_end);
        if (mapped_end <= cursor || (protection & required) != required) {
            return false;
        }
        validation_cache.Store(memory, range_generation, mapped_begin, mapped_end, protection);
        cursor = std::min(end, mapped_end);
    }
    return true;
}

static bool SealGuestExecutableMemory(MemoryManager& memory, std::uintptr_t begin,
                                      std::size_t size) {
    const auto result = memory.SealGuestExecutable(begin, size);
    if (result == ORBIS_OK) return true;
    LOG_ERROR(Core_Linker, "Unable to seal FEX guest code begin={:#x} size={:#x}: {}", begin,
              size, result);
    return false;
}

// HLE veneers live in host mmap pages outside the guest VMM. Dynamic PRX
// loads allocate more of them after the FEX thread snapshot is taken, so the
// dynamic QueryGuestExecutableRange path must see them here. Context layout
// matches Linker::FexExecutableQueryContext.
static std::optional<GuestExecutionRange> QueryGuestExecutableMemory(void* context,
                                                                     std::uintptr_t address) {
    if (context == nullptr || address == 0) return std::nullopt;
    auto* query = static_cast<FexExecutableQueryContext*>(context);
    if (query->veneers != nullptr) {
        if (auto range = query->veneers->QueryExecutableRange(address)) {
            return range;
        }
    }
    if (query->memory == nullptr) return std::nullopt;
    auto* memory = query->memory;
    void* range_start{};
    void* range_end{};
    u32 protection{};
    if (memory->QueryProtection(address, &range_start, &range_end, &protection) != ORBIS_OK ||
        (protection & static_cast<u32>(MemoryProt::CpuExec)) == 0 ||
        (protection & static_cast<u32>(MemoryProt::CpuWrite)) != 0) {
        return std::nullopt;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(range_start);
    const auto end = reinterpret_cast<std::uintptr_t>(range_end);
    if (begin == 0 || end <= begin) return std::nullopt;
    if (!SealGuestExecutableMemory(*memory, begin, end - begin)) return std::nullopt;
    return GuestExecutionRange{begin, end - begin, true, false};
}

static void ReportGuestHleFailure(void*, const GuestCpu::HleCallFailure& failure) {
    LOG_ERROR(Core_Linker, "FEX HLE call {} failed: {}", failure.name, failure.error);
}
#endif

static PS4_SYSV_ABI void* RunMainEntry [[noreturn]] (EntryParams* params) {
#ifdef ARCH_X86_64
    // Start shared library modules
    asm volatile("andq $-16, %%rsp\n" // Align to 16 bytes
                 "subq $8, %%rsp\n"   // videoout_basic expects the stack to be misaligned

                 // Kernel also pushes some more things here during process init
                 // at least: environment, auxv, possibly other things

                 "pushq 8(%1)\n" // copy EntryParams to top of stack like the kernel does
                 "pushq 0(%1)\n" // OpenOrbis expects to find it there

                 "movq %1, %%rdi\n" // also pass params and exit func
                 "movq %2, %%rsi\n" // as before

                 "jmp *%0\n" // can't use call here, as that would mangle the prepared stack.
                             // there's no coming back
                 :
                 : "r"(params->entry_addr), "r"(params), "r"(ProgramExitFunc)
                 : "rax", "rsi", "rdi");
    UNREACHABLE();
#else
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    auto* linker = Common::Singleton<Linker>::Instance();
    const auto result = linker->RunGuestMain(params);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&result)) {
        LOG_CRITICAL(Core_Linker, "FEX main entry failed at stage {}: {}",
                     static_cast<int>(failure->Stage), failure->Error);
    } else {
        LOG_CRITICAL(Core_Linker, "FEX main entry returned unexpectedly with {:#x}",
                     std::get<u64>(result));
    }
    std::abort();
#else
    UNREACHABLE_MSG("RunMainEntry requires an x86-64 host or FEX guest CPU support.");
#endif
#endif
}

Linker::Linker() : memory{Memory::Instance()} {
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    m_hle_veneers = std::make_unique<GuestCpu::HleVeneerAllocator>();
#endif
}

Linker::~Linker() = default;

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
namespace {

constexpr std::size_t FexTemporaryStackSize = 1_MB;

class GuestStackMapping final {
public:
    GuestStackMapping(MemoryManager& memory_, std::size_t size_, std::string_view name)
        : memory{memory_}, size{size_} {
        void* mapped{};
        const auto result = memory.MapMemory(&mapped, 0, size, MemoryProt::CpuReadWrite,
                                             MemoryMapFlags::NoFlags, VMAType::Stack, name);
        if (result == ORBIS_OK) {
            base = reinterpret_cast<VAddr>(mapped);
        } else {
            error = result;
        }
    }

    GuestStackMapping(const GuestStackMapping&) = delete;
    GuestStackMapping& operator=(const GuestStackMapping&) = delete;

    ~GuestStackMapping() {
        if (base != 0) {
            const auto result = memory.UnmapMemory(base, size);
            if (result != ORBIS_OK) {
                LOG_ERROR(Core_Linker, "Unable to unmap FEX guest stack {:#x}: {}", base,
                          result);
            }
        }
    }

    bool IsValid() const {
        return base != 0;
    }

    VAddr Top() const {
        return base + size;
    }

    int Error() const {
        return error;
    }

private:
    MemoryManager& memory;
    std::size_t size;
    VAddr base{};
    int error{};
};

std::optional<GuestExecutionRange> QueryGuestMemoryRange(MemoryManager& memory,
                                                         std::uintptr_t address,
                                                         bool executable, bool writable) {
    void* range_start{};
    void* range_end{};
    u32 protection{};
    if (memory.QueryProtection(address, &range_start, &range_end, &protection) != ORBIS_OK) {
        return std::nullopt;
    }
    if (executable && ((protection & static_cast<u32>(MemoryProt::CpuExec)) == 0 ||
                       (protection & static_cast<u32>(MemoryProt::CpuWrite)) != 0)) {
        return std::nullopt;
    }
    if (writable && ((protection & static_cast<u32>(MemoryProt::CpuWrite)) == 0 ||
                     (protection & static_cast<u32>(MemoryProt::CpuExec)) != 0)) {
        return std::nullopt;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(range_start);
    const auto end = reinterpret_cast<std::uintptr_t>(range_end);
    if (begin == 0 || end <= begin) return std::nullopt;
    if (executable && !SealGuestExecutableMemory(memory, begin, end - begin)) {
        return std::nullopt;
    }
    return GuestExecutionRange{begin, end - begin, executable, writable};
}

std::optional<GuestExecutionFailure> NormalizeGuestRanges(
    std::vector<GuestExecutionRange>& ranges) {
    std::ranges::sort(ranges, {}, &GuestExecutionRange::Begin);
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const auto& range = ranges[index];
        if (range.Begin == 0 || range.Size == 0 || range.Begin + range.Size < range.Begin) {
            LOG_ERROR(Core_Linker,
                      "Invalid FEX guest range index={} begin={:#x} size={:#x} executable={} "
                      "writable={}",
                      index, range.Begin, range.Size, range.Executable, range.Writable);
            return GuestExecutionFailure{GuestExecutionStage::Mapping, EINVAL};
        }
        if (index != 0) {
            const auto& previous = ranges[index - 1];
            if (previous.Begin + previous.Size > range.Begin) {
                LOG_ERROR(Core_Linker,
                          "FEX guest range overlap previous_index={} previous_begin={:#x} "
                          "previous_size={:#x} previous_executable={} previous_writable={} "
                          "index={} begin={:#x} size={:#x} executable={} writable={}",
                          index - 1, previous.Begin, previous.Size, previous.Executable,
                          previous.Writable, index, range.Begin, range.Size, range.Executable,
                          range.Writable);
                return GuestExecutionFailure{GuestExecutionStage::Mapping, EACCES};
            }
        }
    }
    return std::nullopt;
}

void SetGuestIntegerArguments(GuestExecutionRequest& request,
                              std::span<const u64> arguments) {
    constexpr std::array<std::size_t, 6> registers{7, 6, 2, 1, 8, 9};
    const auto count = std::min(arguments.size(), registers.size());
    for (std::size_t index = 0; index < count; ++index) {
        request.Gpr[registers[index]] = arguments[index];
    }
}

} // namespace

std::optional<GuestExecutionFailure> Linker::InitializeFexRuntime() {
    std::scoped_lock lock{m_fex_runtime_mutex};
    if (m_fex_backend != nullptr) return std::nullopt;

    auto& registry = m_hle_symbols.GetHleCallRegistry();
    if (m_fex_exit_veneer == 0) {
        const auto adapter = registry.Register(GuestCpu::MakeHleCallAdapter(ProgramExitFunc),
                                               "bachata.program_exit");
        if (adapter == nullptr) {
            return GuestExecutionFailure{GuestExecutionStage::Execute, EIO};
        }
        const auto veneer = m_hle_veneers->Allocate(*adapter);
        if (const auto* failure = std::get_if<GuestCpu::HleVeneerFailure>(&veneer)) {
            return GuestExecutionFailure{GuestExecutionStage::Mapping, failure->error};
        }
        m_fex_exit_veneer = std::get<u64>(veneer);
    }

    m_fex_exec_query = {memory, m_hle_veneers.get()};
    m_fex_bridge = std::make_unique<GuestCpu::HleGuestBridge>(
        registry, ValidateGuestMemory, memory, ReportGuestHleFailure, nullptr,
        QueryGuestExecutableMemory, &m_fex_exec_query);
    auto backend = FexGuestCpuBackend::Create(*m_fex_bridge);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&backend)) {
        m_fex_bridge.reset();
        return *failure;
    }
    m_fex_backend =
        std::move(std::get<std::unique_ptr<FexGuestCpuBackend>>(backend));
    return std::nullopt;
}

Linker::GuestFunctionResult Linker::RunGuestFunction(VAddr entry,
                                                      std::span<const u64> arguments,
                                                      VAddr stack_top) {
    if (const auto failure = InitializeFexRuntime()) return *failure;

    auto nested = m_fex_backend->CallGuest(entry, arguments);
    if (const auto* state = std::get_if<GuestExecutionState>(&nested)) {
        return state->Gpr[0];
    }
    const auto nestedFailure = std::get<GuestExecutionFailure>(nested);
    if (nestedFailure.Stage != GuestExecutionStage::Thread || nestedFailure.Error != ENXIO) {
        return nestedFailure;
    }

    std::optional<GuestStackMapping> temporaryStack;
    if (stack_top == 0) {
        temporaryStack.emplace(*memory, FexTemporaryStackSize, "FEX guest call stack");
        if (!temporaryStack->IsValid()) {
            return GuestExecutionFailure{GuestExecutionStage::Mapping,
                                         temporaryStack->Error() == 0 ? ENOMEM
                                                                      : temporaryStack->Error()};
        }
        stack_top = temporaryStack->Top();
    }
    if (arguments.size() > 32 || stack_top < 0x1000) {
        return GuestExecutionFailure{GuestExecutionStage::Request, E2BIG};
    }

    const auto stackArgumentBytes =
        (arguments.size() > 6 ? arguments.size() - 6 : 0) * sizeof(u64);
    if (stack_top < stackArgumentBytes + sizeof(u64)) {
        return GuestExecutionFailure{GuestExecutionStage::Request, EOVERFLOW};
    }
    const auto argumentTop = Common::AlignDown(stack_top - stackArgumentBytes, 16ULL);
    const auto rsp = argumentTop - sizeof(u64);
    const auto returnAddress = m_fex_backend->ReturnAddress();
    if (!ValidateGuestMemory(memory, rsp, sizeof(u64) + stackArgumentBytes, true) ||
        returnAddress == 0) {
        return GuestExecutionFailure{GuestExecutionStage::Mapping, EFAULT};
    }
    std::memcpy(reinterpret_cast<void*>(rsp), &returnAddress, sizeof(returnAddress));
    for (std::size_t index = 6; index < arguments.size(); ++index) {
        std::memcpy(reinterpret_cast<void*>(rsp + sizeof(u64) * (index - 5)),
                    &arguments[index], sizeof(u64));
    }

    const auto codeRange = QueryGuestMemoryRange(*memory, entry, true, false);
    const auto stackRange = QueryGuestMemoryRange(*memory, rsp, false, true);
    if (!codeRange || !stackRange) {
        return GuestExecutionFailure{GuestExecutionStage::Mapping, EFAULT};
    }

    GuestExecutionRequest request;
    request.Rip = entry;
    request.Rsp = rsp;
    request.Rflags = 1U << 1;
    request.FsBase = reinterpret_cast<std::uintptr_t>(GetTcbBase());
    SetGuestIntegerArguments(request, arguments);
    request.MappedRanges = {*codeRange, *stackRange, m_fex_backend->ReturnRange(),
                            m_fex_backend->CallbackReturnRange()};
    const auto veneerRanges = m_hle_veneers->GetExecutableRanges();
    request.MappedRanges.insert(request.MappedRanges.end(), veneerRanges.begin(),
                                veneerRanges.end());
    if (const auto failure = NormalizeGuestRanges(request.MappedRanges)) return *failure;

    const auto result = m_fex_backend->Run(request);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&result)) return *failure;
    const auto& state = std::get<GuestExecutionState>(result);
    if (state.StopReason != GuestStopReason::Halted ||
        state.Rip < returnAddress || state.Rip >= returnAddress + 0x1000) {
        return GuestExecutionFailure{GuestExecutionStage::Execute, EPROTO};
    }
    return state.Gpr[0];
}

Linker::GuestFunctionResult Linker::RunGuestMain(EntryParams* params) {
    if (params == nullptr || params->entry_addr == 0 || params->argc < 0 || params->argc > 33) {
        return GuestExecutionFailure{GuestExecutionStage::Request, EINVAL};
    }
    if (const auto failure = InitializeFexRuntime()) return *failure;

    std::size_t stackSize = FexTemporaryStackSize;
    if (const auto* proc = GetProcParam(); proc != nullptr && proc->main_thread_stack_size != nullptr) {
        stackSize = std::clamp<std::size_t>(*proc->main_thread_stack_size, 64_KB, 64_MB);
        stackSize = Common::AlignUp(stackSize, 16_KB);
    }
    GuestStackMapping stack{*memory, stackSize, "FEX main guest stack"};
    if (!stack.IsValid()) {
        return GuestExecutionFailure{GuestExecutionStage::Mapping,
                                     stack.Error() == 0 ? ENOMEM : stack.Error()};
    }

    EntryParams guestParams = *params;
    auto cursor = stack.Top();
    for (int index = params->argc - 1; index >= 0; --index) {
        if (params->argv[index] == nullptr) {
            guestParams.argv[index] = nullptr;
            continue;
        }
        const auto length = std::strlen(params->argv[index]) + 1;
        if (cursor < length) {
            return GuestExecutionFailure{GuestExecutionStage::Mapping, EOVERFLOW};
        }
        cursor -= length;
        std::memcpy(reinterpret_cast<void*>(cursor), params->argv[index], length);
        guestParams.argv[index] = reinterpret_cast<const char*>(cursor);
    }
    cursor = Common::AlignDown(cursor - sizeof(EntryParams), 16ULL);
    std::memcpy(reinterpret_cast<void*>(cursor), &guestParams, sizeof(guestParams));
    const auto guestParamsAddress = cursor;
    if (cursor < 24) {
        return GuestExecutionFailure{GuestExecutionStage::Mapping, EOVERFLOW};
    }
    const auto rsp = Common::AlignDown(cursor - 24, 16ULL) + 8;
    std::memcpy(reinterpret_cast<void*>(rsp), &guestParams, 16);

    const auto codeRange = QueryGuestMemoryRange(*memory, params->entry_addr, true, false);
    const auto stackRange = QueryGuestMemoryRange(*memory, rsp, false, true);
    if (!codeRange || !stackRange) {
        return GuestExecutionFailure{GuestExecutionStage::Mapping, EFAULT};
    }

    GuestExecutionRequest request;
    request.Rip = params->entry_addr;
    request.Rsp = rsp;
    request.Rflags = 1U << 1;
    request.Gpr[7] = guestParamsAddress;
    request.Gpr[6] = m_fex_exit_veneer;
    request.FsBase = reinterpret_cast<std::uintptr_t>(GetTcbBase());
    request.MappedRanges = {*codeRange, *stackRange, m_fex_backend->ReturnRange(),
                            m_fex_backend->CallbackReturnRange()};
    const auto veneerRanges = m_hle_veneers->GetExecutableRanges();
    request.MappedRanges.insert(request.MappedRanges.end(), veneerRanges.begin(),
                                veneerRanges.end());
    if (const auto failure = NormalizeGuestRanges(request.MappedRanges)) return *failure;

    const auto result = m_fex_backend->Run(request);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&result)) return *failure;
    return std::get<GuestExecutionState>(result).Gpr[0];
}
#endif

void Linker::Execute(const std::vector<std::string>& args) {
    if (EmulatorSettings.IsDebugDump()) {
        DebugDump();
    }

    // Calculate static TLS size.
    Module* module = m_modules[0].get();
    static_tls_size = module->tls.offset = module->tls.image_size;

    // Relocate all modules
    for (const auto& m : m_modules) {
        Relocate(m.get());
    }

    // Configure the direct and flexible memory regions.
    u64 fmem_size = ORBIS_KERNEL_FLEXIBLE_MEMORY_SIZE;
    bool use_extended_mem1 = true, use_extended_mem2 = true;

    const auto* proc_param = GetProcParam();
    ASSERT(proc_param);

    Core::OrbisKernelMemParam mem_param{};
    if (proc_param->size >= offsetof(OrbisProcParam, mem_param) + sizeof(OrbisKernelMemParam*)) {
        if (proc_param->mem_param) {
            mem_param = *proc_param->mem_param;
            if (mem_param.size >=
                offsetof(OrbisKernelMemParam, flexible_memory_size) + sizeof(u64*)) {
                if (const auto* flexible_size = mem_param.flexible_memory_size) {
                    fmem_size = *flexible_size + ORBIS_KERNEL_FLEXIBLE_MEMORY_BASE;
                }
            }
        }
    }

    if (mem_param.size < offsetof(OrbisKernelMemParam, extended_memory_1) + sizeof(u64*)) {
        mem_param.extended_memory_1 = nullptr;
    }
    if (mem_param.size < offsetof(OrbisKernelMemParam, extended_memory_2) + sizeof(u64*)) {
        mem_param.extended_memory_2 = nullptr;
    }

    const u64 sdk_ver = proc_param->sdk_version;
    if (sdk_ver < Common::ElfInfo::FW_500) {
        use_extended_mem1 = mem_param.extended_memory_1 ? *mem_param.extended_memory_1 : false;
        use_extended_mem2 = mem_param.extended_memory_2 ? *mem_param.extended_memory_2 : false;
    }

    memory->SetupMemoryRegions(fmem_size, use_extended_mem1, use_extended_mem2);

    main_thread.Run([this, module, &args](std::stop_token) {
        Common::SetCurrentThreadName("Game:Main");
        std::set_terminate(Common::Log::Terminate);

#ifndef _WIN32 // Clear any existing signal mask for game threads.
        sigset_t emptyset;
        sigemptyset(&emptyset);
        pthread_sigmask(SIG_SETMASK, &emptyset, nullptr);
#endif
        if (auto& ipc = IPC::Instance()) {
            ipc.WaitForStart();
        }

        // Have libSceSysmodule preload our libraries.
        Libraries::SysModule::sceSysmodulePreloadModuleForLibkernel();

        // Load and start custom modules from the user directory.
        std::string_view id = Common::ElfInfo::Instance().GameSerial();
        const auto& custom_mod_directory =
            Common::FS::GetUserPath(Common::FS::PathType::CustomModulesDir) / id;
        if (!std::filesystem::exists(custom_mod_directory)) {
            std::filesystem::create_directory(custom_mod_directory);
        }
        for (const auto& entry : std::filesystem::directory_iterator(custom_mod_directory)) {
            if (entry.is_regular_file()) {
                LOG_INFO(Core_Linker, "Loading custom module: {}",
                         fmt::UTF(entry.path().u8string()));
                if (LoadAndStartModule(entry.path(), 0, nullptr, nullptr) == -1) {
                    LOG_ERROR(Core_Linker, "Failed to load custom module: {}",
                              fmt::UTF(entry.path().u8string()));
                }
            }
        }

        // Simulate libSceGnmDriver initialization, which maps a chunk of direct memory.
        // Some games fail without accurately emulating this behavior.
        s64 phys_addr{};
        s32 result = Libraries::Kernel::sceKernelAllocateDirectMemory(
            0, Libraries::Kernel::sceKernelGetDirectMemorySize(), 0x10000, 0x10000, 3, &phys_addr);
        if (result == 0) {
            void* addr{reinterpret_cast<void*>(0xfe0000000)};
            result = Libraries::Kernel::sceKernelMapNamedDirectMemory(
                &addr, 0x10000, 0x13, 0, phys_addr, 0x10000, "SceGnmDriver");
        }
        ASSERT_MSG(result == 0, "Unable to emulate libSceGnmDriver initialization");

        // Add all guest arguments, we will always have the executable path in argv[0]
        EntryParams& params = Libraries::Kernel::entry_params;
        constexpr int MaxArgs = sizeof(params.argv) / sizeof(params.argv[0]);
        params.argc = std::min<int>(args.size(), MaxArgs);
        for (int i = 0; i < params.argc; i++) {
            params.argv[i] = args[i].c_str();
        }

        // Run the game's entry function
        params.entry_addr = module->GetEntryAddress();
        RunMainEntry(&params);
    });
}

s32 Linker::LoadModule(const std::filesystem::path& elf_name, bool is_dynamic) {
    std::scoped_lock lk{mutex};

    if (!std::filesystem::exists(elf_name)) {
        LOG_ERROR(Core_Linker, "Provided file {} does not exist", elf_name.string());
        return -1;
    }

    auto module = std::make_unique<Module>(memory, elf_name, max_tls_index);
    if (!module->IsValid()) {
        LOG_ERROR(Core_Linker, "Provided file {} is not valid ELF file", elf_name.string());
        return -1;
    }

    num_static_modules += !is_dynamic;
    m_modules.emplace_back(std::move(module));

    Core::Devtools::Widget::ModuleList::AddModule(elf_name.filename().string(), elf_name);

    return m_modules.size() - 1;
}

s32 Linker::LoadAndStartModule(const std::filesystem::path& path, u64 args, const void* argp,
                               int* pRes) {
    u32 handle = FindByName(path);
    if (handle != -1) {
        return handle;
    }
    handle = LoadModule(path, true);
    if (handle == -1) {
        return -1;
    }
    auto* module = GetModule(handle);
    RelocateAnyImports(module);

    // If the new module has a TLS image, trigger its load when TlsGetAddr is called.
    if (module->tls.image_size != 0) {
        AdvanceGenerationCounter();
    }

    // Retrieve and verify proc param according to libkernel.
    auto* param = module->GetProcParam<OrbisProcParam*>();
    ASSERT_MSG(!param || param->size >= 0x18, "Invalid module param size: {}", param->size);
    s32 ret = module->Start(args, argp, param);
    if (pRes) {
        *pRes = ret;
    }

    return handle;
}

Module* Linker::FindByAddress(VAddr address) {
    for (auto& module : m_modules) {
        const VAddr base = module->GetBaseAddress();
        if (address >= base && address < base + module->aligned_base_size) {
            return module.get();
        }
    }
    return nullptr;
}

void Linker::Relocate(Module* module) {
    module->ForEachRelocation([&](elf_relocation* rel, u32 i, bool is_jmp_rel) {
        const u32 num_relocs = module->dynamic_info.relocation_table_size / sizeof(elf_relocation);
        const u32 bit_idx = (is_jmp_rel ? num_relocs : 0) + i;
        if (module->TestRelaBit(bit_idx)) {
            return;
        }
        auto type = rel->GetType();
        auto symbol = rel->GetSymbol();
        auto addend = rel->rel_addend;
        auto* symbol_table = module->dynamic_info.symbol_table;
        auto* names_tlb = module->dynamic_info.str_table;

        const VAddr rel_base_virtual_addr = module->GetBaseAddress();
        const VAddr rel_virtual_addr = rel_base_virtual_addr + rel->rel_offset;
        bool rel_is_resolved = false;
        u64 rel_value = 0;
        Loader::SymbolType rel_sym_type = Loader::SymbolType::Unknown;
        std::string rel_name;

        switch (type) {
        case R_X86_64_RELATIVE:
            rel_value = rel_base_virtual_addr + addend;
            rel_is_resolved = true;
            module->SetRelaBit(bit_idx);
            break;
        case R_X86_64_DTPMOD64:
            rel_value = static_cast<u64>(module->tls.modid);
            rel_is_resolved = true;
            rel_sym_type = Loader::SymbolType::Tls;
            module->SetRelaBit(bit_idx);
            break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            addend = 0;
        case R_X86_64_64: {
            auto sym = symbol_table[symbol];
            auto sym_bind = sym.GetBind();
            auto sym_type = sym.GetType();
            auto sym_visibility = sym.GetVisibility();
            u64 symbol_virtual_addr = 0;
            Loader::SymbolRecord symrec{};
            switch (sym_type) {
            case STT_FUN:
                rel_sym_type = Loader::SymbolType::Function;
                break;
            case STT_OBJECT:
                rel_sym_type = Loader::SymbolType::Object;
                break;
            case STT_NOTYPE:
                rel_sym_type = Loader::SymbolType::NoType;
                break;
            default:
                ASSERT_MSG(0, "unknown symbol type {}", sym_type);
            }

            if (sym_visibility != 0) {
                LOG_INFO(Core_Linker, "symbol visibility !=0");
            }

            switch (sym_bind) {
            case STB_LOCAL:
                symbol_virtual_addr = rel_base_virtual_addr + sym.st_value;
                module->SetRelaBit(bit_idx);
                break;
            case STB_GLOBAL:
            case STB_WEAK: {
                rel_name = names_tlb + sym.st_name;
                if (Resolve(rel_name, rel_sym_type, module, &symrec)) {
                    // Only set the rela bit if the symbol was actually resolved and not stubbed.
                    module->SetRelaBit(bit_idx);
                }
                symbol_virtual_addr = symrec.virtual_address;
                break;
            }
            default:
                UNREACHABLE_MSG("Unknown bind type {}", sym_bind);
            }
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
            if (rel_sym_type == Loader::SymbolType::Function && symrec.hle_adapter != nullptr) {
                if (m_hle_veneers == nullptr) {
                    m_hle_veneers = std::make_unique<GuestCpu::HleVeneerAllocator>();
                }
                const auto veneer = m_hle_veneers->Allocate(*symrec.hle_adapter);
                if (const auto* failure = std::get_if<GuestCpu::HleVeneerFailure>(&veneer)) {
                    LOG_ERROR(Core_Linker, "Unable to allocate FEX HLE veneer for {}: {}", symrec.name,
                              failure->error);
                } else {
                    symbol_virtual_addr = std::get<u64>(veneer);
                }
            }
#endif
            rel_is_resolved = (symbol_virtual_addr != 0);
            const u64 resolved_base = rel_is_resolved ? symbol_virtual_addr : 0;
            rel_value = resolved_base + addend;
            rel_name = symrec.name;
            break;
        }
        default:
            LOG_INFO(Core_Linker, "UNK type {:#010x} rel symbol : {:#010x}", type, symbol);
        }

        if (rel_is_resolved) {
            std::memcpy(reinterpret_cast<void*>(rel_virtual_addr), &rel_value, sizeof(rel_value));
        } else {
            LOG_INFO(Core_Linker, "Function not patched! {}", rel_name);
        }
    });
}

bool Linker::Resolve(const std::string& name, Loader::SymbolType sym_type, Module* m,
                     Loader::SymbolRecord* return_info) {
    const auto ids = Common::SplitString(name, '#');
    if (ids.size() != 3) {
        return_info->virtual_address = 0;
        return_info->name = name;
        LOG_ERROR(Core_Linker, "Not Resolved {}", name);
        return false;
    }

    const LibraryInfo* library = m->FindLibrary(ids[1]);
    const ModuleInfo* module = m->FindModule(ids[2]);
    ASSERT_MSG(library && module, "Unable to find library and module");

    Loader::SymbolResolver sr{};
    sr.name = ids.at(0);
    sr.library = library->name;
    sr.library_version = library->version;
    sr.module = module->name;
    sr.type = sym_type;

    const auto* record = m_hle_symbols.FindSymbol(sr);
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    if (record != nullptr && !record->hle_fallback) {
#else
    if (record != nullptr) {
#endif
        *return_info = *record;
        Core::Devtools::Widget::ModuleList::AddModule(sr.library);
        return true;
    }

    // Check if it an exported function from one of our loaded libraries
    for (const auto& mod : m_modules) {
        if (!std::ranges::contains(mod->GetExportLibs(), *library) ||
            !std::ranges::contains(mod->GetExportModules(), *module)) {
            continue;
        }
        if (mod->export_sym.GetSize() == 0) {
            continue;
        }
        record = mod->export_sym.FindSymbol(sr);
        if (record) {
            *return_info = *record;
            return true;
        }
    }

    const auto aeronid = AeroLib::FindByNid(sr.name.c_str());
    if (sym_type == Loader::SymbolType::Object) {
        return_info->name = aeronid ? aeronid->name : "Unknown object";
        return_info->virtual_address = 0;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    } else if (sym_type == Loader::SymbolType::Function) {
        return_info->name = aeronid ? aeronid->name : "Unknown function";
        return_info->virtual_address = 0;
        if (record != nullptr && record->hle_fallback) {
            return_info->hle_adapter = record->hle_adapter;
        } else {
            return_info->hle_adapter = m_hle_symbols.AddUnsupportedFunction(sr);
        }
        LOG_WARNING(Core_Linker, "FEX: unresolved HLE {} uses temporary ENOSYS fallback",
                    return_info->name);
        return false;
#endif
    } else if (aeronid) {
        return_info->name = aeronid->name;
        return_info->virtual_address = AeroLib::GetStub(aeronid->nid);
    } else {
        return_info->virtual_address = AeroLib::GetStub(sr.name.c_str());
        return_info->name = "Unknown !!!";
    }
    if (library->name != "libc" && library->name != "libSceFios2") {
        LOG_WARNING(Core_Linker, "Linker: Stub resolved {} as {} (lib: {}, mod: {})", sr.name,
                    return_info->name, library->name, module->name);
    }
    return false;
}

void* Linker::TlsGetAddr(u64 module_index, u64 offset) {
    std::scoped_lock lk{mutex};

    DtvEntry* dtv_table = GetTcbBase()->tcb_dtv;
    if (dtv_table[0].counter != dtv_generation_counter) {
        // Generation counter changed, a dynamic module was either loaded or unloaded.
        const u32 old_num_dtvs = dtv_table[1].counter;
        ASSERT_MSG(max_tls_index > old_num_dtvs, "Module unloading unsupported");
        // Module was loaded, increase DTV table size.
        DtvEntry* new_dtv_table = new DtvEntry[max_tls_index + 2]{};
        std::memcpy(new_dtv_table + 2, dtv_table + 2, old_num_dtvs * sizeof(DtvEntry));
        new_dtv_table[0].counter = dtv_generation_counter;
        new_dtv_table[1].counter = max_tls_index;
        delete[] dtv_table;

        // Update TCB pointer.
        GetTcbBase()->tcb_dtv = new_dtv_table;
        dtv_table = new_dtv_table;
    }

    u8* addr = dtv_table[module_index + 1].pointer;
    Module* module = m_modules[module_index - 1].get();
    if (!addr) {
        // Module was just loaded by above code. Allocate TLS block for it.
        const u32 init_image_size = module->tls.init_image_size;
        u8* dest = reinterpret_cast<u8*>(CallAppHeapMalloc(module->tls.image_size));
        const u8* src = reinterpret_cast<const u8*>(module->tls.image_virtual_addr);
        std::memcpy(dest, src, init_image_size);
        std::memset(dest + init_image_size, 0, module->tls.image_size - init_image_size);
        dtv_table[module_index + 1].pointer = dest;
        addr = dest;
    }
    return addr + offset;
}

void* Linker::CallAppHeapMalloc(u64 size) {
    ASSERT_MSG(heap_api != nullptr && heap_api->heap_malloc != nullptr,
               "Guest heap malloc callback is unavailable");
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    const std::array<u64, 1> arguments{size};
    const auto result =
        RunGuestFunction(reinterpret_cast<VAddr>(heap_api->heap_malloc), arguments);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&result)) {
        LOG_CRITICAL(Core_Linker, "FEX guest heap malloc failed at stage {}: {}",
                     static_cast<int>(failure->Stage), failure->Error);
        std::abort();
    }
    return reinterpret_cast<void*>(std::get<u64>(result));
#else
    return heap_api->heap_malloc(size);
#endif
}

void Linker::CallAppHeapFree(void* pointer) {
    ASSERT_MSG(heap_api != nullptr && heap_api->heap_free != nullptr,
               "Guest heap free callback is unavailable");
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    const std::array<u64, 1> arguments{reinterpret_cast<u64>(pointer)};
    const auto result = RunGuestFunction(reinterpret_cast<VAddr>(heap_api->heap_free), arguments);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&result)) {
        LOG_CRITICAL(Core_Linker, "FEX guest heap free failed at stage {}: {}",
                     static_cast<int>(failure->Stage), failure->Error);
        std::abort();
    }
#else
    heap_api->heap_free(pointer);
#endif
}

void* Linker::AllocateTlsForThread(bool is_primary) {
    static constexpr size_t TcbSize = 0x40;
    static constexpr size_t TlsAllocAlign = 0x20;
    const size_t total_tls_size = Common::AlignUp(static_tls_size, TlsAllocAlign) + TcbSize;

    // If sceKernelMapNamedFlexibleMemory is being called from libkernel and addr = 0
    // it automatically places mappings in system reserved area instead of managed.
    // Since the system reserved area already has a mapping in it, this address is slightly higher.
    static constexpr VAddr KernelAllocBase = 0x881000000ULL;

    // The kernel module has a few different paths for TLS allocation.
    // For SDK < 1.7 it allocates both main and secondary thread blocks using libc mspace/malloc.
    // In games compiled with newer SDK, the main thread gets mapped from flexible memory,
    // with addr = 0, so system managed area. Here we will only implement the latter.
    void* addr_out{reinterpret_cast<void*>(KernelAllocBase)};
    if (is_primary) {
        const size_t tls_aligned = Common::AlignUp(total_tls_size, 16_KB);
        const int ret = Libraries::Kernel::sceKernelMapNamedFlexibleMemory(
            &addr_out, tls_aligned, 3, 0, "SceKernelPrimaryTcbTls");
        ASSERT_MSG(ret == 0, "Unable to allocate TLS+TCB for the primary thread");
    } else {
        if (heap_api) {
            addr_out = CallAppHeapMalloc(total_tls_size);
        } else {
            addr_out = std::malloc(total_tls_size);
        }
    }
    return addr_out;
}

void Linker::FreeTlsForNonPrimaryThread(void* pointer) {
    if (heap_api) {
        CallAppHeapFree(pointer);
    } else {
        std::free(pointer);
    }
}

void Linker::DebugDump() {
    const auto& log_dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
    const std::filesystem::path debug(log_dir / "debugdump");
    std::filesystem::create_directory(debug);
    for (const auto& m : m_modules) {
        Module* module = m.get();
        auto& elf = module->elf;
        const std::filesystem::path filepath(debug / module->file.stem());
        std::filesystem::create_directory(filepath);
        module->import_sym.DebugDump(filepath / "imports.txt");
        module->export_sym.DebugDump(filepath / "exports.txt");
        if (elf.IsSelfFile()) {
            elf.SelfHeaderDebugDump(filepath / "selfHeader.txt");
            elf.SelfSegHeaderDebugDump(filepath / "selfSegHeaders.txt");
        }
        elf.ElfHeaderDebugDump(filepath / "elfHeader.txt");
        elf.PHeaderDebugDump(filepath / "elfPHeaders.txt");
    }
}

} // namespace Core
