// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/e3ap_codec.h"

extern "C" {
#include "ClassCPortableRecord.h"
#include "GnbMetricsJsonRecord.h"
#include "Mgmt-RanFunctionData.h"
#include "PuschCrcRecord.h"
#include "PuschDmrsChannelRecord.h"
#include "PuschGrantParameters.h"
#include "PuschGridRecord.h"
#include "PuschSymbolsRecord.h"
#include "RadioSlot.h"
#include "SrsChannelRecord.h"
#include "SrsConfigRecord.h"
#include "SrsDerivedMapping.h"
#include "SrsResourceParameters.h"
#include "TaCommandRecord.h"
#include "Telemetry-DropPolicy.h"
#include "Telemetry-IndicationData.h"
#include "Telemetry-Object.h"
#include "Telemetry-RanFunctionData.h"
#include "Telemetry-StreamDescriptor.h"
#include "Telemetry-SubscriptionConfig.h"
#include "Telemetry-SubscriptionData.h"
#include "aper_decoder.h"
#include "aper_encoder.h"
#include "asn_SEQUENCE_OF.h"
#include "constraints.h"
}

#include "asn1_codec_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;
using namespace ocudu::dapp::e3::asn1_detail;

namespace {

constexpr std::size_t maximum_telemetry_streams = 64;
constexpr std::size_t maximum_rnti_filter       = 32;
constexpr std::size_t maximum_telemetry_bytes   = e3ap_maximum_payload_bytes + 4096;

// ---------------------------------------------------------------------------
// Small conversions
// ---------------------------------------------------------------------------

template <typename T>
void set_optional_long(long*& destination, const std::optional<T>& value)
{
  if (value) {
    destination = make_asn<long>();
    set_u64(*destination, static_cast<uint64_t>(*value));
  }
}

template <typename T>
void set_optional_typed(T*& destination, const std::optional<uint16_t>& value)
{
  if (value) {
    destination = make_asn<T>();
    set_u64(*destination, *value);
  }
}

uint16_t read_u16(long value, const char* what)
{
  return static_cast<uint16_t>(read_ranged(value, 0, 65535, what));
}

uint16_t read_id16(long value, const char* what)
{
  return static_cast<uint16_t>(read_ranged(value, 1, 65535, what));
}

void set_optional_u64(OCTET_STRING_t*& destination, const std::optional<uint64_t>& value)
{
  if (value) {
    destination = make_asn<OCTET_STRING_t>();
    set_u64(*destination, *value);
  }
}

// ---------------------------------------------------------------------------
// OCUDU-TELEMETRY-SM
// ---------------------------------------------------------------------------

void set_radio_slot(RadioSlot_t*& destination, const telemetry_radio_slot_value& source)
{
  destination = make_asn<RadioSlot_t>();
  set_u64(destination->sfn, read_ranged(source.sfn, 0, 1023, "SFN"));
  set_u64(destination->slot, read_ranged(source.slot, 0, 159, "slot"));
  set_u64(destination->numerology, read_ranged(source.numerology, 0, 4, "numerology"));
}

telemetry_radio_slot_value read_radio_slot(const RadioSlot_t* source)
{
  if (source == nullptr) {
    throw std::runtime_error("radio slot is missing");
  }
  telemetry_radio_slot_value result;
  result.sfn        = static_cast<uint16_t>(read_ranged(source->sfn, 0, 1023, "SFN"));
  result.slot       = static_cast<uint8_t>(read_ranged(source->slot, 0, 159, "slot"));
  result.numerology = static_cast<uint8_t>(read_ranged(source->numerology, 0, 4, "numerology"));
  return result;
}

void set_optional_real(double*& destination, const std::optional<double>& value)
{
  if (value && std::isfinite(*value)) {
    destination  = make_asn<double>();
    *destination = *value;
  }
}

std::optional<double> read_optional_real(const double* source)
{
  if (source == nullptr) {
    return std::nullopt;
  }
  if (!std::isfinite(*source)) {
    throw std::runtime_error("non-finite real value");
  }
  return *source;
}

template <typename T>
void set_optional_small(long*& destination, const std::optional<T>& value, uint64_t maximum, const char* what)
{
  if (value) {
    destination = make_asn<long>();
    set_u64(*destination, read_ranged(*value, 0, maximum, what));
  }
}

template <typename T>
std::optional<T> read_optional_small(const long* source, uint64_t maximum, const char* what)
{
  if (source == nullptr) {
    return std::nullopt;
  }
  return static_cast<T>(read_ranged(*source, 0, maximum, what));
}

void set_samples(OCTET_STRING_t& destination, const std::vector<uint8_t>& samples)
{
  if (samples.size() > e3ap_maximum_payload_bytes || samples.size() % (2 * sizeof(float)) != 0) {
    throw std::runtime_error("complex sample vector must hold whole binary32 pairs within the bound");
  }
  set_octets(destination, samples);
}

std::vector<uint8_t> read_samples(const OCTET_STRING_t& source)
{
  auto bytes = read_bytes(source, e3ap_maximum_payload_bytes);
  if (bytes.size() % (2 * sizeof(float)) != 0) {
    throw std::runtime_error("complex sample vector must hold whole binary32 pairs");
  }
  return bytes;
}

pusch_grant_parameters grant_from_packed(const pusch_grant_metadata_v1& source)
{
  pusch_grant_parameters result;
  result.harq_id         = source.harq_id;
  result.n_id            = source.n_id;
  result.tbs_bytes       = source.tbs_bytes;
  result.rv              = source.rv;
  result.new_data        = source.new_data != 0;
  result.ldpc_base_graph = source.ldpc_base_graph;
  result.tbs_lbrm_bytes  = source.tbs_lbrm_bytes;
  if ((source.flags & 0x01U) != 0) {
    result.mcs_index = source.mcs_index;
    result.mcs_table = source.mcs_table;
  }
  result.transform_precoding = (source.flags & 0x02U) != 0;
  if (result.transform_precoding) {
    result.n_rs_id = source.n_rs_id;
  }
  result.cyclic_prefix               = source.cyclic_prefix;
  result.start_symbol                = source.start_symbol;
  result.nof_symbols                 = source.nof_symbols;
  result.dmrs_symbol_mask            = source.dmrs_symbol_mask;
  result.dmrs_type                   = source.dmrs_type;
  result.nof_cdm_groups_without_data = source.nof_cdm_groups_without_data;
  result.dmrs_scrambling_id          = source.dmrs_scrambling_id;
  result.n_scid                      = (source.flags & 0x04U) != 0;
  result.bwp_start_rb                = source.bwp_start_rb;
  result.bwp_size_rb                 = source.bwp_size_rb;
  std::copy(std::begin(source.prb_mask), std::end(source.prb_mask), result.prb_mask.begin());
  result.nof_rx_ports = source.nof_rx_ports;
  result.nof_layers   = source.nof_layers;
  if ((source.flags & 0x08U) != 0) {
    result.dc_position = source.dc_position;
  }
  result.nof_harq_ack_bits     = source.nof_harq_ack_bits;
  result.nof_csi_part1_bits    = source.nof_csi_part1_bits;
  result.csi_part2_expected    = (source.flags & 0x10U) != 0;
  result.alpha_scaling         = source.alpha_scaling;
  result.beta_offset_harq_ack  = source.beta_offset_harq_ack;
  result.beta_offset_csi_part1 = source.beta_offset_csi_part1;
  result.beta_offset_csi_part2 = source.beta_offset_csi_part2;
  return result;
}

void set_grant(PuschGrantParameters_t& destination, const pusch_grant_parameters& source)
{
  set_u64(destination.harqId, read_ranged(source.harq_id, 0, 15, "HARQ id"));
  set_u64(destination.nId, read_ranged(source.n_id, 0, 1023, "scrambling identity"));
  set_u64(destination.tbsBytes, read_ranged(source.tbs_bytes, 0, 16777215, "TBS"));
  set_u64(destination.rvIndex, read_ranged(source.rv, 0, 3, "RV"));
  destination.newData = source.new_data ? 1 : 0;
  set_u64(destination.ldpcBaseGraph, read_ranged(source.ldpc_base_graph, 1, 2, "LDPC base graph"));
  set_u64(destination.tbsLbrmBytes, read_ranged(source.tbs_lbrm_bytes, 0, 16777215, "LBRM TBS"));
  set_optional_long(destination.mcsIndex, source.mcs_index);
  set_optional_long(destination.mcsTable, source.mcs_table);
  destination.transformPrecoding = source.transform_precoding ? 1 : 0;
  set_optional_long(destination.nRsId, source.n_rs_id);
  set_u64(destination.cyclicPrefix, read_ranged(source.cyclic_prefix, 0, 1, "cyclic prefix"));
  set_u64(destination.startSymbol, read_ranged(source.start_symbol, 0, 13, "start symbol"));
  set_u64(destination.nofSymbols, read_ranged(source.nof_symbols, 1, 14, "symbol count"));
  set_u64(destination.dmrsSymbolMask, read_ranged(source.dmrs_symbol_mask, 0, 16383, "DM-RS symbol mask"));
  set_u64(destination.dmrsType, read_ranged(source.dmrs_type, 1, 2, "DM-RS type"));
  set_u64(destination.nofCdmGroupsWithoutData,
          read_ranged(source.nof_cdm_groups_without_data, 1, 3, "CDM groups without data"));
  set_u64(destination.dmrsScramblingId, source.dmrs_scrambling_id);
  destination.nScid = source.n_scid ? 1 : 0;
  set_u64(destination.bwpStartRb, read_ranged(source.bwp_start_rb, 0, 274, "BWP start"));
  set_u64(destination.bwpSizeRb, read_ranged(source.bwp_size_rb, 1, 275, "BWP size"));
  set_octets(destination.prbMask, source.prb_mask.data(), source.prb_mask.size());
  set_u64(destination.nofRxPorts, read_ranged(source.nof_rx_ports, 0, 8, "receive port count"));
  set_u64(destination.nofLayers, read_ranged(source.nof_layers, 0, 4, "layer count"));
  set_optional_long(destination.dcPosition, source.dc_position);
  set_u64(destination.nofHarqAckBits, read_ranged(source.nof_harq_ack_bits, 0, 1706, "HARQ-ACK bits"));
  set_u64(destination.nofCsiPart1Bits, read_ranged(source.nof_csi_part1_bits, 0, 1706, "CSI part 1 bits"));
  destination.csiPart2Expected   = source.csi_part2_expected ? 1 : 0;
  destination.alphaScaling       = source.alpha_scaling;
  destination.betaOffsetHarqAck  = source.beta_offset_harq_ack;
  destination.betaOffsetCsiPart1 = source.beta_offset_csi_part1;
  destination.betaOffsetCsiPart2 = source.beta_offset_csi_part2;
}

pusch_grant_parameters read_grant(const PuschGrantParameters_t& source)
{
  pusch_grant_parameters result;
  result.harq_id         = static_cast<uint8_t>(read_ranged(source.harqId, 0, 15, "HARQ id"));
  result.n_id            = static_cast<uint16_t>(read_ranged(source.nId, 0, 1023, "scrambling identity"));
  result.tbs_bytes       = static_cast<uint32_t>(read_ranged(source.tbsBytes, 0, 16777215, "TBS"));
  result.rv              = static_cast<uint8_t>(read_ranged(source.rvIndex, 0, 3, "RV"));
  result.new_data        = source.newData != 0;
  result.ldpc_base_graph = static_cast<uint8_t>(read_ranged(source.ldpcBaseGraph, 1, 2, "LDPC base graph"));
  result.tbs_lbrm_bytes  = static_cast<uint32_t>(read_ranged(source.tbsLbrmBytes, 0, 16777215, "LBRM TBS"));
  if (source.mcsIndex != nullptr) {
    result.mcs_index = static_cast<uint8_t>(read_ranged(*source.mcsIndex, 0, 31, "MCS index"));
  }
  if (source.mcsTable != nullptr) {
    result.mcs_table = static_cast<uint8_t>(read_ranged(*source.mcsTable, 0, 2, "MCS table"));
  }
  result.transform_precoding = source.transformPrecoding != 0;
  if (source.nRsId != nullptr) {
    result.n_rs_id = static_cast<uint16_t>(read_ranged(*source.nRsId, 0, 1023, "DM-RS identity"));
  }
  result.cyclic_prefix    = static_cast<uint8_t>(read_ranged(source.cyclicPrefix, 0, 1, "cyclic prefix"));
  result.start_symbol     = static_cast<uint8_t>(read_ranged(source.startSymbol, 0, 13, "start symbol"));
  result.nof_symbols      = static_cast<uint8_t>(read_ranged(source.nofSymbols, 1, 14, "symbol count"));
  result.dmrs_symbol_mask = static_cast<uint16_t>(read_ranged(source.dmrsSymbolMask, 0, 16383, "DM-RS symbol mask"));
  result.dmrs_type        = static_cast<uint8_t>(read_ranged(source.dmrsType, 1, 2, "DM-RS type"));
  result.nof_cdm_groups_without_data =
      static_cast<uint8_t>(read_ranged(source.nofCdmGroupsWithoutData, 1, 3, "CDM groups without data"));
  result.dmrs_scrambling_id = static_cast<uint16_t>(read_ranged(source.dmrsScramblingId, 0, 65535, "DM-RS scrambling"));
  result.n_scid             = source.nScid != 0;
  result.bwp_start_rb       = static_cast<uint16_t>(read_ranged(source.bwpStartRb, 0, 274, "BWP start"));
  result.bwp_size_rb        = static_cast<uint16_t>(read_ranged(source.bwpSizeRb, 1, 275, "BWP size"));
  const auto mask           = read_bytes(source.prbMask, result.prb_mask.size());
  if (mask.size() != result.prb_mask.size()) {
    throw std::runtime_error("PRB mask must hold 35 octets");
  }
  std::copy(mask.begin(), mask.end(), result.prb_mask.begin());
  result.nof_rx_ports = static_cast<uint8_t>(read_ranged(source.nofRxPorts, 0, 8, "receive port count"));
  result.nof_layers   = static_cast<uint8_t>(read_ranged(source.nofLayers, 0, 4, "layer count"));
  if (source.dcPosition != nullptr) {
    result.dc_position = static_cast<uint16_t>(read_ranged(*source.dcPosition, 0, 3299, "DC position"));
  }
  result.nof_harq_ack_bits     = static_cast<uint16_t>(read_ranged(source.nofHarqAckBits, 0, 1706, "HARQ-ACK bits"));
  result.nof_csi_part1_bits    = static_cast<uint16_t>(read_ranged(source.nofCsiPart1Bits, 0, 1706, "CSI part 1 bits"));
  result.csi_part2_expected    = source.csiPart2Expected != 0;
  result.alpha_scaling         = source.alphaScaling;
  result.beta_offset_harq_ack  = source.betaOffsetHarqAck;
  result.beta_offset_csi_part1 = source.betaOffsetCsiPart1;
  result.beta_offset_csi_part2 = source.betaOffsetCsiPart2;
  return result;
}

void set_object(Telemetry_Object_t& destination, const pusch_symbols_record& source)
{
  auto& record        = *(destination.choice.puschSymbols = make_asn<PuschSymbolsRecord_t>());
  destination.present = Telemetry_Object_PR_puschSymbols;
  set_u64(record.rnti, source.rnti);
  set_radio_slot(record.radioSlot, source.radio_slot);
  set_u64(record.nofPrb, read_ranged(source.nof_prb, 0, 275, "PRB count"));
  set_u64(record.nofDataSymbols, read_ranged(source.nof_data_symbols, 0, 14, "symbol count"));
  set_u64(record.nofLayers, read_ranged(source.nof_layers, 0, 4, "layer count"));
  set_u64(record.modulationBits, read_ranged(source.modulation_bits, 0, 8, "modulation bits"));
  set_optional_real(record.sinrDb, source.sinr_db);
  set_optional_real(record.epreDb, source.epre_db);
  set_optional_real(record.rsrpDb, source.rsrp_db);
  set_optional_real(record.evm, source.evm);
  set_optional_real(record.timeAlignmentUs, source.time_alignment_us);
  set_optional_real(record.cfoHz, source.cfo_hz);
  set_optional_real(record.targetCodeRate, source.target_code_rate);
  set_samples(record.equalizedSymbols, source.equalized_symbols);
  if (source.grant) {
    record.grant = make_asn<PuschGrantParameters_t>();
    set_grant(*record.grant, *source.grant);
  }
  if (source.crc_ok) {
    record.crcOk  = make_asn<BOOLEAN_t>();
    *record.crcOk = *source.crc_ok ? 1 : 0;
  }
  if (source.symbols_truncated) {
    record.symbolsTruncated  = make_asn<BOOLEAN_t>();
    *record.symbolsTruncated = 1;
  }
}

pusch_symbols_record read_object(const PuschSymbolsRecord_t& source)
{
  pusch_symbols_record result;
  result.rnti              = read_u16(source.rnti, "RNTI");
  result.radio_slot        = read_radio_slot(source.radioSlot);
  result.nof_prb           = static_cast<uint16_t>(read_ranged(source.nofPrb, 0, 275, "PRB count"));
  result.nof_data_symbols  = static_cast<uint8_t>(read_ranged(source.nofDataSymbols, 0, 14, "symbol count"));
  result.nof_layers        = static_cast<uint8_t>(read_ranged(source.nofLayers, 0, 4, "layer count"));
  result.modulation_bits   = static_cast<uint8_t>(read_ranged(source.modulationBits, 0, 8, "modulation bits"));
  result.sinr_db           = read_optional_real(source.sinrDb);
  result.epre_db           = read_optional_real(source.epreDb);
  result.rsrp_db           = read_optional_real(source.rsrpDb);
  result.evm               = read_optional_real(source.evm);
  result.time_alignment_us = read_optional_real(source.timeAlignmentUs);
  result.cfo_hz            = read_optional_real(source.cfoHz);
  result.target_code_rate  = read_optional_real(source.targetCodeRate);
  result.equalized_symbols = read_samples(source.equalizedSymbols);
  if (source.grant != nullptr) {
    result.grant = read_grant(*source.grant);
  }
  if (source.crcOk != nullptr) {
    result.crc_ok = *source.crcOk != 0;
  }
  result.symbols_truncated = source.symbolsTruncated != nullptr && *source.symbolsTruncated != 0;
  return result;
}

void set_object(Telemetry_Object_t& destination, const pusch_grid_record& source)
{
  auto& record        = *(destination.choice.puschGrid = make_asn<PuschGridRecord_t>());
  destination.present = Telemetry_Object_PR_puschGrid;
  set_u64(record.rnti, source.rnti);
  set_radio_slot(record.radioSlot, source.radio_slot);
  set_u64(record.nofPrb, read_ranged(source.nof_prb, 0, 275, "PRB count"));
  set_u64(record.nofRxPorts, read_ranged(source.nof_rx_ports, 0, 8, "receive port count"));
  set_u64(record.startSymbol, read_ranged(source.start_symbol, 0, 13, "start symbol"));
  set_u64(record.nofSymbols, read_ranged(source.nof_symbols, 1, 14, "symbol count"));
  record.grant = make_asn<PuschGrantParameters_t>();
  set_grant(*record.grant, source.grant);
  set_samples(record.resourceElements, source.resource_elements);
}

pusch_grid_record read_object(const PuschGridRecord_t& source)
{
  pusch_grid_record result;
  result.rnti         = read_u16(source.rnti, "RNTI");
  result.radio_slot   = read_radio_slot(source.radioSlot);
  result.nof_prb      = static_cast<uint16_t>(read_ranged(source.nofPrb, 0, 275, "PRB count"));
  result.nof_rx_ports = static_cast<uint8_t>(read_ranged(source.nofRxPorts, 0, 8, "receive port count"));
  result.start_symbol = static_cast<uint8_t>(read_ranged(source.startSymbol, 0, 13, "start symbol"));
  result.nof_symbols  = static_cast<uint8_t>(read_ranged(source.nofSymbols, 1, 14, "symbol count"));
  if (source.grant == nullptr) {
    throw std::runtime_error("PUSCH grid record is missing its grant");
  }
  result.grant             = read_grant(*source.grant);
  result.resource_elements = read_samples(source.resourceElements);
  return result;
}

void set_object(Telemetry_Object_t& destination, const pusch_crc_record& source)
{
  auto& record        = *(destination.choice.puschCrc = make_asn<PuschCrcRecord_t>());
  destination.present = Telemetry_Object_PR_puschCrc;
  set_u64(record.rnti, source.rnti);
  set_radio_slot(record.radioSlot, source.radio_slot);
  record.crcOk = source.crc_ok ? 1 : 0;
}

pusch_crc_record read_object(const PuschCrcRecord_t& source)
{
  if (source.crcOk != 0 && source.crcOk != 1) {
    throw std::runtime_error("invalid CRC flag");
  }
  pusch_crc_record result;
  result.rnti       = read_u16(source.rnti, "RNTI");
  result.radio_slot = read_radio_slot(source.radioSlot);
  result.crc_ok     = source.crcOk != 0;
  return result;
}

void set_object(Telemetry_Object_t& destination, const srs_channel_record& source)
{
  auto& record        = *(destination.choice.srsChannel = make_asn<SrsChannelRecord_t>());
  destination.present = Telemetry_Object_PR_srsChannel;
  set_u64(record.rnti, source.rnti);
  set_radio_slot(record.radioSlot, source.radio_slot);
  set_u64(record.nofSubcarriers, source.nof_subcarriers);
  set_u64(record.combSize, source.comb_size);
  set_u64(record.nofRxPorts, source.nof_rx_ports);
  set_u64(record.nofTxPorts, source.nof_tx_ports);
  set_u64(record.rxPortIndex, source.rx_port_index);
  set_u64(record.txPortIndex, source.tx_port_index);
  set_u64(record.startSymbol, read_ranged(source.start_symbol, 0, 13, "start symbol"));
  set_u64(record.nofSymbols, read_ranged(source.nof_symbols, 0, 14, "symbol count"));
  set_optional_real(record.rsrpDb, source.rsrp_db);
  set_optional_real(record.epreDb, source.epre_db);
  set_optional_real(record.noiseVariance, source.noise_variance);
  set_optional_real(record.timeAlignmentNs, source.time_alignment_ns);
  set_optional_small(record.mappingInitialSubcarrier, source.mapping_initial_subcarrier, 65535, "subcarrier");
  set_optional_small(record.combOffset, source.comb_offset, 255, "comb offset");
  set_optional_small(record.cyclicShift, source.cyclic_shift, 255, "cyclic shift");
  set_optional_small(record.sequenceId, source.sequence_id, 65535, "sequence id");
  set_optional_small(record.sequenceGroup, source.sequence_group, 65535, "sequence group");
  set_optional_small(record.sequenceNumber, source.sequence_number, 65535, "sequence number");
  set_optional_small(record.cyclicShiftIndex, source.cyclic_shift_index, 65535, "cyclic shift index");
  set_optional_small(record.cyclicShiftMaximum, source.cyclic_shift_maximum, 65535, "cyclic shift maximum");
  set_optional_small(record.bandwidthIndex, source.bandwidth_index, 255, "bandwidth index");
  set_optional_small(record.frequencyPosition, source.frequency_position, 255, "frequency position");
  set_optional_small(record.frequencyShift, source.frequency_shift, 65535, "frequency shift");
  set_optional_small(record.frequencyHopping, source.frequency_hopping, 255, "frequency hopping");
  set_samples(record.coefficients, source.coefficients);
  if (!source.received_pilots.empty()) {
    record.receivedPilots = make_asn<Cf32Samples_t>();
    set_samples(*record.receivedPilots, source.received_pilots);
  }
  if (!source.reference_symbols.empty()) {
    record.referenceSymbols = make_asn<Cf32Samples_t>();
    set_samples(*record.referenceSymbols, source.reference_symbols);
  }
}

srs_channel_record read_object(const SrsChannelRecord_t& source)
{
  srs_channel_record result;
  result.rnti              = read_u16(source.rnti, "RNTI");
  result.radio_slot        = read_radio_slot(source.radioSlot);
  result.nof_subcarriers   = read_u16(source.nofSubcarriers, "subcarrier count");
  result.comb_size         = static_cast<uint8_t>(read_ranged(source.combSize, 0, 255, "comb size"));
  result.nof_rx_ports      = static_cast<uint8_t>(read_ranged(source.nofRxPorts, 0, 255, "rx ports"));
  result.nof_tx_ports      = static_cast<uint8_t>(read_ranged(source.nofTxPorts, 0, 255, "tx ports"));
  result.rx_port_index     = static_cast<uint8_t>(read_ranged(source.rxPortIndex, 0, 255, "rx port"));
  result.tx_port_index     = static_cast<uint8_t>(read_ranged(source.txPortIndex, 0, 255, "tx port"));
  result.start_symbol      = static_cast<uint8_t>(read_ranged(source.startSymbol, 0, 13, "start symbol"));
  result.nof_symbols       = static_cast<uint8_t>(read_ranged(source.nofSymbols, 0, 14, "symbol count"));
  result.rsrp_db           = read_optional_real(source.rsrpDb);
  result.epre_db           = read_optional_real(source.epreDb);
  result.noise_variance    = read_optional_real(source.noiseVariance);
  result.time_alignment_ns = read_optional_real(source.timeAlignmentNs);
  result.mapping_initial_subcarrier =
      read_optional_small<uint16_t>(source.mappingInitialSubcarrier, 65535, "subcarrier");
  result.comb_offset          = read_optional_small<uint8_t>(source.combOffset, 255, "comb offset");
  result.cyclic_shift         = read_optional_small<uint8_t>(source.cyclicShift, 255, "cyclic shift");
  result.sequence_id          = read_optional_small<uint16_t>(source.sequenceId, 65535, "sequence id");
  result.sequence_group       = read_optional_small<uint16_t>(source.sequenceGroup, 65535, "sequence group");
  result.sequence_number      = read_optional_small<uint16_t>(source.sequenceNumber, 65535, "sequence number");
  result.cyclic_shift_index   = read_optional_small<uint16_t>(source.cyclicShiftIndex, 65535, "cyclic shift index");
  result.cyclic_shift_maximum = read_optional_small<uint16_t>(source.cyclicShiftMaximum, 65535, "cyclic shift max");
  result.bandwidth_index      = read_optional_small<uint8_t>(source.bandwidthIndex, 255, "bandwidth index");
  result.frequency_position   = read_optional_small<uint8_t>(source.frequencyPosition, 255, "frequency position");
  result.frequency_shift      = read_optional_small<uint16_t>(source.frequencyShift, 65535, "frequency shift");
  result.frequency_hopping    = read_optional_small<uint8_t>(source.frequencyHopping, 255, "frequency hopping");
  result.coefficients         = read_samples(source.coefficients);
  if (source.receivedPilots != nullptr) {
    result.received_pilots = read_samples(*source.receivedPilots);
  }
  if (source.referenceSymbols != nullptr) {
    result.reference_symbols = read_samples(*source.referenceSymbols);
  }
  return result;
}

void set_object(Telemetry_Object_t& destination, const pusch_dmrs_channel_record& source)
{
  auto& record        = *(destination.choice.puschDmrsChannel = make_asn<PuschDmrsChannelRecord_t>());
  destination.present = Telemetry_Object_PR_puschDmrsChannel;
  set_u64(record.rnti, source.rnti);
  set_radio_slot(record.radioSlot, source.radio_slot);
  set_u64(record.startPrb, read_ranged(source.start_prb, 0, 274, "start PRB"));
  set_u64(record.nofPrb, read_ranged(source.nof_prb, 0, 275, "PRB count"));
  set_u64(record.nofSubcarriers, source.nof_subcarriers);
  set_u64(record.dmrsSymbolIndex, read_ranged(source.dmrs_symbol_index, 0, 13, "DM-RS symbol"));
  set_u64(record.startSymbol, read_ranged(source.start_symbol, 0, 13, "start symbol"));
  set_u64(record.nofSymbols, read_ranged(source.nof_symbols, 0, 14, "symbol count"));
  set_u64(record.nofRxPorts, source.nof_rx_ports);
  set_u64(record.nofTxLayers, source.nof_tx_layers);
  set_u64(record.rxPortIndex, source.rx_port_index);
  set_u64(record.txLayerIndex, source.tx_layer_index);
  set_u64(record.dmrsType, read_ranged(source.dmrs_type, 1, 2, "DM-RS type"));
  set_u64(record.nofCdmGroupsWithoutData, read_ranged(source.nof_cdm_groups_without_data, 0, 3, "CDM groups"));
  record.transformPrecoding = source.transform_precoding ? 1 : 0;
  set_u64(record.scramblingId, source.scrambling_id);
  record.nScid = source.n_scid ? 1 : 0;
  set_optional_real(record.sinrDb, source.sinr_db);
  set_optional_real(record.epreDb, source.epre_db);
  set_optional_real(record.rsrpDb, source.rsrp_db);
  set_optional_real(record.noiseVariance, source.noise_variance);
  set_optional_real(record.timeAlignmentUs, source.time_alignment_us);
  set_optional_real(record.cfoHz, source.cfo_hz);
  set_optional_real(record.targetCodeRate, source.target_code_rate);
  set_samples(record.estimates, source.estimates);
}

pusch_dmrs_channel_record read_object(const PuschDmrsChannelRecord_t& source)
{
  if ((source.transformPrecoding != 0 && source.transformPrecoding != 1) || (source.nScid != 0 && source.nScid != 1)) {
    throw std::runtime_error("invalid DM-RS flags");
  }
  pusch_dmrs_channel_record result;
  result.rnti                        = read_u16(source.rnti, "RNTI");
  result.radio_slot                  = read_radio_slot(source.radioSlot);
  result.start_prb                   = static_cast<uint16_t>(read_ranged(source.startPrb, 0, 274, "start PRB"));
  result.nof_prb                     = static_cast<uint16_t>(read_ranged(source.nofPrb, 0, 275, "PRB count"));
  result.nof_subcarriers             = read_u16(source.nofSubcarriers, "subcarrier count");
  result.dmrs_symbol_index           = static_cast<uint8_t>(read_ranged(source.dmrsSymbolIndex, 0, 13, "DM-RS symbol"));
  result.start_symbol                = static_cast<uint8_t>(read_ranged(source.startSymbol, 0, 13, "start symbol"));
  result.nof_symbols                 = static_cast<uint8_t>(read_ranged(source.nofSymbols, 0, 14, "symbol count"));
  result.nof_rx_ports                = static_cast<uint8_t>(read_ranged(source.nofRxPorts, 0, 255, "rx ports"));
  result.nof_tx_layers               = static_cast<uint8_t>(read_ranged(source.nofTxLayers, 0, 255, "layers"));
  result.rx_port_index               = static_cast<uint8_t>(read_ranged(source.rxPortIndex, 0, 255, "rx port"));
  result.tx_layer_index              = static_cast<uint8_t>(read_ranged(source.txLayerIndex, 0, 255, "layer"));
  result.dmrs_type                   = static_cast<uint8_t>(read_ranged(source.dmrsType, 1, 2, "DM-RS type"));
  result.nof_cdm_groups_without_data = static_cast<uint8_t>(read_ranged(source.nofCdmGroupsWithoutData, 0, 3, "CDM"));
  result.transform_precoding         = source.transformPrecoding != 0;
  result.scrambling_id               = read_u16(source.scramblingId, "scrambling id");
  result.n_scid                      = source.nScid != 0;
  result.sinr_db                     = read_optional_real(source.sinrDb);
  result.epre_db                     = read_optional_real(source.epreDb);
  result.rsrp_db                     = read_optional_real(source.rsrpDb);
  result.noise_variance              = read_optional_real(source.noiseVariance);
  result.time_alignment_us           = read_optional_real(source.timeAlignmentUs);
  result.cfo_hz                      = read_optional_real(source.cfoHz);
  result.target_code_rate            = read_optional_real(source.targetCodeRate);
  result.estimates                   = read_samples(source.estimates);
  return result;
}

void set_object(Telemetry_Object_t& destination, const srs_config_record& source)
{
  auto& record        = *(destination.choice.srsConfig = make_asn<SrsConfigRecord_t>());
  destination.present = Telemetry_Object_PR_srsConfig;
  set_u64(record.rnti, source.rnti);
  set_u64(record.event, read_ranged(source.event, 1, 5, "SRS config event"));
  set_u64(record.configEpoch, source.config_epoch);
  set_u64(record.ueIndex, source.ue_index);
  set_u64(record.numerology, read_ranged(source.numerology, 0, 4, "numerology"));
  set_u64(record.nTaOffset, source.n_ta_offset);
  if (!source.resource) {
    return;
  }
  const srs_resource_parameters& from = *source.resource;
  auto&                          to   = *(record.resource = make_asn<SrsResourceParameters_t>());
  set_u64(to.resourceId, read_ranged(from.resource_id, 0, 63, "SRS resource id"));
  set_u64(to.nofPorts, read_ranged(from.nof_ports, 1, 4, "SRS port count"));
  set_u64(to.combSize, read_ranged(from.comb_size, 2, 8, "comb size"));
  set_u64(to.combOffset, read_ranged(from.comb_offset, 0, 7, "comb offset"));
  set_u64(to.cyclicShift, read_ranged(from.cyclic_shift, 0, 11, "cyclic shift"));
  set_u64(to.startPosition, read_ranged(from.start_position, 0, 13, "start position"));
  set_u64(to.nofSymbols, read_ranged(from.nof_symbols, 1, 14, "SRS symbol count"));
  set_u64(to.repetitionFactor, read_ranged(from.repetition_factor, 1, 14, "repetition factor"));
  set_u64(to.freqDomainPosition, read_ranged(from.freq_domain_position, 0, 67, "frequency domain position"));
  set_u64(to.freqDomainShift, read_ranged(from.freq_domain_shift, 0, 268, "frequency domain shift"));
  set_u64(to.cSrs, read_ranged(from.c_srs, 0, 63, "C_SRS"));
  set_u64(to.bSrs, read_ranged(from.b_srs, 0, 3, "B_SRS"));
  set_u64(to.bHop, read_ranged(from.b_hop, 0, 3, "b_hop"));
  set_u64(to.groupOrSequenceHopping, read_ranged(from.group_or_sequence_hopping, 0, 2, "hopping"));
  set_u64(to.sequenceId, read_ranged(from.sequence_id, 0, 1023, "SRS sequence id"));
  set_u64(to.periodSlots, read_ranged(from.period_slots, 1, 2560, "SRS period"));
  set_u64(to.offsetSlots, read_ranged(from.offset_slots, 0, 2559, "SRS offset"));
  set_u64(to.bwpStartRb, read_ranged(from.bwp_start_rb, 0, 274, "BWP start"));
  set_u64(to.bwpSizeRb, read_ranged(from.bwp_size_rb, 1, 275, "BWP size"));
  if (from.derived) {
    const srs_derived_mapping& mapping = *from.derived;
    auto&                      derived = *(to.derived = make_asn<SrsDerivedMapping_t>());
    set_u64(derived.startSymbol, read_ranged(mapping.start_symbol, 0, 13, "start symbol"));
    set_u64(derived.sequenceLength, mapping.sequence_length);
    set_u64(derived.mappingInitialSubcarrier, mapping.mapping_initial_subcarrier);
    set_u64(derived.sequenceGroup, read_ranged(mapping.sequence_group, 0, 29, "sequence group"));
    set_u64(derived.sequenceNumber, read_ranged(mapping.sequence_number, 0, 1, "sequence number"));
    set_u64(derived.cyclicShiftMaximum, read_ranged(mapping.cyclic_shift_maximum, 0, 12, "cyclic shift maximum"));
  }
}

srs_config_record read_object(const SrsConfigRecord_t& source)
{
  srs_config_record result;
  result.rnti         = read_u16(source.rnti, "RNTI");
  result.event        = static_cast<uint8_t>(read_ranged(source.event, 1, 5, "SRS config event"));
  result.config_epoch = static_cast<uint32_t>(read_ranged(source.configEpoch, 0, 4294967295ULL, "config epoch"));
  result.ue_index     = read_u16(source.ueIndex, "UE index");
  result.numerology   = static_cast<uint8_t>(read_ranged(source.numerology, 0, 4, "numerology"));
  result.n_ta_offset  = read_u16(source.nTaOffset, "N_TA offset");
  if (source.resource == nullptr) {
    return result;
  }
  const auto&              from = *source.resource;
  srs_resource_parameters& to   = result.resource.emplace();
  to.resource_id                = static_cast<uint8_t>(read_ranged(from.resourceId, 0, 63, "SRS resource id"));
  to.nof_ports                  = static_cast<uint8_t>(read_ranged(from.nofPorts, 1, 4, "SRS port count"));
  to.comb_size                  = static_cast<uint8_t>(read_ranged(from.combSize, 2, 8, "comb size"));
  to.comb_offset                = static_cast<uint8_t>(read_ranged(from.combOffset, 0, 7, "comb offset"));
  to.cyclic_shift               = static_cast<uint8_t>(read_ranged(from.cyclicShift, 0, 11, "cyclic shift"));
  to.start_position             = static_cast<uint8_t>(read_ranged(from.startPosition, 0, 13, "start position"));
  to.nof_symbols                = static_cast<uint8_t>(read_ranged(from.nofSymbols, 1, 14, "SRS symbol count"));
  to.repetition_factor = static_cast<uint8_t>(read_ranged(from.repetitionFactor, 1, 14, "repetition factor"));
  to.freq_domain_position =
      static_cast<uint8_t>(read_ranged(from.freqDomainPosition, 0, 67, "frequency domain position"));
  to.freq_domain_shift = static_cast<uint16_t>(read_ranged(from.freqDomainShift, 0, 268, "frequency domain shift"));
  to.c_srs             = static_cast<uint8_t>(read_ranged(from.cSrs, 0, 63, "C_SRS"));
  to.b_srs             = static_cast<uint8_t>(read_ranged(from.bSrs, 0, 3, "B_SRS"));
  to.b_hop             = static_cast<uint8_t>(read_ranged(from.bHop, 0, 3, "b_hop"));
  to.group_or_sequence_hopping = static_cast<uint8_t>(read_ranged(from.groupOrSequenceHopping, 0, 2, "hopping"));
  to.sequence_id               = static_cast<uint16_t>(read_ranged(from.sequenceId, 0, 1023, "SRS sequence id"));
  to.period_slots              = static_cast<uint16_t>(read_ranged(from.periodSlots, 1, 2560, "SRS period"));
  to.offset_slots              = static_cast<uint16_t>(read_ranged(from.offsetSlots, 0, 2559, "SRS offset"));
  to.bwp_start_rb              = static_cast<uint16_t>(read_ranged(from.bwpStartRb, 0, 274, "BWP start"));
  to.bwp_size_rb               = static_cast<uint16_t>(read_ranged(from.bwpSizeRb, 1, 275, "BWP size"));
  if (from.derived != nullptr) {
    const auto&          mapping = *from.derived;
    srs_derived_mapping& derived = to.derived.emplace();
    derived.start_symbol         = static_cast<uint8_t>(read_ranged(mapping.startSymbol, 0, 13, "start symbol"));
    derived.sequence_length      = read_u16(mapping.sequenceLength, "sequence length");
    derived.mapping_initial_subcarrier = read_u16(mapping.mappingInitialSubcarrier, "subcarrier");
    derived.sequence_group = static_cast<uint8_t>(read_ranged(mapping.sequenceGroup, 0, 29, "sequence group"));
    derived.sequence_number = static_cast<uint8_t>(read_ranged(mapping.sequenceNumber, 0, 1, "sequence number"));
    derived.cyclic_shift_maximum =
        static_cast<uint8_t>(read_ranged(mapping.cyclicShiftMaximum, 0, 12, "cyclic shift maximum"));
  }
  return result;
}

void set_object(Telemetry_Object_t& destination, const ta_command_record& source)
{
  auto& record        = *(destination.choice.taCommand = make_asn<TaCommandRecord_t>());
  destination.present = Telemetry_Object_PR_taCommand;
  set_u64(record.rnti, source.rnti);
  set_radio_slot(record.radioSlot, source.radio_slot);
  set_u64(record.source, read_ranged(source.source, 1, 2, "TA command source"));
  set_u64(record.tagId, read_ranged(source.tag_id, 0, 3, "TAG id"));
  set_u64(record.taCommand, read_ranged(source.ta_command, 0, 3846, "TA command"));
}

ta_command_record read_object(const TaCommandRecord_t& source)
{
  ta_command_record result;
  result.rnti       = read_u16(source.rnti, "RNTI");
  result.radio_slot = read_radio_slot(source.radioSlot);
  result.source     = static_cast<uint8_t>(read_ranged(source.source, 1, 2, "TA command source"));
  result.tag_id     = static_cast<uint8_t>(read_ranged(source.tagId, 0, 3, "TAG id"));
  result.ta_command = static_cast<uint16_t>(read_ranged(source.taCommand, 0, 3846, "TA command"));
  return result;
}

void set_object(Telemetry_Object_t& destination, const gnb_metrics_json_record& source)
{
  auto& record        = *(destination.choice.gnbMetricsJson = make_asn<GnbMetricsJsonRecord_t>());
  destination.present = Telemetry_Object_PR_gnbMetricsJson;
  if (source.json.size() > e3ap_maximum_payload_bytes || !valid_utf8(source.json.data(), source.json.size())) {
    throw std::runtime_error("gNB metrics JSON must be nonempty UTF-8 within the bound");
  }
  set_octets(record.json, source.json);
}

gnb_metrics_json_record read_object(const GnbMetricsJsonRecord_t& source)
{
  gnb_metrics_json_record result;
  result.json = read_bytes(source.json, e3ap_maximum_payload_bytes);
  if (!result.json.empty() && !valid_utf8(result.json.data(), result.json.size())) {
    throw std::runtime_error("gNB metrics JSON must be nonempty UTF-8");
  }
  return result;
}

void set_object(Telemetry_Object_t& destination, const class_c_portable_record& source)
{
  auto& record        = *(destination.choice.classCPortable = make_asn<ClassCPortableRecord_t>());
  destination.present = Telemetry_Object_PR_classCPortable;
  set_u64(record.interfaceId, read_ranged(source.interface_id, 1, 4294967295ULL, "interface id"));
  // An empty tensor is legal: the bytes were delivered by reference.
  if (source.metadata.size() > 65536 || source.tensor.size() > e3ap_maximum_payload_bytes) {
    throw std::runtime_error("Class-C portable record exceeds its bounds");
  }
  set_octets(record.metadata, source.metadata);
  set_octets(record.tensor, source.tensor);
}

class_c_portable_record read_object(const ClassCPortableRecord_t& source)
{
  class_c_portable_record result;
  result.interface_id = static_cast<uint32_t>(read_ranged(source.interfaceId, 1, 4294967295ULL, "interface id"));
  result.metadata     = read_bytes(source.metadata, 65536);
  result.tensor       = read_bytes(source.tensor, e3ap_maximum_payload_bytes);
  return result;
}

void set_indication(Telemetry_IndicationData_t& destination, const telemetry_indication& source)
{
  set_u64(destination.telemetryId, read_ranged(source.telemetry_id, 1, 65535, "telemetry id"));
  set_u64(destination.cellId, source.cell_id);
  set_u64(destination.sequence, source.sequence);
  set_u64(destination.routeEpoch, source.route_epoch);
  set_u64(destination.monotonicNs, source.monotonic_ns);
  set_u64(destination.droppedObjects, source.dropped_objects);
  destination.object = make_asn<Telemetry_Object_t>();
  std::visit([&](const auto& record) { set_object(*destination.object, record); }, source.object);
}

telemetry_indication read_indication(const Telemetry_IndicationData_t& source)
{
  telemetry_indication result;
  result.telemetry_id    = read_id16(source.telemetryId, "telemetry id");
  result.cell_id         = read_u16(source.cellId, "cell id");
  result.sequence        = read_u64(source.sequence);
  result.route_epoch     = read_u64(source.routeEpoch);
  result.monotonic_ns    = read_u64(source.monotonicNs);
  result.dropped_objects = read_u64(source.droppedObjects);
  if (source.object == nullptr) {
    throw std::runtime_error("telemetry object is missing");
  }
  const auto& object = *source.object;
  switch (object.present) {
    case Telemetry_Object_PR_puschSymbols:
      result.object = read_object(*object.choice.puschSymbols);
      break;
    case Telemetry_Object_PR_puschCrc:
      result.object = read_object(*object.choice.puschCrc);
      break;
    case Telemetry_Object_PR_srsChannel:
      result.object = read_object(*object.choice.srsChannel);
      break;
    case Telemetry_Object_PR_puschDmrsChannel:
      result.object = read_object(*object.choice.puschDmrsChannel);
      break;
    case Telemetry_Object_PR_gnbMetricsJson:
      result.object = read_object(*object.choice.gnbMetricsJson);
      break;
    case Telemetry_Object_PR_classCPortable:
      result.object = read_object(*object.choice.classCPortable);
      break;
    case Telemetry_Object_PR_puschGrid:
      result.object = read_object(*object.choice.puschGrid);
      break;
    case Telemetry_Object_PR_srsConfig:
      result.object = read_object(*object.choice.srsConfig);
      break;
    case Telemetry_Object_PR_taCommand:
      result.object = read_object(*object.choice.taCommand);
      break;
    default:
      throw std::runtime_error("unknown telemetry object");
  }
  return result;
}

void set_stream(Telemetry_StreamDescriptor_t& destination, const telemetry_stream_record& source)
{
  set_u64(destination.telemetryId, read_ranged(source.telemetry_id, 1, 65535, "telemetry id"));
  if (source.name.empty() || source.name.size() > 128 || source.description.size() > 128) {
    throw std::runtime_error("telemetry stream name/description exceed their bounds");
  }
  set_text(destination.name, source.name);
  set_text(destination.description, source.description);
  destination.classCPortable = source.class_c_portable ? 1 : 0;
  set_u64(destination.interfaceId, read_ranged(source.interface_id, 1, 4294967295ULL, "interface id"));
  set_optional_u64(destination.minimumIntervalUs, source.minimum_interval_us);
  set_optional_u64(destination.hardFloorIntervalUs, source.hard_floor_interval_us);
  if (!source.cell_ids.empty()) {
    if (source.cell_ids.size() > 32) {
      throw std::runtime_error("telemetry cell list exceeds its bound");
    }
    destination.cellIds = make_asn<Telemetry_StreamDescriptor::Telemetry_StreamDescriptor__cellIds>();
    for (const uint16_t cell : source.cell_ids) {
      long* item = make_asn<long>();
      *item      = cell;
      if (ASN_SEQUENCE_ADD(&destination.cellIds->list, item) != 0) {
        std::free(item);
        throw std::bad_alloc();
      }
    }
  }
}

telemetry_stream_record read_stream(const Telemetry_StreamDescriptor_t& source)
{
  telemetry_stream_record result;
  result.telemetry_id     = read_id16(source.telemetryId, "telemetry id");
  result.name             = read_string(source.name, 128);
  result.description      = read_string(source.description, 128);
  result.class_c_portable = source.classCPortable != 0;
  result.interface_id     = static_cast<uint32_t>(read_ranged(source.interfaceId, 1, 4294967295ULL, "interface id"));
  if (source.hardFloorIntervalUs != nullptr) {
    result.hard_floor_interval_us = read_u64(*source.hardFloorIntervalUs);
  }
  if (source.minimumIntervalUs != nullptr) {
    result.minimum_interval_us = read_u64(*source.minimumIntervalUs);
  }
  if (source.cellIds != nullptr) {
    if (source.cellIds->list.count < 0 || source.cellIds->list.count > 32) {
      throw std::runtime_error("telemetry cell list exceeds its bound");
    }
    for (int index = 0; index != source.cellIds->list.count; ++index) {
      if (source.cellIds->list.array[index] == nullptr) {
        throw std::runtime_error("telemetry cell entry is missing");
      }
      result.cell_ids.push_back(read_u16(*source.cellIds->list.array[index], "cell id"));
    }
  }
  return result;
}

void set_function(Telemetry_RanFunctionData_t& destination, const telemetry_ran_function_data& source)
{
  if (source.name.empty() || source.name.size() > 32 || source.description.empty() || source.description.size() > 128 ||
      source.streams.size() > maximum_telemetry_streams) {
    throw std::runtime_error("telemetry RAN function data exceeds its bounds");
  }
  set_text(destination.name, source.name);
  set_u64(destination.version, source.version);
  set_text(destination.description, source.description);
  for (const auto& stream : source.streams) {
    set_stream(append_asn<Telemetry_StreamDescriptor_t>(destination.streams), stream);
  }
  set_u64(destination.maximumObjectBytes,
          read_ranged(source.maximum_object_bytes, 1, e3ap_maximum_payload_bytes, "object bound"));
  set_u64(destination.maximumSubscriptions, read_ranged(source.maximum_subscriptions, 0, 1024, "subscription bound"));
}

telemetry_ran_function_data read_function(const Telemetry_RanFunctionData_t& source)
{
  telemetry_ran_function_data result;
  result.name        = read_string(source.name, 32);
  result.version     = read_u16(source.version, "version");
  result.description = read_string(source.description, 128);
  if (source.streams.list.count < 0 ||
      static_cast<std::size_t>(source.streams.list.count) > maximum_telemetry_streams) {
    throw std::runtime_error("telemetry stream list exceeds its bound");
  }
  result.streams.clear();
  for (int index = 0; index != source.streams.list.count; ++index) {
    if (source.streams.list.array[index] == nullptr) {
      throw std::runtime_error("telemetry stream entry is missing");
    }
    result.streams.push_back(read_stream(*source.streams.list.array[index]));
  }
  result.maximum_object_bytes =
      static_cast<uint32_t>(read_ranged(source.maximumObjectBytes, 1, e3ap_maximum_payload_bytes, "object bound"));
  result.maximum_subscriptions =
      static_cast<uint32_t>(read_ranged(source.maximumSubscriptions, 0, 1024, "subscription bound"));
  return result;
}

void set_config(Telemetry_SubscriptionConfig_t& destination, const telemetry_subscription_config& source)
{
  set_u64(destination.telemetryId, read_ranged(source.telemetry_id, 1, 65535, "telemetry id"));
  set_optional_typed(destination.cellId, source.cell_id);
  if (source.sample_every_n == 0) {
    throw std::runtime_error("sampleEveryN must be positive");
  }
  if (source.sample_every_n != 1) {
    destination.sampleEveryN  = make_asn<long>();
    *destination.sampleEveryN = source.sample_every_n;
  }
  if (source.drop_policy != telemetry_drop_policy::coalesce_latest) {
    destination.dropPolicy  = make_asn<Telemetry_DropPolicy_t>();
    *destination.dropPolicy = static_cast<long>(source.drop_policy);
  }
  set_optional_small(destination.minimumIntervalMs, source.minimum_interval_ms, 60000, "minimum interval");
  if (!source.rnti_filter.empty()) {
    if (source.rnti_filter.size() > maximum_rnti_filter) {
      throw std::runtime_error("RNTI filter exceeds its bound");
    }
    destination.rntiFilter = make_asn<Telemetry_SubscriptionConfig::Telemetry_SubscriptionConfig__rntiFilter>();
    for (const uint16_t rnti : source.rnti_filter) {
      Rnti_t* item = make_asn<Rnti_t>();
      *item        = rnti;
      if (ASN_SEQUENCE_ADD(&destination.rntiFilter->list, item) != 0) {
        std::free(item);
        throw std::bad_alloc();
      }
    }
  }
}

telemetry_subscription_config read_config(const Telemetry_SubscriptionConfig_t& source)
{
  telemetry_subscription_config result;
  result.telemetry_id = read_id16(source.telemetryId, "telemetry id");
  if (source.cellId != nullptr) {
    result.cell_id = read_u16(*source.cellId, "cell id");
  }
  result.sample_every_n = source.sampleEveryN != nullptr
                              ? static_cast<uint16_t>(read_ranged(*source.sampleEveryN, 1, 65535, "sampleEveryN"))
                              : 1;
  if (source.dropPolicy != nullptr) {
    if (*source.dropPolicy < Telemetry_DropPolicy_dropNewest ||
        *source.dropPolicy > Telemetry_DropPolicy_coalesceLatest) {
      throw std::runtime_error("invalid drop policy");
    }
    result.drop_policy = static_cast<telemetry_drop_policy>(*source.dropPolicy);
  }
  result.minimum_interval_ms = read_optional_small<uint32_t>(source.minimumIntervalMs, 60000, "minimum interval");
  if (source.rntiFilter != nullptr) {
    if (source.rntiFilter->list.count <= 0 ||
        static_cast<std::size_t>(source.rntiFilter->list.count) > maximum_rnti_filter) {
      throw std::runtime_error("RNTI filter exceeds its bound");
    }
    for (int index = 0; index != source.rntiFilter->list.count; ++index) {
      if (source.rntiFilter->list.array[index] == nullptr) {
        throw std::runtime_error("RNTI filter entry is missing");
      }
      result.rnti_filter.push_back(read_u16(*source.rntiFilter->list.array[index], "RNTI"));
    }
  }
  return result;
}

void set_management_function(Mgmt_RanFunctionData_t& destination, const management_ran_function_data& source)
{
  if (source.name.empty() || source.name.size() > 32 || source.description.empty() || source.description.size() > 128 ||
      source.schema_module.empty() || source.schema_module.size() > 64) {
    throw std::runtime_error("management RAN function data exceeds its bounds");
  }
  set_text(destination.name, source.name);
  set_u64(destination.version, source.version);
  set_text(destination.description, source.description);
  set_text(destination.schemaModule, source.schema_module);
  set_u64(destination.maximumBytes, read_ranged(source.maximum_bytes, 1, 2U * 1024U * 1024U, "management bound"));
}

management_ran_function_data read_management_function(const Mgmt_RanFunctionData_t& source)
{
  management_ran_function_data result;
  result.name          = read_string(source.name, 32);
  result.version       = read_u16(source.version, "version");
  result.description   = read_string(source.description, 128);
  result.schema_module = read_string(source.schemaModule, 64);
  result.maximum_bytes =
      static_cast<uint32_t>(read_ranged(source.maximumBytes, 1, 2U * 1024U * 1024U, "management bound"));
  return result;
}

template <typename AsnType, typename Fill>
codec_result<std::vector<uint8_t>>
encode_with(asn_TYPE_descriptor_t& descriptor, std::size_t bound, Fill&& fill) noexcept
{
  try {
    asn_owner<AsnType> value(descriptor, make_asn<AsnType>());
    fill(*value.get());
    return {encode_asn(descriptor, *value.get(), bound), {}};
  } catch (const std::bad_alloc&) {
    return {std::nullopt, "APER encoding allocation failed"};
  } catch (const std::exception& exception) {
    return {std::nullopt, exception.what()};
  } catch (...) {
    return {std::nullopt, "unknown APER encoding failure"};
  }
}

template <typename Result, typename AsnType, typename Read>
codec_result<Result> decode_with(asn_TYPE_descriptor_t&     descriptor,
                                 ocudu::span<const uint8_t> bytes,
                                 std::size_t                bound,
                                 const char*                what,
                                 Read&&                     read) noexcept
{
  try {
    auto owner = decode_asn<AsnType>(descriptor, bytes, bound, what);
    return {read(*owner.get()), {}};
  } catch (const std::bad_alloc&) {
    return {std::nullopt, "APER decoding allocation failed"};
  } catch (const std::exception& exception) {
    return {std::nullopt, exception.what()};
  } catch (...) {
    return {std::nullopt, "unknown APER decoding failure"};
  }
}

} // namespace

