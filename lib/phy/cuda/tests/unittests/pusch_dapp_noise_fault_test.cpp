// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Link-wrapped CUDA submission. No kernels, memory transfers or GPU allocation.
#include "pusch_e2e.h"
#include "../../src/pusch_e2e_internal.cuh"
#include <cstdio>
#include <stdexcept>

namespace {
unsigned launches{}, copies{}, fail_launch{};
bool fail_copy{};
cudaError_t pending = cudaSuccess;
dim3 saved_grid{}, saved_block{};
size_t saved_shared{};
void* saved_stream{};
void require(bool ok) { if (!ok) throw std::runtime_error("metric submission fault check failed"); }
void reset(unsigned failed_launch = 0, bool failed_copy = false) {
    launches = copies = 0; fail_launch = failed_launch; fail_copy = failed_copy; pending = cudaSuccess;
}

extern "C" unsigned __wrap___cudaPushCallConfiguration(dim3 grid, dim3 block, size_t shared, void* stream) {
    saved_grid = grid; saved_block = block; saved_shared = shared; saved_stream = stream; return 0;
}
extern "C" cudaError_t __wrap___cudaPopCallConfiguration(dim3* grid, dim3* block, size_t* shared, void* stream) {
    *grid = saved_grid; *block = saved_block; *shared = saved_shared;
    *static_cast<void**>(stream) = saved_stream; return cudaSuccess;
}
}

extern "C" cudaError_t __wrap_cudaLaunchKernel(const void*, dim3 grid, dim3 block, void**, size_t, cudaStream_t)
{
    ++launches;
    require(grid.x >= 1 && grid.x <= 32 && grid.y == 1 && grid.z == 1);
    require(block.x == (launches == 1 ? 256 : 1) && block.y == 1 && block.z == 1);
    pending = launches == fail_launch ? cudaErrorInvalidConfiguration : cudaSuccess;
    return pending;
}
#if CUDART_VERSION >= 13000
extern "C" cudaError_t __wrap___cudaGetKernel(cudaKernel_t* kernel, const void* function) {
    *kernel = reinterpret_cast<cudaKernel_t>(const_cast<void*>(function)); return cudaSuccess;
}
extern "C" cudaError_t __wrap___cudaLaunchKernel(cudaKernel_t, dim3 grid, dim3 block,
                                                 void** args, size_t shared, cudaStream_t stream) {
    return __wrap_cudaLaunchKernel(nullptr, grid, block, args, shared, stream);
}
#endif
extern "C" cudaError_t __wrap_cudaGetLastError() {
    const cudaError_t value = pending; pending = cudaSuccess; return value;
}
extern "C" cudaError_t __wrap_cudaMemcpyAsync(void*, const void*, size_t bytes, cudaMemcpyKind kind, cudaStream_t) {
    ++copies; require(bytes == 8 && kind == cudaMemcpyDeviceToHost);
    return fail_copy ? cudaErrorInvalidValue : cudaSuccess;
}

int main()
{
    try {
        struct accum { float sum; unsigned count; } device{}, host{};
        alignas(16) float scratch[2048]{};
        float input{};
        pusch_e2e_ctx ctx{};
        ctx.d_eq_noise_var_sum = &device.sum; ctx.d_eq_noise_var_count = &device.count;
        ctx.h_sinr_eq_block = &host; ctx.h_sinr_noise_var_sum = &host.sum; ctx.h_sinr_count = &host.count;
        ctx.d_noise_var_partial = scratch; ctx.noise_var_partial_capacity = sizeof(scratch);
        for (unsigned fail = 0; fail < 4; ++fail) {
            reset(fail == 3 ? 0 : fail, fail == 3);
            const auto status = pusch_e2e_dapp_noise_async(&ctx, &input, 576, nullptr);
            require(status == (fail ? NR_LDPC_ERROR_CUDA_FAILED : NR_LDPC_SUCCESS));
            require(launches == (fail == 1 ? 1 : 2));
            require(copies == ((fail == 0 || fail == 3) ? 1 : 0));
            require(host.count == 0 && host.sum == 0.f);
        }
        // Managed aliases enqueue the same two kernels but no transfer.
        ctx.sinr_managed = true;
        reset();
        require(pusch_e2e_dapp_noise_async(&ctx, &input, 576, nullptr) == NR_LDPC_ERROR_INVALID_CONFIG);
        require(launches == 0 && copies == 0);
        ctx.h_sinr_eq_block = &device; ctx.h_sinr_noise_var_sum = &device.sum; ctx.h_sinr_count = &device.count;
        reset();
        require(pusch_e2e_dapp_noise_async(&ctx, &input, 183456, nullptr) == NR_LDPC_SUCCESS);
        require(launches == 2 && copies == 0);
        reset();
        ctx.noise_var_partial_capacity = 1;
        require(pusch_e2e_dapp_noise_async(&ctx, &input, 1, nullptr) == NR_LDPC_ERROR_INVALID_CONFIG);
        require(launches == 0 && copies == 0);
        std::puts("DAPP_NOISE_SUBMISSION_FAULT_CHECKS_OK"); return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: launches=%u copies=%u fail_launch=%u fail_copy=%d\n",
                     error.what(), launches, copies, fail_launch, fail_copy); return 1;
    }
}
