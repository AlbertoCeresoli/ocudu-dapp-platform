// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace ocudu::dapp::e3 {

/// Telemetry stream identifiers (OCUDU-TELEMETRY-SM). They share the
/// portable data-store interface namespace with the Class-C v1 interfaces
/// (0x0001xxxx) and are therefore also reachable through the management
/// `dataSubscriptions` / `dataPoll` pull path.
constexpr uint32_t telemetry_stream_pusch_symbols      = 0x00020001;
constexpr uint32_t telemetry_stream_pusch_crc          = 0x00020002;
constexpr uint32_t telemetry_stream_srs_channel        = 0x00020003;
constexpr uint32_t telemetry_stream_pusch_dmrs_channel = 0x00020004;
constexpr uint32_t telemetry_stream_gnb_metrics_json   = 0x00020005;
constexpr uint32_t telemetry_stream_pusch_grid         = 0x00020006;
constexpr uint32_t telemetry_stream_srs_config         = 0x00020007;
constexpr uint32_t telemetry_stream_ta_command         = 0x00020008;
/// Highest telemetry stream identifier: per-stream tables are sized from it.
constexpr uint32_t telemetry_stream_last = telemetry_stream_ta_command;

constexpr bool is_telemetry_stream(uint32_t interface_id) noexcept
{
  return interface_id >= telemetry_stream_pusch_symbols && interface_id <= telemetry_stream_last;
}

/// Kind discriminator stored in the portable metadata header.
enum class telemetry_kind : uint16_t {
  pusch_symbols      = 1,
  pusch_crc          = 2,
  srs_channel        = 3,
  pusch_dmrs_channel = 4,
  gnb_metrics_json   = 5,
  pusch_grid         = 6,
  srs_config         = 7,
  ta_command         = 8,
};

constexpr std::optional<telemetry_kind> telemetry_kind_of(uint32_t interface_id) noexcept
{
  switch (interface_id) {
    case telemetry_stream_pusch_symbols:
      return telemetry_kind::pusch_symbols;
    case telemetry_stream_pusch_crc:
      return telemetry_kind::pusch_crc;
    case telemetry_stream_srs_channel:
      return telemetry_kind::srs_channel;
    case telemetry_stream_pusch_dmrs_channel:
      return telemetry_kind::pusch_dmrs_channel;
    case telemetry_stream_gnb_metrics_json:
      return telemetry_kind::gnb_metrics_json;
    case telemetry_stream_pusch_grid:
      return telemetry_kind::pusch_grid;
    case telemetry_stream_srs_config:
      return telemetry_kind::srs_config;
    case telemetry_stream_ta_command:
      return telemetry_kind::ta_command;
    default:
      return std::nullopt;
  }
}

/// Little-endian packed metadata carried in the portable data store for every
/// telemetry publication. Producers copy one of these records verbatim (no
/// codec on the radio thread); the E3AP agent and independent pull clients
/// decode it. The tensor part of the publication holds the sample payload:
/// interleaved binary32 (re, im) pairs for complex vectors, one byte for CRC,
/// or UTF-8 JSON for the gNB metrics report.
#pragma pack(push, 1)
struct telemetry_metadata_header {
  static constexpr uint32_t expected_magic   = 0x3345544FU; // "OTE3"
  static constexpr uint16_t expected_version = 1;

  uint32_t magic{expected_magic};
  uint16_t version{expected_version};
  uint16_t kind{};
  uint64_t monotonic_ns{};
};

struct telemetry_radio_slot {
  uint16_t sfn{};
  uint8_t  slot{};
  uint8_t  numerology{};
};

/// Per-UE PUSCH link quality; the tensor is the equalized data symbols.
/// Non-finite measurements are sanitized to zero before packing.
struct pusch_symbols_metadata_v1 {
  telemetry_metadata_header header{telemetry_metadata_header::expected_magic,
                                   telemetry_metadata_header::expected_version,
                                   static_cast<uint16_t>(telemetry_kind::pusch_symbols),
                                   0};
  uint16_t                  rnti{};
  telemetry_radio_slot      radio_slot{};
  uint16_t                  nof_prb{};
  uint8_t                   nof_data_symbols{};
  uint8_t                   nof_layers{};
  uint8_t                   modulation_bits{}; ///< Bits per modulation symbol (2 = QPSK .. 8 = 256QAM).
  uint8_t                   reserved{};
  float                     sinr_db{}; ///< Post-equalization SINR in dB.
  float                     epre_db{}; ///< Energy per resource element in dB (relative to full scale).
  float                     rsrp_db{}; ///< Reference-signal received power in dB (same scale as EPRE).
  float                     evm{};     ///< RMS error vector magnitude as a linear fraction (not percent).
  float                     time_alignment_us{}; ///< Measured time alignment in microseconds.
  float                     cfo_hz{};            ///< Carrier frequency offset in Hz.
  float                     target_code_rate{};  ///< Normalized target code rate R (0..1).
};

