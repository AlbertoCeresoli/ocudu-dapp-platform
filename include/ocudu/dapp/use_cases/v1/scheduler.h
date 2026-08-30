// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#ifndef OCUDU_DAPP_USE_CASES_V1_SCHEDULER_H
#define OCUDU_DAPP_USE_CASES_V1_SCHEDULER_H
#include "common.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define OCUDU_DAPP_INTERFACE_SCHEDULER_V1 0x00010006u
#define OCUDU_DAPP_SCHEDULER_MAX_CANDIDATES_V1 8192u
#define OCUDU_DAPP_SCHEDULER_CONTEXT_INTERFERENCE_MAP_V1 OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1
#define OCUDU_DAPP_SCHEDULER_LAYOUT_PRB_INTERFERENCE_U8_V1 OCUDU_DAPP_LAYOUT_PRB_INTERFERENCE_U8_V1
/// Free-for-newTx VRB occupancy snapshot (1 = free, 0 = occupied). Host-owned.
#define OCUDU_DAPP_SCHEDULER_CONTEXT_FREE_VRB_MASK_V1 0x00010002u
#define OCUDU_DAPP_SCHEDULER_LAYOUT_PRB_FREE_U8_V1 0x00010002u

/// Class-B is a synchronous in-process host-record contract. A package selected
/// for a CUDA or other accelerator backend may use that backend internally, but
/// v1 supplies no resident tensor or host-owned execution-stream contract: all
/// candidate, intent and optional mask storage remains host-visible, and the
/// complete result must return within the invocation deadline.

typedef enum { OCUDU_DAPP_SCHEDULER_DL_V1 = 1, OCUDU_DAPP_SCHEDULER_UL_V1 = 2 } ocudu_dapp_scheduler_direction_v1;

/// Host-granted output authority for this invocation. Unset capabilities are observe-only.
enum {
  OCUDU_DAPP_SCHEDULER_ALLOW_AVOID_PRB_MASK_V1 = 1u << 0,
  OCUDU_DAPP_SCHEDULER_ALLOW_MCS_V1            = 1u << 1,
  OCUDU_DAPP_SCHEDULER_ALLOW_PRB_LIMITS_V1     = 1u << 2,
  OCUDU_DAPP_SCHEDULER_ALLOW_PRB_PREFERENCE_V1 = 1u << 3,
  /// Hard multi-UE packing: preferred_start_prb/preferred_nof_prbs are exact assignments.
  OCUDU_DAPP_SCHEDULER_ALLOW_PRB_ASSIGNMENT_V1 = 1u << 4,
  OCUDU_DAPP_SCHEDULER_ALLOW_TPC_V1            = 1u << 5,
  OCUDU_DAPP_SCHEDULER_ALLOW_LAYERS_V1         = 1u << 6
};

#define OCUDU_DAPP_SCHEDULER_ALLOW_KNOWN_MASK_V1                                                                       \
  (OCUDU_DAPP_SCHEDULER_ALLOW_AVOID_PRB_MASK_V1 | OCUDU_DAPP_SCHEDULER_ALLOW_MCS_V1 |                                  \
   OCUDU_DAPP_SCHEDULER_ALLOW_PRB_LIMITS_V1 | OCUDU_DAPP_SCHEDULER_ALLOW_PRB_PREFERENCE_V1 |                           \
   OCUDU_DAPP_SCHEDULER_ALLOW_PRB_ASSIGNMENT_V1 | OCUDU_DAPP_SCHEDULER_ALLOW_TPC_V1 |                                  \
   OCUDU_DAPP_SCHEDULER_ALLOW_LAYERS_V1)

enum {
  OCUDU_DAPP_SCHEDULER_CANDIDATE_PENDING_SR_V1          = 1u << 0,
  OCUDU_DAPP_SCHEDULER_CANDIDATE_HOL_DELAY_VALID_V1     = 1u << 1,
  OCUDU_DAPP_SCHEDULER_CANDIDATE_AVERAGE_RATE_VALID_V1  = 1u << 2,
  OCUDU_DAPP_SCHEDULER_CANDIDATE_CQI_VALID_V1           = 1u << 3,
  OCUDU_DAPP_SCHEDULER_CANDIDATE_SINR_VALID_V1          = 1u << 4,
  OCUDU_DAPP_SCHEDULER_CANDIDATE_BLER_VALID_V1          = 1u << 5,
  OCUDU_DAPP_SCHEDULER_CANDIDATE_PHR_VALID_V1           = 1u << 6,
  OCUDU_DAPP_SCHEDULER_CANDIDATE_MCS_VALID_V1           = 1u << 7,
  OCUDU_DAPP_SCHEDULER_CANDIDATE_LAYERS_VALID_V1        = 1u << 8,
  OCUDU_DAPP_SCHEDULER_CANDIDATE_CONVENTIONAL_FORBID_V1 = 1u << 9,
  /// last_tx_outcome / last_used_mcs are meaningful for package-local OLLA.
  OCUDU_DAPP_SCHEDULER_CANDIDATE_OUTCOME_VALID_V1     = 1u << 10,
  OCUDU_DAPP_SCHEDULER_CANDIDATE_OLLA_OFFSET_VALID_V1 = 1u << 11,
  OCUDU_DAPP_SCHEDULER_CANDIDATE_EXPERT_MCS_VALID_V1  = 1u << 12,
  /// phr_db carries the grant-size-normalized power headroom: raw Type-1 PH
  /// plus 10log10(N_PRB of the PUSCH that carried the PHR), i.e. the headroom
  /// a 1-PRB transmission would have. Raw PH scales with the grant the UE
  /// happened to be measured on, so absolute thresholds against it limit-cycle
  /// between fat and toe-hold grants; the normalized figure is comparable
  /// across reports. Consumers subtract 10log10 of their intended grant size.
  OCUDU_DAPP_SCHEDULER_CANDIDATE_PHR_NORMALIZED_V1 = 1u << 13
};

