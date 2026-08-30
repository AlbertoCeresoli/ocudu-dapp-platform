// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef OCUDU_DAPP_USE_CASES_V1_COORDINATION_H
#define OCUDU_DAPP_USE_CASES_V1_COORDINATION_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OCUDU_DAPP_CONTEXT_UL_RESOURCE_PLAN_V1 0x00010002u
#define OCUDU_DAPP_CONTEXT_QUIET_RESERVATION_V1 0x00010003u
#define OCUDU_DAPP_SPECTRUM_OUTPUT_QUIET_PERIOD_INTENT_V1 0x00010102u
#define OCUDU_DAPP_UL_RESOURCE_PLAN_MAX_SPANS_V1 64u
#define OCUDU_DAPP_QUIET_RESERVATION_MAX_REPEAT_V1 8u
#define OCUDU_DAPP_UL_RESOURCE_PLAN_COMPLETE_V1 (UINT32_C(1) << 0u)

typedef enum {
  OCUDU_DAPP_QUIET_NO_NEW_PUSCH_V1       = 0,
  OCUDU_DAPP_QUIET_NO_PUSCH_V1           = 1,
  OCUDU_DAPP_QUIET_NO_SCHEDULED_UE_UL_V1 = 2,
  OCUDU_DAPP_QUIET_RF_SILENCE_REQUEST_V1 = 3
} ocudu_dapp_quietness_level_v1;

typedef enum {
  OCUDU_DAPP_RESERVATION_ACCEPTED_V1  = 0,
  OCUDU_DAPP_RESERVATION_CLAMPED_V1   = 1,
  OCUDU_DAPP_RESERVATION_REJECTED_V1  = 2,
  OCUDU_DAPP_RESERVATION_CANCELLED_V1 = 3,
  OCUDU_DAPP_RESERVATION_EXPIRED_V1   = 4
} ocudu_dapp_reservation_status_v1;

typedef enum {
  OCUDU_DAPP_QUIET_REASON_NONE_V1              = 0,
  OCUDU_DAPP_QUIET_REASON_INVALID_V1           = 1,
  OCUDU_DAPP_QUIET_REASON_INSUFFICIENT_LEAD_V1 = 2,
  OCUDU_DAPP_QUIET_REASON_OUTSIDE_HORIZON_V1   = 3,
  OCUDU_DAPP_QUIET_REASON_BUDGET_V1            = 4,
  OCUDU_DAPP_QUIET_REASON_CONFLICT_V1          = 5,
  OCUDU_DAPP_QUIET_REASON_CAPABILITY_V1        = 6,
  OCUDU_DAPP_QUIET_REASON_SOURCE_RETIRED_V1    = 7,
  OCUDU_DAPP_QUIET_REASON_EXPIRED_V1           = 8
} ocudu_dapp_quiet_reason_v1;

/// One committed future UL allocation span. Storage is callback-scoped and host-owned.
typedef struct {
  uint32_t struct_size;
  uint8_t  symbol_start;
  uint8_t  symbol_count;
  uint16_t first_prb;
  uint16_t nof_prbs;
  uint8_t  use_category;
  uint8_t  reserved0;
  uint16_t dmrs_symbol_mask;
  uint16_t reserved1;
  uint32_t allocation_id;
  uint64_t reserved[2];
} ocudu_dapp_ul_resource_span_v1;

/// Host-authored advance UL plan. spans and optional_dense_mask are borrowed for one callback only.
typedef struct {
  uint32_t                              struct_size;
  uint16_t                              abi_major;
  uint16_t                              abi_minor;
  ocudu_dapp_context_header_v1          context;
  ocudu_dapp_radio_time_v1              planned_time;
  uint64_t                              plan_generation;
  const ocudu_dapp_ul_resource_span_v1* spans;
  uint32_t                              nof_spans;
  uint32_t                              flags;
  uint32_t                              cell_nof_prbs;
  uint32_t                              reserved0;
  ocudu_dapp_tensor_view_v1             optional_dense_mask;
  uint64_t                              reserved[5];
} ocudu_dapp_ul_resource_plan_v1;

/// Pointer-free Class-C request. A request is advice until OCUDU returns a separate reservation object.
typedef struct {
  uint32_t                 struct_size;
  uint16_t                 abi_major;
  uint16_t                 abi_minor;
  uint64_t                 request_id[2];
  ocudu_dapp_radio_time_v1 earliest_start;
  ocudu_dapp_radio_time_v1 latest_start;
  uint16_t                 duration_slots;
  uint16_t                 repeat_period_slots;
  uint16_t                 repeat_count;
  uint16_t                 first_prb;
  uint16_t                 nof_prbs;
  uint8_t                  requested_level;
  uint8_t                  reserved0;
  uint16_t                 budget_class;
  uint32_t                 confidence_q16;
  uint64_t                 expiry_monotonic_ns;
  uint64_t                 reserved[6];
} ocudu_dapp_quiet_period_intent_v1;

/// Pointer-free authoritative decision. achieved_level, status and reason are OCUDU-owned.
typedef struct {
  uint32_t                     struct_size;
  uint16_t                     abi_major;
  uint16_t                     abi_minor;
  ocudu_dapp_context_header_v1 context;
  uint64_t                     request_id[2];
  uint64_t                     reservation_id[2];
  uint64_t                     reservation_generation;
  uint64_t                     request_source_instance_id[2];
  uint64_t                     request_source_generation;
  uint64_t                     request_source_sequence;
  ocudu_dapp_radio_time_v1     start;
  uint16_t                     duration_slots;
  uint16_t                     repeat_period_slots;
  uint16_t                     repeat_count;
  uint16_t                     first_prb;
  uint16_t                     nof_prbs;
  uint8_t                      achieved_level;
  uint8_t                      status;
  uint16_t                     reason;
  uint16_t                     budget_class;
  uint32_t                     confidence_q16;
  uint64_t                     expiry_monotonic_ns;
  uint64_t                     reserved[5];
} ocudu_dapp_quiet_period_reservation_v1;

#ifdef __cplusplus
}
#endif

#endif