/// Grant parameters shared by the PUSCH symbols (v2) and PUSCH grid records.
/// flags: bit0 mcs_known, bit1 transform_precoding, bit2 n_scid, bit3 dc_present, bit4 csi_part2_expected.
struct pusch_grant_metadata_v1 {
  uint8_t  harq_id{};
  uint8_t  rv{};
  uint8_t  new_data{};
  uint8_t  ldpc_base_graph{};
  uint16_t n_id{};
  uint16_t bwp_start_rb{};
  uint16_t bwp_size_rb{};
  uint16_t dmrs_symbol_mask{};
  uint16_t dmrs_scrambling_id{};
  uint16_t dc_position{};
  uint16_t nof_harq_ack_bits{};
  uint16_t nof_csi_part1_bits{};
  uint16_t n_rs_id{};
  uint32_t tbs_bytes{};
  uint32_t tbs_lbrm_bytes{};
  uint8_t  flags{};
  uint8_t  mcs_index{};
  uint8_t  mcs_table{};
  uint8_t  start_symbol{};
  uint8_t  nof_symbols{};
  uint8_t  dmrs_type{};
  uint8_t  nof_cdm_groups_without_data{};
  uint8_t  nof_rx_ports{};
  uint8_t  nof_layers{};
  uint8_t  cyclic_prefix{};
  float    alpha_scaling{};
  float    beta_offset_harq_ack{};
  float    beta_offset_csi_part1{};
  float    beta_offset_csi_part2{};
  uint8_t  prb_mask[35]{};
  uint8_t  reserved{};
};

/// PUSCH symbols metadata, version 2: the v1 layout followed by capture flags
/// and the grant parameters. symbol_flags: bit0 crc_known, bit1 crc_ok,
/// bit2 symbols_truncated, bit3 grant_valid.
struct pusch_symbols_metadata_v2 {
  pusch_symbols_metadata_v1 base{};
  uint8_t                   symbol_flags{};
  uint8_t                   reserved[3]{};
  pusch_grant_metadata_v1   grant{};
};

/// Raw resource-grid capture: receive-port-major, then OFDM symbol, then
/// subcarrier over the allocated PRBs (nof_prb * 12 per symbol).
struct pusch_grid_metadata_v1 {
  telemetry_metadata_header header{telemetry_metadata_header::expected_magic,
                                   telemetry_metadata_header::expected_version,
                                   static_cast<uint16_t>(telemetry_kind::pusch_grid),
                                   0};
  uint16_t                  rnti{};
  telemetry_radio_slot      radio_slot{};
  uint16_t                  nof_prb{};
  uint8_t                   nof_rx_ports{};
  uint8_t                   start_symbol{};
  uint8_t                   nof_symbols{};
  uint8_t                   reserved{};
  pusch_grant_metadata_v1   grant{};
};

struct pusch_crc_metadata_v1 {
  telemetry_metadata_header header{telemetry_metadata_header::expected_magic,
                                   telemetry_metadata_header::expected_version,
                                   static_cast<uint16_t>(telemetry_kind::pusch_crc),
                                   0};
  uint16_t                  rnti{};
  telemetry_radio_slot      radio_slot{};
  uint8_t                   crc_ok{};
  uint8_t                   reserved[3]{};
};

