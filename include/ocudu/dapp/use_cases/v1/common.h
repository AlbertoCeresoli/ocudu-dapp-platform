// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef OCUDU_DAPP_USE_CASES_V1_COMMON_H
#define OCUDU_DAPP_USE_CASES_V1_COMMON_H

#include "ocudu/dapp/abi/v1/execution.h"
#include "ocudu/dapp/abi/v1/memory.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Canonical schema and tensor layout shared by sensing producers and scheduler consumers.
#define OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1 0x00010001u
#define OCUDU_DAPP_LAYOUT_PRB_INTERFERENCE_U8_V1 0x00010001u

typedef struct {
  uint32_t                struct_size;
  uint16_t                abi_major;
  uint16_t                abi_minor;
  uint32_t                metadata_schema_id;
  uint32_t                metadata_schema_version;
  ocudu_dapp_blob_view_v1 metadata;
} ocudu_dapp_typed_metadata_v1;

typedef struct {
  uint32_t struct_size;
  uint16_t abi_major;
  uint16_t abi_minor;
  ocudu_dapp_status_v1 (*create_worker_context)(void* instance, uint32_t worker_id, void** worker_context);
  void (*destroy_worker_context)(void* instance, void* worker_context);
  uint64_t reserved[6];
} ocudu_dapp_worker_api_v1;

#ifdef __cplusplus
}
#endif
#endif
