// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include "common/config.h"
#include "common/logging/log.h"
#include "core/memory_compression.h"

namespace Core {

MemoryCompression::MemoryCompression() = default;

MemoryCompression::~MemoryCompression() = default;

bool MemoryCompression::TryCompressBlock(VAddr virtual_addr, u64 size, std::vector<u8> data) {
    if (!compression_enabled || compression_level == 0) {
        return false;
    }

    // Don't compress very small blocks (less than 4KB)
    if (size < 4 * 1024) {
        return false;
    }

    std::scoped_lock lock(compression_mutex);

    // Check if already compressed
    if (compressed_blocks.find(virtual_addr) != compressed_blocks.end()) {
        return false;
    }

    // Compress the data
    auto compressed_data = CompressData(data);
    if (compressed_data.empty()) {
        return false;
    }

    // Only compress if we actually save memory
    if (compressed_data.size() >= data.size()) {
        return false;
    }

    // Create compressed block
    CompressedMemoryBlock block{virtual_addr, size, std::move(compressed_data),
                                std::chrono::steady_clock::now(), 1};
    compressed_blocks[virtual_addr] = std::move(block);

    LOG_INFO(
        Kernel_Vmm,
        "Compressed memory block at {:#x}, original: {} bytes, compressed: {} bytes, ratio: {:.2f}",
        virtual_addr, size, compressed_blocks[virtual_addr].compressed_data.size(),
        (double)compressed_blocks[virtual_addr].compressed_data.size() / size);

    return true;
}

std::vector<u8> MemoryCompression::DecompressBlock(VAddr virtual_addr) {
    std::scoped_lock lock(compression_mutex);

    auto it = compressed_blocks.find(virtual_addr);
    if (it == compressed_blocks.end()) {
        return {};
    }

    const auto& block = it->second;
    auto decompressed_data = DecompressData(block.compressed_data, block.size);

    if (!decompressed_data.empty()) {
        // Update access information
        UpdateBlockAccess(virtual_addr);

        LOG_INFO(Kernel_Vmm, "Decompressed memory block at {:#x}", virtual_addr);
    }

    return decompressed_data;
}

bool MemoryCompression::IsCompressed(VAddr virtual_addr) const {
    std::scoped_lock lock(compression_mutex);
    return compressed_blocks.find(virtual_addr) != compressed_blocks.end();
}

MemoryCompression::CompressionStats MemoryCompression::GetStats() const {
    std::scoped_lock lock(compression_mutex);

    CompressionStats stats{};
    stats.total_blocks = compressed_blocks.size();

    for (const auto& [addr, block] : compressed_blocks) {
        stats.compressed_blocks++;
        stats.total_original_size += block.size;
        stats.total_compressed_size += block.compressed_data.size();
    }

    stats.memory_saved_bytes = stats.total_original_size - stats.total_compressed_size;
    if (stats.total_original_size > 0) {
        stats.compression_ratio = (double)stats.total_compressed_size / stats.total_original_size;
    }

    return stats;
}

void MemoryCompression::CleanupOldBlocks() {
    if (!compression_enabled) {
        return;
    }

    std::scoped_lock lock(compression_mutex);

    const auto current_time = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::minutes(30); // 30 minutes timeout

    auto it = compressed_blocks.begin();
    while (it != compressed_blocks.end()) {
        const auto& block = it->second;

        // Remove blocks that haven't been accessed for a while
        if (current_time - block.last_access > timeout) {
            LOG_INFO(Kernel_Vmm, "Removing old compressed block at {:#x}", block.virtual_addr);
            it = compressed_blocks.erase(it);
        } else {
            ++it;
        }
    }
}

void MemoryCompression::SetCompressionLevel(int level) {
    compression_level = std::clamp(level, 0, 3);

    // Disable compression if level is 0
    if (compression_level == 0) {
        SetEnabled(false);
    } else {
        SetEnabled(true);
    }

    LOG_INFO(Kernel_Vmm, "Memory compression level set to {}", compression_level);
}

void MemoryCompression::SetEnabled(bool enabled) {
    compression_enabled = enabled;

    if (!enabled) {
        // Decompress all blocks when disabling
        std::scoped_lock lock(compression_mutex);
        compressed_blocks.clear();
        LOG_INFO(Kernel_Vmm, "Memory compression disabled, all blocks decompressed");
    }
}

bool MemoryCompression::IsEnabled() const {
    return compression_enabled;
}

std::vector<u8> MemoryCompression::CompressData(const std::vector<u8>& data) {
    // Simple RLE compression for now
    if (data.empty()) {
        return {};
    }

    std::vector<u8> compressed;
    compressed.reserve(data.size());

    u8 current_byte = data[0];
    u8 count = 1;

    for (size_t i = 1; i < data.size(); ++i) {
        if (data[i] == current_byte && count < 255) {
            count++;
        } else {
            compressed.push_back(count);
            compressed.push_back(current_byte);
            current_byte = data[i];
            count = 1;
        }
    }

    compressed.push_back(count);
    compressed.push_back(current_byte);

    return compressed;
}

std::vector<u8> MemoryCompression::DecompressData(const std::vector<u8>& compressed_data,
                                                  u64 original_size) {
    if (compressed_data.empty()) {
        return {};
    }

    std::vector<u8> decompressed;
    decompressed.reserve(original_size);

    for (size_t i = 0; i < compressed_data.size(); i += 2) {
        u8 count = compressed_data[i];
        u8 byte = compressed_data[i + 1];

        for (int j = 0; j < count; ++j) {
            decompressed.push_back(byte);
        }
    }

    return decompressed;
}

bool MemoryCompression::ShouldCompressBlock(const CompressedMemoryBlock& block) const {
    // Don't compress if accessed recently (within last 5 minutes)
    const auto current_time = std::chrono::steady_clock::now();
    const auto recent_threshold = std::chrono::minutes(5);

    if (current_time - block.last_access < recent_threshold) {
        return false;
    }

    // Don't compress if accessed frequently (more than 3 times in last hour)
    if (block.access_count > 3) {
        return false;
    }

    return true;
}

void MemoryCompression::UpdateBlockAccess(VAddr virtual_addr) {
    auto it = compressed_blocks.find(virtual_addr);
    if (it != compressed_blocks.end()) {
        auto& block = it->second;
        block.last_access = std::chrono::steady_clock::now();
        block.access_count++;
    }
}

} // namespace Core
