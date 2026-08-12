// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ofh_uplane_rx_symbol_data_flow_writer.h"
#include "ocudu/instrumentation/traces/ofh_traces.h"
#include "ocudu/ofh/serdes/ofh_uplane_message_decoder.h"
#include "ocudu/ofh/serdes/ofh_uplane_message_decoder_properties.h"

using namespace ocudu;
using namespace ofh;

uplane_iq_data_fragment_assembler&
uplane_rx_symbol_data_flow_writer::get_fragment_assembler(slot_point slot, unsigned symbol, unsigned port)
{
  ocudu_assert(symbol < MAX_NSYMB_PER_SLOT, "Invalid symbol index '{}'", symbol);
  ocudu_assert(port < ul_eaxc.size(), "Invalid resource grid port '{}'", port);

  unsigned slot_index = calculate_repository_index(slot, ul_context_repo->get_nof_slot_entries());
  size_t   index      = (static_cast<size_t>(slot_index) * MAX_NSYMB_PER_SLOT + symbol) * ul_eaxc.size() + port;
  return fragment_assemblers[index];
}

bool uplane_rx_symbol_data_flow_writer::write_to_resource_grid(unsigned                              eaxc,
                                                               const uplane_message_decoder_results& results)
{
  trace_point access_repo_tp = ofh_tracer.now();

  slot_point     slot       = results.params.slot;
  unsigned       symbol     = results.params.symbol_id;
  uplink_context ul_context = ul_context_repo->get(slot, symbol);
  if (OCUDU_UNLIKELY(ul_context.empty())) {
    logger.info(
        "Sector#{}: dropped received Open Fronthaul message as no uplink slot context was found for slot '{}', symbol "
        "'{}' and eAxC '{}'",
        sector_id,
        results.params.slot,
        results.params.symbol_id,
        eaxc);

    return false;
  }
  ofh_tracer << trace_event("ofh_receiver_access_repo", access_repo_tp);

  // Find resource grid port with eAxC.
  unsigned rg_port = std::distance(ul_eaxc.begin(), std::find(ul_eaxc.begin(), ul_eaxc.end(), eaxc));
  ocudu_assert(rg_port < ul_eaxc.size(), "Invalid resource grid port value '{}'", rg_port);

  // The DU cell bandwidth may be narrower than the operating bandwidth of the RU.
  unsigned du_nof_prbs = ul_context.get_grid_nof_prbs();
  for (const auto& section : results.sections) {
    // Drop the whole section when all PRBs are outside the range of the DU bandwidth and the operating bandwidth of the
    // RU is larger.
    if (section.start_prb >= du_nof_prbs) {
      continue;
    }

    // At this point, we have to care about the following cases:
    //   a) The last PRB of the section falls outside the range of the DU cell bandwidth.
    //   b) The last PRB of the section falls inside the range of the DU cell bandwidth.

    // Take care of case (a), takes the first N PRBs inside the section.
    unsigned nof_prbs_to_write = du_nof_prbs - section.start_prb;
    // Take care of case (b), takes all the PRBs inside the section.
    if (section.start_prb + section.nof_prbs < du_nof_prbs) {
      nof_prbs_to_write = section.nof_prbs;
    }

    trace_point write_rg_tp = ofh_tracer.now();

    ul_context_repo->write_grid(
        slot,
        rg_port,
        symbol,
        section.start_prb * NOF_SUBCARRIERS_PER_RB,
        span<const cbf16_t>(section.iq_samples).first(nof_prbs_to_write * NOF_SUBCARRIERS_PER_RB));

    ofh_tracer << trace_event("ofh_receiver_write_rg", write_rg_tp);

    if (OCUDU_UNLIKELY(logger.debug.enabled())) {
      logger.debug("Sector#{}: written IQ data into UL resource grid PRB range [{},{}), for slot '{}', symbol '{}' and "
                   "port '{}'",
                   sector_id,
                   section.start_prb,
                   section.start_prb + nof_prbs_to_write,
                   slot,
                   symbol,
                   rg_port);
    }
  }

  return true;
}

