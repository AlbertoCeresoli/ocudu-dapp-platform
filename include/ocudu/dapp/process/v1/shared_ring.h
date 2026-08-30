// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef OCUDU_DAPP_PROCESS_V1_SHARED_RING_H
#define OCUDU_DAPP_PROCESS_V1_SHARED_RING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OCUDU_DAPP_SHARED_RING_MAGIC_V1 UINT64_C(0x4f43554455453352)
#define OCUDU_DAPP_SHARED_RING_VERSION_V1 1u
#define OCUDU_DAPP_SHARED_RING_HEADER_BYTES_V1 UINT32_C(128)
#define OCUDU_DAPP_SHARED_RING_ALIGNMENT_V1 UINT32_C(64)
#define OCUDU_DAPP_SHARED_RING_MAX_BYTES_V1 (UINT64_C(1) << 30u)

typedef struct {
  uint64_t magic;
  uint32_t version;
  uint32_t header_size;
  uint64_t mapped_bytes;
  uint32_t slots;
  uint32_t payload_capacity;
  uint64_t slot_stride;
  /* Access these counters only with lock-free acquire/release atomics. */
  uint64_t write_index;
  uint64_t read_index;
  uint64_t epoch;
  uint64_t reserved[5];
} ocudu_dapp_shared_ring_header_v1;

typedef struct {
  uint32_t size;
  uint32_t flags;
  uint64_t sequence;
  uint64_t reserved[6];
} ocudu_dapp_shared_ring_slot_v1;

#if defined(__cplusplus)
static_assert(sizeof(ocudu_dapp_shared_ring_header_v1) == 104, "Class-C ring header layout changed");
static_assert(offsetof(ocudu_dapp_shared_ring_header_v1, write_index) == 40, "Class-C write index moved");
static_assert(offsetof(ocudu_dapp_shared_ring_header_v1, read_index) == 48, "Class-C read index moved");
static_assert(offsetof(ocudu_dapp_shared_ring_header_v1, epoch) == 56, "Class-C epoch moved");
static_assert(sizeof(ocudu_dapp_shared_ring_slot_v1) == 64, "Class-C slot header layout changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ocudu_dapp_shared_ring_header_v1) == 104, "Class-C ring header layout changed");
_Static_assert(offsetof(ocudu_dapp_shared_ring_header_v1, write_index) == 40, "Class-C write index moved");
_Static_assert(offsetof(ocudu_dapp_shared_ring_header_v1, read_index) == 48, "Class-C read index moved");
_Static_assert(offsetof(ocudu_dapp_shared_ring_header_v1, epoch) == 56, "Class-C epoch moved");
_Static_assert(sizeof(ocudu_dapp_shared_ring_slot_v1) == 64, "Class-C slot header layout changed");
#endif

#ifdef __cplusplus
}
#endif
#endif
