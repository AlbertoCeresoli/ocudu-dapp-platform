// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#ifndef OCUDU_DAPP_USE_CASES_V1_EQUALIZER_H
#define OCUDU_DAPP_USE_CASES_V1_EQUALIZER_H
#include "common.h"
#include "pusch_metadata.h"
#ifdef __cplusplus
extern "C" {
#endif
#define OCUDU_DAPP_INTERFACE_EQUALIZER_V1 0x00010004u
typedef struct {
  ocudu_dapp_invocation_v1     invocation;
  ocudu_dapp_tensor_view_v1    rx_grid;
  ocudu_dapp_typed_metadata_v1 pusch;
  ocudu_dapp_typed_metadata_v1 dmrs;
} ocudu_dapp_equalizer_input_v1;
typedef struct {
  /// Caller-owned writable CF32 [rx_port, layer, symbol, subcarrier] tensor.
  ocudu_dapp_tensor_view_v1 channel_estimates;
  /// Caller-owned writable CF32 [layer, symbol, subcarrier], or compact [data_re, layer]
  /// tensor with OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1.
  ocudu_dapp_tensor_view_v1 equalized_symbols;
  /// Shape-matched writable F32 noise tensor. Compact output uses
  /// OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1.
  ocudu_dapp_tensor_view_v1 post_equalization_noise;
  uint64_t                  reserved[4];
} ocudu_dapp_equalizer_output_v1;
typedef ocudu_dapp_status_v1 (*ocudu_dapp_equalizer_invoke_v1)(void*,
                                                               const ocudu_dapp_equalizer_input_v1*,
                                                               ocudu_dapp_equalizer_output_v1*);
typedef struct {
  ocudu_dapp_worker_api_v1       worker;
  ocudu_dapp_equalizer_invoke_v1 invoke;
  uint64_t                       reserved[8];
} ocudu_dapp_equalizer_interface_v1;
#ifdef __cplusplus
}
#endif
#endif
