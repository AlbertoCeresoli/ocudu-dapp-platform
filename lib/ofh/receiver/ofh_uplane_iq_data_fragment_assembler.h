// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/adt/bounded_bitset.h"
#include "ocudu/adt/span.h"
#include "ocudu/ofh/compression/compression_params.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/ran/slot_point.h"
#include <cstdint>
#include <vector>

namespace ocudu {
namespace ofh {

/// Assembles the compressed IQ fragments of one resource-grid port and OFDM symbol.
class uplane_iq_data_fragment_assembler
{
public:
  /// Result of adding one fragment.
  enum class result { incomplete, complete, invalid };

  /// Adds one compressed IQ fragment to the assembly.
  result add_fragment(slot_point                   slot,
                      unsigned                     symbol,
                      unsigned                     port,
                      unsigned                     nof_symbol_prbs,
                      unsigned                     start_prb,
                      unsigned                     nof_prbs,
                      span<const uint8_t>          compressed_data,
                      const ru_compression_params& params);

  /// Returns the complete, contiguous compressed symbol. Valid only after add_fragment() returns complete.
  span<const uint8_t> get_assembled_data() const { return assembled_data; }

  /// Clears the current assembly while retaining allocated storage for the next symbol.
  void clear();

private:
  bool matches(slot_point                   slot,
               unsigned                     symbol,
               unsigned                     port,
               unsigned                     nof_symbol_prbs,
               unsigned                     prb_size,
               const ru_compression_params& params) const;

  void start(slot_point                   slot,
             unsigned                     symbol,
             unsigned                     port,
             unsigned                     nof_symbol_prbs,
             unsigned                     prb_size,
             const ru_compression_params& params);

  bool                         active          = false;
  slot_point                   assembly_slot;
  unsigned                     assembly_symbol = 0;
  unsigned                     assembly_port   = 0;
  unsigned                     nof_prbs        = 0;
  unsigned                     prb_size        = 0;
  ru_compression_params        compression     = {};
  bounded_bitset<MAX_NOF_PRBS> received_prbs;
  std::vector<uint8_t>         assembled_data;
};

} // namespace ofh
} // namespace ocudu
