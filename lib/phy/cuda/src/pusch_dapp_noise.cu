// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_e2e.h"
#include "pusch_e2e_internal.cuh"
#include "pusch_equalized_metrics.h"
#include <cmath>

namespace {
constexpr unsigned threads = 256;
constexpr unsigned max_blocks = 32;
constexpr unsigned max_values = MAX_PRB * 12 * MAX_SYMBOLS * 4;
struct partial_metrics {
    double noise_sum;
    double error_sum;
    unsigned noise_count;
    unsigned symbol_count;
};

/// Compensated FP32 accumulator: the long per-thread loop stays in FP32 on devices with low
/// double throughput while retaining small contributions of a wide-dynamic-range tensor.
struct compensated_sum {
    float sum = 0.f;
    float correction = 0.f;
    __device__ void add(float value)
    {
        const float adjusted = value - correction;
        const float next = sum + adjusted;
        correction = (next - sum) - adjusted;
        sum = next;
    }
    __device__ double value() const { return static_cast<double>(sum) - static_cast<double>(correction); }
};

__global__ void partials(const float* noise, const float2* symbols, unsigned bits, unsigned size,
                         partial_metrics* output)
{
    __shared__ double noise_sums[threads];
    __shared__ double error_sums[threads];
    __shared__ unsigned noise_counts[threads];
    __shared__ unsigned symbol_counts[threads];
    const unsigned lane = threadIdx.x;
    compensated_sum noise_sum;
    compensated_sum error_sum;
    unsigned noise_count = 0;
    unsigned symbol_count = 0;
    for (unsigned i = blockIdx.x * threads + lane; i < size; i += gridDim.x * threads) {
        if (noise) {
            const float value = noise[i];
            if (isfinite(value) && value > 0.f && value < 1.e5f) {
                noise_sum.add(value);
                ++noise_count;
            }
        }
        // Same finite-symbol rule and unit-power QAM decision as the fused baseline EVM.
        if (symbols && isfinite(symbols[i].x) && isfinite(symbols[i].y)) {
            error_sum.add(ocudu::detail::pusch_qam_error_power(symbols[i].x, symbols[i].y, bits));
            ++symbol_count;
        }
    }
    noise_sums[lane] = noise_sum.value();
    error_sums[lane] = error_sum.value();
    noise_counts[lane] = noise_count;
    symbol_counts[lane] = symbol_count;
    __syncthreads();
    for (unsigned stride = threads / 2; stride; stride /= 2) {
        if (lane < stride) {
            noise_sums[lane] += noise_sums[lane + stride];
            error_sums[lane] += error_sums[lane + stride];
            noise_counts[lane] += noise_counts[lane + stride];
            symbol_counts[lane] += symbol_counts[lane + stride];
        }
        __syncthreads();
    }
    if (!lane) {
        output[blockIdx.x] = {noise_sums[0], error_sums[0], noise_counts[0], symbol_counts[0]};
    }
}

__global__ void finish(const partial_metrics* input, unsigned blocks, float* noise_sum, unsigned* noise_count,
                       float* error_sum, unsigned* symbol_count)
{
    // At most 32 partials; one lane avoids atomics and nondeterministic update order.
    double noise_total = 0.;
    double error_total = 0.;
    unsigned noise_valid = 0;
    unsigned symbols_valid = 0;
    for (unsigned i = 0; i < blocks; ++i) {
        noise_total += input[i].noise_sum;
        error_total += input[i].error_sum;
        noise_valid += input[i].noise_count;
        symbols_valid += input[i].symbol_count;
    }
    *noise_sum = static_cast<float>(noise_total);
    *noise_count = noise_valid;
    if (error_sum) {
        *error_sum = static_cast<float>(error_total);
        *symbol_count = symbols_valid;
    }
}
} // namespace

