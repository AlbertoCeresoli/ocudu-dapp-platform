// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef OCUDU_DAPP_PROCESS_V1_CONTROL_H
#define OCUDU_DAPP_PROCESS_V1_CONTROL_H

#include "ocudu/dapp/abi/v1/management.h"
#include "ocudu/dapp/use_cases/v1/coordination.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OCUDU_DAPP_CONTROL_MAGIC_V1 UINT32_C(0x4f433343)
#define OCUDU_DAPP_CONTROL_VERSION_V1 1u
#define OCUDU_DAPP_CONTROL_MAX_PAYLOAD_V1 4096u
#define OCUDU_DAPP_CONTROL_MAX_DESCRIPTORS_V1 8u
#define OCUDU_DAPP_MAX_PRODUCER_SHARDS_V1 64u

#define OCUDU_DAPP_CONTROL_ROUTE_OFFER_V1 1u
#define OCUDU_DAPP_CONTROL_ROUTE_READY_V1 2u
#define OCUDU_DAPP_CONTROL_SPECTRUM_RESULT_V1 3u
#define OCUDU_DAPP_CONTROL_SRS_ISAC_RESULT_V1 4u
#define OCUDU_DAPP_CONTROL_QUIET_INTENT_RESULT_V1 5u
#define OCUDU_DAPP_CONTROL_HEARTBEAT_REQUEST_V1 6u
#define OCUDU_DAPP_CONTROL_HEARTBEAT_RESPONSE_V1 7u
#define OCUDU_DAPP_CONTROL_MANAGEMENT_REQUEST_V1 8u
#define OCUDU_DAPP_CONTROL_MANAGEMENT_RESPONSE_V1 9u
#define OCUDU_DAPP_CONTROL_ROUTE_BUNDLE_V1 10u
#define OCUDU_DAPP_CONTROL_ROUTE_BUNDLE_READY_V1 11u
#define OCUDU_DAPP_PROCESS_CAP_HEARTBEAT_V1 (UINT32_C(1) << 0u)
#define OCUDU_DAPP_PROCESS_CAP_CONFIG_V1 (UINT32_C(1) << 1u)
#define OCUDU_DAPP_PROCESS_CAP_MODELS_V1 (UINT32_C(1) << 2u)
#define OCUDU_DAPP_PROCESS_CAP_NO_NEW_PRIVILEGES_V1 (UINT32_C(1) << 3u)
#define OCUDU_DAPP_PROCESS_CAP_PARENT_DEATH_SIGNAL_V1 (UINT32_C(1) << 4u)
#define OCUDU_DAPP_PROCESS_CAP_CUDA_IPC_V1 (UINT32_C(1) << 5u)
#define OCUDU_DAPP_PROCESS_CAP_MULTI_INTERFACE_V1 (UINT32_C(1) << 6u)
#define OCUDU_DAPP_PROCESS_REQUIRED_CAPABILITIES_V1                                                                    \
  (OCUDU_DAPP_PROCESS_CAP_HEARTBEAT_V1 | OCUDU_DAPP_PROCESS_CAP_CONFIG_V1 | OCUDU_DAPP_PROCESS_CAP_MODELS_V1 |         \
   OCUDU_DAPP_PROCESS_CAP_NO_NEW_PRIVILEGES_V1 | OCUDU_DAPP_PROCESS_CAP_PARENT_DEATH_SIGNAL_V1)
