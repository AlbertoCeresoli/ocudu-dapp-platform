// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef OCUDU_DAPP_PROCESS_V1_CUDA_EXPORT_H
#define OCUDU_DAPP_PROCESS_V1_CUDA_EXPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OCUDU_DAPP_CUDA_EXPORT_MAGIC_V1 UINT64_C(0x4f43435544414950)
#define OCUDU_DAPP_CUDA_EXPORT_VERSION_V1 1u
#define OCUDU_DAPP_CUDA_EXPORT_HEADER_BYTES_V1 64u
#define OCUDU_DAPP_CUDA_IPC_HANDLE_BYTES_V1 64u
#define OCUDU_DAPP_CUDA_EXPORT_SLOT_BYTES_V1 192u
#define OCUDU_DAPP_CUDA_EXPORT_MAX_SLOTS_V1 64u

/// Read-only route descriptor passed once during CUDA process admission.
/// The opaque handles are interpreted only by the matching CUDA extension.
typedef struct {
  uint64_t magic;
  uint32_t version;
  uint32_t header_size;
  uint32_t backend_id;
  uint32_t device_ordinal;
  uint32_t slot_count;
  uint32_t slot_stride;
  uint64_t slot_capacity;
  uint64_t reserved[3];
} ocudu_dapp_cuda_export_header_v1;

typedef struct {
  uint32_t struct_size;
  uint32_t slot_id;
  uint64_t allocation_id;
  uint64_t capacity;
  uint8_t  memory_handle[OCUDU_DAPP_CUDA_IPC_HANDLE_BYTES_V1];
  uint8_t  ready_event_handle[OCUDU_DAPP_CUDA_IPC_HANDLE_BYTES_V1];
  uint64_t reserved[5];
} ocudu_dapp_cuda_export_slot_v1;

#if defined(__cplusplus)
static_assert(sizeof(ocudu_dapp_cuda_export_header_v1) == OCUDU_DAPP_CUDA_EXPORT_HEADER_BYTES_V1,
              "CUDA export header layout changed");
static_assert(sizeof(ocudu_dapp_cuda_export_slot_v1) == OCUDU_DAPP_CUDA_EXPORT_SLOT_BYTES_V1,
              "CUDA export slot layout changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ocudu_dapp_cuda_export_header_v1) == OCUDU_DAPP_CUDA_EXPORT_HEADER_BYTES_V1,
               "CUDA export header layout changed");
_Static_assert(sizeof(ocudu_dapp_cuda_export_slot_v1) == OCUDU_DAPP_CUDA_EXPORT_SLOT_BYTES_V1,
               "CUDA export slot layout changed");
#endif

#ifdef __cplusplus
}
#endif
#endif