/// Last newTx outcome for package-local OLLA (host-filled).
enum {
  OCUDU_DAPP_SCHEDULER_OUTCOME_NONE_V1           = 0,
  OCUDU_DAPP_SCHEDULER_OUTCOME_ACK_OR_CRC_OK_V1  = 1,
  OCUDU_DAPP_SCHEDULER_OUTCOME_NACK_OR_CRC_KO_V1 = 2,
  OCUDU_DAPP_SCHEDULER_OUTCOME_DTX_V1            = 3
};

/// MCS table id for portable table selection (host-filled).
enum {
  OCUDU_DAPP_SCHEDULER_MCS_TABLE_QAM64_V1       = 0,
  OCUDU_DAPP_SCHEDULER_MCS_TABLE_QAM256_V1      = 1,
  OCUDU_DAPP_SCHEDULER_MCS_TABLE_QAM64_LOWSE_V1 = 2
};

/// One pointer-free UE feature record. candidate_id is dense and valid only for this invocation.
typedef struct {
  uint32_t struct_size;
  uint32_t flags;
  uint32_t candidate_id;
  uint32_t ue_index;
  uint16_t rnti;
  uint16_t recommended_mcs;
  uint8_t  nof_layers;
  uint8_t  reserved0[3];
  uint64_t pending_bytes;
  /// Age of the oldest pending DL SDU in this slice at transmission_time.
  /// UL BSRs carry no packet timestamp, so v1 leaves this invalid for UL.
  uint64_t hol_delay_us;
  /// Exponentially averaged served rate for this UE and slice (alpha=0.01),
  /// normalized to bits/s using the invocation numerology. The average advances
  /// once per effective slice scheduling opportunity (one with newTx grants).
  uint64_t average_rate_bps;
  double   conventional_priority;
  float    cqi;
  float    sinr_db;
  /// Recent first-transmission error ratio for this direction in [0, 1].
  /// Retransmission outcomes and DL DTX are excluded. See bler_nof_observations.
  float bler;
  /// Type-1 PH (dB), lower edge of the reported interval. Valid when PHR_VALID.
  float phr_db;
  /// Host OLLA offset (dB) for bootstrap/observe; package may maintain its own.
  float host_olla_offset_db;
  /// Last grant outcome enum (OUTCOME_*); valid when OUTCOME_VALID.
  uint8_t last_tx_outcome;
  /// MCS used on the grant that produced last_tx_outcome.
  uint8_t last_used_mcs;
  uint8_t expert_mcs_min;
  uint8_t expert_mcs_max;
  uint8_t mcs_table_id;
  uint8_t transform_precoder;
  uint8_t reserved1[2];
  /// Optional UL closed-loop target SINR (dB) when host provides it.
  float tpc_target_sinr_db;
  /// Number of outcomes represented by bler, saturated at
  /// OCUDU_DAPP_SCHEDULER_BLER_WINDOW_SIZE_V1. Consumers must first check
  /// struct_size >= OCUDU_DAPP_SCHEDULER_CANDIDATE_BLER_COUNT_SIZE_V1.
  uint32_t bler_nof_observations;
  uint32_t reserved2;
  uint64_t reserved[1];
} ocudu_dapp_scheduler_candidate_v1;

/// Immutable v1 base record through tpc_target_sinr_db. Validators must use
/// this instead of offsetof(reserved), because reserved tail bytes can be
/// promoted by compatible v1 extensions.
#define OCUDU_DAPP_SCHEDULER_CANDIDATE_BASE_SIZE_V1                                                                    \
  ((uint32_t)(offsetof(ocudu_dapp_scheduler_candidate_v1, tpc_target_sinr_db) + sizeof(float)))
