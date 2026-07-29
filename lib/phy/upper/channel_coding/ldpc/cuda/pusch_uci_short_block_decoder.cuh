// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include <cuda_fp16.h>
#include <math_constants.h>
#include "pusch_sch_llr_compactor.h"

namespace cuocudu {

    constexpr unsigned MAX_UCI_SHORT_BITS      = 11;
    constexpr unsigned MAX_UCI_SHORT_CODE_BITS = 32;

    __device__ __constant__ uint8_t UCI_SHORT_BASIS [MAX_UCI_SHORT_BITS][MAX_UCI_SHORT_CODE_BITS] = 
    {
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
        {1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 0, 0, 1, 0},
        {0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 0},
        {0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1, 0},
        {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0},
        {0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0},
        {0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0, 1, 1, 0},
        {0, 0, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0},
        {0, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0},
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 1, 0}
    };

inline __device__ unsigned calculate_uci_min_encoded_bits_device(unsigned nof_payload_bits)
{
  constexpr unsigned min_small_block_rm_size[MAX_UCI_SHORT_BITS] = {2, 3, 9, 10, 11, 12, 13, 14, 14, 15, 17};
  if (nof_payload_bits == 0 || nof_payload_bits > MAX_UCI_SHORT_BITS) {
    return 0;
  }
  return min_small_block_rm_size[nof_payload_bits - 1];
}

