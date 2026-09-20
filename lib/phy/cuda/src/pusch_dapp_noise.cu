// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_e2e.h"
#include "pusch_e2e_internal.cuh"
#include <cmath>

namespace {
constexpr unsigned threads = 256;
constexpr unsigned max_blocks = 32;
constexpr unsigned max_values = MAX_PRB * 12 * MAX_SYMBOLS * 4;
struct partial_noise {
    double sum;
    unsigned count;
};

__global__ void partials(const float* noise, unsigned size, partial_noise* output)
{
    __shared__ double sums[threads];
    __shared__ unsigned counts[threads];
    const unsigned lane = threadIdx.x;
    // Compensated FP32 in the long per-thread loop, then FP64 tree reduction.
    // Avoid one FP64 addition per RE on devices with low double throughput
    // while retaining small contributions in a wide-dynamic-range noise tensor.
    float sum = 0.f;
    float correction = 0.f;
    unsigned count = 0;
    for (unsigned i = blockIdx.x * threads + lane; i < size; i += gridDim.x * threads) {
        const float value = noise[i];
        if (isfinite(value) && value > 0.f && value < 1.e5f) {
            const float adjusted = value - correction;
            const float next = sum + adjusted;
            correction = (next - sum) - adjusted;
            sum = next;
            ++count;
        }
    }
    sums[lane] = static_cast<double>(sum) - static_cast<double>(correction);
    counts[lane] = count;
    __syncthreads();
    for (unsigned stride = threads / 2; stride; stride /= 2) {
        if (lane < stride) {
            sums[lane] += sums[lane + stride];
            counts[lane] += counts[lane + stride];
        }
        __syncthreads();
    }
    if (!lane) {
        output[blockIdx.x].sum = sums[0];
        output[blockIdx.x].count = counts[0];
    }
}

__global__ void finish(const partial_noise* input, unsigned blocks, float* sum, unsigned* count)
{
    // At most 32 pairs; one lane avoids atomics and nondeterministic update order.
    double total = 0.;
    unsigned valid = 0;
    for (unsigned i = 0; i < blocks; ++i) {
        total += input[i].sum;
        valid += input[i].count;
    }
    *sum = static_cast<float>(total);
    *count = valid;
}
} // namespace

nr_ldpc_status_t pusch_e2e_dapp_noise_async(pusch_e2e_handle_t handle,
                                         const float* noise,
                                         unsigned count,
                                         cudaStream_t stream)
{
    if (!handle || count > max_values || (count && !noise) ||
        !handle->d_eq_noise_var_sum || !handle->d_eq_noise_var_count ||
        !handle->h_sinr_eq_block || !handle->h_sinr_noise_var_sum || !handle->h_sinr_count ||
        !handle->d_noise_var_partial || handle->noise_var_partial_capacity < max_blocks * sizeof(partial_noise)) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    // The existing context lays out [float sum, unsigned count] contiguously on
    // both device and host. Keep the copy bounded and fail closed on layout drift.
    if (reinterpret_cast<char*>(handle->d_eq_noise_var_count) !=
            reinterpret_cast<char*>(handle->d_eq_noise_var_sum) + sizeof(float) ||
        handle->h_sinr_eq_block != handle->h_sinr_noise_var_sum ||
        reinterpret_cast<char*>(handle->h_sinr_count) !=
            reinterpret_cast<char*>(handle->h_sinr_noise_var_sum) + sizeof(float) ||
        (handle->sinr_managed &&
         (handle->h_sinr_noise_var_sum != handle->d_eq_noise_var_sum ||
          handle->h_sinr_count != handle->d_eq_noise_var_count))) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    unsigned blocks = (count + threads - 1) / threads;
    blocks = blocks > max_blocks ? max_blocks : (blocks ? blocks : 1);
    auto* scratch = reinterpret_cast<partial_noise*>(handle->d_noise_var_partial);
    partials<<<blocks, threads, 0, stream>>>(noise, count, scratch);
    if (cudaGetLastError() != cudaSuccess) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }
    finish<<<1, 1, 0, stream>>>(scratch, blocks, handle->d_eq_noise_var_sum, handle->d_eq_noise_var_count);
    if (cudaGetLastError() != cudaSuccess) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }
    if (!handle->sinr_managed &&
        cudaMemcpyAsync(handle->h_sinr_eq_block, handle->d_eq_noise_var_sum,
                        sizeof(float) + sizeof(unsigned), cudaMemcpyDeviceToHost, stream) != cudaSuccess) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }
    return NR_LDPC_SUCCESS;
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
