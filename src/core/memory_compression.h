// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "common/types.h"

namespace Core {

struct CompressedMemoryBlock {
    VAddr virtual_addr;
    u64 size;
    std::vector<u8> compressed_data;
    std::chrono::steady_clock::time_point last_access;
    u32 access_count;
};

class MemoryCompression {
public:
    MemoryCompression();
    ~MemoryCompression();

    // Compress a memory block if it hasn't been accessed recently
    bool TryCompressBlock(VAddr virtual_addr, u64 size, std::vector<u8> data);

    // Decompress a memory block when accessed
    std::vector<u8> DecompressBlock(VAddr virtual_addr);

    // Check if a block is compressed
    bool IsCompressed(VAddr virtual_addr) const;

    // Get compression statistics
    struct CompressionStats {
        u64 total_blocks = 0;
        u64 compressed_blocks = 0;
        u64 total_original_size = 0;
        u64 total_compressed_size = 0;
        u64 memory_saved_bytes = 0;
        double compression_ratio = 0.0;
    };

    CompressionStats GetStats() const;

    // Clean up old compressed blocks
    void CleanupOldBlocks();

    // Set compression level (0=disabled, 1=fast, 2=balanced, 3=max)
    void SetCompressionLevel(int level);

    // Enable/disable compression
    void SetEnabled(bool enabled);
    bool IsEnabled() const;

private:
    mutable std::mutex compression_mutex;
    std::map<VAddr, CompressedMemoryBlock> compressed_blocks;
    int compression_level = 0;
    bool compression_enabled = false;

    // Simple compression function (placeholder for now)
    std::vector<u8> CompressData(const std::vector<u8>& data);
    std::vector<u8> DecompressData(const std::vector<u8>& compressed_data, u64 original_size);

    // Check if block should be compressed based on access patterns
    bool ShouldCompressBlock(const CompressedMemoryBlock& block) const;

    // Update block access information
    void UpdateBlockAccess(VAddr virtual_addr);
};

} // namespace Core
