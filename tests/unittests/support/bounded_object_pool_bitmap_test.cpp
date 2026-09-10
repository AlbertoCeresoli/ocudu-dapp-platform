// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "ocudu/support/memory_pool/bounded_object_pool.h"
#include <gtest/gtest.h>

TEST(bounded_object_pool_bitmap, all_rotations_are_defined_and_preserve_bits)
{
  constexpr uint64_t pattern = UINT64_C(0x8912fedc4270ab35);
  static_assert(ocudu::detail::rotate_pool_bitmap_right(pattern, 0) == pattern);
  static_assert(ocudu::detail::rotate_pool_bitmap_right(pattern, 64) == pattern);
  for (unsigned shift = 0; shift != 128; ++shift) {
    uint64_t expected = 0;
    for (unsigned bit = 0; bit != 64; ++bit) {
      expected |= ((pattern >> ((bit + shift) % 64)) & 1U) << bit;
    }
    EXPECT_EQ(ocudu::detail::rotate_pool_bitmap_right(pattern, shift), expected);
    for (unsigned bit = 0; bit != 64; ++bit) {
      EXPECT_EQ(ocudu::detail::rotate_pool_bitmap_right(UINT64_C(1) << bit, shift),
                UINT64_C(1) << ((bit + 64 - (shift % 64)) % 64));
    }
    EXPECT_EQ(ocudu::detail::rotate_pool_bitmap_right(0, shift), 0U);
    EXPECT_EQ(ocudu::detail::rotate_pool_bitmap_right(UINT64_MAX, shift), UINT64_MAX);
  }
}
