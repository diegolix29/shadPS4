// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <random>

#include <gtest/gtest.h>

#include "common/bit_array.h"

namespace {

using TestBits = Common::BitArray<256>;

bool AnyInRangeReference(const std::array<size_t, 257>& prefix, size_t start, size_t end) {
    return start < end && end < prefix.size() && prefix[end] != prefix[start];
}

TEST(BitArrayTest, AnyInRangeHandlesWordBoundaries) {
    TestBits bits;
    bits.Clear();
    bits.Set(0);
    bits.Set(63);
    bits.Set(64);
    bits.Set(127);
    bits.Set(128);
    bits.Set(255);

    EXPECT_TRUE(bits.AnyInRange(0, 1));
    EXPECT_FALSE(bits.AnyInRange(1, 63));
    EXPECT_TRUE(bits.AnyInRange(63, 64));
    EXPECT_TRUE(bits.AnyInRange(64, 65));
    EXPECT_FALSE(bits.AnyInRange(65, 127));
    EXPECT_TRUE(bits.AnyInRange(127, 129));
    EXPECT_FALSE(bits.AnyInRange(129, 255));
    EXPECT_TRUE(bits.AnyInRange(255, 256));
    EXPECT_TRUE(bits.AnyInRange(1, 256));
}

TEST(BitArrayTest, AnyInRangeScansInteriorWords) {
    Common::BitArray<1024> bits;
    bits.Clear();

    EXPECT_FALSE(bits.AnyInRange(1, 1023));
    bits.Set(512);
    EXPECT_TRUE(bits.AnyInRange(1, 1023));
    bits.Unset(512);
    EXPECT_FALSE(bits.AnyInRange(1, 1023));
}

TEST(BitArrayTest, AnyInRangeRejectsInvalidRanges) {
    TestBits bits;
    bits.Fill();

    EXPECT_FALSE(bits.AnyInRange(0, 0));
    EXPECT_FALSE(bits.AnyInRange(64, 64));
    EXPECT_FALSE(bits.AnyInRange(200, 100));
    EXPECT_FALSE(bits.AnyInRange(0, 257));
    EXPECT_FALSE(bits.AnyInRange(256, 257));
}

TEST(BitArrayTest, AnyInRangeMatchesReferenceForRandomPatterns) {
    std::mt19937_64 random{0x53484144505334ULL};

    for (size_t pattern = 0; pattern < 64; ++pattern) {
        TestBits bits;
        bits.Clear();
        std::array<size_t, 257> prefix{};

        for (size_t bit = 0; bit < 256; ++bit) {
            const bool is_set = (random() & 7) == 0;
            if (is_set) {
                bits.Set(bit);
            }
            prefix[bit + 1] = prefix[bit] + static_cast<size_t>(is_set);
        }

        for (size_t start = 0; start < 256; ++start) {
            for (size_t end = start + 1; end <= 256; ++end) {
                EXPECT_EQ(bits.AnyInRange(start, end), AnyInRangeReference(prefix, start, end))
                    << "pattern=" << pattern << " start=" << start << " end=" << end;
            }
        }
    }
}

} // namespace
