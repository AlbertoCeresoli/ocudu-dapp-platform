// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#pragma once

#include "ocudu/ran/pusch/pusch_constants.h"
#include "ocudu/ran/sch/sch_constants.h"
#include "ocudu/ran/sch/sch_segmentation.h"
#include <cstddef>
#include <optional>

namespace ocudu {

/// Construction-time storage limits, not a per-grant algorithm selection.
/// No CUDA dependency: geometry and overflow rejection can be tested without a device.
struct pusch_decoder_capacity {
  unsigned codeword_llrs;
  unsigned codeblocks;
  unsigned tb_bytes;

  constexpr bool accepts(std::size_t nof_cbs, std::size_t nof_llrs, std::size_t output_bytes = 0) const
  {
    return nof_cbs > 0 && nof_cbs <= codeblocks && nof_llrs > 0 && nof_llrs <= codeword_llrs &&
           output_bytes <= tb_bytes;
  }
};

/// One PUSCH codeword cannot exceed the configured PRBs x 164 data RE x layers x 8 bits.
/// Its information payload is bounded by that codeword size. BG2's smaller segment
/// capacity gives a conservative CB bound even when the actual transmission uses BG1.
/// Include the maximum TB CRC in byte staging. Keep worst-case lifting size per CB;
/// changing MCS, BG, Z, RV or grant width within this envelope never grows buffers.
constexpr std::optional<pusch_decoder_capacity> make_pusch_decoder_capacity(unsigned nof_prb, unsigned nof_layers)
{
  if (nof_prb == 0 || nof_prb > MAX_NOF_PRBS || nof_layers == 0 || nof_layers > pusch_constants::MAX_NOF_LAYERS) {
    return std::nullopt;
  }
  // Validate before multiplication. The largest supported product fits unsigned and
  // the signed CUDA-library length fields by a wide margin.
  unsigned bits = pusch_constants::get_max_codeword_size(nof_prb, nof_layers).value();
  unsigned cbs  = compute_nof_codeblocks(units::bits(bits), ldpc_base_graph_type::BG2);
  if (cbs > MAX_NOF_SEGMENTS) {
    return std::nullopt;
  }
  return pusch_decoder_capacity{bits, cbs, (bits + 24 + 7) / 8};
}

} // namespace ocudu
