// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#ifndef OCUDU_DAPP_USE_CASES_V1_SRS_ISAC_H
#define OCUDU_DAPP_USE_CASES_V1_SRS_ISAC_H
#include "common.h"
#ifdef __cplusplus
extern "C" {
#endif
#define OCUDU_DAPP_INTERFACE_SRS_ISAC_V1 0x00010002u
#define OCUDU_DAPP_METADATA_SRS_ISAC_V1 0x00010202u
#define OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1 0x00010002u
#define OCUDU_DAPP_SRS_ISAC_OUTPUT_CIR_PREVIEW_V1 0x00010102u
#define OCUDU_DAPP_LAYOUT_SRS_CHANNEL_RX_TX_FREQUENCY_CF32_V1 0x00010201u
#define OCUDU_DAPP_SRS_ISAC_MAX_CIR_TAPS_V1 64u
typedef struct {
  uint32_t struct_size;
  uint16_t abi_major;
  uint16_t abi_minor;
  uint32_t srs_resource_id;
  uint32_t srs_config_id;
  uint32_t rx_ports;
  uint32_t tx_ports;
  uint32_t comb_size;
  uint32_t comb_offset;
  uint32_t first_subcarrier;
  uint32_t subcarrier_spacing_hz;
  uint32_t frequency_points;
  float    noise_variance;
  float    rsrp_db;
  float    epre_db;
  int64_t  timing_advance_ns;
  /// C-RNTI of the sounding UE; 0 when the host cannot attribute the SRS to a UE
  /// (legacy hosts zero-filled this word as reserved space, so 0 is the compatible
  /// "unknown" value and consumers must fall back to resource-only stream keying).
  uint32_t rnti;
  uint32_t reserved0;
  uint64_t reserved[7];
} ocudu_dapp_srs_isac_metadata_v1;
typedef struct {
  ocudu_dapp_invocation_v1     invocation;
  ocudu_dapp_tensor_view_v1    channel_estimates;
  ocudu_dapp_typed_metadata_v1 srs_metadata;
  uint64_t                     source_sequence;
} ocudu_dapp_srs_isac_input_v1;

/// Bounded diagnostic CIR preview. This is a reference/E2E report, not a production ISAC inference contract.
typedef struct {
  uint32_t                     struct_size;
  uint16_t                     abi_major;
  uint16_t                     abi_minor;
  ocudu_dapp_context_header_v1 context;
  uint64_t                     source_srs_sequence;
  uint32_t                     srs_resource_id;
  uint32_t                     srs_config_id;
  uint32_t                     tap_count;
  uint32_t                     peak_delay_bin;
  float                        mean_channel_power;
  float                        noise_corrected_power;
  float                        peak_cir_power;
  float                        noise_variance;
  float                        tap_power[OCUDU_DAPP_SRS_ISAC_MAX_CIR_TAPS_V1];
  uint64_t                     reserved[4];
} ocudu_dapp_srs_isac_cir_preview_v1;

typedef ocudu_dapp_status_v1 (*ocudu_dapp_srs_isac_publish_v1)(void*, const ocudu_dapp_srs_isac_input_v1*);
typedef struct {
  ocudu_dapp_worker_api_v1       worker;
  ocudu_dapp_srs_isac_publish_v1 publish;
  uint64_t                       reserved[8];
} ocudu_dapp_srs_isac_interface_v1;
#ifdef __cplusplus
}
#endif
#endif
