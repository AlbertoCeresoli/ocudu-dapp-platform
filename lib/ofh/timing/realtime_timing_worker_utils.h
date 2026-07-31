// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <algorithm>

namespace ocudu {
namespace ofh {

/// Returns the number of most recent OTA symbols to notify after a timing-worker stall.
///
/// Replaying more than one slot cannot recover expired real-time windows and would enqueue an unbounded burst of stale
/// work. One slot is retained so downstream repositories can close the most recent pending symbol contexts.
constexpr unsigned get_bounded_symbol_replay_count(unsigned elapsed_symbols, unsigned nof_symbols_per_slot)
{
  return std::min(elapsed_symbols, nof_symbols_per_slot);
}

} // namespace ofh
} // namespace ocudu