uplane_rx_symbol_data_flow_writer::direct_write_result
uplane_rx_symbol_data_flow_writer::write_compressed_to_resource_grid(unsigned                              eaxc,
                                                                     const uplane_message_decoder_results& results,
                                                                     uplane_message_decoder&               decoder)
{
  trace_point access_repo_tp = ofh_tracer.now();

  slot_point     slot       = results.params.slot;
  unsigned       symbol     = results.params.symbol_id;
  uplink_context ul_context = ul_context_repo->get(slot, symbol);
  if (OCUDU_UNLIKELY(ul_context.empty())) {
    logger.info(
        "Sector#{}: dropped received Open Fronthaul message as no uplink slot context was found for slot '{}', symbol "
        "'{}' and eAxC '{}'",
        sector_id,
        results.params.slot,
        results.params.symbol_id,
        eaxc);

    return direct_write_result::failure_missing_context;
  }
  ofh_tracer << trace_event("ofh_receiver_access_repo", access_repo_tp);

  unsigned rg_port = std::distance(ul_eaxc.begin(), std::find(ul_eaxc.begin(), ul_eaxc.end(), eaxc));
  ocudu_assert(rg_port < ul_eaxc.size(), "Invalid resource grid port value '{}'", rg_port);

  unsigned du_nof_prbs = ul_context.get_grid_nof_prbs();

  // Preflight every relevant section before writing any data. A host fallback is safe only if this resource grid or
  // compression format has no direct path. Malformed payloads and device-write failures are terminal packet drops.
  if (!ul_context.supports_device_grid_mapping()) {
    return direct_write_result::fallback_unsupported_destination;
  }

  bool has_relevant_section = false;
  for (const auto& section : results.sections) {
    if (section.start_prb >= du_nof_prbs) {
      continue;
    }
    has_relevant_section = true;
    if (section.compressed_iq_data.empty()) {
      return direct_write_result::failure_invalid_input;
    }
    if (!decoder.supports_resource_grid_decompression(section.ud_comp_hdr)) {
      return direct_write_result::fallback_unsupported_compression;
    }
  }
  if (!has_relevant_section) {
    return direct_write_result::failure_no_relevant_data;
  }

  bool recovered_after_retry = false;
  bool fragment_buffered     = false;
  for (const auto& section : results.sections) {
    if (section.start_prb >= du_nof_prbs) {
      continue;
    }

    unsigned nof_prbs_to_write = du_nof_prbs - section.start_prb;
    if (section.start_prb + section.nof_prbs < du_nof_prbs) {
      nof_prbs_to_write = section.nof_prbs;
    }

    uplane_section_params              assembled_section  = {};
    const uplane_section_params*       section_to_write   = &section;
    uplane_iq_data_fragment_assembler* fragment_assembler = nullptr;
    if (decoder.prefers_full_symbol_decompression(section.ud_comp_hdr)) {
      fragment_assembler   = &get_fragment_assembler(slot, symbol, rg_port);
      auto assembly_result = fragment_assembler->add_fragment(slot,
                                                              symbol,
                                                              rg_port,
                                                              du_nof_prbs,
                                                              section.start_prb,
                                                              nof_prbs_to_write,
                                                              section.compressed_iq_data,
                                                              section.ud_comp_hdr);
      if (assembly_result == uplane_iq_data_fragment_assembler::result::invalid) {
        return direct_write_result::failure_invalid_input;
      }
      if (assembly_result == uplane_iq_data_fragment_assembler::result::incomplete) {
        fragment_buffered = true;
        continue;
      }
      fragment_buffered = false;

      assembled_section.start_prb          = 0;
      assembled_section.nof_prbs           = du_nof_prbs;
      assembled_section.ud_comp_hdr        = section.ud_comp_hdr;
      assembled_section.compressed_iq_data = fragment_assembler->get_assembled_data();
      section_to_write                     = &assembled_section;
    }

    trace_point write_rg_tp = ofh_tracer.now();

    iq_decompression_result decompression_result = iq_decompression_result::destination_failure;
    bool                    written              = ul_context_repo->write_grid_device(
        slot,
        rg_port,
        symbol,
        section_to_write->start_prb * NOF_SUBCARRIERS_PER_RB,
        section_to_write->nof_prbs * NOF_SUBCARRIERS_PER_RB,
        [&decoder, &decompression_result, rg_port, symbol, section_to_write](resource_grid_writer& writer) {
          decompression_result = decoder.decompress_to_resource_grid(
              writer, rg_port, symbol, section_to_write->start_prb, section_to_write->nof_prbs, *section_to_write);
          return is_iq_decompression_success(decompression_result);
        });
    if (fragment_assembler != nullptr) {
      // Direct decompression snapshots the transient span before returning. Retain capacity, but make this ring entry
      // available to the next slot immediately.
      fragment_assembler->clear();
    }
    if (!written) {
      return to_direct_write_result(decompression_result);
    }
    recovered_after_retry |= (decompression_result == iq_decompression_result::success_after_retry);

    ofh_tracer << trace_event("ofh_receiver_write_rg_device", write_rg_tp);

    if (OCUDU_UNLIKELY(logger.debug.enabled())) {
      logger.debug("Sector#{}: written compressed IQ data into UL resource grid PRB range [{},{}), for slot '{}', "
                   "symbol '{}' and port '{}'",
                   sector_id,
                   section_to_write->start_prb,
                   section_to_write->start_prb + section_to_write->nof_prbs,
                   slot,
                   symbol,
                   rg_port);
    }
  }

  if (fragment_buffered) {
    return direct_write_result::fragment_buffered;
  }
  return recovered_after_retry ? direct_write_result::success_after_retry : direct_write_result::success;
}
