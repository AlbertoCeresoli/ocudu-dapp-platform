// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

/// \file base.h
/// Foundation types of the frozen dApp C ABI v1.
///
/// Layout rules (see docs/dapp/api_compatibility.md): every versioned struct
/// opens with \c struct_size / \c abi_major / \c abi_minor, existing fields
/// never change meaning, and evolution happens through trailing \c reserved
/// slots whose zero value must always mean "not provided". Verify
/// \c struct_size >= sizeof(...) and \c abi_major == OCUDU_DAPP_ABI_MAJOR_V1
/// before reading any other field of a struct received across the boundary.

#ifndef OCUDU_DAPP_ABI_V1_BASE_H
#define OCUDU_DAPP_ABI_V1_BASE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OCUDU_DAPP_ABI_MAJOR_V1 1u
#define OCUDU_DAPP_ABI_MINOR_V1 0u
/// Maximum tensor rank carried by ocudu_dapp_tensor_view_v1.
#define OCUDU_DAPP_MAX_TENSOR_RANK_V1 8u

/// Common prologue of every versioned ABI struct. A peer may hand you a LARGER
/// struct_size than you know (extension via trailing fields); never a
/// smaller one.
typedef struct {
  uint32_t struct_size;
  uint16_t abi_major;
  uint16_t abi_minor;
} ocudu_dapp_abi_header_v1;

/// Status codes returned across the ABI. The safety-critical distinction:
/// OCUDU_DAPP_BYPASS_V1 means "I decline this invocation, use the
/// conventional path" and is NOT a failure — returning it for any input you
/// do not handle (instead of crashing or guessing) is the core contract.
/// All other non-OK codes are attributed as incidents and feed the
/// per-instance circuit breaker.
typedef enum {
  OCUDU_DAPP_OK_V1             = 0,
  OCUDU_DAPP_BYPASS_V1         = 1, ///< Declined; host falls back. Never an error.
  OCUDU_DAPP_UNSUPPORTED_V1    = 2, ///< Operation/capability not implemented.
  OCUDU_DAPP_INVALID_INPUT_V1  = 3, ///< Input failed the module's validation.
  OCUDU_DAPP_INVALID_OUTPUT_V1 = 4, ///< Module produced an out-of-contract result.
  OCUDU_DAPP_DEADLINE_V1       = 5, ///< Work exceeded the invocation deadline.
  OCUDU_DAPP_RESOURCE_V1       = 6, ///< Bounded resource exhausted (no allocation retry).
  OCUDU_DAPP_DEVICE_ERROR_V1   = 7, ///< Accelerator/device fault.
  OCUDU_DAPP_EXCEPTION_V1      = 8, ///< An exception was contained at the boundary.
  OCUDU_DAPP_INTERNAL_ERROR_V1 = 9, ///< Invariant violation inside the module.
  OCUDU_DAPP_CONFLICT_V1       = 10 ///< Generation/CAS mismatch; re-read and retry.
} ocudu_dapp_status_v1;

/// The three use-case contracts (see docs/dapp/capabilities.md):
/// A = resident inline L1 (in the slot), B = bounded real-time control with a
/// hard complete-boundary deadline, C = asynchronous observation/advisory
/// (producers never block on it).
typedef enum {
  OCUDU_DAPP_CLASS_A_INLINE_V1    = 1,
  OCUDU_DAPP_CLASS_B_DEADLINE_V1  = 2,
  OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1 = 3
} ocudu_dapp_class_v1;

/// Where the module executes. Classes A and B are in-process only; Class C
/// may also run in a supervised worker process.
typedef enum {
  OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1     = 1,
  OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1 = 2
} ocudu_dapp_placement_v1;

/// Concurrency contract the module declares. PER_WORKER: each host worker
/// lane gets private worker state and calls are never concurrent within a
/// lane (no locking needed for lane-local state). REENTRANT_SHARED: the host
/// may invoke concurrently against shared state; the module synchronizes.
/// SERIALIZED: the host guarantees one invocation at a time globally.
typedef enum {
  OCUDU_DAPP_THREADING_PER_WORKER_V1       = 1,
  OCUDU_DAPP_THREADING_REENTRANT_SHARED_V1 = 2,
  OCUDU_DAPP_THREADING_SERIALIZED_V1       = 3
} ocudu_dapp_threading_v1;

/// Instance lifecycle (transitions are operator-driven and generation-guarded
/// over E3; see docs/dapp/e3_management.md). SHADOW is warmed and
/// configurable with zero traffic exposure; DEGRADED keeps the instance
/// nominally active while the host routes conventionally;
/// QUARANTINED_RESIDENT means quiescence could not be proven and the module
/// will not be unloaded under a live kernel.
typedef enum {
  OCUDU_DAPP_STATE_DISCOVERED_V1           = 1,
  OCUDU_DAPP_STATE_LOADED_V1               = 2,
  OCUDU_DAPP_STATE_PREPARED_V1             = 3,
  OCUDU_DAPP_STATE_ARMED_V1                = 4,
  OCUDU_DAPP_STATE_SHADOW_V1               = 5,
  OCUDU_DAPP_STATE_ACTIVE_V1               = 6,
  OCUDU_DAPP_STATE_DRAINING_V1             = 7,
  OCUDU_DAPP_STATE_INACTIVE_V1             = 8,
  OCUDU_DAPP_STATE_UNLOADED_V1             = 9,
  OCUDU_DAPP_STATE_DEGRADED_V1             = 10,
  OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1 = 11,
  OCUDU_DAPP_STATE_ERROR_V1                = 12
} ocudu_dapp_lifecycle_state_v1;

/// Borrowed, read-only byte range. The pointer is valid only for the duration
/// of the call that delivered it; copy what you need to keep.
typedef struct {
  const void* data;
  uint32_t    size;
  uint32_t    reserved; ///< Must be zero.
} ocudu_dapp_blob_view_v1;

/// Radio timestamp: system frame number, slot within the frame, and the
/// numerology (mu; subcarrier spacing = 15 kHz << numerology).
typedef struct {
  uint16_t sfn;
  uint16_t slot;
  uint16_t numerology;
  uint16_t reserved; ///< Must be zero.
} ocudu_dapp_radio_time_v1;

#ifdef __cplusplus
}
#endif
#endif
