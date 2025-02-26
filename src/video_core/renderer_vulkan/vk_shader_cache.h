#pragma once

#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Vulkan {

class ShaderCache {
public:
    explicit ShaderCache(const std::string& cache_dir);

    std::vector<uint32_t> LoadShader(const std::string& shader_source,
                                     vk::ShaderStageFlagBits stage, uint32_t perm_idx);
    void SaveShader(const std::string& shader_source, vk::ShaderStageFlagBits stage,
                    uint32_t perm_idx, const std::vector<uint32_t>& spirv);

private:
    std::string cache_directory;
    std::string GenerateShaderHash(const std::string& shader_source, vk::ShaderStageFlagBits stage,
                                   uint32_t perm_idx) const;
    std::string GetCachePath(const std::string& hash, uint32_t perm_idx) const;
};

} // namespace Vulkan
