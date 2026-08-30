// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/ran/slot_point.h"
#include <array>
#include <complex>
#include <cstdint>

namespace ocudu {

/// Diagnostic stream identifiers used by upper PHY producers.
///
/// These identifiers are intentionally transport-neutral. Concrete adapters can
/// map them to ZMQ topics, files, plugin callbacks or metrics counters without
/// forcing processing code to include transport-specific headers.
enum class phy_diagnostic_stream : uint8_t {
  pusch_symbols = 0,
  pusch_crc,
  srs_channel,
  pusch_dmrs_channel,
  pusch_grid
};

/// PUSCH CRC diagnostic event.
struct pusch_crc_diagnostic {
  uint16_t   rnti   = 0;
  slot_point slot   = {};
  bool       crc_ok = false;
};

/// Everything an offline receiver needs to regenerate the DM-RS, descramble,
/// de-multiplex UCI, rate-dematch and LDPC-decode one PUSCH grant. Copied from
/// the PUSCH processor PDU at capture time; MCS index/table are optional
/// because the PHY PDU only carries them when the FAPI producer supplies them.
struct pusch_grant_diagnostic {
  uint8_t                 harq_id             = 0;
  uint16_t                n_id                = 0; ///< Data scrambling identity (c_init = RNTI * 2^15 + n_id).
  uint32_t                tbs_bytes           = 0; ///< Transport block size.
  uint8_t                 rv                  = 0;
  bool                    new_data            = true;
  uint8_t                 ldpc_base_graph     = 1; ///< 1 or 2.
  uint32_t                tbs_lbrm_bytes      = 0; ///< Limited-buffer rate-matching TB size.
  bool                    mcs_known           = false;
  uint8_t                 mcs_index           = 0;
  uint8_t                 mcs_table           = 0; ///< 0 qam64, 1 qam256, 2 qam64LowSe (TS 38.214 tables 5.1.3.1-1..3).
  bool                    transform_precoding = false;
  uint16_t                n_rs_id             = 0; ///< DM-RS identity when transform precoding is enabled.
  uint8_t                 cyclic_prefix       = 0; ///< 0 normal, 1 extended.
  uint8_t                 start_symbol        = 0;
  uint8_t                 nof_symbols         = 0;
  uint16_t                dmrs_symbol_mask    = 0; ///< Bit l set when OFDM symbol l carries DM-RS.
  uint8_t                 dmrs_type           = 1; ///< 1 or 2.
  uint8_t                 nof_cdm_groups_without_data = 2;
  uint16_t                dmrs_scrambling_id          = 0;
  bool                    n_scid                      = false;
  uint16_t                bwp_start_rb                = 0;
  uint16_t                bwp_size_rb                 = 0;
  std::array<uint8_t, 35> prb_mask{}; ///< CRB allocation mask, 275 bits, LSB-first per byte.
  uint8_t                 nof_rx_ports          = 0;
  uint8_t                 nof_layers            = 0;
  bool                    dc_present            = false;
  uint16_t                dc_position           = 0;
  uint16_t                nof_harq_ack_bits     = 0;
  uint16_t                nof_csi_part1_bits    = 0;
  bool                    csi_part2_expected    = false;
  float                   alpha_scaling         = 1.0F;
  float                   beta_offset_harq_ack  = 0.0F;
  float                   beta_offset_csi_part1 = 0.0F;
  float                   beta_offset_csi_part2 = 0.0F;
};

/// PUSCH equalized-symbol and link-quality diagnostic event.
///
/// \c equalized_symbols holds the equalized data resource elements of every
/// OFDM symbol of the grant (DM-RS resource elements excluded, data REs in
/// DM-RS symbols included), in codeword order: symbol-major, then resource
/// element, then layer. Captured before transform-precoding removal.
struct pusch_symbols_diagnostic {
  uint16_t                        rnti              = 0;
  slot_point                      slot              = {};
  uint16_t                        nof_prb           = 0;
  uint16_t                        nof_data_symbols  = 0;
  uint8_t                         nof_layers        = 0;
  uint8_t                         mod_bits          = 0;
  float                           sinr_dB           = 0.0F;
  float                           epre_dB           = 0.0F;
  float                           rsrp_dB           = 0.0F;
  float                           evm               = 0.0F;
  float                           time_alignment_us = 0.0F;
  float                           cfo_Hz            = 0.0F;
  float                           target_code_rate  = 0.0F;
  span<const std::complex<float>> equalized_symbols = {};
  bool                            grant_valid       = false;
  pusch_grant_diagnostic          grant             = {};
  bool                            crc_known         = false;
  bool                            crc_ok            = false;
  bool                            symbols_truncated = false;
};

/// Raw (pre-equalization) PUSCH resource grid capture for one grant: every
/// resource element of the allocated PRBs over the grant's OFDM symbols,
/// DM-RS included, for each receive port. Layout is receive-port-major, then
/// OFDM symbol (start_symbol..start_symbol+nof_symbols-1), then subcarrier
/// in ascending allocated-PRB order (nof_prb * 12 per symbol).
struct pusch_grid_diagnostic {
  uint16_t                        rnti              = 0;
  slot_point                      slot              = {};
  uint16_t                        nof_prb           = 0;
  uint8_t                         nof_rx_ports      = 0;
  uint8_t                         start_symbol      = 0;
  uint8_t                         nof_symbols       = 0;
  pusch_grant_diagnostic          grant             = {};
  span<const std::complex<float>> resource_elements = {};
};

/// SRS frequency-domain channel estimate diagnostic event for one Rx/Tx port pair.
struct srs_channel_diagnostic {
  uint16_t                        rnti                       = 0;
  slot_point                      slot                       = {};
  uint16_t                        nof_subcarriers            = 0;
  uint8_t                         comb_size                  = 0;
  uint8_t                         nof_rx_ports               = 0;
  uint8_t                         nof_tx_ports               = 0;
  uint8_t                         rx_port_index              = 0;
  uint8_t                         tx_port_index              = 0;
  float                           rsrp_dB                    = 0.0F;
  float                           epre_dB                    = 0.0F;
  float                           noise_variance             = 0.0F;
  float                           time_alignment_ns          = 0.0F;
  uint8_t                         start_symbol               = 0;
  uint8_t                         nof_symbols                = 0;
  uint16_t                        mapping_initial_subcarrier = 0;
  uint8_t                         comb_offset                = 0;
  uint8_t                         cyclic_shift               = 0;
  uint16_t                        sequence_id                = 0;
  uint16_t                        sequence_group             = 0;
  uint16_t                        sequence_number            = 0;
  uint16_t                        n_cs                       = 0;
  uint16_t                        n_cs_max                   = 0;
  uint8_t                         bandwidth_index            = 0;
  uint8_t                         freq_position              = 0;
  uint16_t                        freq_shift                 = 0;
  uint8_t                         freq_hopping               = 0;
  span<const std::complex<float>> h_coefficients             = {};
};

/// PUSCH DM-RS channel-estimate diagnostic event for one Rx/layer/symbol view.
struct pusch_dmrs_channel_diagnostic {
  uint16_t                        rnti                        = 0;
  slot_point                      slot                        = {};
  uint16_t                        start_prb                   = 0;
  uint16_t                        nof_prb                     = 0;
  uint16_t                        nof_subcarriers             = 0;
  uint8_t                         dmrs_symbol_index           = 0;
  uint8_t                         start_symbol                = 0;
  uint8_t                         nof_symbols                 = 0;
  uint8_t                         nof_rx_ports                = 0;
  uint8_t                         nof_tx_layers               = 0;
  uint8_t                         rx_port_index               = 0;
  uint8_t                         tx_layer_index              = 0;
  uint8_t                         dmrs_type                   = 1;
  uint8_t                         nof_cdm_groups_without_data = 0;
  bool                            transform_precoding         = false;
  uint16_t                        scrambling_id               = 0;
  bool                            n_scid                      = false;
  float                           sinr_dB                     = 0.0F;
  float                           epre_dB                     = 0.0F;
  float                           rsrp_dB                     = 0.0F;
  float                           noise_variance              = 0.0F;
  float                           time_alignment_us           = 0.0F;
  float                           cfo_Hz                      = 0.0F;
  float                           target_code_rate            = 0.0F;
  span<const std::complex<float>> h_estimates                 = {};
};

/// Transport-neutral upper PHY diagnostics sink.
///
/// The capture-policy methods are part of the same abstraction so producers can
/// perform a cheap early gate before doing expensive diagnostic work such as GPU
/// D2H copies, CPU shadow channel estimation or large buffer copies.
class phy_diagnostic_sink
{
public:
  virtual ~phy_diagnostic_sink() = default;

