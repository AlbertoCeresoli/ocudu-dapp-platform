// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <cstdint>

namespace ocudu::dapp {

struct srs_channel_estimate_cuda_config {
  uint32_t nof_rx_ports{};
  uint32_t nof_tx_ports{};
  uint32_t nof_symbols{};
  uint32_t start_symbol{};
  uint32_t frequency_points{};
  uint32_t comb_size{};
  uint32_t grid_nof_symbols{};
  uint32_t grid_nof_subcarriers{};
  uint32_t rx_ports[4]{};
  uint32_t first_subcarrier[4]{};
};

bool launch_srs_channel_estimate_cuda(const void*                             device_grid_cbf16,
                                      const void*                             device_sequences_cf32,
                                      void*                                   device_channel_estimates_cf32,
                                      const srs_channel_estimate_cuda_config& config,
                                      void*                                   stream) noexcept;

/// Compute-copy CBF16 words from a live managed grid onto owned cudaMalloc.
/// Must run on the producer stream immediately after that stream wrote \p src.
bool launch_owned_grid_snapshot_copy(const void* src_cbf16,
                                     void*       dst_cbf16,
                                     uint64_t    n_u32,
                                     void*       stream) noexcept;

} // namespace ocudu::dapp