nr_ldpc_status_t pusch_e2e_dapp_metrics_async(pusch_e2e_handle_t handle,
                                           const float* noise,
                                           const float2* symbols,
                                           unsigned count,
                                           unsigned modulation_bits,
                                           cudaStream_t stream)
{
    const bool evm = symbols != nullptr;
    if (!handle || count > max_values || (count && !noise && !symbols) ||
        (evm && modulation_bits != 2 && modulation_bits != 4 && modulation_bits != 6 && modulation_bits != 8) ||
        !handle->d_eq_noise_var_sum || !handle->d_eq_noise_var_count ||
        !handle->h_sinr_eq_block || !handle->h_sinr_noise_var_sum || !handle->h_sinr_count ||
        !handle->d_noise_var_partial || handle->noise_var_partial_capacity < max_blocks * sizeof(partial_metrics)) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    // The existing context lays out [float sum, unsigned count, float evm_sum, unsigned evm_count]
    // contiguously on both device and host. Keep the copy bounded and fail closed on layout drift.
    const auto* device = reinterpret_cast<const char*>(handle->d_eq_noise_var_sum);
    const auto* host = reinterpret_cast<const char*>(handle->h_sinr_noise_var_sum);
    if (reinterpret_cast<const char*>(handle->d_eq_noise_var_count) != device + sizeof(float) ||
        handle->h_sinr_eq_block != handle->h_sinr_noise_var_sum ||
        reinterpret_cast<const char*>(handle->h_sinr_count) != host + sizeof(float) ||
        (handle->sinr_managed &&
         (handle->h_sinr_noise_var_sum != handle->d_eq_noise_var_sum ||
          handle->h_sinr_count != handle->d_eq_noise_var_count))) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    if (evm && (reinterpret_cast<const char*>(handle->d_evm_error_sum) != device + 2 * sizeof(float) ||
                reinterpret_cast<const char*>(handle->d_evm_symbol_count) != device + 3 * sizeof(float) ||
                reinterpret_cast<const char*>(handle->h_evm_error_sum) != host + 2 * sizeof(float) ||
                reinterpret_cast<const char*>(handle->h_evm_symbol_count) != host + 3 * sizeof(float))) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    unsigned blocks = (count + threads - 1) / threads;
    blocks = blocks > max_blocks ? max_blocks : (blocks ? blocks : 1);
    auto* scratch = reinterpret_cast<partial_metrics*>(handle->d_noise_var_partial);
    partials<<<blocks, threads, 0, stream>>>(noise, symbols, modulation_bits, count, scratch);
    if (cudaGetLastError() != cudaSuccess) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }
    finish<<<1, 1, 0, stream>>>(scratch, blocks, handle->d_eq_noise_var_sum, handle->d_eq_noise_var_count,
                                evm ? handle->d_evm_error_sum : nullptr, evm ? handle->d_evm_symbol_count : nullptr);
    if (cudaGetLastError() != cudaSuccess) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }
    // Noise only: eight bytes, the EVM half of the mailbox is left untouched.
    const size_t bytes = (evm ? 2 : 1) * (sizeof(float) + sizeof(unsigned));
    if (!handle->sinr_managed &&
        cudaMemcpyAsync(handle->h_sinr_eq_block, handle->d_eq_noise_var_sum, bytes, cudaMemcpyDeviceToHost, stream) !=
            cudaSuccess) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }
    return NR_LDPC_SUCCESS;
}

nr_ldpc_status_t pusch_e2e_dapp_noise_async(pusch_e2e_handle_t handle,
                                         const float* noise,
                                         unsigned count,
                                         cudaStream_t stream)
{
    return pusch_e2e_dapp_metrics_async(handle, noise, nullptr, count, 0, stream);
}

float pusch_e2e_dapp_noise_get_mean(pusch_e2e_handle_t handle)
{
    if (!handle || !handle->h_sinr_noise_var_sum || !handle->h_sinr_count) {
        return NAN;
    }
    const float sum = *handle->h_sinr_noise_var_sum;
    const unsigned count = *handle->h_sinr_count;
    if (!count || count > max_values || !std::isfinite(sum) || sum <= 0.f) {
        return NAN;
    }
    const float mean = sum / static_cast<float>(count);
    return std::isfinite(mean) && mean > 0.f && mean <= 1.e5f ? mean : NAN;
}
