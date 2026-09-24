// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_e2e.h"
#include "pusch_equalized_metrics.h"
#include "../../src/pusch_e2e_internal.cuh"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
constexpr unsigned maximum = 273 * 12 * 14 * 4;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void checked(cudaError_t status) { require(status == cudaSuccess, cudaGetErrorString(status)); }

void precheck()
{
    pusch_e2e_ctx ctx{};
    require(pusch_e2e_dapp_noise_async(nullptr, nullptr, 1, nullptr) == NR_LDPC_ERROR_INVALID_CONFIG, "null handle");
    require(pusch_e2e_dapp_noise_async(&ctx, nullptr, maximum+1, nullptr) == NR_LDPC_ERROR_INVALID_CONFIG, "count bound");
    require(pusch_e2e_dapp_noise_async(&ctx, nullptr, 1, nullptr) == NR_LDPC_ERROR_INVALID_CONFIG, "null input");
    require(pusch_e2e_dapp_noise_async(&ctx, nullptr, 0, nullptr) == NR_LDPC_ERROR_INVALID_CONFIG, "missing storage");
    float2 symbol{};
    require(pusch_e2e_dapp_metrics_async(&ctx, nullptr, nullptr, 1, 2, nullptr) == NR_LDPC_ERROR_INVALID_CONFIG,
            "metrics without inputs");
    for (unsigned bits : {0U, 1U, 3U, 5U, 10U}) {
        require(pusch_e2e_dapp_metrics_async(&ctx, nullptr, &symbol, 1, bits, nullptr) == NR_LDPC_ERROR_INVALID_CONFIG,
                "EVM modulation order");
    }
    require(std::isnan(pusch_e2e_dapp_noise_get_mean(nullptr)), "null getter");
    require(std::isnan(pusch_e2e_dapp_noise_get_mean(&ctx)), "missing getter storage");
    struct { float sum; unsigned count; } storage{};
    ctx.h_sinr_noise_var_sum = &storage.sum;
    ctx.h_sinr_count = &storage.count;
    for (float sum : {0.f, -1.f, INFINITY, NAN}) {
        *ctx.h_sinr_noise_var_sum = sum; *ctx.h_sinr_count = 1;
        require(std::isnan(pusch_e2e_dapp_noise_get_mean(&ctx)), "malformed result");
    }
    *ctx.h_sinr_noise_var_sum = 1.f; *ctx.h_sinr_count = 0;
    require(std::isnan(pusch_e2e_dapp_noise_get_mean(&ctx)), "empty result");
    *ctx.h_sinr_count = maximum+1;
    require(std::isnan(pusch_e2e_dapp_noise_get_mean(&ctx)), "bad result count");
}