codec_result<std::vector<uint8_t>> e3ap_codec::encode(const telemetry_indication& indication) noexcept
{
  return encode_with<Telemetry_IndicationData_t>(
      asn_DEF_Telemetry_IndicationData, maximum_telemetry_bytes, [&](Telemetry_IndicationData_t& value) {
        set_indication(value, indication);
      });
}

codec_result<telemetry_indication> e3ap_codec::decode_telemetry(ocudu::span<const uint8_t> bytes) noexcept
{
  return decode_with<telemetry_indication, Telemetry_IndicationData_t>(
      asn_DEF_Telemetry_IndicationData, bytes, maximum_telemetry_bytes, "Telemetry-IndicationData", read_indication);
}

codec_result<std::vector<uint8_t>> e3ap_codec::encode(const telemetry_ran_function_data& data) noexcept
{
  return encode_with<Telemetry_RanFunctionData_t>(
      asn_DEF_Telemetry_RanFunctionData, e3ap_maximum_function_data, [&](Telemetry_RanFunctionData_t& value) {
        set_function(value, data);
      });
}

codec_result<telemetry_ran_function_data>
e3ap_codec::decode_telemetry_function(ocudu::span<const uint8_t> bytes) noexcept
{
  return decode_with<telemetry_ran_function_data, Telemetry_RanFunctionData_t>(
      asn_DEF_Telemetry_RanFunctionData, bytes, e3ap_maximum_function_data, "Telemetry-RanFunctionData", read_function);
}

