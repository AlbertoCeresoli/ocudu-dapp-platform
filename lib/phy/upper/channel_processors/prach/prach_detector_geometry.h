// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Geometry derived from a PRACH detector configuration

#pragma once

#include "ocudu/phy/upper/channel_processors/prach/prach_detector.h"
#include "ocudu/ran/prach/prach_preamble_information.h"

namespace ocudu::detail {

/// \brief Everything a PRACH detection needs that is derived from the detector configuration
///
/// Kept apart from the backends so that the accelerated detector and its tests agree on
/// how a configuration maps to a workload shape. See \ref get_prach_detector_geometry.
struct prach_detector_geometry {
  /// Preamble information for the configured format and RA subcarrier spacing.
  prach_preamble_information preamble_info;
  /// Cyclic shift, in samples of the sequence. Zero when the format admits no cyclic shift multiplexing.
  unsigned n_cs;
  /// Number of preambles obtained from a single root sequence by cyclic shifting.
  unsigned nof_shifts;
  /// Number of root sequences needed to cover all 64 preambles.
  unsigned nof_sequences;
  /// First root sequence covering the requested preamble range.
  unsigned active_sequence_start;
  /// Number of root sequences covering the requested preamble range.
  unsigned active_nof_sequences;
  /// PRACH sequence length, in samples.
  unsigned sequence_length;
  /// Correlation IDFT size.
  unsigned dft_size;
  /// Detection window width, in IDFT samples.
  unsigned win_width;
  /// Detection window margin, in IDFT samples.
  unsigned win_margin;
  /// Largest delay the detector reports, in IDFT samples.
  unsigned max_delay_samples;
  /// Number of OFDM symbols in the preamble.
  unsigned nof_symbols;
  /// Detection threshold.
  float threshold;
  /// Set when the symbols of the preamble are combined before the detection.
  bool combine_symbols;
  /// Sampling rate of the correlation output, in hertz.
  double sampling_rate_hz;
  /// PRACH cyclic prefix, in samples of the sequence.
  unsigned cp_prach;
};

/// \brief Derives the detector geometry from a PRACH detector configuration
///
/// \remark Reports a fatal error if the configuration selects a reserved cyclic shift.
prach_detector_geometry get_prach_detector_geometry(const prach_detector::configuration& config);

/// \brief Number of IDFT batches an accelerated backend runs for this configuration.
///
/// The accelerated detector correlates every active root sequence against every receive port and against every
/// symbol unless the symbols are combined first. This count, together with \ref prach_detector_geometry::dft_size,
/// is what an accelerated backend shapes its IDFT plan around: a plan built for one value cannot serve another, so
/// anything that moves this count (preamble format, RA subcarrier spacing, zero correlation zone,
/// number of receive ports or the requested preamble range) forces the plan to be built again.
unsigned get_nof_idft_batches(const prach_detector::configuration& config, const prach_detector_geometry& geometry);

} // namespace ocudu::detail