#define OCUDU_DAPP_PROCESS_MANAGEMENT_DESCRIBE_CONFIG_V1 1u
#define OCUDU_DAPP_PROCESS_MANAGEMENT_GET_CONFIG_V1 2u
#define OCUDU_DAPP_PROCESS_MANAGEMENT_SET_CONFIG_V1 3u
#define OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_V1 4u
#define OCUDU_DAPP_PROCESS_MANAGEMENT_DRY_RUN_V1 (UINT32_C(1) << 0u)
#define OCUDU_DAPP_PROCESS_MANAGEMENT_MORE_V1 (UINT32_C(1) << 1u)
#define OCUDU_DAPP_PROCESS_MANAGEMENT_MAX_ENTRIES_V1 4u
#define OCUDU_DAPP_PROCESS_MANAGEMENT_KEY_BYTES_V1 129u
#define OCUDU_DAPP_PROCESS_MANAGEMENT_DESCRIPTION_BYTES_V1 257u
#define OCUDU_DAPP_PROCESS_MANAGEMENT_VALUE_BYTES_V1 512u
#define OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_ID_BYTES_V1 129u
#define OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_VERSION_BYTES_V1 65u
#define OCUDU_DAPP_PROCESS_MANAGEMENT_DETAIL_BYTES_V1 193u
#define OCUDU_DAPP_ROUTE_DESCRIPTOR_DATA_V1 0u
#define OCUDU_DAPP_ROUTE_DESCRIPTOR_NOTIFICATION_V1 1u
#define OCUDU_DAPP_ROUTE_DESCRIPTOR_COMPLETION_V1 2u
#define OCUDU_DAPP_ROUTE_DESCRIPTOR_ACCELERATOR_EXPORT_V1 3u
#define OCUDU_DAPP_ROUTE_DESCRIPTOR_COUNT_V1 3u
#define OCUDU_DAPP_ROUTE_DESCRIPTOR_CUDA_COUNT_V1 4u
#define OCUDU_DAPP_ROUTE_DESCRIPTOR_MAX_COUNT_V1 4u
#define OCUDU_DAPP_CLASS_C_PUBLICATION_SCHEMA_V1 UINT32_C(0x00030001)
#define OCUDU_DAPP_ROUTE_FLAG_UL_RESOURCE_PLAN_V1 (UINT32_C(1) << 0u)
#define OCUDU_DAPP_ROUTE_TRANSPORT_CPU_SHARED_RING_V1 0u
#define OCUDU_DAPP_ROUTE_TRANSPORT_CUDA_IPC_EXPORT_V1 1u
#define OCUDU_DAPP_PROCESS_BINDING_REQUIRED_V1 0u
#define OCUDU_DAPP_PROCESS_BINDING_OPTIONAL_V1 1u
#define OCUDU_DAPP_PROCESS_BINDING_BOUND_V1 1u
#define OCUDU_DAPP_PROCESS_BINDING_UNAVAILABLE_V1 2u
#define OCUDU_DAPP_PROCESS_MAX_INTERFACES_V1 64u

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t type;
  uint32_t payload_size;
  uint16_t descriptor_count;
  uint16_t reserved;
  uint64_t sequence;
} ocudu_dapp_control_header_v1;

typedef struct {
  uint32_t struct_size;
  uint32_t interface_id;
  uint32_t metadata_schema_id;
  uint32_t metadata_schema_version;
  uint64_t route_id;
  uint32_t flags;
  uint16_t producer_shard_id;
  uint16_t producer_shard_count;
  uint32_t transport_kind;
  uint32_t backend_id;
  uint32_t export_slot_count;
  uint32_t reserved0;
  uint64_t export_slot_capacity;
  uint64_t reserved;
} ocudu_dapp_route_offer_v1;

typedef struct {
  uint32_t struct_size;
  uint32_t interface_id;
  uint64_t route_generation;
  uint16_t producer_shard_count;
  uint16_t reserved0;
  uint32_t flags;
  uint64_t reserved;
} ocudu_dapp_route_ready_v1;

/// One manifest-ordered interface in a multi-interface process route bundle.
typedef struct {
  uint32_t interface_id;
  uint16_t route_count;
  uint8_t  requirement;
  uint8_t  state;
} ocudu_dapp_process_interface_binding_v1;

/// Sent before route offers when one process package declares multiple interfaces.
/// Single-interface workers retain the original route-offer/route-ready handshake.
typedef struct {
  uint32_t                                struct_size;
  uint16_t                                interface_count;
  uint16_t                                route_count;
  uint32_t                                flags;
  uint32_t                                reserved0;
  uint64_t                                route_generation;
  uint64_t                                bundle_id;
  ocudu_dapp_process_interface_binding_v1 interfaces[OCUDU_DAPP_PROCESS_MAX_INTERFACES_V1];
  uint64_t                                reserved[4];
} ocudu_dapp_route_bundle_v1;

/// Authenticated acknowledgement of the complete manifest-ordered route bundle.
typedef struct {
  uint32_t struct_size;
  uint16_t interface_count;
  uint16_t route_count;
  uint32_t flags;
  uint32_t reserved0;
  uint64_t route_generation;
  uint64_t bundle_id;
  uint64_t reserved[2];
} ocudu_dapp_route_bundle_ready_v1;

/// Authenticated liveness challenge and response. The worker returns the
/// request unchanged; the host owns challenge generation and timing.
typedef struct {
  uint32_t struct_size;
  uint32_t flags;
  uint64_t route_generation;
  uint64_t challenge;
  uint64_t host_monotonic_ns;
  uint64_t reserved[2];
} ocudu_dapp_process_heartbeat_v1;

