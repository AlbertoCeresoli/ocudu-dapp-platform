// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include <cstring>

// Private host-side selection shared by the CUDA API and its test harness.
// CUSTOM means the CUDA mixed-radix implementation, never CPU fallback.
enum pusch_transform_deprecoder_backend {
  PUSCH_DEPRECODER_CUSTOM = 0,
  PUSCH_DEPRECODER_VKFFT  = 1,
  PUSCH_DEPRECODER_AUTO   = 2
};

inline constexpr char PUSCH_TRANSFORM_DEPRECODER_ENV[] = "OCUDU_PHY_CUDA_PUSCH_TRANSFORM_DEPRECODER";

inline pusch_transform_deprecoder_backend pusch_select_transform_deprecoder(const char* value,
                                                                            bool        vkfft_compiled) noexcept
{
  if (value) {
    if (std::strcmp(value, "custom") == 0) {
      return PUSCH_DEPRECODER_CUSTOM;
    }
    if (std::strcmp(value, "vkfft") == 0) {
      return PUSCH_DEPRECODER_VKFFT;
    }
    if (std::strcmp(value, "auto") == 0) {
      return PUSCH_DEPRECODER_AUTO;
    }
  }
  // Preserve the previous default/unknown-value behavior. Explicit VkFFT on a
  // build without VkFFT remains unsupported at preplan/execute, not re-labeled.
  return vkfft_compiled ? PUSCH_DEPRECODER_AUTO : PUSCH_DEPRECODER_CUSTOM;
}
