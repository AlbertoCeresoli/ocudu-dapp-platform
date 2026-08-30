// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

/// \file execution.h
/// Per-invocation context and the provenance header carried on published
/// context objects.

#ifndef OCUDU_DAPP_ABI_V1_EXECUTION_H
#define OCUDU_DAPP_ABI_V1_EXECUTION_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Delivered with every data-path invocation.
///
/// Real-time contract: finish (or enqueue and return) before
/// \c deadline_monotonic_ns (same clock as host_api monotonic_ns). For
/// accelerated Class-A calls, \c execution_stream is the host's stream for
/// this lane (e.g. a cudaStream_t value): enqueue on it and return — never
/// synchronize, never allocate on this path. \c worker_id identifies the
/// lane (stable per worker; lane-local state needs no locks under
/// PER_WORKER threading). \c input_generation / \c output_generation tie
/// the invocation to the tensor snapshots it may read/write.
typedef struct {
  uint32_t                 struct_size;
  uint16_t                 abi_major;
  uint16_t                 abi_minor;
  uint64_t                 invocation_id; ///< Unique per invocation (incident attribution).
  uint32_t                 cell_id;
  uint32_t                 worker_id;
  ocudu_dapp_radio_time_v1 radio_time;
  uint64_t                 deadline_monotonic_ns;
  uint64_t                 execution_stream; ///< Backend stream handle; 0 = none.
  uint64_t                 input_generation;
  uint64_t                 output_generation;
  uint64_t                 reserved[6]; ///< Must be zero.
} ocudu_dapp_invocation_v1;

/// Provenance header prefixed to every published context object (interference
/// maps, quiet-period intents, Class-C results).
///
/// \c schema_id / \c schema_version name the typed payload that follows.
/// \c producer_instance_id (16 bytes) + \c producer_generation attribute the
/// object to one instance incarnation — consumers drop objects whose producer
/// generation no longer matches. \c source_sequence is the producer's
/// monotonic sequence (gaps are visible, not errors). \c valid_from /
/// \c valid_until bound the object's applicability in radio time;
/// \c confidence_q16 is Q16 in [0, 65536]. \c provenance_hash is an opaque
/// integrity discriminator; \c reserved is zero.
typedef struct {
  uint32_t                 struct_size;
  uint32_t                 schema_id;
  uint32_t                 schema_version;
  uint32_t                 flags;
  uint64_t                 producer_instance_id[2];
  uint64_t                 producer_generation;
  uint64_t                 source_sequence;
  uint32_t                 cell_id;
  uint32_t                 confidence_q16;
  ocudu_dapp_radio_time_v1 created;
  ocudu_dapp_radio_time_v1 valid_from;
  ocudu_dapp_radio_time_v1 valid_until;
  uint64_t                 provenance_hash;
  uint64_t                 reserved[4]; ///< Must be zero.
} ocudu_dapp_context_header_v1;

#ifdef __cplusplus
}
#endif
#endif