inline __device__ bool contains_sorted_re_index(const int* re_indices, unsigned nof_re, unsigned re_index)
{
  unsigned low  = 0;
  unsigned high = nof_re;
  while (low != high) {
    unsigned mid       = low + (high - low) / 2;
    unsigned candidate = static_cast<unsigned>(re_indices[mid]);
    if (candidate < re_index) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return (low != nof_re) && (static_cast<unsigned>(re_indices[low]) == re_index);
}

inline __device__ float read_indexed_uci_llr(const __half* full_llrs, const int* re_indices, unsigned llr_index, unsigned nof_bits_per_re)
{
  unsigned re_index  = llr_index / nof_bits_per_re;
  unsigned bit_index = llr_index - re_index * nof_bits_per_re;
  return __half2float(full_llrs[static_cast<unsigned>(re_indices[re_index]) * nof_bits_per_re + bit_index]);
}

inline __device__ void rate_dematch_short_half_indexed(float*        output,
                                                unsigned      output_size,
                                                const __half* full_llrs,
                                                const int*    re_indices,
                                                unsigned      input_size,
                                                unsigned      nof_bits_per_re)
{
  for (unsigned i = 0; i != output_size; ++i) {
    output[i] = 0.0F;
  }

  unsigned nof_copy = min(input_size, output_size);
  for (unsigned i = 0; i != nof_copy; ++i) {
    output[i] = read_indexed_uci_llr(full_llrs, re_indices, i, nof_bits_per_re);
  }

  if (input_size <= output_size) {
    return;
  }

  unsigned offset = output_size;
  while (offset != input_size) {
    unsigned block_size = min(output_size, input_size - offset);
    for (unsigned i = 0; i != block_size; ++i) {
      output[i] += read_indexed_uci_llr(full_llrs, re_indices, offset + i, nof_bits_per_re);
    }
    offset += block_size;
  }
}

inline __device__ float detect_2_device(uint8_t* output, const float* input, unsigned input_size)
{
  float llr[3] = {};

  if (input_size == 3) {
    llr[0] = input[0];
    llr[1] = input[1];
    llr[2] = input[2];
  } else {
    unsigned step = input_size / 3 - 2;
    llr[0]        = input[0] + input[step + 3];
    llr[1]        = input[1] + input[2 * step + 4];
    llr[2]        = input[step + 2] + input[2 * step + 5];
  }

  constexpr int table[4][3] = {{1, 1, 1}, {-1, 1, -1}, {1, -1, -1}, {-1, -1, 1}};

  unsigned max_idx    = 0;
  float    max_metric = -CUDART_INF_F;
  for (unsigned cdwd_idx = 0; cdwd_idx != 4; ++cdwd_idx) {
    float metric = 0.0F;
    for (unsigned i = 0; i != 3; ++i) {
      metric += llr[i] * static_cast<float>(table[cdwd_idx][i]);
    }
    if (metric > max_metric) {
      max_metric = metric;
      max_idx    = cdwd_idx;
    }
  }

  output[0] = static_cast<uint8_t>(max_idx & 1U);
  output[1] = static_cast<uint8_t>((max_idx >> 1U) & 1U);

  float norm_sqr = 0.0F;
  for (unsigned i = 0; i != 3; ++i) {
    norm_sqr += llr[i] * llr[i];
  }
  float metric_sqr = max_metric * max_metric;
  float denom      = 3.0F * norm_sqr - metric_sqr;
  return (denom > 0.0F) ? (2.0F * metric_sqr / denom) : CUDART_INF_F;
}

inline __device__ float detect_3_11_device(uint8_t* output, const float* input, unsigned nof_payload_bits)
{
  unsigned nof_codewords = 1U << (nof_payload_bits - 1U);

  unsigned max_idx    = 0;
  float    max_metric = -CUDART_INF_F;
  uint8_t  bit0       = 0;
  for (unsigned cdwd_idx = 0; cdwd_idx != nof_codewords; ++cdwd_idx) {
    unsigned value  = 2U * cdwd_idx;
    float    metric = 0.0F;

    for (unsigned bit = 0; bit != MAX_UCI_SHORT_CODE_BITS; ++bit) {
      uint8_t code_bit = 0;
      for (unsigned msg_bit = 0; msg_bit != MAX_UCI_SHORT_BITS; ++msg_bit) {
        if (((value >> msg_bit) & 1U) != 0U) {
          code_bit ^= UCI_SHORT_BASIS[msg_bit][bit];
        }
      }
      metric += input[bit] * (code_bit ? -1.0F : 1.0F);
    }

    float metric_abs = fabsf(metric);
    if (metric_abs > max_metric) {
      max_metric = metric_abs;
      max_idx    = cdwd_idx;
      bit0       = static_cast<uint8_t>(metric < 0.0F);
    }
  }

  unsigned detected = 2U * max_idx + bit0;
  for (unsigned i = 0; i != nof_payload_bits; ++i) {
    output[i] = static_cast<uint8_t>((detected >> i) & 1U);
  }

  float norm_sqr = 0.0F;
  for (unsigned i = 0; i != MAX_UCI_SHORT_CODE_BITS; ++i) {
    norm_sqr += input[i] * input[i];
  }
  float metric_sqr = max_metric * max_metric;
  float denom      = static_cast<float>(MAX_UCI_SHORT_CODE_BITS) * norm_sqr - metric_sqr;
  return (denom > 0.0F) ? ((static_cast<float>(MAX_UCI_SHORT_CODE_BITS - 1) * metric_sqr) / denom) : CUDART_INF_F;
}

inline __device__ void decode_short_block_indexed(const __half*                  full_llrs,
                                           const int*                     re_indices,
                                           unsigned                       nof_re,
                                           unsigned                       nof_payload_bits,
                                           unsigned                       nof_bits_per_re,
                                           unsigned                       bits_per_symbol,
                                           pusch_uci_short_decode_result* result)
{
  result->nof_bits = static_cast<uint8_t>(min(nof_payload_bits, MAX_UCI_SHORT_BITS));
  result->status   = 0;
  result->decoded  = 0;
  for (unsigned i = 0; i != MAX_UCI_SHORT_BITS; ++i) {
    result->payload[i] = 1;
  }

  if (!full_llrs || !re_indices || nof_re == 0 || nof_payload_bits == 0 || nof_payload_bits > MAX_UCI_SHORT_BITS ||
      nof_bits_per_re == 0 || bits_per_symbol == 0) {
    return;
  }

  unsigned nof_llrs       = nof_re * nof_bits_per_re;
  unsigned non_zero_count = 0;
  for (unsigned i = 0; i != nof_llrs; ++i) {
    non_zero_count += (read_indexed_uci_llr(full_llrs, re_indices, i, nof_bits_per_re) != 0.0F) ? 1U : 0U;
  }
  if (non_zero_count < calculate_uci_min_encoded_bits_device(nof_payload_bits)) {
    result->decoded = 1;
    return;
  }
  if (nof_payload_bits <= 2 && nof_llrs < bits_per_symbol) {
    result->decoded = 1;
    return;
  }

  float   tmp[MAX_UCI_SHORT_CODE_BITS] = {};
  float   metric                       = 0.0F;
  uint8_t payload[MAX_UCI_SHORT_BITS]  = {};
  if (nof_payload_bits == 1) {
    rate_dematch_short_half_indexed(tmp, bits_per_symbol, full_llrs, re_indices, nof_llrs, nof_bits_per_re);
    payload[0] = (tmp[0] > 0.0F) ? 0 : 1;
    metric     = 1.0F;
  } else if (nof_payload_bits == 2) {
    unsigned tmp_size = 3U * bits_per_symbol;
    if (tmp_size > MAX_UCI_SHORT_CODE_BITS) {
      return;
    }
    rate_dematch_short_half_indexed(tmp, tmp_size, full_llrs, re_indices, nof_llrs, nof_bits_per_re);
    metric = detect_2_device(payload, tmp, tmp_size);
  } else {
    rate_dematch_short_half_indexed(tmp, MAX_UCI_SHORT_CODE_BITS, full_llrs, re_indices, nof_llrs, nof_bits_per_re);
    metric = detect_3_11_device(payload, tmp, nof_payload_bits);
  }

  constexpr float thresholds[MAX_UCI_SHORT_BITS] = {
      0.0F, 0.0F, 12.0F, 14.0F, 16.0F, 18.0F, 20.0F, 22.0F, 24.0F, 26.0F, 29.0F};
  for (unsigned i = 0; i != nof_payload_bits; ++i) {
    result->payload[i] = payload[i];
  }
  result->status  = (metric > thresholds[nof_payload_bits - 1]) ? 1 : 0;
  result->decoded = 1;
}

} // namespace cuocudu
