// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef OCUDU_DAPP_ABI_V1_MANAGEMENT_H
#define OCUDU_DAPP_ABI_V1_MANAGEMENT_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OCUDU_DAPP_MANAGEMENT_MAJOR_V1 1u
#define OCUDU_DAPP_MANAGEMENT_MINOR_V1 0u

#define OCUDU_DAPP_MANAGEMENT_CONFIG_V1 (1ull << 0u)
#define OCUDU_DAPP_MANAGEMENT_METRICS_V1 (1ull << 1u)
#define OCUDU_DAPP_MANAGEMENT_MODELS_V1 (1ull << 2u)

#define OCUDU_DAPP_CONFIG_SECRET_V1 (1u << 0u)
#define OCUDU_DAPP_CONFIG_RESTART_REQUIRED_V1 (1u << 1u)
#define OCUDU_DAPP_CONFIG_READ_ONLY_V1 (1u << 2u)

/// A cumulative value that never decreases during one instance generation.
/// Monitoring systems derive rates from successive samples and timestamps.
#define OCUDU_DAPP_METRIC_COUNTER_V1 (1u << 0u)
/// A point-in-time value that may increase or decrease.
#define OCUDU_DAPP_METRIC_GAUGE_V1 (1u << 1u)

typedef enum {
  OCUDU_DAPP_VALUE_BOOL_V1   = 1,
  OCUDU_DAPP_VALUE_INT64_V1  = 2,
  OCUDU_DAPP_VALUE_UINT64_V1 = 3,
  OCUDU_DAPP_VALUE_DOUBLE_V1 = 4,
  OCUDU_DAPP_VALUE_STRING_V1 = 5,
  OCUDU_DAPP_VALUE_BYTES_V1  = 6
} ocudu_dapp_value_kind_v1;

typedef struct {
  uint32_t kind;
  uint32_t reserved0;
  union {
    uint8_t                 boolean;
    int64_t                 int64_value;
    uint64_t                uint64_value;
    double                  double_value;
    ocudu_dapp_blob_view_v1 blob;
  } value;
  uint64_t reserved[2];
} ocudu_dapp_value_v1;

typedef struct {
  uint32_t            struct_size;
  const char*         key;
  const char*         description;
  uint32_t            value_kind;
  uint32_t            flags;
  ocudu_dapp_value_v1 default_value;
  ocudu_dapp_value_v1 minimum_value;
  ocudu_dapp_value_v1 maximum_value;
  uint64_t            reserved[4];
} ocudu_dapp_config_descriptor_v1;

typedef struct {
  const char*         key;
  ocudu_dapp_value_v1 value;
  uint64_t            reserved[2];
} ocudu_dapp_config_entry_v1;

typedef enum {
  OCUDU_DAPP_CONFIG_BOUNDARY_IMMEDIATE_V1  = 1,
  OCUDU_DAPP_CONFIG_BOUNDARY_INVOCATION_V1 = 2,
  OCUDU_DAPP_CONFIG_BOUNDARY_SLOT_V1       = 3,
  OCUDU_DAPP_CONFIG_BOUNDARY_RESTART_V1    = 4
} ocudu_dapp_config_boundary_v1;

typedef struct {
  uint32_t                          struct_size;
  uint32_t                          nof_entries;
  const ocudu_dapp_config_entry_v1* entries;
  uint64_t                          expected_generation;
  uint32_t                          boundary;
  uint8_t                           dry_run;
  uint8_t                           reserved0[3];
  uint64_t                          reserved[4];
} ocudu_dapp_config_update_v1;

typedef struct {
  uint32_t    struct_size;
  const char* name;
  const char* description;
  const char* unit;
  uint32_t    value_kind;
  uint32_t    flags;
  uint64_t    reserved[4];
} ocudu_dapp_metric_descriptor_v1;

typedef struct {
  const char*         name;
  ocudu_dapp_value_v1 value;
  uint64_t            monotonic_timestamp_ns;
  uint64_t            reserved[2];
} ocudu_dapp_metric_sample_v1;

typedef enum {
  OCUDU_DAPP_MODEL_STAGE_V1    = 1,
  OCUDU_DAPP_MODEL_VALIDATE_V1 = 2,
  OCUDU_DAPP_MODEL_WARM_V1     = 3,
  OCUDU_DAPP_MODEL_ACTIVATE_V1 = 4,
  OCUDU_DAPP_MODEL_ROLLBACK_V1 = 5,
  OCUDU_DAPP_MODEL_RETIRE_V1   = 6
} ocudu_dapp_model_action_v1;

typedef struct {
  uint32_t                struct_size;
  uint32_t                action;
  const char*             model_id;
  const char*             model_version;
  ocudu_dapp_blob_view_v1 artifact;
  uint64_t                expected_generation;
  uint8_t                 dry_run;
  uint8_t                 reserved0[7];
  uint64_t                reserved[4];
} ocudu_dapp_model_request_v1;

typedef struct {
  uint32_t struct_size;
  uint16_t major;
  uint16_t minor;
  uint64_t capability_bits;
  ocudu_dapp_status_v1 (*describe_config)(void*                            instance,
                                          ocudu_dapp_config_descriptor_v1* descriptors,
                                          uint32_t                         capacity,
                                          uint32_t*                        count);
  ocudu_dapp_status_v1 (*get_config)(void*                       instance,
                                     ocudu_dapp_config_entry_v1* entries,
                                     uint32_t                    capacity,
                                     uint32_t*                   count,
                                     uint64_t*                   generation);
  ocudu_dapp_status_v1 (*set_config)(void*                              instance,
                                     const ocudu_dapp_config_update_v1* update,
                                     uint64_t*                          resulting_generation);
  ocudu_dapp_status_v1 (*describe_metrics)(void*                            instance,
                                           ocudu_dapp_metric_descriptor_v1* descriptors,
                                           uint32_t                         capacity,
                                           uint32_t*                        count);
  ocudu_dapp_status_v1 (*read_metrics)(void*                        instance,
                                       ocudu_dapp_metric_sample_v1* samples,
                                       uint32_t                     capacity,
                                       uint32_t*                    count);
  ocudu_dapp_status_v1 (*manage_model)(void*                              instance,
                                       const ocudu_dapp_model_request_v1* request,
                                       uint64_t*                          resulting_generation);
  uint64_t reserved[10];
} ocudu_dapp_management_v1;

#ifdef __cplusplus
}
#endif
#endif
