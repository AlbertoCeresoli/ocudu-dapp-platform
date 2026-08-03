// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "realtime_timing_worker_utils.h"
#include "gtest/gtest.h"

using namespace ocudu;

TEST(realtime_timing_worker_utils_test, replays_all_symbols_within_one_slot)
{
  static constexpr unsigned nof_symbols_per_slot = 14;

  EXPECT_EQ(ofh::get_bounded_symbol_replay_count(0, nof_symbols_per_slot), 0);
  EXPECT_EQ(ofh::get_bounded_symbol_replay_count(1, nof_symbols_per_slot), 1);
  EXPECT_EQ(ofh::get_bounded_symbol_replay_count(nof_symbols_per_slot, nof_symbols_per_slot), nof_symbols_per_slot);
}

TEST(realtime_timing_worker_utils_test, bounds_long_stall_replay_to_one_slot)
{
  static constexpr unsigned nof_symbols_per_slot = 14;

  EXPECT_EQ(ofh::get_bounded_symbol_replay_count(nof_symbols_per_slot + 1, nof_symbols_per_slot), nof_symbols_per_slot);
  EXPECT_EQ(ofh::get_bounded_symbol_replay_count(5038, nof_symbols_per_slot), nof_symbols_per_slot);
}
