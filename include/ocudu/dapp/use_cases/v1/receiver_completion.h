// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#ifndef OCUDU_DAPP_USE_CASES_V1_RECEIVER_COMPLETION_H
#define OCUDU_DAPP_USE_CASES_V1_RECEIVER_COMPLETION_H

#include "receiver.h"
#include "ocudu/dapp/abi/v1/module.h"
#ifdef __cplusplus
extern "C" {
#endif

/// Opt-in receiver capability, independent of the unchanged invoke-only v1 table.
#define OCUDU_DAPP_RECEIVER_CAP_COMPLETION_V1 (UINT64_C(1) << 1u)
/// A module requiring completion MUST reject query on a host without this bit.
/// Hosts advertise it only after their actual PHY consumer/lifetime adapter is wired.
#define OCUDU_DAPP_HOST_CAP_RECEIVER_COMPLETION_V1 (UINT64_C(1) << 0u)
#define OCUDU_DAPP_RECEIVER_COMPLETION_QUERY_SYMBOL_V1 "ocudu_dapp_query_receiver_completion_v1"
#define OCUDU_DAPP_RECEIVER_RECEIPT_WORDS_V1 8u

/// Bounded opaque module receipt. Host owns and retains the original bytes;
/// never transmit through E3 or interpret as a pointer/authenticated capability.
/// At least one opaque word must be nonzero on successful invocation. The
/// original invocation identity is passed separately on inspect/release.
typedef struct {
  uint32_t struct_size;
  uint16_t abi_major;
  uint16_t abi_minor;
  uint64_t opaque[OCUDU_DAPP_RECEIVER_RECEIPT_WORDS_V1];
} ocudu_dapp_receiver_receipt_v1;

/// Only status OK, ready=1 AND valid=1 permits CRC/HARQ/CSI/payload acceptance.
/// inspect never acknowledges a receipt or releases its tensor storage.
typedef struct {
  uint32_t struct_size;
  uint16_t abi_major;
  uint16_t abi_minor;
  uint32_t ready;
  uint32_t valid;
  uint64_t reserved[4];
} ocudu_dapp_receiver_completion_result_v1;

/// Separately queried C table; no reserved v1 slot is reinterpreted.
/// Uses the SAME worker context as the receiver interface, not a second lane.
/// Host supplies valid, disjoint, size-tagged HOST receipt/result storage.
/// Invoke/inspect/release are nonblocking; RESOURCE means retain ownership and retry.
/// Control-only retirement may run backend destruction with no real-time wall-clock guarantee.
/// Completion/release remain callable after admission closes and breakers open.
typedef struct {
  uint32_t struct_size;
  uint16_t abi_major;
  uint16_t abi_minor;
  /// On success publishes a receipt only after the whole asynchronous tail is
  /// enqueued. BYPASS/UNSUPPORTED/INVALID_INPUT/RESOURCE assert NOTHING queued.
  /// Any possible partial submission must return DEVICE_ERROR. Exceptions,
  /// malformed outputs and uncertain errors require quarantine, not overwrite.
  ocudu_dapp_status_v1 (*invoke)(void*, const ocudu_dapp_receiver_input_v1*,
                                ocudu_dapp_receiver_output_v1*, ocudu_dapp_receiver_receipt_v1*);
  ocudu_dapp_status_v1 (*inspect)(void*, const ocudu_dapp_invocation_v1*,
                                 const ocudu_dapp_receiver_receipt_v1*,
                                 ocudu_dapp_receiver_completion_result_v1*);
  /// Host calls ONLY after every downstream consumer has finished/fenced.
  /// OK acknowledges the receipt even if its output was invalid. Other statuses
  /// retain it. Acknowledgment is NOT a global host pool/stream/module release.
  ocudu_dapp_status_v1 (*release)(void*, const ocudu_dapp_invocation_v1*,
                                 const ocudu_dapp_receiver_receipt_v1*);
  /// Control-path only, after withdrawing every consumer. Fences/retire-retries
  /// all grants, including partial submissions without receipts. OK makes all
  /// former receipts unusable and permanently closes this worker to new work.
  /// Does not destroy the C worker context; normal destroy follows retirement.
  ocudu_dapp_status_v1 (*retire)(void*);
  uint64_t reserved[8];
} ocudu_dapp_receiver_completion_api_v1;

typedef ocudu_dapp_status_v1 (*ocudu_dapp_query_receiver_completion_v1_fn)(
    const ocudu_dapp_host_info_v1*, ocudu_dapp_receiver_completion_api_v1*);
OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_receiver_completion_v1(
    const ocudu_dapp_host_info_v1*, ocudu_dapp_receiver_completion_api_v1*);

#ifdef __cplusplus
}
#endif
#endif
