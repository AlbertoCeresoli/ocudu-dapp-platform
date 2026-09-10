// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <cmath>
#include <limits>

namespace ocudu::detail {

/// Interpret only the completed equalizer accumulator; never substitute DM-RS or EVM.
inline float pusch_post_eq_sinr_from_accumulator(float noise_sum, unsigned count)
{
  if (count == 0 || !std::isfinite(noise_sum) || noise_sum <= 0.0F) {
    return -std::numeric_limits<float>::infinity();
  }
  const float mean = noise_sum / static_cast<float>(count);
  if (!std::isfinite(mean) || mean <= 0.0F) {
    return -std::numeric_limits<float>::infinity();
  }
  return -10.0F * std::log10(mean);
}

} // namespace ocudu::detail
