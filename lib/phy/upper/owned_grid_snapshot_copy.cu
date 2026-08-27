/*
 *
 * Copyright 2021-2026 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

/// \file
/// \brief Device-to-device copy of a CUDA-visible resource grid into an owned snapshot buffer.
///
/// GB10-class devices cannot CPU-load GPU-resident managed pages, and D2H of the live managed VA faults as well
/// (Xid 31). Host reads of a CUDA-visible grid therefore go through an owned cudaMalloc snapshot that this kernel
/// fills on the producer's stream; the host then copies from the snapshot, never from the live grid.

#include <cstdint>
#include <cuda_runtime.h>

namespace ocudu {
namespace dapp {

namespace {

__global__ void owned_grid_snapshot_copy_kernel(const uint32_t* __restrict__ src,
                                                uint32_t* __restrict__ dst,
                                                uint64_t n)
{
  const uint64_t i = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (i < n) {
    dst[i] = src[i];
  }
}

} // namespace

bool launch_owned_grid_snapshot_copy(const void* src_cbf16, void* dst_cbf16, uint64_t n_u32, void* stream) noexcept
{
  if (src_cbf16 == nullptr || dst_cbf16 == nullptr || stream == nullptr || n_u32 == 0) {
    return false;
  }
  constexpr uint32_t threads = 256;
  const uint32_t     blocks  = static_cast<uint32_t>((n_u32 + threads - 1) / threads);
  owned_grid_snapshot_copy_kernel<<<blocks, threads, 0, static_cast<cudaStream_t>(stream)>>>(
      static_cast<const uint32_t*>(src_cbf16), static_cast<uint32_t*>(dst_cbf16), n_u32);
  return cudaGetLastError() == cudaSuccess;
}

} // namespace dapp
} // namespace ocudu