struct resources {
    pusch_e2e_ctx ctx{};
    float* input{};
    float2* symbols{};
    cudaStream_t stream{};
    cudaEvent_t begin{}, end{};
    ~resources() {
        // Even a failed launch/copy may have queued work; drain before freeing.
        if (stream && cudaStreamSynchronize(stream) != cudaSuccess) std::_Exit(5);
        if (begin) cudaEventDestroy(begin);
        if (end) cudaEventDestroy(end);
        if (input) cudaFree(input);
        if (symbols) cudaFree(symbols);
        if (ctx.d_noise_var_partial) cudaFree(ctx.d_noise_var_partial);
        if (ctx.h_sinr_eq_block && !ctx.sinr_managed) cudaFreeHost(ctx.h_sinr_eq_block);
        if (ctx.d_eq_noise_var_sum) cudaFree(ctx.d_eq_noise_var_sum);
        if (stream) cudaStreamDestroy(stream);
    }
    void create(bool managed) {
        int least{}, greatest{};
        checked(cudaDeviceGetStreamPriorityRange(&least, &greatest));
        checked(cudaStreamCreateWithPriority(&stream, cudaStreamNonBlocking, least));
        checked(cudaEventCreate(&begin)); checked(cudaEventCreate(&end));
        checked(cudaMalloc(&input, maximum*sizeof(float)));
        checked(cudaMalloc(&symbols, maximum*sizeof(float2)));
        checked(cudaMalloc(&ctx.d_noise_var_partial, 8192));
        ctx.noise_var_partial_capacity = 8192;
        ctx.sinr_managed = managed;
        if (managed) {
            checked(cudaMallocManaged(&ctx.d_eq_noise_var_sum, 16));
            ctx.h_sinr_eq_block = ctx.d_eq_noise_var_sum;
        } else {
            checked(cudaMalloc(&ctx.d_eq_noise_var_sum, 16));
            checked(cudaMallocHost(&ctx.h_sinr_eq_block, 16));
        }
        ctx.d_eq_noise_var_count = reinterpret_cast<unsigned*>(ctx.d_eq_noise_var_sum+1);
        ctx.h_sinr_noise_var_sum = static_cast<float*>(ctx.h_sinr_eq_block);
        ctx.h_sinr_count = reinterpret_cast<unsigned*>(ctx.h_sinr_noise_var_sum+1);
        // The EVM half of the same [sum, count, evm_sum, evm_count] mailbox.
        ctx.d_evm_error_sum = ctx.d_eq_noise_var_sum+2;
        ctx.d_evm_symbol_count = reinterpret_cast<unsigned*>(ctx.d_eq_noise_var_sum+3);
        ctx.h_evm_error_sum = ctx.h_sinr_noise_var_sum+2;
        ctx.h_evm_symbol_count = reinterpret_cast<unsigned*>(ctx.h_sinr_noise_var_sum+3);
        ctx.config.enable_evm_metric = 1;
        require(pusch_e2e_dapp_noise_async(&ctx, nullptr, 0, stream) == NR_LDPC_SUCCESS, "warmup");
        checked(cudaStreamSynchronize(stream));
        require(std::isnan(pusch_e2e_dapp_noise_get_mean(&ctx)), "warmup unavailable");
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
};

void execute(bool managed)
{
    // Declared before owner so an exceptional exit drains any queued H2D before
    // destroying its host source. Resize only after the prior completion fence.
    std::vector<float> values;
    resources owner;
    owner.create(managed);
    auto& ctx = owner.ctx;
    unsigned cases = 0;
    for (unsigned size : {maximum, 0U, 1U, 31U, 255U, 256U, 257U, 576U, 1728U, 7344U, maximum}) {
        for (unsigned pattern = 0; pattern < 5; ++pattern) {
            values.resize(size);
            const float special[] = {NAN, INFINITY, -INFINITY, 0.f, -1.f, 1.e5f, 1.e6f};
            for (unsigned i = 0; i < size; ++i) {
                if (pattern == 0) values[i] = .01f;
                if (pattern == 1) values[i] = special[i % 7];
                if (pattern == 2) values[i] = i % 3 ? std::pow(10.f, -5.f+9.f*float(i%107)/107.f) : special[i%7];
                if (pattern == 3) values[i] = std::nextafter(1.e5f, 0.f);
                if (pattern == 4) values[i] = 1.e-30f;
            }
            double sum = 0.; unsigned count = 0;
            for (float v : values) if (std::isfinite(v) && v > 0.f && v < 1.e5f) { sum += v; ++count; }
            const float expected = count ? static_cast<float>(sum/count) : NAN;
            // Sentinel proves an empty result replaces the last successful slot;
            // the eight-byte copy must leave unrelated EVM host storage unchanged.
            *ctx.h_sinr_noise_var_sum = 123.f; *ctx.h_sinr_count = 123;
            static_cast<unsigned*>(ctx.h_sinr_eq_block)[2] = 0x5a5a5a5a;
            static_cast<unsigned*>(ctx.h_sinr_eq_block)[3] = 0x5a5a5a5a;
            if (size) checked(cudaMemcpyAsync(owner.input, values.data(), size*sizeof(float), cudaMemcpyHostToDevice, owner.stream));
            checked(cudaEventRecord(owner.begin, owner.stream));
            const auto start = std::chrono::steady_clock::now();
            require(pusch_e2e_dapp_noise_async(&ctx, owner.input, size, owner.stream) == NR_LDPC_SUCCESS, "enqueue");
            const auto submitted = std::chrono::steady_clock::now();
            checked(cudaEventRecord(owner.end, owner.stream));
            // This is the caller's existing completion fence, not a sync in the API.
            checked(cudaEventSynchronize(owner.end));
            float ms{}; checked(cudaEventElapsedTime(&ms, owner.begin, owner.end));
            const float actual = pusch_e2e_dapp_noise_get_mean(&ctx);
            require(*ctx.h_sinr_count == count, "exact eligible count");
            require(static_cast<unsigned*>(ctx.h_sinr_eq_block)[2] == 0x5a5a5a5a &&
                    static_cast<unsigned*>(ctx.h_sinr_eq_block)[3] == 0x5a5a5a5a, "readback extent");
            if (!count) require(std::isnan(actual), "empty/erased must be unavailable");
            else require(std::isfinite(actual) && std::abs(actual/expected-1.f) < 3.e-7f &&
                         std::abs(-10.*std::log10(static_cast<double>(actual))+
                                   10.*std::log10(static_cast<double>(expected))) < 2.e-6, "CPU metric parity");
            const double error_db = count ? -10.*std::log10(static_cast<double>(actual))+
                                            10.*std::log10(static_cast<double>(expected)) : 0.;
            std::printf("{\"managed\":%s,\"size\":%u,\"pattern\":%u,\"count\":%u,\"error_db\":%.12g,\"gpu_us\":%.6f,\"enqueue_us\":%.6f}\n",
                        managed?"true":"false", size, pattern, count, error_db, ms*1000.,
                        std::chrono::duration<double,std::micro>(submitted-start).count());
            ++cases;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    require(cases == 55, "complete panel");
}
void execute_evm(bool managed)
{
    std::vector<float> noise;
    std::vector<float2> symbols;
    resources owner;
    owner.create(managed);
    auto& ctx = owner.ctx;
    // Missing EVM storage fails closed instead of writing past the noise half.
    {
        pusch_e2e_ctx drift = ctx;
        drift.h_evm_error_sum = nullptr;
        require(pusch_e2e_dapp_metrics_async(&drift, nullptr, owner.symbols, 1, 2, owner.stream) ==
                    NR_LDPC_ERROR_INVALID_CONFIG, "EVM layout drift");
    }
    std::mt19937 rng(20260924);
    std::normal_distribution<float> jitter(0.f, .03f);
    unsigned cases = 0;
    for (unsigned bits : {2U, 4U, 6U, 8U}) {
        const int axis = 1 << (bits / 2);
        const float scale = std::sqrt(3.f / (2.f * float(axis * axis - 1)));
        for (unsigned size : {0U, 1U, 257U, 7344U, maximum}) {
            for (bool with_noise : {false, true}) {
                noise.assign(size, .02f);
                symbols.resize(size);
                for (unsigned i = 0; i < size; ++i) {
                    const float re = float(int(rng() % unsigned(axis)) * 2 + 1 - axis) * scale;
                    const float im = float(int(rng() % unsigned(axis)) * 2 + 1 - axis) * scale;
                    symbols[i] = i % 97 == 5 ? float2{NAN, 0.f} : float2{re + jitter(rng), im + jitter(rng)};
                }
                double error = 0.; unsigned count = 0;
                for (const float2& v : symbols) {
                    if (std::isfinite(v.x) && std::isfinite(v.y)) {
                        error += ocudu::detail::pusch_qam_error_power(v.x, v.y, bits);
                        ++count;
                    }
                }
                const float expected = count ? static_cast<float>(std::sqrt(error / count)) : NAN;
                if (size) {
                    checked(cudaMemcpyAsync(owner.symbols, symbols.data(), size*sizeof(float2), cudaMemcpyHostToDevice, owner.stream));
                    checked(cudaMemcpyAsync(owner.input, noise.data(), size*sizeof(float), cudaMemcpyHostToDevice, owner.stream));
                }
                *ctx.h_evm_error_sum = 123.f; *ctx.h_evm_symbol_count = 123;
                require(pusch_e2e_dapp_metrics_async(&ctx, with_noise ? owner.input : nullptr, owner.symbols, size, bits,
                                                     owner.stream) == NR_LDPC_SUCCESS, "EVM enqueue");
                checked(cudaEventRecord(owner.end, owner.stream));
                checked(cudaEventSynchronize(owner.end));
                const float actual = pusch_e2e_evm_get_result(&ctx);
                require(*ctx.h_evm_symbol_count == count, "exact EVM symbol count");
                if (!count) require(std::isnan(actual), "empty EVM must be unavailable");
                else require(std::isfinite(actual) && std::abs(actual/expected-1.f) < 1.e-5f, "EVM CPU parity");
                const float mean = pusch_e2e_dapp_noise_get_mean(&ctx);
                if (with_noise && size) require(std::abs(mean/.02f-1.f) < 1.e-6f, "noise alongside EVM");
                else require(std::isnan(mean), "EVM-only grant publishes no noise");
                std::printf("{\"managed\":%s,\"bits\":%u,\"size\":%u,\"noise\":%s,\"evm\":%.9g,\"expected\":%.9g}\n",
                            managed?"true":"false", bits, size, with_noise?"true":"false", actual, expected);
                ++cases;
            }
        }
    }
    require(cases == 40, "complete EVM panel");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        precheck();
        if (argc == 2 && !std::strcmp(argv[1], "--precheck")) {
            std::puts("PRE_CUDA_DAPP_NOISE_CHECKS_OK"); return 0;
        }
        if (argc != 2 || std::strcmp(argv[1], "--execute")) return 2;
        execute(false); execute(true);
        execute_evm(false); execute_evm(true);
        std::puts("COMPLETE_DAPP_METRICS_GPU_PARITY_110_EVM_80"); return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what()); return 1;
    }
}
