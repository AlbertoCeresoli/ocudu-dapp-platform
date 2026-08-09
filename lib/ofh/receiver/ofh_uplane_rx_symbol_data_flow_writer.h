// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "../support/uplink_context_repository.h"
#include "ofh_direct_write_result.h"
#include "ofh_uplane_iq_data_fragment_assembler.h"
#include "ocudu/ocudulog/logger.h"

namespace ocudu {
namespace ofh {

struct uplane_message_decoder_results;
class uplane_message_decoder;

/// \brief Open Fronthaul User-Plane uplink received symbol data flow writer.
///
/// Writes IQ data received in an Open Fronthaul message to the corresponding resource grid.
class uplane_rx_symbol_data_flow_writer
{
public:
  using direct_write_result = ofh::direct_write_result;

  uplane_rx_symbol_data_flow_writer(span<const unsigned>                       ul_eaxc_,
                                    unsigned                                   sector_id_,
                                    ocudulog::basic_logger&                    logger_,
                                    std::shared_ptr<uplink_context_repository> ul_context_repo_) :
    ul_eaxc(ul_eaxc_.begin(), ul_eaxc_.end()),
    sector_id(sector_id_),
    logger(logger_),
    ul_context_repo(std::move(ul_context_repo_))
  {
    ocudu_assert(!ul_eaxc.empty(), "Invalid number of uplink eAxCs");
    ocudu_assert(ul_context_repo, "Invalid uplink context repository");
    fragment_assemblers.resize(static_cast<size_t>(ul_context_repo->get_nof_slot_entries()) * MAX_NSYMB_PER_SLOT *
                               ul_eaxc.size());
  }

  /// Writes the given decoder results in the corresponding resource grid using the given eAxC. Returns true on success,
  /// otherwise false.
  bool write_to_resource_grid(unsigned eaxc, const uplane_message_decoder_results& results);

  /// Writes compressed decoder results directly into the corresponding resource grid when the grid and decompressor
  /// support a device-resident path. A fallback result means that no device write was attempted and host decoding is
  /// safe. A buffered result means that the fragment was retained until the complete symbol arrives. A failure result
  /// is terminal for the packet and must not trigger host decoding.
  direct_write_result write_compressed_to_resource_grid(unsigned                              eaxc,
                                                        const uplane_message_decoder_results& results,
                                                        uplane_message_decoder&               decoder);

private:
  uplane_iq_data_fragment_assembler& get_fragment_assembler(slot_point slot, unsigned symbol, unsigned port);

  const static_vector<unsigned, MAX_NOF_SUPPORTED_EAXC> ul_eaxc;
  const unsigned                                        sector_id;
  ocudulog::basic_logger&                               logger;
  std::shared_ptr<uplink_context_repository>            ul_context_repo;
  std::vector<uplane_iq_data_fragment_assembler>        fragment_assemblers;
};

} // namespace ofh
} // namespace ocudu