/// Minimum struct_size required before reading bler_nof_observations.
#define OCUDU_DAPP_SCHEDULER_CANDIDATE_BLER_COUNT_SIZE_V1                                                              \
  ((uint32_t)(offsetof(ocudu_dapp_scheduler_candidate_v1, bler_nof_observations) + sizeof(uint32_t)))
#define OCUDU_DAPP_SCHEDULER_BLER_WINDOW_SIZE_V1 64u

typedef struct {
  ocudu_dapp_invocation_v1                 invocation;
  uint32_t                                 direction;
  uint32_t                                 slice_id;
  ocudu_dapp_radio_time_v1                 transmission_time;
  uint32_t                                 nof_candidates;
  uint32_t                                 cell_nof_prbs;
  uint64_t                                 candidate_generation;
  const ocudu_dapp_scheduler_candidate_v1* candidates;
  ocudu_dapp_context_header_v1             interference_context;
  ocudu_dapp_tensor_view_v1                optional_interference_mask;
  uint32_t                                 interference_context_present;
  uint32_t                                 allowed_output_flags;
  /// Free-for-newTx VRB mask (1=free). Required for ALLOW_PRB_ASSIGNMENT.
  ocudu_dapp_context_header_v1 free_vrb_context;
  ocudu_dapp_tensor_view_v1    free_vrb_mask;
  uint32_t                     free_vrb_context_present;
  /// Remaining slice PRB budget for this newTx opportunity (host-filled).
  uint16_t remaining_slice_prbs;
  uint16_t reserved1;
  uint64_t reserved[3];
} ocudu_dapp_scheduler_input_v1;

enum {
  OCUDU_DAPP_SCHEDULER_INTENT_PRIORITY_V1       = 1u << 0,
  OCUDU_DAPP_SCHEDULER_INTENT_FORBID_V1         = 1u << 1,
  OCUDU_DAPP_SCHEDULER_INTENT_PRB_LIMITS_V1     = 1u << 2,
  OCUDU_DAPP_SCHEDULER_INTENT_PRB_PREFERENCE_V1 = 1u << 3,
  OCUDU_DAPP_SCHEDULER_INTENT_MCS_V1            = 1u << 4,
  OCUDU_DAPP_SCHEDULER_INTENT_LAYERS_V1         = 1u << 5,
  OCUDU_DAPP_SCHEDULER_INTENT_TPC_V1            = 1u << 6,
  OCUDU_DAPP_SCHEDULER_INTENT_BEAM_V1           = 1u << 7
};

/// A constrained recommendation. OCUDU validates every flagged field before changing allocator inputs.
/// When ALLOW_PRB_ASSIGNMENT is granted, a preferred run that is a subset of free_vrb, does not overlap
/// another hard assign in this invoke, and fits the slice budget is committed as a hard assignment.
/// An in-band preference that fails those occupancy checks is kept as a soft preference (host first-fit
/// fallback) and does not reject the invoke. Off-cell / zero-length ranges are still invalid.
/// Fields stay under INTENT_PRB_PREFERENCE for ABI reuse.
typedef struct {
  uint32_t struct_size;
  uint32_t candidate_id;
  uint32_t flags;
  uint32_t reserved0;
  double   priority;
  uint16_t min_prbs;
  uint16_t max_prbs;
  uint16_t preferred_start_prb;
  uint16_t preferred_nof_prbs;
  int16_t  recommended_mcs;
  int16_t  maximum_mcs;
  uint8_t  nof_layers;
  int8_t   tpc_command;
  uint16_t beam_id;
  uint64_t reserved[4];
} ocudu_dapp_scheduler_intent_v1;

enum { OCUDU_DAPP_SCHEDULER_OUTPUT_AVOID_PRB_MASK_V1 = 1u << 0 };

typedef struct {
  uint32_t                        struct_size;
  uint16_t                        abi_major;
  uint16_t                        abi_minor;
  ocudu_dapp_scheduler_intent_v1* intents;
  uint32_t                        intent_capacity;
  uint32_t                        nof_intents;
  uint32_t                        flags;
  uint32_t                        reserved0;
  uint64_t                        decision_generation;
  ocudu_dapp_tensor_view_v1       avoid_prb_mask;
  uint64_t                        reserved[4];
} ocudu_dapp_scheduler_output_v1;

typedef ocudu_dapp_status_v1 (*ocudu_dapp_scheduler_invoke_v1)(void*,
                                                               const ocudu_dapp_scheduler_input_v1*,
                                                               ocudu_dapp_scheduler_output_v1*);
typedef struct {
  ocudu_dapp_worker_api_v1       worker;
  ocudu_dapp_scheduler_invoke_v1 invoke;
  uint64_t                       reserved[8];
} ocudu_dapp_scheduler_interface_v1;
#ifdef __cplusplus
}
#endif

#endif