/// Pointer-free value used only on the authenticated local process-control wire.
typedef struct {
  uint32_t kind;
  uint32_t size;
  union {
    uint8_t  boolean;
    int64_t  int64_value;
    uint64_t uint64_value;
    double   double_value;
    uint8_t  bytes[OCUDU_DAPP_PROCESS_MANAGEMENT_VALUE_BYTES_V1];
  } value;
  uint64_t reserved[2];
} ocudu_dapp_process_value_v1;

typedef struct {
  char                        key[OCUDU_DAPP_PROCESS_MANAGEMENT_KEY_BYTES_V1];
  ocudu_dapp_process_value_v1 value;
} ocudu_dapp_process_config_entry_v1;

typedef struct {
  char                        key[OCUDU_DAPP_PROCESS_MANAGEMENT_KEY_BYTES_V1];
  char                        description[OCUDU_DAPP_PROCESS_MANAGEMENT_DESCRIPTION_BYTES_V1];
  uint32_t                    value_kind;
  uint32_t                    flags;
  ocudu_dapp_process_value_v1 default_value;
  ocudu_dapp_process_value_v1 minimum_value;
  ocudu_dapp_process_value_v1 maximum_value;
  uint64_t                    reserved[4];
} ocudu_dapp_process_config_descriptor_v1;

/// Model bytes, when present, are transferred as one sealed read-only descriptor.
typedef struct {
  uint32_t                           struct_size;
  uint32_t                           operation;
  uint64_t                           route_generation;
  uint64_t                           request_id;
  uint64_t                           expected_generation;
  uint32_t                           boundary_or_action;
  uint32_t                           entry_count;
  uint32_t                           flags;
  uint32_t                           item_offset;
  ocudu_dapp_process_config_entry_v1 entries[OCUDU_DAPP_PROCESS_MANAGEMENT_MAX_ENTRIES_V1];
  char                               model_id[OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_ID_BYTES_V1];
  char                               model_version[OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_VERSION_BYTES_V1];
  uint64_t                           artifact_size;
  uint64_t                           reserved[4];
} ocudu_dapp_process_management_request_v1;

typedef union {
  ocudu_dapp_process_config_descriptor_v1 descriptor;
  ocudu_dapp_process_config_entry_v1      entries[OCUDU_DAPP_PROCESS_MANAGEMENT_MAX_ENTRIES_V1];
} ocudu_dapp_process_management_payload_v1;

typedef struct {
  uint32_t                                 struct_size;
  uint32_t                                 operation;
  int32_t                                  status;
  uint32_t                                 count;
  uint64_t                                 route_generation;
  uint64_t                                 request_id;
  uint64_t                                 resulting_generation;
  uint32_t                                 flags;
  uint32_t                                 reserved0;
  char                                     detail[OCUDU_DAPP_PROCESS_MANAGEMENT_DETAIL_BYTES_V1];
  ocudu_dapp_process_management_payload_v1 payload;
  uint64_t                                 reserved[4];
} ocudu_dapp_process_management_response_v1;

/// Pointer-free, bounded Class-C spectrum result returned on the authenticated control channel.
/// The host authenticates producer identity and generation; neither is supplied by the worker.
typedef struct {
  uint32_t                 struct_size;
  uint32_t                 schema_id;
  uint64_t                 route_id;
  uint64_t                 source_grid_sequence;
  uint64_t                 source_plan_generation;
  uint32_t                 cell_id;
  uint32_t                 confidence_q16;
  ocudu_dapp_radio_time_v1 created;
  ocudu_dapp_radio_time_v1 valid_from;
  ocudu_dapp_radio_time_v1 valid_until;
  uint32_t                 first_prb;
  uint32_t                 nof_prbs;
  uint8_t                  prb_confidence[OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1];
  uint8_t                  reserved0[5];
  uint64_t                 reserved[4];
} ocudu_dapp_process_spectrum_result_v1;

/// Source-correlated quiet intent returned by an isolated spectrum worker.
typedef struct {
  uint32_t                          struct_size;
  uint32_t                          schema_id;
  uint64_t                          route_id;
  uint64_t                          source_grid_sequence;
  uint64_t                          source_plan_generation;
  uint32_t                          cell_id;
  uint32_t                          reserved0;
  ocudu_dapp_radio_time_v1          created;
  ocudu_dapp_quiet_period_intent_v1 intent;
  uint64_t                          reserved[4];
} ocudu_dapp_process_quiet_intent_result_v1;

