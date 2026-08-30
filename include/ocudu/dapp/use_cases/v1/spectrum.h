// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#ifndef OCUDU_DAPP_USE_CASES_V1_SPECTRUM_H
#define OCUDU_DAPP_USE_CASES_V1_SPECTRUM_H
#include "coordination.h"
#ifdef __cplusplus
extern "C" {
#endif
#define OCUDU_DAPP_INTERFACE_SPECTRUM_V1 0x00010001u
#define OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1 0x00010201u
#define OCUDU_DAPP_SPECTRUM_OUTPUT_INTERFERENCE_MAP_V1 0x00010101u
#define OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 275u
#define OCUDU_DAPP_SPECTRUM_CAP_INTERFERENCE_MAP_V1 (UINT64_C(1) << 0u)
#define OCUDU_DAPP_SPECTRUM_CAP_QUIET_PERIOD_INTENT_V1 (UINT64_C(1) << 1u)
#define OCUDU_DAPP_SPECTRUM_CAP_FUTURE_UL_PLAN_V1 (UINT64_C(1) << 2u)
#define OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1 0x00010002u
#define OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1 (UINT32_C(1) << 0u)

/// Values in OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1, indexed [symbol][PRB].
typedef enum {
  OCUDU_DAPP_UL_USE_UNKNOWN_V1          = 0,
  OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1    = 1,
  OCUDU_DAPP_UL_USE_PUSCH_V1            = 2,
  OCUDU_DAPP_UL_USE_PUCCH_V1            = 3,
  OCUDU_DAPP_UL_USE_SRS_V1              = 4,
  OCUDU_DAPP_UL_USE_PRACH_OR_COMMON_V1  = 5,
  OCUDU_DAPP_UL_USE_REFERENCE_SIGNAL_V1 = 6,
  OCUDU_DAPP_UL_USE_RESERVED_QUIET_V1   = 7
} ocudu_dapp_ul_use_category_v1;

typedef struct {
  uint32_t struct_size;
  uint16_t abi_major;
  uint16_t abi_minor;
  uint64_t bandwidth_hz;
  uint32_t rx_ports;
  uint32_t symbols;
  uint32_t subcarriers;
  float    amplitude_scale;
  uint32_t flags;
  /// Generation of scheduled_re_mask, or zero when no complete expected-use plan is supplied.
  uint64_t source_plan_generation;
  uint64_t reserved[7];
} ocudu_dapp_spectrum_slot_metadata_v1;
typedef struct {
  ocudu_dapp_invocation_v1     invocation;
  ocudu_dapp_tensor_view_v1    uplink_resource_grid;
  ocudu_dapp_tensor_view_v1    scheduled_re_mask;
  ocudu_dapp_typed_metadata_v1 slot_metadata;
  uint64_t                     source_sequence;
} ocudu_dapp_spectrum_input_v1;

/// Prepared future-valid per-PRB sensing result. The host copies it before publish_prepared returns.
/// producer_instance_id and producer_generation are host-owned and shall be zero on submission.
typedef struct {
  uint32_t                     struct_size;
  uint16_t                     abi_major;
  uint16_t                     abi_minor;
  ocudu_dapp_context_header_v1 context;
  ocudu_dapp_tensor_view_v1    prb_confidence;
  uint64_t                     source_grid_sequence;
  uint64_t                     source_plan_generation;
  uint32_t                     first_prb;
  uint32_t                     nof_prbs;
  uint64_t                     reserved[6];
} ocudu_dapp_spectrum_interference_map_v1;

typedef ocudu_dapp_status_v1 (*ocudu_dapp_spectrum_publish_v1)(void*, const ocudu_dapp_spectrum_input_v1*);
typedef ocudu_dapp_status_v1 (*ocudu_dapp_spectrum_publish_plan_v1)(void*, const ocudu_dapp_ul_resource_plan_v1*);
typedef struct {
  ocudu_dapp_worker_api_v1            worker;
  ocudu_dapp_spectrum_publish_v1      publish;
  ocudu_dapp_spectrum_publish_plan_v1 publish_plan;
  uint64_t                            reserved[7];
} ocudu_dapp_spectrum_interface_v1;
#ifdef __cplusplus
}
#endif
#endif
