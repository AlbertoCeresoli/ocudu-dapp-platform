// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#ifndef OCUDU_DAPP_USE_CASES_V1_CHANNEL_ESTIMATOR_H
#define OCUDU_DAPP_USE_CASES_V1_CHANNEL_ESTIMATOR_H
#include "common.h"
#include "pusch_metadata.h"
#ifdef __cplusplus
extern "C" {
#endif
#define OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1 0x00010003u
// Optional in-process resident extension carried in channel-estimator output.reserved.
// The host supplies a same-device writable F32 [rx_port, layer] buffer. A module
// that can estimate scheduler-facing total disturbance enqueues that output on
// invocation.execution_stream and acknowledges it synchronously. Older hosts and
// modules leave these words zero, so the base v1 contract remains compatible.
#define OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_REQUEST_V1 0x4345534e4f495345ull
#define OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_RESPONSE_V1 0x53434845444f4b31ull
#define OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_REQUEST_INDEX_V1 0u
#define OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_HANDLE_INDEX_V1 1u
#define OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_BYTES_INDEX_V1 2u
#define OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_RESPONSE_INDEX_V1 3u
typedef struct {
  ocudu_dapp_invocation_v1     invocation;
  ocudu_dapp_tensor_view_v1    rx_grid;
  ocudu_dapp_typed_metadata_v1 pusch;
  ocudu_dapp_typed_metadata_v1 dmrs;
} ocudu_dapp_channel_estimator_input_v1;
typedef struct {
  /// Caller-owned writable CF32 [rx_port, layer, symbol, subcarrier] tensor.
  ocudu_dapp_tensor_view_v1 channel_estimates;
  /// Caller-owned writable F32 [rx_port, layer] tensor.
  ocudu_dapp_tensor_view_v1 noise_variances;
  /// Optional resident scheduler-noise extension described above.
  uint64_t                  reserved[4];
} ocudu_dapp_channel_estimator_output_v1;
typedef ocudu_dapp_status_v1 (*ocudu_dapp_channel_estimator_invoke_v1)(void*,
                                                                       const ocudu_dapp_channel_estimator_input_v1*,
                                                                       ocudu_dapp_channel_estimator_output_v1*);
typedef struct {
  ocudu_dapp_worker_api_v1               worker;
  ocudu_dapp_channel_estimator_invoke_v1 invoke;
  uint64_t                               reserved[8];
} ocudu_dapp_channel_estimator_interface_v1;
#ifdef __cplusplus
}
#endif
#endif
