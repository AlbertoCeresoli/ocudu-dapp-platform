// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/e3ap_protocol.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"

using namespace ocudu::dapp::e3;

const char* ocudu::dapp::e3::to_string(e3ap_cause cause) noexcept
{
  switch (cause) {
    case e3ap_cause::unspecified:
      return "unspecified";
    case e3ap_cause::protocol_error:
      return "protocol error";
    case e3ap_cause::setup_required:
      return "setup required";
    case e3ap_cause::setup_already_complete:
      return "setup already complete";
    case e3ap_cause::unsupported_protocol_version:
      return "unsupported protocol version";
    case e3ap_cause::unsupported_ran_function:
      return "unsupported RAN function";
    case e3ap_cause::unsupported_identifier:
      return "unsupported identifier";
    case e3ap_cause::invalid_request:
      return "invalid request";
    case e3ap_cause::not_found:
      return "not found";
    case e3ap_cause::duplicate:
      return "duplicate";
    case e3ap_cause::resource_exhausted:
      return "resource exhausted";
    case e3ap_cause::authorization_rejected:
      return "authorization rejected";
    case e3ap_cause::payload_too_large:
      return "payload too large";
    case e3ap_cause::transport_overrun:
      return "transport overrun";
    case e3ap_cause::shutting_down:
      return "shutting down";
  }
  return "unknown cause";
}

std::optional<uint32_t> ocudu::dapp::e3::telemetry_interface_for(uint16_t telemetry_id) noexcept
{
  switch (telemetry_id) {
    case telemetry_id_pusch_symbols:
      return telemetry_stream_pusch_symbols;
    case telemetry_id_pusch_crc:
      return telemetry_stream_pusch_crc;
    case telemetry_id_srs_channel:
      return telemetry_stream_srs_channel;
    case telemetry_id_pusch_dmrs_channel:
      return telemetry_stream_pusch_dmrs_channel;
    case telemetry_id_gnb_metrics_json:
      return telemetry_stream_gnb_metrics_json;
    case telemetry_id_pusch_grid:
      return telemetry_stream_pusch_grid;
    case telemetry_id_srs_config:
      return telemetry_stream_srs_config;
    case telemetry_id_ta_command:
      return telemetry_stream_ta_command;
    case telemetry_id_class_c_spectrum:
      return OCUDU_DAPP_INTERFACE_SPECTRUM_V1;
    case telemetry_id_class_c_srs_isac:
      return OCUDU_DAPP_INTERFACE_SRS_ISAC_V1;
    default:
      return std::nullopt;
  }
}

std::optional<uint16_t> ocudu::dapp::e3::telemetry_id_for(uint32_t interface_id) noexcept
{
  switch (interface_id) {
    case telemetry_stream_pusch_symbols:
      return telemetry_id_pusch_symbols;
    case telemetry_stream_pusch_crc:
      return telemetry_id_pusch_crc;
    case telemetry_stream_srs_channel:
      return telemetry_id_srs_channel;
    case telemetry_stream_pusch_dmrs_channel:
      return telemetry_id_pusch_dmrs_channel;
    case telemetry_stream_gnb_metrics_json:
      return telemetry_id_gnb_metrics_json;
    case telemetry_stream_pusch_grid:
      return telemetry_id_pusch_grid;
    case telemetry_stream_srs_config:
      return telemetry_id_srs_config;
    case telemetry_stream_ta_command:
      return telemetry_id_ta_command;
    case OCUDU_DAPP_INTERFACE_SPECTRUM_V1:
      return telemetry_id_class_c_spectrum;
    case OCUDU_DAPP_INTERFACE_SRS_ISAC_V1:
      return telemetry_id_class_c_srs_isac;
    default:
      return std::nullopt;
  }
}

std::vector<telemetry_stream_record> ocudu::dapp::e3::default_telemetry_streams()
{
  return {
      {telemetry_id_pusch_symbols,
       "pusch.symbols",
       "Per-UE PUSCH link quality with equalized data symbols (binary32 I/Q pairs)",
       false,
       telemetry_stream_pusch_symbols,
       50'000,
       {}},
      {telemetry_id_pusch_crc,
       "pusch.crc",
       "PUSCH transport-block CRC outcome per UE and slot",
       false,
       telemetry_stream_pusch_crc,
       std::nullopt,
       {}},
      {telemetry_id_srs_channel,
       "srs.channel",
       "SRS channel estimate per UE, receive port, and transmit port (binary32 pairs)",
       false,
       telemetry_stream_srs_channel,
       20'000,
       {}},
      {telemetry_id_pusch_dmrs_channel,
       "pusch.dmrs_channel",
       "PUSCH DM-RS channel estimate per UE, receive port, and layer (binary32 pairs)",
       false,
       telemetry_stream_pusch_dmrs_channel,
       20'000,
       {}},
      {telemetry_id_gnb_metrics_json,
       "gnb.metrics_json",
       "Periodic gNB JSON metrics report (scheduler, MAC, RLC, DU-low, CU) relayed byte for byte",
       false,
       telemetry_stream_gnb_metrics_json,
       std::nullopt,
       {}},
      {telemetry_id_class_c_spectrum,
       "classc.spectrum",
       "Host snapshot of the Class-C Spectrum publication: OCC3 metadata plus uplink resource grid",
       true,
       OCUDU_DAPP_INTERFACE_SPECTRUM_V1,
       std::nullopt,
       {}},
      {telemetry_id_class_c_srs_isac,
       "classc.srs_isac",
       "Host snapshot of the Class-C SRS-ISAC publication: OCC3 metadata plus channel estimates",
       true,
       OCUDU_DAPP_INTERFACE_SRS_ISAC_V1,
       std::nullopt,
       {}},
      {telemetry_id_pusch_grid,
       "pusch.grid",
       "Raw pre-equalization PUSCH resource elements per grant (all receive ports, DM-RS included) with grant "
       "parameters",
       false,
       telemetry_stream_pusch_grid,
       100'000,
       {}},
      {telemetry_id_srs_config,
       "srs.config",
       "SRS configuration per UE context (TS 38.331 SRS-Resource, period, offset, epoch) on every change",
       false,
       telemetry_stream_srs_config,
       std::nullopt,
       {}},
      {telemetry_id_ta_command,
       "ta.command",
       "Timing advance commands sent to UEs: RAR T_A and every Timing Advance Command MAC CE",
       false,
       telemetry_stream_ta_command,
       std::nullopt,
       {}},
  };
}
