// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#ifndef OCUDU_DAPP_USE_CASES_V1_RECEIVER_H
#define OCUDU_DAPP_USE_CASES_V1_RECEIVER_H
#include "common.h"
#include "pusch_metadata.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define OCUDU_DAPP_INTERFACE_RECEIVER_V1 0x00010005u
#define OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1 0x00010103u

/// The receiver filled the requested post-equalization-noise tensor.
#define OCUDU_DAPP_RECEIVER_METRICS_VALID_V1 (1u << 0)
/// The receiver filled the optional caller-owned equalized-symbol tensor.
#define OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_VALID_V1 (1u << 1)
#define OCUDU_DAPP_RECEIVER_KNOWN_OUTPUT_FLAGS_V1                                                                      \
  (OCUDU_DAPP_RECEIVER_METRICS_VALID_V1 | OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_VALID_V1)

/// Optional receiver auxiliary-output storage supplied and owned by the caller.
///
/// post_equalization_noise is F32 [data_re, layer], using
/// OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1. A module that fills
/// the complete tensor sets OCUDU_DAPP_RECEIVER_METRICS_VALID_V1 in the parent
/// output's reserved0 field. Leaving that bit clear is supported and means the
/// receiver produced LLRs without scheduler-facing post-EQ noise.
typedef struct {
  uint32_t                  struct_size;
  uint16_t                  abi_major;
  uint16_t                  abi_minor;
  ocudu_dapp_tensor_view_v1 post_equalization_noise;
  uint64_t                  reserved[4];
  /// Optional append-only extension: CF32 [data_re, layer], using
  /// OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1, in the same RE/layer
  /// order as llrs and post_equalization_noise. A zero struct_size means no
  /// request. Access only when both the metadata extent and this struct_size
  /// cover the complete field. Older prefix-only objects remain valid.
  ///
  /// The caller owns storage, stream and completion lifetime. A receiver may
  /// leave this unfilled even when requested (for example, direct-bit neural
  /// receivers without an explicit symbol estimate). Set EQUALIZED_SYMBOLS_VALID
  /// only after enqueueing the complete output on the invocation stream; it
  /// does not assert completion or physical noise/SINR authority. The host must
  /// not consume missing/invalid output or substitute stale baseline symbols.
  /// Symbols are unsliced estimates in the transmitted QAM symbol domain,
  /// not hard decisions or variance-normalized neural features. Invalid REs
  /// may be represented by finite zero erasures. Storage must not overlap
  /// inputs, LLRs, noise, metadata or the receiver's private workspace.
  ocudu_dapp_tensor_view_v1 equalized_symbols;
} ocudu_dapp_receiver_metrics_v1;

#define OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_MIN_SIZE_V1                                                              \
  (offsetof(ocudu_dapp_receiver_metrics_v1, equalized_symbols) + sizeof(ocudu_dapp_tensor_view_v1))

typedef struct {
  ocudu_dapp_invocation_v1     invocation;
  ocudu_dapp_tensor_view_v1    rx_grid;
  ocudu_dapp_typed_metadata_v1 pusch;
  ocudu_dapp_typed_metadata_v1 dmrs;
} ocudu_dapp_receiver_input_v1;
typedef struct {
  /// Caller-owned writable soft-bit tensor before scrambling reversal.
  ///
  /// Host invocations use I8 [data_re * modulation_order]. Accelerator-resident invocations use F16
  /// [data_re, layer, bit] with OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1. In both cases, bit is
  /// the fastest-varying dimension, positive values favor bit zero, negative values favor bit one, and magnitude
  /// expresses reliability. Values must be finite (and I8 must not use -128). The host retains ownership of storage
  /// and completion signaling.
  ocudu_dapp_tensor_view_v1 llrs;
  /// Reserved for a future versioned UCI result schema. Must be all-zero in interface version 1.
  ocudu_dapp_typed_metadata_v1 uci;
  /// Optional OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1 caller-owned output request. Empty remains valid.
  ocudu_dapp_typed_metadata_v1 metrics;
  /// Optional advisory confidence in Q16 [0, 1]. Zero means unspecified; the v1 host does not gate decoding on it.
  uint32_t confidence_q16;
  /// OCUDU_DAPP_RECEIVER_* output flags. Must contain only known bits.
  uint32_t reserved0;
} ocudu_dapp_receiver_output_v1;
typedef ocudu_dapp_status_v1 (*ocudu_dapp_receiver_invoke_v1)(void*,
                                                              const ocudu_dapp_receiver_input_v1*,
                                                              ocudu_dapp_receiver_output_v1*);
typedef struct {
  ocudu_dapp_worker_api_v1      worker;
  ocudu_dapp_receiver_invoke_v1 invoke;
  uint64_t                      reserved[8];
} ocudu_dapp_receiver_interface_v1;
#ifdef __cplusplus
}
#endif
#endif
