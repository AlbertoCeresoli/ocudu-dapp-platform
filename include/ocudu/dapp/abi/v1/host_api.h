// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

/// \file host_api.h
/// The host-services table handed to every module at creation.
///
/// Lifetime: the table and its \c host_context outlive the instance — both
/// remain valid from module creation until the instance's destroy returns.
/// Callbacks may be invoked from any worker lane; each entry below states its
/// own thread-safety and real-time constraints. Function pointers may be NULL
/// when a host does not provide the service: check before calling.

#ifndef OCUDU_DAPP_ABI_V1_HOST_API_H
#define OCUDU_DAPP_ABI_V1_HOST_API_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t struct_size;
  uint16_t abi_major;
  uint16_t abi_minor;
  /// Opaque host handle; pass it back verbatim as the first argument of every
  /// callback. Never dereference or store beyond the instance's lifetime.
  void* host_context;
  /// Reserved capability bits; a zero bit always reads as "capability absent".
  uint64_t capabilities;
  /// Monotonic clock in nanoseconds (CLOCK_MONOTONIC domain, the same one
  /// invocation deadlines are expressed in). Thread-safe, wait-free,
  /// RT-safe to call from the invocation path.
  uint64_t (*monotonic_ns)(void* host_context);
  /// Pushes one counter sample to the host metric sink. Thread-safe and
  /// non-blocking (drops under pressure rather than waiting).
  /// \warning The numeric \p metric_id namespace is vendor-private and
  /// UNREGISTERED — no catalog maps ids to names and independent
  /// vendors may collide. Anything a management consumer must discover or
  /// interpret belongs in the string-named describe_metrics/read_metrics
  /// catalog on the management interface instead.
  void (*metric_add)(void* host_context, uint32_t metric_id, uint64_t value);
  /// Records a lightweight incident token. Thread-safe, non-blocking. The
  /// \p cause id namespace is likewise vendor-private/unregistered;
  /// \p detail_hash is an opaque discriminator (e.g. a hash of the detail).
  void (*incident_token)(void* host_context, uint32_t cause, uint64_t detail_hash);
  /// Publishes one prepared, size-tagged output object (Class-C results,
  /// interference maps, quiet-period intents) to the host authority for the
  /// given output port. The object is copied before return; the host
  /// validates schema_id/object_size and may clamp or reject per policy —
  /// a request is advice until the host accepts it. Thread-safe;
  /// non-blocking towards radio producers.
  ocudu_dapp_status_v1 (*publish_prepared)(void*       host_context,
                                           uint32_t    output_port_id,
                                           uint32_t    schema_id,
                                           const void* typed_object,
                                           uint32_t    object_size);
  /// Negotiates an optional host extension table. On OK, *extension points to
  /// a table owned by the host, valid for the instance lifetime. Unknown
  /// extension ids return OCUDU_DAPP_UNSUPPORTED_V1; call at setup, not on
  /// the invocation path.
  ocudu_dapp_status_v1 (*query_extension)(void*        host_context,
                                          uint32_t     extension_id,
                                          uint16_t     extension_major,
                                          const void** extension);
  /// Must be zero; a zero slot always reads as "service absent".
  uint64_t reserved[15];
} ocudu_dapp_host_api_v1;

#ifdef __cplusplus
}
#endif
#endif