struct srs_channel_metadata_v1 {
  telemetry_metadata_header header{telemetry_metadata_header::expected_magic,
                                   telemetry_metadata_header::expected_version,
                                   static_cast<uint16_t>(telemetry_kind::srs_channel),
                                   0};
  uint16_t                  rnti{};
  telemetry_radio_slot      radio_slot{};
  uint16_t                  nof_subcarriers{};
  uint8_t                   comb_size{};
  uint8_t                   nof_rx_ports{};
  uint8_t                   nof_tx_ports{};
  uint8_t                   rx_port_index{};
  uint8_t                   tx_port_index{};
  uint8_t                   start_symbol{};
  uint8_t                   nof_symbols{};
  uint8_t                   comb_offset{};
  uint8_t                   cyclic_shift{};
  uint8_t                   bandwidth_index{};
  uint8_t                   frequency_position{};
  uint8_t                   frequency_hopping{};
  uint16_t                  mapping_initial_subcarrier{};
  uint16_t                  sequence_id{};
  uint16_t                  sequence_group{};
  uint16_t                  sequence_number{};
  uint16_t                  cyclic_shift_index{};
  uint16_t                  cyclic_shift_maximum{};
  uint16_t                  frequency_shift{};
  float                     rsrp_db{};          ///< SRS RSRP in dB.
  float                     epre_db{};          ///< Energy per resource element in dB.
  float                     noise_variance{};   ///< Linear noise variance (sigma^2), not dB.
  float                     time_alignment_ns{}; ///< Measured time alignment in nanoseconds (note: ns, not us).
};

/// SRS channel metadata with the SRS channel-estimator dApp input. The tensor holds the least-squares estimate
/// (base.nof_subcarriers), then the received pilots (pilot_symbols x pilot_length, symbol major) and the
/// reference sequence (pilot_length); pilot_length zero means only the estimate.
struct srs_channel_metadata_v2 {
  srs_channel_metadata_v1 base{};
  uint16_t                pilot_length{};
  uint8_t                 pilot_symbols{};
  uint8_t                 reserved{};
};

struct pusch_dmrs_channel_metadata_v1 {
  telemetry_metadata_header header{telemetry_metadata_header::expected_magic,
                                   telemetry_metadata_header::expected_version,
                                   static_cast<uint16_t>(telemetry_kind::pusch_dmrs_channel),
                                   0};
  uint16_t                  rnti{};
  telemetry_radio_slot      radio_slot{};
  uint16_t                  start_prb{};
  uint16_t                  nof_prb{};
  uint16_t                  nof_subcarriers{};
  uint8_t                   dmrs_symbol_index{};
  uint8_t                   start_symbol{};
  uint8_t                   nof_symbols{};
  uint8_t                   nof_rx_ports{};
  uint8_t                   nof_tx_layers{};
  uint8_t                   rx_port_index{};
  uint8_t                   tx_layer_index{};
  uint8_t                   dmrs_type{1};
  uint8_t                   nof_cdm_groups_without_data{};
  uint8_t                   transform_precoding{};
  uint8_t                   n_scid{};
  uint8_t                   reserved{};
  uint16_t                  scrambling_id{};
  float                     sinr_db{};           ///< DM-RS based SINR in dB.
  float                     epre_db{};           ///< Energy per resource element in dB.
  float                     rsrp_db{};           ///< RSRP in dB (same scale as EPRE).
  float                     noise_variance{};    ///< Linear noise variance (sigma^2), not dB.
  float                     time_alignment_us{}; ///< Measured time alignment in microseconds (note: us, not ns).
  float                     cfo_hz{};            ///< Carrier frequency offset in Hz.
  float                     target_code_rate{};  ///< Normalized target code rate R (0..1).
};

/// Event of an SRS configuration record.
enum class srs_config_event : uint8_t { created = 1, reconfigured = 2, applied = 3, removed = 4, snapshot = 5 };

