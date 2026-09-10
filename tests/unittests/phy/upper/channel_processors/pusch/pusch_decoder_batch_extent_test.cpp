// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "phy/upper/channel_coding/ldpc/cuda/pusch_decoder_batch_extent.h"
#include <array>
#include <gtest/gtest.h>
#include <limits>

TEST(pusch_decoder_batch_extent, rejects_empty_oversized_and_inconsistent_extents)
{
  auto length = [](std::size_t value) { return value; };
  EXPECT_FALSE(ocudu::pusch_decoder_batch_extent(std::array<std::size_t, 0>{}, 4, 100, length));
  EXPECT_FALSE(ocudu::pusch_decoder_batch_extent(std::array<std::size_t, 2>{10, 10}, 1, 100, length));
  EXPECT_FALSE(ocudu::pusch_decoder_batch_extent(std::array<std::size_t, 2>{10, 0}, 4, 100, length));
  EXPECT_FALSE(ocudu::pusch_decoder_batch_extent(std::array<std::size_t, 2>{50, 51}, 4, 100, length));
  EXPECT_EQ(ocudu::pusch_decoder_batch_extent(std::array<std::size_t, 2>{50, 50}, 4, 100, length), 100U);
  constexpr auto maximum = std::numeric_limits<std::size_t>::max();
  EXPECT_FALSE(ocudu::pusch_decoder_batch_extent(std::array<std::size_t, 2>{maximum, 1}, 4, maximum, length));
  EXPECT_EQ(ocudu::pusch_decoder_batch_extent(std::array<std::size_t, 2>{maximum - 1, 1}, 4, maximum, length), maximum);
}
