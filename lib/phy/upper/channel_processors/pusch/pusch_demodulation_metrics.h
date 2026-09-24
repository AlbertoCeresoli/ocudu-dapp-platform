// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator_notifier.h"
#include <cmath>

namespace ocudu {

/// Build the same statistics after either immediate or deferred CUDA completion.
/// Inputs must belong to the current, completion-fenced grant. Missing sources
/// stay absent: EVM, reference-signal SINR and post-EQ SINR are not substitutes.
inline pusch_demodulator_notifier::demodulation_stats make_pusch_e2e_demodulation_stats(float post_eq_sinr_dB,
                                                                                        float ch_est_sinr_dB,
                                                                                        float epre_dB,
                                                                                        float rsrp_dB,
                                                                                        float time_alignment_s,
                                                                                        float cfo_Hz,
                                                                                        float evm)
{
  pusch_demodulator_notifier::demodulation_stats stats;
  if (std::isfinite(post_eq_sinr_dB)) {
    stats.sinr_dB = post_eq_sinr_dB;
  }
  if (std::isfinite(ch_est_sinr_dB)) {
    stats.channel_estimator_sinr_dB = ch_est_sinr_dB;
  }
  if (std::isfinite(evm) && evm > 0.0F) {
    stats.evm = evm;
  }
  if (std::isfinite(epre_dB)) {
    stats.epre_dB = epre_dB;
  }
  if (std::isfinite(rsrp_dB)) {
    stats.rsrp_dB = rsrp_dB;
  }
  if (std::isfinite(time_alignment_s)) {
    stats.time_alignment_s = time_alignment_s;
  }
  if (std::isfinite(cfo_Hz)) {
    stats.cfo_Hz = cfo_Hz;
  }
  return stats;
}

/// Select only current, completion-fenced inline-receiver sources. A receiver's
/// learned LLR scale is not a noise measurement contract; EVM is kept only when the
/// host reduced the receiver's equalized symbols with the baseline QAM convention.
inline pusch_demodulator_notifier::demodulation_stats select_pusch_inline_demodulation_stats(
    pusch_demodulator_notifier::demodulation_stats stats,
    bool completion_ok,
    bool pilot_valid,
    bool post_eq_valid,
    bool evm_valid)
{
  if (!completion_ok) {
    return {};
  }
  if (!pilot_valid) {
    stats.channel_estimator_sinr_dB.reset();
    stats.epre_dB.reset();
    stats.rsrp_dB.reset();
    stats.time_alignment_s.reset();
    stats.cfo_Hz.reset();
  }
  if (!post_eq_valid) {
    stats.sinr_dB.reset();
  }
  if (!evm_valid) {
    stats.evm.reset();
  }
  return stats;
}

} // namespace ocudu