  /// Returns true when a producer should capture and publish the stream now.
  ///
  /// Implementations may consume rate-limit state in this call. Producers should
  /// call it once before doing any expensive diagnostic preparation.
  virtual bool should_collect(phy_diagnostic_stream stream, uint16_t rnti) = 0;

  /// Returns true when a stream is due without consuming rate-limit state.
  ///
  /// This is useful when a producer needs to decide whether to run an expensive
  /// shadow computation whose output will later be published through
  /// should_collect().
  virtual bool is_collect_due(phy_diagnostic_stream stream, uint16_t rnti) = 0;

  /// Publishes a PUSCH CRC diagnostic event.
  virtual void on_pusch_crc(const pusch_crc_diagnostic& event) = 0;

  /// Publishes a PUSCH equalized-symbol diagnostic event.
  virtual void on_pusch_symbols(const pusch_symbols_diagnostic& event) = 0;

  /// Publishes an SRS channel diagnostic event.
  virtual void on_srs_channel(const srs_channel_diagnostic& event) = 0;

  /// Publishes a PUSCH DM-RS channel diagnostic event.
  virtual void on_pusch_dmrs_channel(const pusch_dmrs_channel_diagnostic& event) = 0;

  /// Raw resource-grid capture; sinks that do not carry it ignore the event.
  virtual void on_pusch_grid(const pusch_grid_diagnostic& /*event*/) {}
};

} // namespace ocudu
