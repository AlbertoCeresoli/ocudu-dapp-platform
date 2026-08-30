// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "pusch_resident_dapp_cuda.h"
#include <cuda_bf16.h>

namespace ocudu {
namespace {

constexpr float minimum_soft_demap_noise = 1.0e-4F;

__device__ float squared_norm(float2 value)
{
  return value.x * value.x + value.y * value.y;
}

__device__ float2 unpack_cbf16(uint32_t value)
{
  return __bfloat1622float2(*reinterpret_cast<const __nv_bfloat162*>(&value));
}

__global__ void equalize_one_or_two_layers(const uint32_t* grid,
                                           const float2*   channel,
                                           const float*    path_noise,
                                           const uint32_t* data_indices,
                                           float2*         equalized,
                                           float*          output_noise,
                                           uint32_t        nof_ports,
                                           uint32_t        nof_layers,
                                           uint32_t        plane,
                                           uint32_t        nof_data_re)
{
  const uint32_t data_index = blockIdx.x * blockDim.x + threadIdx.x;
  if (data_index >= nof_data_re) {
    return;
  }
  const uint32_t re = data_indices[data_index];
  if (re >= plane) {
    for (uint32_t layer = 0; layer != nof_layers; ++layer) {
      equalized[nof_layers * data_index + layer]    = {};
      output_noise[nof_layers * data_index + layer] = minimum_soft_demap_noise;
    }
    return;
  }

  float2 matched[2]{};
  float  diagonal[2]{};
  float2 cross{};
  float  noise_diagonal[2]{};
  float2 noise_cross{};
  for (uint32_t port = 0; port != nof_ports; ++port) {
    const float2 y  = unpack_cbf16(grid[port * plane + re]);
    const float2 h0 = channel[(nof_layers * port) * plane + re];
    const float  variance =
        nof_layers == 1 ? path_noise[port] : 0.5F * (path_noise[2U * port] + path_noise[2U * port + 1U]);
    matched[0].x += y.x * h0.x + y.y * h0.y;
    matched[0].y += y.y * h0.x - y.x * h0.y;
    diagonal[0] += squared_norm(h0);
    noise_diagonal[0] += variance * squared_norm(h0);
    if (nof_layers == 2) {
      const float2 h1 = channel[(2U * port + 1U) * plane + re];
      matched[1].x += y.x * h1.x + y.y * h1.y;
      matched[1].y += y.y * h1.x - y.x * h1.y;
      diagonal[1] += squared_norm(h1);
      cross.x += h0.x * h1.x + h0.y * h1.y;
      cross.y += h0.x * h1.y - h0.y * h1.x;
      noise_diagonal[1] += variance * squared_norm(h1);
      noise_cross.x += variance * (h0.x * h1.x + h0.y * h1.y);
      noise_cross.y += variance * (h0.x * h1.y - h0.y * h1.x);
    }
  }

  if (nof_layers == 1) {
    const float denominator  = fmaxf(diagonal[0], 1.0e-12F);
    equalized[data_index]    = {matched[0].x / denominator, matched[0].y / denominator};
    output_noise[data_index] = fmaxf(minimum_soft_demap_noise, noise_diagonal[0] / (denominator * denominator));
    return;
  }

  const float  determinant        = fmaxf(diagonal[0] * diagonal[1] - squared_norm(cross), 1.0e-12F);
  const float2 cross_b1           = {cross.x * matched[1].x - cross.y * matched[1].y,
                                     cross.x * matched[1].y + cross.y * matched[1].x};
  const float2 cross_h_b0         = {cross.x * matched[0].x + cross.y * matched[0].y,
                                     cross.x * matched[0].y - cross.y * matched[0].x};
  equalized[2U * data_index]      = {(diagonal[1] * matched[0].x - cross_b1.x) / determinant,
                                     (diagonal[1] * matched[0].y - cross_b1.y) / determinant};
  equalized[2U * data_index + 1U] = {(diagonal[0] * matched[1].x - cross_h_b0.x) / determinant,
                                     (diagonal[0] * matched[1].y - cross_h_b0.y) / determinant};

  const float weighted_cross    = noise_cross.x * cross.x + noise_cross.y * cross.y;
  const float cross_power       = squared_norm(cross);
  const float determinant_sq    = determinant * determinant;
  output_noise[2U * data_index] = fmaxf(minimum_soft_demap_noise,
                                        (diagonal[1] * diagonal[1] * noise_diagonal[0] +
                                         cross_power * noise_diagonal[1] - 2.0F * diagonal[1] * weighted_cross) /
                                            determinant_sq);
  output_noise[2U * data_index + 1U] =
      fmaxf(minimum_soft_demap_noise,
            (cross_power * noise_diagonal[0] + diagonal[0] * diagonal[0] * noise_diagonal[1] -
             2.0F * diagonal[0] * weighted_cross) /
                determinant_sq);
}

// Scheduler reporting needs the same post-equalization variance calculation with a
// different input disturbance, after the demapper consumed its calibrated variance.
// Avoid repeating the matched filter or rewriting equalized symbols.
__global__ void report_noise_one_or_two_layers(const float2*   channel,
                                               const float*    path_noise,
                                               const uint32_t* data_indices,
                                               float*          output_noise,
                                               uint32_t        nof_ports,
                                               uint32_t        nof_layers,
                                               uint32_t        plane,
                                               uint32_t        nof_data_re)
{
  const uint32_t data_index = blockIdx.x * blockDim.x + threadIdx.x;
  if (data_index >= nof_data_re) {
    return;
  }
  const uint32_t re = data_indices[data_index];
  if (re >= plane) {
    for (uint32_t layer = 0; layer != nof_layers; ++layer) {
      output_noise[nof_layers * data_index + layer] = minimum_soft_demap_noise;
    }
    return;
  }

  float  diagonal[2]{};
  float2 cross{};
  float  noise_diagonal[2]{};
  float2 noise_cross{};
  for (uint32_t port = 0; port != nof_ports; ++port) {
    const float2 h0 = channel[(nof_layers * port) * plane + re];
    const float  variance =
        nof_layers == 1 ? path_noise[port] : 0.5F * (path_noise[2U * port] + path_noise[2U * port + 1U]);
    diagonal[0] += squared_norm(h0);
    noise_diagonal[0] += variance * squared_norm(h0);
    if (nof_layers == 2) {
      const float2 h1 = channel[(2U * port + 1U) * plane + re];
      diagonal[1] += squared_norm(h1);
      cross.x += h0.x * h1.x + h0.y * h1.y;
      cross.y += h0.x * h1.y - h0.y * h1.x;
      noise_diagonal[1] += variance * squared_norm(h1);
      noise_cross.x += variance * (h0.x * h1.x + h0.y * h1.y);
      noise_cross.y += variance * (h0.x * h1.y - h0.y * h1.x);
    }
  }

  if (nof_layers == 1) {
    const float denominator = fmaxf(diagonal[0], 1.0e-12F);
    output_noise[data_index] = fmaxf(minimum_soft_demap_noise, noise_diagonal[0] / (denominator * denominator));
    return;
  }

  const float determinant     = fmaxf(diagonal[0] * diagonal[1] - squared_norm(cross), 1.0e-12F);
  const float weighted_cross  = noise_cross.x * cross.x + noise_cross.y * cross.y;
  const float cross_power     = squared_norm(cross);
  const float determinant_sq  = determinant * determinant;
  output_noise[2U * data_index] = fmaxf(minimum_soft_demap_noise,
                                        (diagonal[1] * diagonal[1] * noise_diagonal[0] +
                                         cross_power * noise_diagonal[1] - 2.0F * diagonal[1] * weighted_cross) /
                                            determinant_sq);
  output_noise[2U * data_index + 1U] =
      fmaxf(minimum_soft_demap_noise,
            (cross_power * noise_diagonal[0] + diagonal[0] * diagonal[0] * noise_diagonal[1] -
             2.0F * diagonal[0] * weighted_cross) /
                determinant_sq);
}

} // namespace

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
                                        cudaStream_t    stream) noexcept
{
  if (grid_cbf16 == nullptr || channel_estimates == nullptr || channel_noise == nullptr || data_re_indices == nullptr ||
      equalized == nullptr || post_eq_noise == nullptr || nof_rx_ports == 0 || nof_rx_ports > 8 || nof_layers == 0 ||
      nof_layers > 2 || nof_rx_ports < nof_layers || nof_grid_symbols == 0 || nof_grid_subcarriers == 0 ||
      nof_data_re == 0 || stream == nullptr) {
    return false;
  }
  equalize_one_or_two_layers<<<(nof_data_re + 127U) / 128U, 128, 0, stream>>>(grid_cbf16,
                                                                              channel_estimates,
                                                                              channel_noise,
                                                                              data_re_indices,
                                                                              equalized,
                                                                              post_eq_noise,
                                                                              nof_rx_ports,
                                                                              nof_layers,
                                                                              nof_grid_symbols * nof_grid_subcarriers,
                                                                              nof_data_re);
  return cudaPeekAtLastError() == cudaSuccess;
}