/// SRS configuration of one UE context as set by the DU: the periodic SRS-Resource of TS 38.331 plus the mapping it
/// derives to (TS 38.211 6.4.1.4, transmit port 0). Published on every change and replayed to new subscribers, so a
/// consumer always learns the current configuration. The tensor is one byte, a copy of \c event.
/// flags: bit0 srs_configured (a periodic resource is present), bit1 derived_valid (no hopping, derived fields set).
struct srs_config_metadata_v1 {
  telemetry_metadata_header header{telemetry_metadata_header::expected_magic,
                                   telemetry_metadata_header::expected_version,
                                   static_cast<uint16_t>(telemetry_kind::srs_config),
                                   0};
  uint16_t                  rnti{};
  uint8_t                   event{};
  uint8_t                   flags{};
  /// Incremented by the publisher whenever the configuration of any UE context changes, created or removed.
  uint32_t                  config_epoch{};
  uint16_t                  ue_index{};
  uint8_t                   numerology{};
  uint8_t                   resource_id{};
  uint8_t                   nof_ports{};
  uint8_t                   comb_size{};
  uint8_t                   comb_offset{};
  uint8_t                   cyclic_shift{};
  uint8_t                   start_position{}; ///< startPosition: symbols counted from the end of the slot.
  uint8_t                   nof_symbols{};
  uint8_t                   repetition_factor{};
  uint8_t                   freq_domain_position{};
  uint8_t                   c_srs{};
  uint8_t                   b_srs{};
  uint8_t                   b_hop{};
  uint8_t                   group_or_sequence_hopping{}; ///< 0 neither, 1 group, 2 sequence.
  uint16_t                  freq_domain_shift{};
  uint16_t                  sequence_id{};
  uint16_t                  period_slots{};
  uint16_t                  offset_slots{};
  /// N_TA,offset of the cell in units of T_c (TS 38.133 Table 7.1.2-2).
  uint16_t                  n_ta_offset{};
  uint16_t                  bwp_start_rb{};
  uint16_t                  bwp_size_rb{};
  uint8_t                   start_symbol{};
  uint8_t                   cyclic_shift_maximum{};
  uint16_t                  sequence_length{};
  uint16_t                  mapping_initial_subcarrier{}; ///< k0 of port 0 within the BWP, as in srs.channel.
  uint16_t                  sequence_group{};
  uint16_t                  sequence_number{};
};

/// Source of a timing advance command record.
enum class ta_command_source : uint8_t { random_access_response = 1, mac_ce = 2 };

/// One timing advance command sent to a UE (TS 38.321 6.1.3.4 and 6.2.3): the RAR's 12-bit T_A (0..3846) or a MAC
/// CE's 6-bit T_A (0..63, 31 = no change). radio_slot is the PDSCH slot carrying it. The tensor is one byte, a copy of
/// \c source.
struct ta_command_metadata_v1 {
  telemetry_metadata_header header{telemetry_metadata_header::expected_magic,
                                   telemetry_metadata_header::expected_version,
                                   static_cast<uint16_t>(telemetry_kind::ta_command),
                                   0};
  uint16_t                  rnti{};
  telemetry_radio_slot      radio_slot{};
  uint8_t                   source{};
  uint8_t                   tag_id{};
  uint16_t                  ta_command{};
};

struct gnb_metrics_json_metadata_v1 {
  telemetry_metadata_header header{telemetry_metadata_header::expected_magic,
                                   telemetry_metadata_header::expected_version,
                                   static_cast<uint16_t>(telemetry_kind::gnb_metrics_json),
                                   0};
};
#pragma pack(pop)

static_assert(sizeof(telemetry_metadata_header) == 16);
static_assert(sizeof(pusch_symbols_metadata_v1) == 56);
static_assert(sizeof(pusch_grant_metadata_v1) == 92);
static_assert(sizeof(pusch_symbols_metadata_v2) == 152);
static_assert(sizeof(pusch_grid_metadata_v1) == 120);
static_assert(sizeof(pusch_crc_metadata_v1) == 26);
static_assert(sizeof(srs_channel_metadata_v1) == 66);
static_assert(sizeof(srs_channel_metadata_v2) == 70);
static_assert(sizeof(pusch_dmrs_channel_metadata_v1) == 70);
static_assert(sizeof(gnb_metrics_json_metadata_v1) == 16);
static_assert(sizeof(srs_config_metadata_v1) == 64);
static_assert(sizeof(ta_command_metadata_v1) == 26);

/// Reads a packed metadata header from the front of a metadata buffer.
inline std::optional<telemetry_metadata_header> read_telemetry_header(const uint8_t* data, std::size_t size) noexcept
{
  if (data == nullptr || size < sizeof(telemetry_metadata_header)) {
    return std::nullopt;
  }
  telemetry_metadata_header header{};
  std::memcpy(&header, data, sizeof(header));
  // Version 1 is the original layout; version 2 appends the PUSCH grant block
  // to the PUSCH symbols record and leaves every other kind unchanged.
  if (header.magic != telemetry_metadata_header::expected_magic ||
      (header.version != telemetry_metadata_header::expected_version && header.version != 2)) {
    return std::nullopt;
  }
  return header;
}

} // namespace ocudu::dapp::e3
