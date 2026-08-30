// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "srs_channel_estimate_cuda.h"
#include <cuda_runtime_api.h>

namespace ocudu::dapp {
namespace {

__device__ float bfloat16_to_float(uint16_t value)
{
  return __uint_as_float(static_cast<uint32_t>(value) << 16U);
}

__global__ void srs_channel_estimate_kernel(const uint32_t* __restrict__ grid,
                                            const float2* __restrict__ sequences,
                                            float2* __restrict__ channel,
                                            srs_channel_estimate_cuda_config config)
{
  const uint32_t tx        = blockIdx.x;
  const uint32_t rx        = blockIdx.y;
  const uint32_t frequency = threadIdx.x + blockIdx.z * blockDim.x;
  if (tx >= config.nof_tx_ports || rx >= config.nof_rx_ports || frequency >= config.frequency_points) {
    return;
  }
  const uint32_t subcarrier  = config.first_subcarrier[tx] + frequency * config.comb_size;
  const uint64_t port_stride = static_cast<uint64_t>(config.grid_nof_symbols) * config.grid_nof_subcarriers;
  float          sum_real    = 0.0F;
  float          sum_imag    = 0.0F;
  for (uint32_t symbol_offset = 0; symbol_offset != config.nof_symbols; ++symbol_offset) {
    const uint32_t symbol = config.start_symbol + symbol_offset;
    const uint64_t index  = static_cast<uint64_t>(config.rx_ports[rx]) * port_stride +
                           static_cast<uint64_t>(symbol) * config.grid_nof_subcarriers + subcarrier;
    const uint32_t packed = grid[index];
    sum_real += bfloat16_to_float(static_cast<uint16_t>(packed));
    sum_imag += bfloat16_to_float(static_cast<uint16_t>(packed >> 16U));
  }
  const float  scale    = 1.0F / static_cast<float>(config.nof_symbols);
  const float2 received = {sum_real * scale, sum_imag * scale};
  const float2 sequence = sequences[tx * config.frequency_points + frequency];
  const float2 estimate = {received.x * sequence.x + received.y * sequence.y,
                           received.y * sequence.x - received.x * sequence.y};
  channel[(rx * config.nof_tx_ports + tx) * config.frequency_points + frequency] = estimate;
}

} // namespace

bool launch_srs_channel_estimate_cuda(const void*                             device_grid_cbf16,
                                      const void*                             device_sequences_cf32,
                                      void*                                   device_channel_estimates_cf32,
                                      const srs_channel_estimate_cuda_config& config,
                                      void*                                   stream) noexcept
{
  if (device_grid_cbf16 == nullptr || device_sequences_cf32 == nullptr || device_channel_estimates_cf32 == nullptr ||
      stream == nullptr || config.nof_rx_ports == 0 || config.nof_rx_ports > 4 || config.nof_tx_ports == 0 ||
      config.nof_tx_ports > 4 || config.nof_symbols == 0 || config.frequency_points == 0 || config.comb_size == 0) {
    return false;
  }
  constexpr uint32_t threads = 256;
  const dim3 blocks(config.nof_tx_ports, config.nof_rx_ports, (config.frequency_points + threads - 1) / threads);
  srs_channel_estimate_kernel<<<blocks, threads, 0, static_cast<cudaStream_t>(stream)>>>(
      static_cast<const uint32_t*>(device_grid_cbf16),
      static_cast<const float2*>(device_sequences_cf32),
      static_cast<float2*>(device_channel_estimates_cf32),
      config);
  return cudaGetLastError() == cudaSuccess;
}

} // namespace ocudu::dapp
