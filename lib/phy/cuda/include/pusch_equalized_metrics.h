// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include <cmath>

#ifdef __CUDACC__
#define OCUDU_EQ_METRIC_HD __host__ __device__
#else
#define OCUDU_EQ_METRIC_HD
#endif

namespace ocudu::detail {

/// Unit-average-power QAM, identical for the fused baseline and external receivers.
/// This is decision-directed EVM, not a known-transmitted-symbol error estimate.
OCUDU_EQ_METRIC_HD inline float pusch_qam_axis(float value, unsigned bits)
{
  const int   level   = (1 << (bits / 2)) - 1;
  const float scale   = bits == 2   ? 0.7071067811865475F
                        : bits == 4 ? 0.31622776601683794F
                        : bits == 6 ? 0.1543033499620919F
                                    : 0.07669649888473704F;
  const float clipped = fminf(float(level), fmaxf(float(-level), value / scale));
  const float index   = floorf((clipped + float(level)) * 0.5F + 0.5F);
  return (-float(level) + 2.0F * index) * scale;
}

OCUDU_EQ_METRIC_HD inline float pusch_qam_error_power(float real, float imag, unsigned bits)
{
  const float re = real - pusch_qam_axis(real, bits);
  const float im = imag - pusch_qam_axis(imag, bits);
  return re * re + im * im;
}

/// Erasure sentinels must not turn a single intentionally blanked RE into negative link SINR.
OCUDU_EQ_METRIC_HD inline bool pusch_metric_noise_valid(float variance)
{
  return variance > 0.0F && variance < 1.0e5F;
}

} // namespace ocudu::detail
#undef OCUDU_EQ_METRIC_HD
