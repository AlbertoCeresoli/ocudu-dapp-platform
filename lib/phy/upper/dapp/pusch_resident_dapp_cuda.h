// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <cstdint>
#include <cuda_runtime_api.h>

namespace ocudu {

/// Queues the one- or two-layer resident CE continuation into caller-owned compact buffers.
bool launch_pusch_resident_ce_equalizer(const uint32_t* grid_cbf16,
                                        const float2*   channel_estimates,
                                        const float*    channel_noise,
                                        const uint32_t* data_re_indices,
                                        float2*         equalized,
                                        float*          post_eq_noise,
                                        uint32_t        nof_rx_ports,
                                        uint32_t        nof_layers,
                                        uint32_t        nof_grid_symbols,
                                        uint32_t        nof_grid_subcarriers,
                                        uint32_t        nof_data_re,
                                        cudaStream_t    stream) noexcept;

/// Queues scheduler-facing post-EQ noise without recomputing or rewriting symbols.
bool launch_pusch_resident_ce_scheduler_noise(const float2*   channel_estimates,
                                              const float*    channel_noise,
                                              const uint32_t* data_re_indices,
                                              float*          post_eq_noise,
                                              uint32_t        nof_rx_ports,
                                              uint32_t        nof_layers,
                                              uint32_t        nof_grid_symbols,
                                              uint32_t        nof_grid_subcarriers,
                                              uint32_t        nof_data_re,
                                              cudaStream_t    stream) noexcept;

} // namespace ocudu
