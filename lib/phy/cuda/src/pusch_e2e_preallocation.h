// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include "pusch_e2e_internal.cuh"

// Match the existing startup provision for four receive ports/four layers.
// This storage is independent of QAM order and covers all supported grant
// widths/DMRS counts in that provision; it is not a per-grant allocation.
inline constexpr size_t pusch_initial_fp16_estimates_bytes =
    size_t{MAX_DMRS_SYMBOLS} * MAX_PRB * 12 * 4 * 4 * sizeof(__half2);

/// One startup allocation, also exercised with a CPU allocator fake. Publish
/// pointer/capacity together only after success. The creator owns rollback via
/// pusch_e2e_destroy; do not invoke this on a configured/in-use context.
inline nr_ldpc_status_t pusch_preallocate_fp16_estimates(pusch_e2e_ctx& context)
{
  if (context.configured || context.d_estimates_fp16 || context.estimates_fp16_capacity != 0) {
    return NR_LDPC_ERROR_INVALID_CONFIG;
  }
  __half2* storage = nullptr;
  if (cudaMalloc(&storage, pusch_initial_fp16_estimates_bytes) != cudaSuccess || !storage) {
    return NR_LDPC_ERROR_ALLOC_FAILED;
  }
  context.d_estimates_fp16        = storage;
  context.estimates_fp16_capacity = pusch_initial_fp16_estimates_bytes;
  return NR_LDPC_SUCCESS;
}

/// Pilot preparation also needs one Gold-sequence seed per DMRS symbol before
/// the first receiver invocation. Like the FP16 storage, do not leave this to
/// the conventional processing branch after admission has already occurred.
inline nr_ldpc_status_t pusch_preallocate_dmrs_c_inits(pusch_e2e_ctx& context)
{
  if (context.configured || context.d_dmrs_c_inits) {
    return NR_LDPC_ERROR_INVALID_CONFIG;
  }
  uint32_t* storage = nullptr;
  if (cudaMalloc(&storage, MAX_DMRS_SYMBOLS * sizeof(uint32_t)) != cudaSuccess || !storage) {
    return NR_LDPC_ERROR_ALLOC_FAILED;
  }
  context.d_dmrs_c_inits = storage;
  return NR_LDPC_SUCCESS;
}