/// Pointer-free SRS CIR preview returned on the same authenticated control channel.
typedef struct {
  uint32_t                           struct_size;
  uint32_t                           schema_id;
  uint64_t                           route_id;
  ocudu_dapp_srs_isac_cir_preview_v1 preview;
  uint64_t                           reserved[2];
} ocudu_dapp_process_srs_isac_result_v1;

#if defined(__cplusplus)
static_assert(sizeof(ocudu_dapp_control_header_v1) == 24, "Class-C control header layout changed");
static_assert(sizeof(ocudu_dapp_route_offer_v1) == 64, "Class-C route offer layout changed");
static_assert(sizeof(ocudu_dapp_route_ready_v1) == 32, "Class-C route ready layout changed");
static_assert(sizeof(ocudu_dapp_process_interface_binding_v1) == 8, "Class-C process binding layout changed");
static_assert(sizeof(ocudu_dapp_route_bundle_v1) == 576, "Class-C route bundle layout changed");
static_assert(sizeof(ocudu_dapp_route_bundle_ready_v1) == 48, "Class-C route bundle ready layout changed");
static_assert(sizeof(ocudu_dapp_process_heartbeat_v1) == 48, "Class-C heartbeat layout changed");
static_assert(sizeof(ocudu_dapp_process_value_v1) == 536, "Class-C management value layout changed");
static_assert(sizeof(ocudu_dapp_process_config_entry_v1) == 672, "Class-C config entry layout changed");
static_assert(sizeof(ocudu_dapp_process_config_descriptor_v1) == 2040, "Class-C config descriptor layout changed");
static_assert(sizeof(ocudu_dapp_process_management_request_v1) == 2976, "Class-C management request layout changed");
static_assert(sizeof(ocudu_dapp_process_management_response_v1) == 2968, "Class-C management response layout changed");
static_assert(sizeof(ocudu_dapp_process_spectrum_result_v1) == 384, "Class-C spectrum result layout changed");
static_assert(sizeof(ocudu_dapp_process_quiet_intent_result_v1) == 200, "Class-C quiet intent result layout changed");
static_assert(sizeof(ocudu_dapp_srs_isac_cir_preview_v1) == 456, "SRS CIR preview layout changed");
static_assert(sizeof(ocudu_dapp_process_srs_isac_result_v1) == 488, "Class-C SRS result layout changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ocudu_dapp_control_header_v1) == 24, "Class-C control header layout changed");
_Static_assert(sizeof(ocudu_dapp_route_offer_v1) == 64, "Class-C route offer layout changed");
_Static_assert(sizeof(ocudu_dapp_route_ready_v1) == 32, "Class-C route ready layout changed");
_Static_assert(sizeof(ocudu_dapp_process_interface_binding_v1) == 8, "Class-C process binding layout changed");
_Static_assert(sizeof(ocudu_dapp_route_bundle_v1) == 576, "Class-C route bundle layout changed");
_Static_assert(sizeof(ocudu_dapp_route_bundle_ready_v1) == 48, "Class-C route bundle ready layout changed");
_Static_assert(sizeof(ocudu_dapp_process_heartbeat_v1) == 48, "Class-C heartbeat layout changed");
_Static_assert(sizeof(ocudu_dapp_process_value_v1) == 536, "Class-C management value layout changed");
_Static_assert(sizeof(ocudu_dapp_process_config_entry_v1) == 672, "Class-C config entry layout changed");
_Static_assert(sizeof(ocudu_dapp_process_config_descriptor_v1) == 2040, "Class-C config descriptor layout changed");
_Static_assert(sizeof(ocudu_dapp_process_management_request_v1) == 2976, "Class-C management request layout changed");
_Static_assert(sizeof(ocudu_dapp_process_management_response_v1) == 2968, "Class-C management response layout changed");
_Static_assert(sizeof(ocudu_dapp_process_spectrum_result_v1) == 384, "Class-C spectrum result layout changed");
_Static_assert(sizeof(ocudu_dapp_process_quiet_intent_result_v1) == 200, "Class-C quiet intent result layout changed");
_Static_assert(sizeof(ocudu_dapp_srs_isac_cir_preview_v1) == 456, "SRS CIR preview layout changed");
_Static_assert(sizeof(ocudu_dapp_process_srs_isac_result_v1) == 488, "Class-C SRS result layout changed");
#endif

#ifdef __cplusplus
}
#endif
#endif
