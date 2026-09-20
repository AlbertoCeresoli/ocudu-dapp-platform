// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once
#include "ocudu/phy/upper/acceleration/phy_acceleration.h"

namespace ocudu {
/// Preserve the original acceptance identity only for the exact untransformed resident codeword. This neither
/// fences the producer nor acquires ownership. A mismatched/compacted output must not shed its required check.
inline resident_softbit_buffer forward_receiver_completion(const resident_softbit_buffer& original,
                                                            const resident_softbit_buffer& current) noexcept
{
  if (!original.valid || !original.data || !original.nof_softbits || !original.execution_context ||
      !original.acceptance_required || !original.completion_context || !original.accept_completion ||
      !original.completion_generation || current.acceptance_required || current.completion_context ||
      current.accept_completion || current.completion_generation ||
      original.data != current.data || original.nof_softbits != current.nof_softbits ||
      original.execution_context != current.execution_context || original.completion_token != current.completion_token ||
      original.valid != current.valid || original.n_id != current.n_id || original.rnti != current.rnti ||
      original.buffer_index != current.buffer_index) {
    return {};
  }
  return original;
}
} // namespace ocudu
