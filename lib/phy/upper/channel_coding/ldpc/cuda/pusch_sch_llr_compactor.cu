// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

//
// File implements SCH LLR compaction
//

#include "pusch_uci_short_block_decoder.cuh"
#include "ercheck.cuh"

__global__ void pusch_compact_sch_llrs_half_kernel (const __half* __restrict__ full_llrs,
                                                   __half* __restrict__ sch_llrs,
                                                   const int* __restrict__ sch_re_indices,
                                                   unsigned nof_sch_llrs,
                                                   unsigned nof_bits_per_re,
                                                   const int* __restrict__ sch_erasure_re_indices,
                                                   unsigned nof_sch_erasure_re)
{
  unsigned dst_idx = blockIdx.x * blockDim.x + threadIdx.x;
  if (dst_idx >= nof_sch_llrs) {
    return;
  }

  unsigned sch_re_idx = dst_idx / nof_bits_per_re;
  unsigned bit_idx    = dst_idx - sch_re_idx * nof_bits_per_re;
  unsigned src_re_idx = static_cast<unsigned>(sch_re_indices[sch_re_idx]);
  bool     erase      = (sch_erasure_re_indices != nullptr) &&
               cuocudu::contains_sorted_re_index (sch_erasure_re_indices, nof_sch_erasure_re, src_re_idx);
  sch_llrs[dst_idx] = erase ? __float2half(0.0F) : full_llrs[src_re_idx * nof_bits_per_re + bit_idx];
}

__global__ void
pusch_compact_sch_and_decode_uci_short_blocks_half_kernel(const __half* __restrict__ full_llrs,
                                                          __half* __restrict__ sch_llrs,
                                                          const int* __restrict__ sch_re_indices,
                                                          unsigned nof_sch_llrs,
                                                          const int* __restrict__ harq_ack_re_indices,
                                                          unsigned                       nof_harq_ack_re,
                                                          unsigned                       nof_harq_ack_bits,
                                                          pusch_uci_short_decode_result* harq_ack_result,
                                                          const int* __restrict__ csi_part1_re_indices,
                                                          unsigned                       nof_csi_part1_re,
                                                          unsigned                       nof_csi_part1_bits,
                                                          pusch_uci_short_decode_result* csi_part1_result,
                                                          unsigned                       nof_bits_per_re,
                                                          unsigned                       bits_per_symbol,
                                                          const int* __restrict__ sch_erasure_re_indices,
                                                          unsigned nof_sch_erasure_re)
{
  unsigned dst_idx = blockIdx.x * blockDim.x + threadIdx.x;
  if (dst_idx < nof_sch_llrs) {
    unsigned sch_re_idx = dst_idx / nof_bits_per_re;
    unsigned bit_idx    = dst_idx - sch_re_idx * nof_bits_per_re;
    unsigned src_re_idx = static_cast<unsigned>(sch_re_indices[sch_re_idx]);
    bool     erase      = (sch_erasure_re_indices != nullptr) &&
                 cuocudu::contains_sorted_re_index(sch_erasure_re_indices, nof_sch_erasure_re, src_re_idx);
    sch_llrs[dst_idx] = erase ? __float2half(0.0F) : full_llrs[src_re_idx * nof_bits_per_re + bit_idx];
  }

  if (blockIdx.x == 0 && threadIdx.x == 0) {
    if (harq_ack_result) {
      cuocudu::decode_short_block_indexed(full_llrs,
                                 harq_ack_re_indices,
                                 nof_harq_ack_re,
                                 nof_harq_ack_bits,
                                 nof_bits_per_re,
                                 bits_per_symbol,
                                 harq_ack_result);
    }
    if (csi_part1_result) {
      cuocudu::decode_short_block_indexed(full_llrs,
                                 csi_part1_re_indices,
                                 nof_csi_part1_re,
                                 nof_csi_part1_bits,
                                 nof_bits_per_re,
                                 bits_per_symbol,
                                 csi_part1_result);
    }
  }
}

void pusch_compact_sch_llrs_half (const void*  d_full_llrs_half,
                                 void*        d_sch_llrs_half,
                                 const int*   d_sch_re_indices,
                                 unsigned     nof_sch_re,
                                 unsigned     nof_bits_per_re,
                                 cudaStream_t stream,
                                 const int*   d_sch_erasure_re_indices,
                                 unsigned     nof_sch_erasure_re)
{
  if (nof_sch_re == 0 || nof_bits_per_re == 0) {
    return;
  }

  unsigned           nof_sch_llrs = nof_sch_re * nof_bits_per_re;
  constexpr unsigned block_size   = 256;
  unsigned           grid_size    = (nof_sch_llrs + block_size - 1) / block_size;

  pusch_compact_sch_llrs_half_kernel <<<grid_size, block_size, 0, stream>>> (static_cast<const __half*>(d_full_llrs_half),
                                                                           static_cast<__half*>(d_sch_llrs_half),
                                                                           d_sch_re_indices,
                                                                           nof_sch_llrs,
                                                                           nof_bits_per_re,
                                                                           d_sch_erasure_re_indices,
                                                                           nof_sch_erasure_re);

  CHECK_NVFAILURE (cudaPeekAtLastError());
}

void pusch_compact_sch_and_decode_uci_short_blocks_half(const void*  d_full_llrs_half,
                                                        void*        d_sch_llrs_half,
                                                        const int*   d_sch_re_indices,
                                                        unsigned     nof_sch_re,
                                                        const int*   d_harq_ack_re_indices,
                                                        unsigned     nof_harq_ack_re,
                                                        unsigned     nof_harq_ack_bits,
                                                        void*        d_harq_ack_result,
                                                        const int*   d_csi_part1_re_indices,
                                                        unsigned     nof_csi_part1_re,
                                                        unsigned     nof_csi_part1_bits,
                                                        void*        d_csi_part1_result,
                                                        unsigned     nof_bits_per_re,
                                                        unsigned     bits_per_symbol,
                                                        cudaStream_t stream,
                                                        const int*   d_sch_erasure_re_indices,
                                                        unsigned     nof_sch_erasure_re)
{
  if (!d_full_llrs_half || !d_sch_llrs_half || !d_sch_re_indices || nof_sch_re == 0 || nof_bits_per_re == 0 ||
      bits_per_symbol == 0) {
    return;
  }

  unsigned           nof_sch_llrs = nof_sch_re * nof_bits_per_re;
  constexpr unsigned block_size   = 256;
  unsigned           grid_size    = (nof_sch_llrs + block_size - 1) / block_size;

  pusch_compact_sch_and_decode_uci_short_blocks_half_kernel<<<grid_size, block_size, 0, stream>>>(
      static_cast<const __half*>(d_full_llrs_half),
      static_cast<__half*>(d_sch_llrs_half),
      d_sch_re_indices,
      nof_sch_llrs,
      d_harq_ack_re_indices,
      nof_harq_ack_re,
      nof_harq_ack_bits,
      static_cast<pusch_uci_short_decode_result*>(d_harq_ack_result),
      d_csi_part1_re_indices,
      nof_csi_part1_re,
      nof_csi_part1_bits,
      static_cast<pusch_uci_short_decode_result*>(d_csi_part1_result),
      nof_bits_per_re,
      bits_per_symbol,
      d_sch_erasure_re_indices,
      nof_sch_erasure_re);

  CHECK_NVFAILURE (cudaPeekAtLastError());
}
