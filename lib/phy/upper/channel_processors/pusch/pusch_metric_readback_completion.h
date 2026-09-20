// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "../../channel_coding/ldpc/cuda/cuda_rt_utils.h"

namespace ocudu {

/// A decoder join covers the producer stream, not a sibling readback stream.
/// Verify current completion before reading any pinned/managed metric. Failed
/// submissions still need their possibly partial side-stream copies drained.
inline bool pusch_metric_readback_completed(bool         submitted,
                                            bool         event_recorded,
                                            bool         side_stream,
                                            cudaEvent_t  event,
                                            cudaStream_t stream)
{
  cudaError_t status;
  if (event_recorded && event != nullptr) {
    status = side_stream ? cudaEventSynchronizeYielding(event) : cudaEventQuery(event);
  } else {
    // A missing/failed event must never lead to querying a previous generation.
    status = side_stream ? cudaStreamSynchronizeYielding(stream) : cudaStreamQuery(stream);
  }
  return submitted && status == cudaSuccess;
}

} // namespace ocudu
