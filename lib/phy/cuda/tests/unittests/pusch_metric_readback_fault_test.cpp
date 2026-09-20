// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_e2e.h"
#include "../../src/pusch_e2e_internal.cuh"
#include "../../../upper/channel_processors/pusch/pusch_metric_readback_completion.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {
unsigned copies{}, failed_copy{}, event_queries{}, stream_queries{}, cases{};
cudaError_t event_result = cudaSuccess, stream_result = cudaSuccess;
void require(bool ok) { if (!ok) throw std::runtime_error("metric readback fault check failed"); }
void reset(unsigned fail = 0)
{
    copies = event_queries = stream_queries = 0;
    failed_copy = fail; event_result = stream_result = cudaSuccess;
}
struct accum { float noise; unsigned count; float evm; unsigned evm_count; };
static_assert(sizeof(accum) == 16);
}

extern "C" cudaError_t __wrap_cudaMemcpyAsync(void*, const void*, size_t bytes, cudaMemcpyKind kind, cudaStream_t)
{
    ++copies;
    require(copies <= 2 && bytes == (copies == 1 ? 16U : 20U) && kind == cudaMemcpyDeviceToHost);
    return copies == failed_copy ? cudaErrorInvalidValue : cudaSuccess;
}
extern "C" cudaError_t __wrap_cudaEventQuery(cudaEvent_t)
{
    ++event_queries;
    return event_result;
}
extern "C" cudaError_t __wrap_cudaStreamQuery(cudaStream_t)
{
    ++stream_queries;
    return stream_result;
}

int main()
{
    try {
        // Force the production yielding completion helper to use the wrapped
        // queries. No allocation, context, copy, kernel or actual wait occurs.
        setenv("OCUDU_CUDA_SYNC_BLOCKING", "0", 1);
        accum device{1,100,1,100}, host{0.25f,100,0.25f,100};
        float device_pilot[5]{}, host_pilot[5]{25,-20,-21,0,0};
        pusch_e2e_ctx ctx{};
        ctx.d_eq_noise_var_sum = &device.noise; ctx.d_eq_noise_var_count = &device.count;
        ctx.h_sinr_eq_block = &host; ctx.h_sinr_noise_var_sum = &host.noise; ctx.h_sinr_count = &host.count;
        ctx.d_sinr_epre_result = device_pilot; ctx.h_sinr_epre_result = host_pilot;
        const accum sentinel = host;
        for (unsigned fail = 0; fail <= 2; ++fail) {
            reset(fail);
            const auto status = pusch_e2e_sinr_async_launch_checked(&ctx, nullptr);
            require(status == (fail ? NR_LDPC_ERROR_CUDA_FAILED : NR_LDPC_SUCCESS));
            require(copies == (fail == 1 ? 1U : 2U));
            // No racing memset to invalidate a partly queued destination.
            require(std::memcmp(&host, &sentinel, sizeof(host)) == 0 && host_pilot[0] == 25);
            ++cases;
        }
        for (unsigned field = 0; field < 9; ++field) {
            auto bad = ctx;
            if (field == 0) bad.d_eq_noise_var_sum = nullptr;
            if (field == 1) bad.d_eq_noise_var_count = nullptr;
            if (field == 2) bad.h_sinr_eq_block = nullptr;
            if (field == 3) bad.h_sinr_noise_var_sum = nullptr;
            if (field == 4) bad.h_sinr_count = nullptr;
            if (field == 5) bad.d_eq_noise_var_count = &host.count;
            if (field == 6) bad.h_sinr_count = &device.count;
            if (field == 7) bad.d_sinr_epre_result = nullptr;
            if (field == 8) bad.h_sinr_epre_result = nullptr;
            reset();
            require(pusch_e2e_sinr_async_launch_checked(&bad, nullptr) == NR_LDPC_ERROR_INVALID_CONFIG);
            require(copies == 0); ++cases;
        }
        reset();
        require(pusch_e2e_sinr_async_launch_checked(nullptr, nullptr) == NR_LDPC_ERROR_INVALID_CONFIG);
        require(copies == 0); ++cases;
        auto no_pilot = ctx;
        no_pilot.d_sinr_epre_result = no_pilot.h_sinr_epre_result = nullptr;
        reset();
        require(pusch_e2e_sinr_async_launch_checked(&no_pilot, nullptr) == NR_LDPC_SUCCESS);
        require(copies == 1); ++cases;

        ctx.sinr_managed = true;
        reset();
        require(pusch_e2e_sinr_async_launch_checked(&ctx, nullptr) == NR_LDPC_ERROR_INVALID_CONFIG);
        require(copies == 0); ++cases;
        ctx.h_sinr_eq_block = &device; ctx.h_sinr_noise_var_sum = &device.noise; ctx.h_sinr_count = &device.count;
        reset();
        require(pusch_e2e_sinr_async_launch_checked(&ctx, nullptr) == NR_LDPC_ERROR_INVALID_CONFIG);
        require(copies == 0); ++cases;
        ctx.h_sinr_epre_result = device_pilot;
        reset();
        require(pusch_e2e_sinr_async_launch_checked(&ctx, nullptr) == NR_LDPC_SUCCESS);
        require(copies == 0); ++cases;

        // Real host completion helper: successful old events must not authorize
        // a failed submission, and failed/unrecorded events cannot be consumed.
        const auto event = reinterpret_cast<cudaEvent_t>(&ctx);
        const auto stream = reinterpret_cast<cudaStream_t>(&device);
        for (bool submitted : {false, true}) {
            for (bool recorded : {false, true}) {
                for (bool side : {false, true}) {
                    for (bool error : {false, true}) {
                        reset();
                        if (error) event_result = stream_result = cudaErrorInvalidResourceHandle;
                        require(ocudu::pusch_metric_readback_completed(submitted, recorded, side, event, stream) ==
                                (submitted && !error));
                        require(event_queries == (recorded ? 1U : 0U));
                        require(stream_queries == (recorded ? 0U : 1U));
                        ++cases;
                    }
                }
            }
        }
        reset();
        require(ocudu::pusch_metric_readback_completed(true, true, false, nullptr, stream));
        require(event_queries == 0 && stream_queries == 1); ++cases;
        reset(); event_result = cudaErrorNotReady;
        require(!ocudu::pusch_metric_readback_completed(true, true, false, event, stream));
        require(event_queries == 1 && stream_queries == 0); ++cases;
        std::printf("{\"status\":\"READBACK_SUBMISSION_AND_COMPLETION_FAULTS_OK\",\"cases\":%u}\n", cases);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: cases=%u copies=%u event_queries=%u stream_queries=%u\n",
                     e.what(), cases, copies, event_queries, stream_queries);
        return 1;
    }
}
