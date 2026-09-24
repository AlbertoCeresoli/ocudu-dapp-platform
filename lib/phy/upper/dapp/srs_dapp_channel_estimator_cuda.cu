// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "srs_dapp_channel_estimator_cuda.h"
#include <cuda_runtime_api.h>

namespace ocudu::dapp {
namespace {

__device__ float bfloat16_to_float(uint16_t value)
{
  return __uint_as_float(static_cast<uint32_t>(value) << 16U);
}

/// One thread per pilot; blockIdx.y enumerates (pilot set, rx port, symbol) rows of the output.
__global__ void srs_rx_symbol_gather_kernel(const uint32_t* __restrict__ grid,
                                            float2* __restrict__ rx_symbols,
                                            srs_rx_symbol_gather_config config)
{
  const uint32_t pilot = blockIdx.x * blockDim.x + threadIdx.x;
  const uint32_t row   = blockIdx.y;
  if (pilot >= config.sequence_length) {
    return;
  }
  const uint32_t symbol     = row % config.nof_symbols;
  const uint32_t rx         = (row / config.nof_symbols) % config.nof_rx_ports;
  const uint32_t set        = row / (config.nof_symbols * config.nof_rx_ports);
  const uint32_t subcarrier = config.first_subcarrier[set] + pilot * config.comb_size;
  const uint64_t index =
      (static_cast<uint64_t>(config.rx_ports[rx]) * config.grid_nof_symbols + config.start_symbol + symbol) *
          config.grid_nof_subcarriers +
      subcarrier;
  const uint32_t packed                                                   = grid[index];
  rx_symbols[static_cast<uint64_t>(row) * config.sequence_length + pilot] = {
      bfloat16_to_float(static_cast<uint16_t>(packed)), bfloat16_to_float(static_cast<uint16_t>(packed >> 16U))};
}

} // namespace

bool launch_srs_rx_symbol_gather(const void*                        device_grid_cbf16,
                                 void*                              device_rx_symbols_cf32,
                                 const srs_rx_symbol_gather_config& config,
                                 void*                              stream) noexcept
{
  if (device_grid_cbf16 == nullptr || device_rx_symbols_cf32 == nullptr || stream == nullptr ||
      config.nof_pilot_sets == 0 || config.nof_pilot_sets > 2 || config.nof_rx_ports == 0 || config.nof_rx_ports > 8 ||
      config.nof_symbols == 0 || config.sequence_length == 0 || config.comb_size == 0) {
    return false;
  }
  constexpr uint32_t threads = 128;
  const dim3         blocks((config.sequence_length + threads - 1) / threads,
                    config.nof_pilot_sets * config.nof_rx_ports * config.nof_symbols);
  srs_rx_symbol_gather_kernel<<<blocks, threads, 0, static_cast<cudaStream_t>(stream)>>>(
      static_cast<const uint32_t*>(device_grid_cbf16), static_cast<float2*>(device_rx_symbols_cf32), config);
  return cudaGetLastError() == cudaSuccess;
}

} // namespace ocudu::dapp