codec_result<std::vector<uint8_t>> e3ap_codec::encode(const telemetry_subscription_data& data) noexcept
{
  return encode_with<Telemetry_SubscriptionData_t>(
      asn_DEF_Telemetry_SubscriptionData, e3ap_maximum_function_data, [&](Telemetry_SubscriptionData_t& value) {
        if (data.configs.size() > maximum_telemetry_streams) {
          throw std::runtime_error("subscription config list exceeds its bound");
        }
        for (const auto& config : data.configs) {
          set_config(append_asn<Telemetry_SubscriptionConfig_t>(value.configs), config);
        }
      });
}

codec_result<telemetry_subscription_data> e3ap_codec::decode_subscription(ocudu::span<const uint8_t> bytes) noexcept
{
  return decode_with<telemetry_subscription_data, Telemetry_SubscriptionData_t>(
      asn_DEF_Telemetry_SubscriptionData,
      bytes,
      e3ap_maximum_function_data,
      "Telemetry-SubscriptionData",
      [](const Telemetry_SubscriptionData_t& value) {
        telemetry_subscription_data result;
        if (value.configs.list.count < 0 ||
            static_cast<std::size_t>(value.configs.list.count) > maximum_telemetry_streams) {
          throw std::runtime_error("subscription config list exceeds its bound");
        }
        for (int index = 0; index != value.configs.list.count; ++index) {
          if (value.configs.list.array[index] == nullptr) {
            throw std::runtime_error("subscription config entry is missing");
          }
          result.configs.push_back(read_config(*value.configs.list.array[index]));
        }
        return result;
      });
}

