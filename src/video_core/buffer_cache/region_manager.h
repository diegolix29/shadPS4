// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/page_manager.h"

namespace VideoCore {

/**
 * Allows tracking CPU and GPU modification of pages in a contigious virtual address region.
 * Information is stored in bitsets for spacial locality and fast update of single pages.
 */
class RegionManager {
public:
    explicit RegionManager(PageManager* tracker_, VAddr cpu_addr_)
        : tracker{tracker_}, cpu_addr{cpu_addr_} {
        cpu.Fill(~0ULL);
        gpu.Fill(0ULL);
        writeable.Fill(~0ULL);
        readable.Fill(~0ULL);
    }
    explicit RegionManager() = default;

    void SetCpuAddress(VAddr new_cpu_addr) {
        cpu_addr = new_cpu_addr;
    }

    static constexpr Bounds GetBounds(VAddr address, u64 size) {
        const u64 end_address = address + size + BYTES_PER_PAGE - 1ULL;
        return Bounds{
            .start_word = address / BYTES_PER_WORD,
            .start_page = (address % BYTES_PER_WORD) / BYTES_PER_PAGE,
            .end_word = end_address / BYTES_PER_WORD,
            .end_page = (end_address % BYTES_PER_WORD) / BYTES_PER_PAGE,
        };
    }

    static constexpr std::pair<u64, u64> GetMasks(u64 start_page, u64 end_page) {
        const u64 start_mask = ~((1ULL << start_page) - 1);
        const u64 end_mask =
            (end_page == PAGES_PER_WORD - 1) ? ~0ULL : (1ULL << (end_page + 1)) - 1;
        return std::make_pair(start_mask, end_mask);
    }

    static constexpr void IterateWords(const Bounds& bounds, auto&& func) {
        const auto [start_word, start_page, end_word, end_page] = bounds;
        const auto [start_mask, end_mask] = GetMasks(start_page, end_page);
        if (start_word == end_word) [[likely]] {
            func(start_word, start_mask & end_mask);
        } else {
            func(start_word, start_mask);
            for (s64 i = start_word + 1; i < end_word; ++i) {
                func(i, ~0ULL);
            }
            if (end_mask) {
                func(end_word, end_mask);
            }
        }
    }

    static constexpr void IteratePages(u64 word, auto&& func) {
        u64 offset{};
        while (word != 0) {
            const u64 empty_bits = std::countr_zero(word);
            offset += empty_bits;
            word >>= empty_bits;
            const u64 set_bits = std::countr_one(word);
            func(offset, set_bits);
            word = set_bits < PAGES_PER_WORD ? (word >> set_bits) : 0;
            offset += set_bits;
        }
    }

    template <Type type, LockOp lock_op, bool enable>
    void ChangeRegionState(u64 offset, u64 size) {
        auto& state = GetRegionBits<type>();
        RegionBits prot;
        bool update_watchers{};
        const auto bounds = GetBounds(offset, size);
        IterateWords(bounds, [&](u64 index, u64 mask) {
            if constexpr (lock_op & LockOp::Lock) {
                locks[index].Lock(mask);
            }
            if constexpr (enable) {
                state[index] |= mask;
            } else {
                state[index] &= ~mask;
            }
            update_watchers |= UpdateProtection<type, !enable>(prot, index, mask);
        });
        if (update_watchers) {
            tracker->UpdatePageWatchersForRegion<enable, type == Type::GPU>(cpu_addr, bounds, prot);
        }
        if constexpr (lock_op & LockOp::Unlock) {
            IterateWords(bounds, [&](u64 index, u64 mask) { locks[index].Unlock(mask); });
        }
    }

    template <Type type, LockOp lock_op, bool clear>
    void ForEachModifiedRange(u64 offset, s64 size, auto&& func) {
        auto& state = GetRegionBits<type>();
        RegionBits prot;
        bool update_watchers{};
        bool pending{};
        u64 start_page{};
        u64 end_page{};
        const auto bounds = GetBounds(offset, size);
        const auto on_range = [&] {
            func(cpu_addr + start_page * BYTES_PER_PAGE, (end_page - start_page) * BYTES_PER_PAGE);
        };
        IterateWords(bounds, [&](u64 index, u64 mask) {
            if constexpr (lock_op & LockOp::Lock) {
                locks[index].Lock(mask);
            }
            const u64 word = state[index] & mask;
            const u64 base_page = index * PAGES_PER_WORD;
            IteratePages(word, [&](u64 pages_offset, u64 pages_size) {
                const auto reset = [&] {
                    start_page = base_page + pages_offset;
                    end_page = start_page + pages_size;
                };
                if (!pending) {
                    reset();
                    pending = true;
                    return;
                }
                if (end_page == base_page + pages_offset) {
                    end_page += pages_size;
                    return;
                }
                on_range();
                reset();
            });
            if constexpr (clear) {
                state[index] &= ~mask;
                update_watchers |= UpdateProtection<type, clear>(prot, index, mask);
            }
        });
        if (pending) {
            on_range();
        }
        if (update_watchers) {
            tracker->UpdatePageWatchersForRegion<false, type == Type::GPU>(cpu_addr, bounds, prot);
        }
        if constexpr (lock_op & LockOp::Unlock) {
            IterateWords(bounds, [&](u64 index, u64 mask) { locks[index].Unlock(mask); });
        }
    }

    template <Type type>
    bool IsRegionModified(u64 offset, u64 size) noexcept {
        auto& state = GetRegionBits<type>();
        const auto [start_word, start_page, end_word, end_page] = GetBounds(offset, size);
        const auto [start_mask, end_mask] = GetMasks(start_page, end_page);
        if (start_word == end_word) [[likely]] {
            return state[start_word] & (start_mask & end_mask);
        } else {
            if (state[start_word] & start_mask) {
                return true;
            }
            for (s64 i = start_word + 1; i < end_word; ++i) {
                if (state[i]) {
                    return true;
                }
            }
            if (state[end_word] & end_mask) {
                return true;
            }
            return false;
        }
    }

private:
    template <Type type, bool clear>
    bool UpdateProtection(RegionBits& prot, u64 index, u64 mask) {
        if constexpr (type == Type::CPU) {
            const u64 perm = writeable[index];
            if constexpr (clear) {
                writeable[index] &= ~mask;
            } else {
                writeable[index] |= mask;
            }
            prot[index] = cpu[index] ^ perm;
        } else {
            const u64 perm = readable[index];
            if constexpr (clear) {
                readable[index] |= mask;
            } else {
                readable[index] &= ~mask;
            }
            prot[index] = ~gpu[index] ^ perm;
        }
        return prot[index] & mask;
    }

    template <Type type>
    RegionBits& GetRegionBits() noexcept {
        if constexpr (type == Type::CPU) {
            return cpu;
        } else if constexpr (type == Type::GPU) {
            return gpu;
        }
    }

    RegionBits cpu;
    RegionBits gpu;
    RegionBits writeable;
    RegionBits readable;

    PageManager* tracker;
    std::array<BitLock<u64>, NUM_REGION_WORDS> locks{};
    VAddr cpu_addr{};
};

} // namespace VideoCore
