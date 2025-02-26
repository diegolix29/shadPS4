#include "vk_shader_cache.h"
#include <fstream>
#include <filesystem>
#include <functional>

namespace Vulkan {

ShaderCache::ShaderCache(const std::string& cache_dir) : cache_directory(cache_dir) {
    std::filesystem::create_directories(cache_directory);
}

std::string ShaderCache::GenerateShaderHash(const std::string& shader_source,
                                            vk::ShaderStageFlagBits stage,
                                            uint32_t perm_idx) const {
    std::hash<std::string> hasher;
    return std::to_string(hasher(shader_source + std::to_string(static_cast<uint32_t>(stage)) +
                                 std::to_string(perm_idx)));
}

std::string ShaderCache::GetCachePath(const std::string& hash, uint32_t perm_idx) const {
    return cache_directory + "/" + hash + "_" + std::to_string(perm_idx) + ".spv";
}

std::vector<uint32_t> ShaderCache::LoadShader(const std::string& shader_source,
                                              vk::ShaderStageFlagBits stage, uint32_t perm_idx) {
    const std::string path =
        GetCachePath(GenerateShaderHash(shader_source, stage, perm_idx), perm_idx);
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return {};

    std::vector<uint32_t> spirv(file.tellg() / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(spirv.data()), spirv.size() * sizeof(uint32_t));
    return spirv;
}

void ShaderCache::SaveShader(const std::string& shader_source, vk::ShaderStageFlagBits stage,
                             uint32_t perm_idx, const std::vector<uint32_t>& spirv) {
    const std::string path =
        GetCachePath(GenerateShaderHash(shader_source, stage, perm_idx), perm_idx);
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(spirv.data()), spirv.size() * sizeof(uint32_t));
}

} // namespace Vulkan