bool launch_pusch_resident_ce_scheduler_noise(const float2*   channel_estimates,
                                              const float*    channel_noise,
                                              const uint32_t* data_re_indices,
                                              float*          post_eq_noise,
                                              uint32_t        nof_rx_ports,
                                              uint32_t        nof_layers,
                                              uint32_t        nof_grid_symbols,
                                              uint32_t        nof_grid_subcarriers,
                                              uint32_t        nof_data_re,
                                              cudaStream_t    stream) noexcept
{
  if (channel_estimates == nullptr || channel_noise == nullptr || data_re_indices == nullptr ||
      post_eq_noise == nullptr || nof_rx_ports == 0 || nof_rx_ports > 8 || nof_layers == 0 || nof_layers > 2 ||
      nof_rx_ports < nof_layers || nof_grid_symbols == 0 || nof_grid_subcarriers == 0 || nof_data_re == 0 ||
      stream == nullptr) {
    return false;
  }
  report_noise_one_or_two_layers<<<(nof_data_re + 127U) / 128U, 128, 0, stream>>>(channel_estimates,
                                                                                  channel_noise,
                                                                                  data_re_indices,
                                                                                  post_eq_noise,
                                                                                  nof_rx_ports,
                                                                                  nof_layers,
                                                                                  nof_grid_symbols *
                                                                                      nof_grid_subcarriers,
                                                                                  nof_data_re);
  return cudaPeekAtLastError() == cudaSuccess;
}

} // namespace ocudu
