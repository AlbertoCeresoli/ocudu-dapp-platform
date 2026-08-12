// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ofh_uplane_iq_data_fragment_assembler.h"
#include "ocudu/ofh/compression/compression_properties.h"
#include <cstring>

using namespace ocudu;
using namespace ofh;

bool uplane_iq_data_fragment_assembler::matches(slot_point                   slot,
                                                unsigned                     symbol,
                                                unsigned                     port,
                                                unsigned                     nof_symbol_prbs,
                                                unsigned                     fragment_prb_size,
                                                const ru_compression_params& params) const
{
  return active && (assembly_slot == slot) && (assembly_symbol == symbol) && (assembly_port == port) &&
         (nof_prbs == nof_symbol_prbs) && (prb_size == fragment_prb_size) && (compression.type == params.type) &&
         (compression.data_width == params.data_width);
}

void uplane_iq_data_fragment_assembler::start(slot_point                   slot,
                                              unsigned                     symbol,
                                              unsigned                     port,
                                              unsigned                     nof_symbol_prbs,
                                              unsigned                     fragment_prb_size,
                                              const ru_compression_params& params)
{
  active          = true;
  assembly_slot   = slot;
  assembly_symbol = symbol;
  assembly_port   = port;
  nof_prbs        = nof_symbol_prbs;
  prb_size        = fragment_prb_size;
  compression     = params;
  received_prbs   = bounded_bitset<MAX_NOF_PRBS>(nof_symbol_prbs);
  assembled_data.resize(static_cast<size_t>(nof_symbol_prbs) * fragment_prb_size);
}

uplane_iq_data_fragment_assembler::result
uplane_iq_data_fragment_assembler::add_fragment(slot_point                   slot,
                                                unsigned                     symbol,
                                                unsigned                     port,
                                                unsigned                     nof_symbol_prbs,
                                                unsigned                     start_prb,
                                                unsigned                     fragment_nof_prbs,
                                                span<const uint8_t>          compressed_data,
                                                const ru_compression_params& params)
{
  if ((nof_symbol_prbs == 0) || (nof_symbol_prbs > MAX_NOF_PRBS) || (fragment_nof_prbs == 0) ||
      (start_prb >= nof_symbol_prbs) || (fragment_nof_prbs > nof_symbol_prbs - start_prb) || (params.data_width == 0) ||
      (params.data_width > MAX_IQ_WIDTH)) {
    clear();
    return result::invalid;
  }

  unsigned fragment_prb_size = get_compressed_prb_size(params).value();
  size_t   fragment_size     = static_cast<size_t>(fragment_nof_prbs) * fragment_prb_size;
  if (compressed_data.size() < fragment_size) {
    clear();
    return result::invalid;
  }

  if (!active) {
    start(slot, symbol, port, nof_symbol_prbs, fragment_prb_size, params);
  } else if (!matches(slot, symbol, port, nof_symbol_prbs, fragment_prb_size, params)) {
    // A different symbol reusing the same bounded-ring entry starts a new assembly. A format or extent change for the
    // same symbol is malformed and must not combine incompatible payloads.
    if ((assembly_slot == slot) && (assembly_symbol == symbol) && (assembly_port == port)) {
      clear();
      return result::invalid;
    }
    start(slot, symbol, port, nof_symbol_prbs, fragment_prb_size, params);
  }

  if (received_prbs.any(start_prb, start_prb + fragment_nof_prbs)) {
    clear();
    return result::invalid;
  }

  std::memcpy(assembled_data.data() + static_cast<size_t>(start_prb) * prb_size, compressed_data.data(), fragment_size);
  received_prbs.fill(start_prb, start_prb + fragment_nof_prbs);
  return received_prbs.all() ? result::complete : result::incomplete;
}

void uplane_iq_data_fragment_assembler::clear()
{
  active          = false;
  assembly_symbol = 0;
  assembly_port   = 0;
  nof_prbs        = 0;
  prb_size        = 0;
  compression     = {};
  received_prbs   = {};
  assembled_data.clear();
}