codec_result<std::vector<uint8_t>> e3ap_codec::encode(const management_ran_function_data& data) noexcept
{
  return encode_with<Mgmt_RanFunctionData_t>(
      asn_DEF_Mgmt_RanFunctionData, e3ap_maximum_function_data, [&](Mgmt_RanFunctionData_t& value) {
        set_management_function(value, data);
      });
}

codec_result<management_ran_function_data>
e3ap_codec::decode_management_function(ocudu::span<const uint8_t> bytes) noexcept
{
  return decode_with<management_ran_function_data, Mgmt_RanFunctionData_t>(asn_DEF_Mgmt_RanFunctionData,
                                                                           bytes,
                                                                           e3ap_maximum_function_data,
                                                                           "Mgmt-RanFunctionData",
                                                                           read_management_function);
}

codec_result<telemetry_indication> e3ap_codec::from_publication(uint32_t                   interface_id,
                                                                uint32_t                   cell_id,
                                                                uint64_t                   sequence,
                                                                uint64_t                   route_epoch,
                                                                uint64_t                   dropped_objects,
                                                                ocudu::span<const uint8_t> metadata,
                                                                ocudu::span<const uint8_t> tensor,
                                                                bool                       include_bulk) noexcept
{
  try {
    const auto telemetry_id = telemetry_id_for(interface_id);
    if (!telemetry_id || tensor.empty() || cell_id > 65535) {
      return {std::nullopt, "publication does not map to a telemetry stream"};
    }
    telemetry_indication result;
    result.telemetry_id    = *telemetry_id;
    result.cell_id         = static_cast<uint16_t>(cell_id);
    result.sequence        = sequence;
    result.route_epoch     = route_epoch;
    result.dropped_objects = dropped_objects;

    const auto kind = telemetry_kind_of(interface_id);
    if (!kind) {
      class_c_portable_record record;
      record.interface_id = interface_id;
      record.metadata.assign(metadata.begin(), metadata.end());
      if (include_bulk) {
        record.tensor.assign(tensor.begin(), tensor.end());
      }
      result.monotonic_ns = 0;
      result.object       = std::move(record);
      return {std::move(result), {}};
    }

    const auto header = read_telemetry_header(metadata.data(), metadata.size());
    if (!header || header->kind != static_cast<uint16_t>(*kind)) {
      return {std::nullopt, "telemetry metadata header is invalid"};
    }
    result.monotonic_ns = header->monotonic_ns;

    const auto optional_float = [](float value) -> std::optional<double> {
      return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
    };
    const auto radio = [](const telemetry_radio_slot& slot) {
      return telemetry_radio_slot_value{slot.sfn, slot.slot, slot.numerology};
    };

    switch (*kind) {
      case telemetry_kind::pusch_symbols: {
        pusch_symbols_metadata_v1                source;
        std::optional<pusch_symbols_metadata_v2> extended;
        if (metadata.size() == sizeof(pusch_symbols_metadata_v2)) {
          extended.emplace();
          std::memcpy(&*extended, metadata.data(), sizeof(*extended));
          source = extended->base;
        } else if (metadata.size() == sizeof(source)) {
          std::memcpy(&source, metadata.data(), sizeof(source));
        } else {
          return {std::nullopt, "PUSCH symbols metadata size mismatch"};
        }
        pusch_symbols_record record;
        if (extended) {
          if ((extended->symbol_flags & 0x08U) != 0) {
            record.grant = grant_from_packed(extended->grant);
          }
          if ((extended->symbol_flags & 0x01U) != 0) {
            record.crc_ok = (extended->symbol_flags & 0x02U) != 0;
          }
          record.symbols_truncated = (extended->symbol_flags & 0x04U) != 0;
        }
        record.rnti              = source.rnti;
        record.radio_slot        = radio(source.radio_slot);
        record.nof_prb           = source.nof_prb;
        record.nof_data_symbols  = source.nof_data_symbols;
        record.nof_layers        = source.nof_layers;
        record.modulation_bits   = source.modulation_bits;
        record.sinr_db           = optional_float(source.sinr_db);
        record.epre_db           = optional_float(source.epre_db);
        record.rsrp_db           = optional_float(source.rsrp_db);
        record.evm               = optional_float(source.evm);
        record.time_alignment_us = optional_float(source.time_alignment_us);
        record.cfo_hz            = optional_float(source.cfo_hz);
        record.target_code_rate  = optional_float(source.target_code_rate);
        if (include_bulk) {
          record.equalized_symbols.assign(tensor.begin(), tensor.end());
        }
        result.object = std::move(record);
        break;
      }
      case telemetry_kind::pusch_grid: {
        pusch_grid_metadata_v1 source;
        if (metadata.size() != sizeof(source)) {
          return {std::nullopt, "PUSCH grid metadata size mismatch"};
        }
        std::memcpy(&source, metadata.data(), sizeof(source));
        pusch_grid_record record;
        record.rnti         = source.rnti;
        record.radio_slot   = radio(source.radio_slot);
        record.nof_prb      = source.nof_prb;
        record.nof_rx_ports = source.nof_rx_ports;
        record.start_symbol = source.start_symbol;
        record.nof_symbols  = source.nof_symbols;
        record.grant        = grant_from_packed(source.grant);
        if (include_bulk) {
          record.resource_elements.assign(tensor.begin(), tensor.end());
        }
        result.object = std::move(record);
        break;
      }
      case telemetry_kind::pusch_crc: {
        pusch_crc_metadata_v1 source;
        if (metadata.size() != sizeof(source)) {
          return {std::nullopt, "PUSCH CRC metadata size mismatch"};
        }
        std::memcpy(&source, metadata.data(), sizeof(source));
        pusch_crc_record record;
        record.rnti       = source.rnti;
        record.radio_slot = radio(source.radio_slot);
        record.crc_ok     = source.crc_ok != 0;
        result.object     = record;
        break;
      }
      case telemetry_kind::srs_channel: {
        srs_channel_metadata_v2 extended;
        if (metadata.size() == sizeof(extended)) {
          std::memcpy(&extended, metadata.data(), sizeof(extended));
        } else if (metadata.size() == sizeof(extended.base)) {
          std::memcpy(&extended.base, metadata.data(), sizeof(extended.base));
        } else {
          return {std::nullopt, "SRS channel metadata size mismatch"};
        }
        const srs_channel_metadata_v1& source = extended.base;
        // Tensor: estimate, then optionally pilots (symbols x length) and the reference sequence (length).
        constexpr std::size_t sample   = 2 * sizeof(float);
        const std::size_t     estimate = static_cast<std::size_t>(source.nof_subcarriers) * sample;
        const std::size_t pilots    = static_cast<std::size_t>(extended.pilot_symbols) * extended.pilot_length * sample;
        const std::size_t reference = static_cast<std::size_t>(extended.pilot_length) * sample;
        const bool        with_pilots = extended.pilot_length != 0;
        if (include_bulk && with_pilots && tensor.size() != estimate + pilots + reference) {
          return {std::nullopt, "SRS channel tensor size mismatch"};
        }
        srs_channel_record record;
        record.rnti                       = source.rnti;
        record.radio_slot                 = radio(source.radio_slot);
        record.nof_subcarriers            = source.nof_subcarriers;
        record.comb_size                  = source.comb_size;
        record.nof_rx_ports               = source.nof_rx_ports;
        record.nof_tx_ports               = source.nof_tx_ports;
        record.rx_port_index              = source.rx_port_index;
        record.tx_port_index              = source.tx_port_index;
        record.start_symbol               = source.start_symbol;
        record.nof_symbols                = source.nof_symbols;
        record.rsrp_db                    = optional_float(source.rsrp_db);
        record.epre_db                    = optional_float(source.epre_db);
        record.noise_variance             = optional_float(source.noise_variance);
        record.time_alignment_ns          = optional_float(source.time_alignment_ns);
        record.mapping_initial_subcarrier = source.mapping_initial_subcarrier;
        record.comb_offset                = source.comb_offset;
        record.cyclic_shift               = source.cyclic_shift;
        record.sequence_id                = source.sequence_id;
        record.sequence_group             = source.sequence_group;
        record.sequence_number            = source.sequence_number;
        record.cyclic_shift_index         = source.cyclic_shift_index;
        record.cyclic_shift_maximum       = source.cyclic_shift_maximum;
        record.bandwidth_index            = source.bandwidth_index;
        record.frequency_position         = source.frequency_position;
        record.frequency_shift            = source.frequency_shift;
        record.frequency_hopping          = source.frequency_hopping;
        if (include_bulk && with_pilots) {
          record.coefficients.assign(tensor.begin(), tensor.begin() + estimate);
          record.received_pilots.assign(tensor.begin() + estimate, tensor.begin() + estimate + pilots);
          record.reference_symbols.assign(tensor.begin() + estimate + pilots, tensor.end());
        } else if (include_bulk) {
          record.coefficients.assign(tensor.begin(), tensor.end());
        }
        result.object = std::move(record);
        break;
      }
      case telemetry_kind::pusch_dmrs_channel: {
        pusch_dmrs_channel_metadata_v1 source;
        if (metadata.size() != sizeof(source)) {
          return {std::nullopt, "PUSCH DM-RS metadata size mismatch"};
        }
        std::memcpy(&source, metadata.data(), sizeof(source));
        pusch_dmrs_channel_record record;
        record.rnti                        = source.rnti;
        record.radio_slot                  = radio(source.radio_slot);
        record.start_prb                   = source.start_prb;
        record.nof_prb                     = source.nof_prb;
        record.nof_subcarriers             = source.nof_subcarriers;
        record.dmrs_symbol_index           = source.dmrs_symbol_index;
        record.start_symbol                = source.start_symbol;
        record.nof_symbols                 = source.nof_symbols;
        record.nof_rx_ports                = source.nof_rx_ports;
        record.nof_tx_layers               = source.nof_tx_layers;
        record.rx_port_index               = source.rx_port_index;
        record.tx_layer_index              = source.tx_layer_index;
        record.dmrs_type                   = source.dmrs_type;
        record.nof_cdm_groups_without_data = source.nof_cdm_groups_without_data;
        record.transform_precoding         = source.transform_precoding != 0;
        record.scrambling_id               = source.scrambling_id;
        record.n_scid                      = source.n_scid != 0;
        record.sinr_db                     = optional_float(source.sinr_db);
        record.epre_db                     = optional_float(source.epre_db);
        record.rsrp_db                     = optional_float(source.rsrp_db);
        record.noise_variance              = optional_float(source.noise_variance);
        record.time_alignment_us           = optional_float(source.time_alignment_us);
        record.cfo_hz                      = optional_float(source.cfo_hz);
        record.target_code_rate            = optional_float(source.target_code_rate);
        if (include_bulk) {
          record.estimates.assign(tensor.begin(), tensor.end());
        }
        result.object = std::move(record);
        break;
      }
      case telemetry_kind::srs_config: {
        srs_config_metadata_v1 source;
        if (metadata.size() != sizeof(source)) {
          return {std::nullopt, "SRS config metadata size mismatch"};
        }
        std::memcpy(&source, metadata.data(), sizeof(source));
        srs_config_record record;
        record.rnti         = source.rnti;
        record.event        = source.event;
        record.config_epoch = source.config_epoch;
        record.ue_index     = source.ue_index;
        record.numerology   = source.numerology;
        record.n_ta_offset  = source.n_ta_offset;
        if ((source.flags & 0x01U) != 0) {
          srs_resource_parameters& resource  = record.resource.emplace();
          resource.resource_id               = source.resource_id;
          resource.nof_ports                 = source.nof_ports;
          resource.comb_size                 = source.comb_size;
          resource.comb_offset               = source.comb_offset;
          resource.cyclic_shift              = source.cyclic_shift;
          resource.start_position            = source.start_position;
          resource.nof_symbols               = source.nof_symbols;
          resource.repetition_factor         = source.repetition_factor;
          resource.freq_domain_position      = source.freq_domain_position;
          resource.freq_domain_shift         = source.freq_domain_shift;
          resource.c_srs                     = source.c_srs;
          resource.b_srs                     = source.b_srs;
          resource.b_hop                     = source.b_hop;
          resource.group_or_sequence_hopping = source.group_or_sequence_hopping;
          resource.sequence_id               = source.sequence_id;
          resource.period_slots              = source.period_slots;
          resource.offset_slots              = source.offset_slots;
          resource.bwp_start_rb              = source.bwp_start_rb;
          resource.bwp_size_rb               = source.bwp_size_rb;
          if ((source.flags & 0x02U) != 0) {
            srs_derived_mapping& derived       = resource.derived.emplace();
            derived.start_symbol               = source.start_symbol;
            derived.sequence_length            = source.sequence_length;
            derived.mapping_initial_subcarrier = source.mapping_initial_subcarrier;
            derived.sequence_group             = static_cast<uint8_t>(source.sequence_group);
            derived.sequence_number            = static_cast<uint8_t>(source.sequence_number);
            derived.cyclic_shift_maximum       = source.cyclic_shift_maximum;
          }
        }
        result.object = record;
        break;
      }
      case telemetry_kind::ta_command: {
        ta_command_metadata_v1 source;
        if (metadata.size() != sizeof(source)) {
          return {std::nullopt, "TA command metadata size mismatch"};
        }
        std::memcpy(&source, metadata.data(), sizeof(source));
        ta_command_record record;
        record.rnti       = source.rnti;
        record.radio_slot = radio(source.radio_slot);
        record.source     = source.source;
        record.tag_id     = source.tag_id;
        record.ta_command = source.ta_command;
        result.object     = record;
        break;
      }
      case telemetry_kind::gnb_metrics_json: {
        if (metadata.size() != sizeof(gnb_metrics_json_metadata_v1)) {
          return {std::nullopt, "gNB metrics metadata size mismatch"};
        }
        gnb_metrics_json_record record;
        if (include_bulk) {
          record.json.assign(tensor.begin(), tensor.end());
        }
        result.object = std::move(record);
        break;
      }
    }
    return {std::move(result), {}};
  } catch (const std::bad_alloc&) {
    return {std::nullopt, "telemetry conversion allocation failed"};
  } catch (...) {
    return {std::nullopt, "unknown telemetry conversion failure"};
  }
}

std::vector<ocudu::span<const uint8_t>> e3ap_codec::fragment(ocudu::span<const uint8_t> payload,
                                                             std::size_t                maximum_fragment_bytes) noexcept
{
  std::vector<ocudu::span<const uint8_t>> fragments;
  if (payload.empty() || maximum_fragment_bytes == 0) {
    return fragments;
  }
  try {
    for (std::size_t offset = 0; offset < payload.size(); offset += maximum_fragment_bytes) {
      fragments.push_back(payload.subspan(offset, std::min(maximum_fragment_bytes, payload.size() - offset)));
    }
  } catch (...) {
    fragments.clear();
  }
  return fragments;
}
