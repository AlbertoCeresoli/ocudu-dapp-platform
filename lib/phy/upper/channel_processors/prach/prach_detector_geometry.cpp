// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "prach_detector_geometry.h"
#include "prach_detector_generic_thresholds.h"
#include "ocudu/ran/prach/prach_cyclic_shifts.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/math/math_utils.h"
#include <algorithm>
#include <cmath>

using namespace ocudu;
using namespace ocudu::detail;

prach_detector_geometry ocudu::detail::get_prach_detector_geometry(const prach_detector::configuration& config)
{
  prach_detector_geometry geometry;
  geometry.preamble_info = is_long_preamble(config.format)
                               ? get_prach_preamble_long_info(config.format)
                               : get_prach_preamble_short_info(config.format, config.ra_scs, false);
  geometry.n_cs          = prach_cyclic_shifts_get(config.ra_scs, config.restricted_set, config.zero_correlation_zone);
  report_fatal_error_if_not(geometry.n_cs != PRACH_CYCLIC_SHIFTS_RESERVED, "Reserved PRACH cyclic shift");

  geometry.sequence_length =
      is_short_preamble(config.format) ? prach_constants::SHORT_SEQUENCE_LENGTH : prach_constants::LONG_SEQUENCE_LENGTH;
  geometry.nof_shifts    = 1;
  geometry.nof_sequences = 64;
  if (geometry.n_cs != 0) {
    geometry.nof_shifts    = std::min(prach_constants::MAX_NUM_PREAMBLES, geometry.sequence_length / geometry.n_cs);
    geometry.nof_sequences = divide_ceil(64U, geometry.nof_shifts);
  }
  unsigned first_requested_sequence =
      std::min(config.start_preamble_index / geometry.nof_shifts, geometry.nof_sequences - 1U);
  unsigned requested_preamble_end =
      std::min(prach_constants::MAX_NUM_PREAMBLES, config.start_preamble_index + config.nof_preamble_indices);
  unsigned last_requested_sequence =
      std::min(geometry.nof_sequences, divide_ceil(requested_preamble_end, geometry.nof_shifts));
  if (last_requested_sequence <= first_requested_sequence) {
    last_requested_sequence = first_requested_sequence + 1U;
  }
  geometry.active_sequence_start = first_requested_sequence;
  geometry.active_nof_sequences  = last_requested_sequence - first_requested_sequence;

  geometry.dft_size         = is_long_preamble(config.format) ? 1024U : 256U;
  geometry.sampling_rate_hz = geometry.dft_size * ra_scs_to_Hz(geometry.preamble_info.scs);

  double cp_duration = geometry.preamble_info.cp_length.to_seconds();
  geometry.cp_prach  = static_cast<unsigned>(
      std::floor(cp_duration * geometry.sequence_length * ra_scs_to_Hz(geometry.preamble_info.scs)));
  geometry.win_width = std::min(geometry.n_cs, geometry.cp_prach);
  if (geometry.n_cs == 0) {
    geometry.win_width = geometry.cp_prach;
  }
  if (geometry.win_width == geometry.sequence_length) {
    geometry.win_width -= 20;
  }
  geometry.win_width = (geometry.win_width * geometry.dft_size) / geometry.sequence_length;

  threshold_params th_params;
  th_params.nof_rx_ports                        = config.nof_rx_ports;
  th_params.scs                                 = config.ra_scs;
  th_params.format                              = config.format;
  th_params.zero_correlation_zone               = config.zero_correlation_zone;
  auto [threshold, combine_symbols, win_margin] = get_threshold_and_margin(th_params);
  geometry.threshold                            = threshold;
  geometry.combine_symbols                      = combine_symbols;
  geometry.win_margin                           = win_margin;
  geometry.max_delay_samples =
      (geometry.n_cs == 0) ? geometry.cp_prach : std::min(std::max(geometry.n_cs, 1U) - 1U, geometry.cp_prach);
  geometry.max_delay_samples = (geometry.max_delay_samples * geometry.dft_size) / geometry.sequence_length;
  geometry.nof_symbols       = geometry.preamble_info.nof_symbols;

  return geometry;
}

unsigned ocudu::detail::get_nof_idft_batches(const prach_detector::configuration& config,
                                             const prach_detector_geometry&       geometry)
{
  unsigned batches_per_sequence = config.nof_rx_ports * (geometry.combine_symbols ? 1U : geometry.nof_symbols);
  return batches_per_sequence * geometry.active_nof_sequences;
}
