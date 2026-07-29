// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "prach_detector.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cuComplex.h>
#include <cuda.h>
#include <mutex>
#include <new>
#include <vector>
#include "vkFFT.h"

#ifndef OCUDU_PHY_CUDA_ENABLE_VKFFT
#error "OCUDU PHY CUDA PRACH acceleration requires the bundled VkFFT backend."
#endif

/// Maximum number of PRACH preambles, and therefore candidate slots, per detection.
constexpr int MAX_PRACH_CANDIDATES = 64;

/// Device-to-host results of one detection: keeping the candidate list and the RSSI accumulator
/// contiguous allows a single copy/readback per call instead of one per member.
struct prach_detector_result_block {
    prach_detector_candidate_t candidates[MAX_PRACH_CANDIDATES];
    float rssi;
};

struct prach_detector_ctx {
    uint32_t* d_input = nullptr;
    size_t d_input_capacity = 0;
    cuFloatComplex* d_roots = nullptr;
    size_t d_roots_capacity = 0;
    cuFloatComplex* d_idft = nullptr;
    size_t d_idft_capacity = 0;
    prach_detector_result_block* d_results = nullptr;        // active device view for kernels
    prach_detector_result_block* d_results_device = nullptr; // dedicated device allocation
    prach_detector_result_block* d_results_mapped = nullptr; // mapped view of h_results (or null)
    prach_detector_result_block* h_results = nullptr;        // host view after sync
    bool h_result_pinned = false;
    bool mapped_results_available = false;
    bool results_zero_copy = false; // current call uses mapped (no D2H)
    cudaStream_t owned_stream = nullptr;
    cudaEvent_t done_event = nullptr;
    VkFFTApplication vkfft_app{};
    CUdevice vkfft_device = 0;
    cudaStream_t vkfft_stream = nullptr;
    void* vkfft_buffer = nullptr;
    pfUINT vkfft_buffer_size = 0;
    int vkfft_dft_size = 0;
    int vkfft_batch = 0;
    bool vkfft_valid = false;
    uint64_t root_cache_key = 0;
    int cached_nof_sequences = 0;
    int cached_sequence_length = 0;

    // Optional CUDA graph cache (keyed on geometry + input pointer + stream).
    struct graph_entry {
        const uint32_t* d_input = nullptr;
        cudaStream_t stream = nullptr;
        int dft_size = 0;
        int nof_sequences = 0;
        int batches_per_sequence = 0;
        int nof_shifts = 0;
        int nof_rx_ports = 0;
        int nof_symbols = 0;
        int combine_symbols = 0;
        int sequence_length = 0;
        int zero_copy_results = 0;
        cudaGraph_t graph = nullptr;
        cudaGraphExec_t exec = nullptr;
    };
    std::vector<graph_entry> graphs;
    size_t next_graph_replacement = 0;
    bool graph_capture_failed = false;
};

namespace {

constexpr int THREADS = 256;
constexpr int MAX_BATCHES_PER_SEQUENCE = 128;
constexpr size_t GRAPH_CACHE_LIMIT = 32;

bool env_flag_enabled(const char* name, bool default_value = false)
{
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return default_value;
    }
    return (std::strcmp(value, "1") == 0) || (std::strcmp(value, "true") == 0) || (std::strcmp(value, "on") == 0) ||
           (std::strcmp(value, "yes") == 0) || (std::strcmp(value, "enabled") == 0);
}

bool env_flag_disabled(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }
    return (std::strcmp(value, "0") == 0) || (std::strcmp(value, "false") == 0) || (std::strcmp(value, "off") == 0) ||
           (std::strcmp(value, "no") == 0) || (std::strcmp(value, "disabled") == 0);
}

// Default on: amortizes multi-kernel detector launches (prepare + VkFFT + candidates).
// Disable with OCUDU_PRACH_CUDA_GRAPHS=0.
bool cuda_graphs_enabled()
{
    static const bool enabled = []() {
        if (env_flag_disabled("OCUDU_PRACH_CUDA_GRAPHS")) {
            return false;
        }
        if (std::getenv("OCUDU_PRACH_CUDA_GRAPHS") == nullptr) {
            return true;
        }
        return env_flag_enabled("OCUDU_PRACH_CUDA_GRAPHS", true);
    }();
    return enabled;
}

std::mutex& graph_capture_mutex()
{
    static std::mutex mutex;
    return mutex;
}

cudaError_t create_realtime_stream(cudaStream_t* stream)
{
    int least_priority = 0;
    int greatest_priority = 0;
    cudaError_t status = cudaDeviceGetStreamPriorityRange(&least_priority, &greatest_priority);
    if (status == cudaSuccess) {
        status = cudaStreamCreateWithPriority(stream, cudaStreamNonBlocking, greatest_priority);
        if (status == cudaSuccess) {
            return cudaSuccess;
        }
        (void)cudaGetLastError();
    }
    return cudaStreamCreateWithFlags(stream, cudaStreamNonBlocking);
}

__device__ __forceinline__ float bf16_to_float(uint16_t v)
{
    union {
        uint32_t u;
        float f;
    } conv;
    conv.u = static_cast<uint32_t>(v) << 16;
    return conv.f;
}

__device__ __forceinline__ cuFloatComplex cbf16_to_cfloat(uint32_t packed)
{
    return make_cuFloatComplex(bf16_to_float(static_cast<uint16_t>(packed & 0xffffu)),
                               bf16_to_float(static_cast<uint16_t>(packed >> 16)));
}

__device__ __forceinline__ cuFloatComplex cmul_conj(cuFloatComplex x, cuFloatComplex y)
{
    return make_cuFloatComplex(x.x * y.x + x.y * y.y, x.y * y.x - x.x * y.y);
}

__device__ __forceinline__ float cabs2(cuFloatComplex x)
{
    return x.x * x.x + x.y * x.y;
}

__device__ __forceinline__ float warp_reduce_sum(float value)
{
    // Full-mask warp reduction (THREADS is a multiple of 32).
    value += __shfl_down_sync(0xffffffffu, value, 16);
    value += __shfl_down_sync(0xffffffffu, value, 8);
    value += __shfl_down_sync(0xffffffffu, value, 4);
    value += __shfl_down_sync(0xffffffffu, value, 2);
    value += __shfl_down_sync(0xffffffffu, value, 1);
    return value;
}

__device__ __forceinline__ void warp_reduce_max_metric(float& metric, float& power, int& delay)
{
    float other_metric = __shfl_down_sync(0xffffffffu, metric, 16);
    float other_power = __shfl_down_sync(0xffffffffu, power, 16);
    int other_delay = __shfl_down_sync(0xffffffffu, delay, 16);
    if (other_metric > metric) {
        metric = other_metric;
        power = other_power;
        delay = other_delay;
    }
    other_metric = __shfl_down_sync(0xffffffffu, metric, 8);
    other_power = __shfl_down_sync(0xffffffffu, power, 8);
    other_delay = __shfl_down_sync(0xffffffffu, delay, 8);
    if (other_metric > metric) {
        metric = other_metric;
        power = other_power;
        delay = other_delay;
    }
    other_metric = __shfl_down_sync(0xffffffffu, metric, 4);
    other_power = __shfl_down_sync(0xffffffffu, power, 4);
    other_delay = __shfl_down_sync(0xffffffffu, delay, 4);
    if (other_metric > metric) {
        metric = other_metric;
        power = other_power;
        delay = other_delay;
    }
    other_metric = __shfl_down_sync(0xffffffffu, metric, 2);
    other_power = __shfl_down_sync(0xffffffffu, power, 2);
    other_delay = __shfl_down_sync(0xffffffffu, delay, 2);
    if (other_metric > metric) {
        metric = other_metric;
        power = other_power;
        delay = other_delay;
    }
    other_metric = __shfl_down_sync(0xffffffffu, metric, 1);
    other_power = __shfl_down_sync(0xffffffffu, power, 1);
    other_delay = __shfl_down_sync(0xffffffffu, delay, 1);
    if (other_metric > metric) {
        metric = other_metric;
        power = other_power;
        delay = other_delay;
    }
}

__global__ void kernel_prepare_idft(const uint32_t* __restrict__ input,
                                    const cuFloatComplex* __restrict__ roots,
                                    cuFloatComplex* __restrict__ idft,
                                    float* __restrict__ rssi,
                                    prach_detector_config_t cfg,
                                    int batches_per_sequence)
{
    __shared__ float rssi_partial[THREADS];

    int local_sequence = blockIdx.y;
    int batch_in_sequence = blockIdx.x;
    int freq_bin = threadIdx.x + blockIdx.z * blockDim.x;
    if (local_sequence >= cfg.nof_sequences || batch_in_sequence >= batches_per_sequence) {
        return;
    }

    int port = 0;
    int symbol = 0;
    if (cfg.combine_symbols) {
        port = batch_in_sequence;
    } else {
        port = batch_in_sequence / cfg.nof_symbols;
        symbol = batch_in_sequence - port * cfg.nof_symbols;
    }

    const int half = cfg.sequence_length / 2;
    int root_re = -1;
    if (freq_bin < cfg.sequence_length - half) {
        root_re = freq_bin + half;
    } else if (freq_bin >= cfg.dft_size - half && freq_bin < cfg.dft_size) {
        root_re = freq_bin - (cfg.dft_size - half);
    }

    cuFloatComplex sample = make_cuFloatComplex(0.0f, 0.0f);
    float rssi_sum = 0.0f;
    if (root_re >= 0) {
        if (cfg.combine_symbols) {
            for (int i_symbol = 0; i_symbol != cfg.nof_symbols; ++i_symbol) {
                const uint32_t packed =
                    input[port * cfg.input_port_stride + i_symbol * cfg.input_symbol_stride + root_re];
                cuFloatComplex v = cbf16_to_cfloat(packed);
                sample.x += v.x;
                sample.y += v.y;
                rssi_sum += cabs2(v);
            }
        } else {
            sample = cbf16_to_cfloat(input[port * cfg.input_port_stride + symbol * cfg.input_symbol_stride + root_re]);
            rssi_sum = cabs2(sample);
        }
    }

    if (local_sequence == 0) {
        // Warp-then-block reduction: fewer __syncthreads than a pure shared tree.
        float warp_sum = warp_reduce_sum(rssi_sum);
        if ((threadIdx.x & 31) == 0) {
            rssi_partial[threadIdx.x >> 5] = warp_sum;
        }
        __syncthreads();
        if (threadIdx.x < 32) {
            constexpr int nof_warps = THREADS / 32;
            float block_sum = (threadIdx.x < nof_warps) ? rssi_partial[threadIdx.x] : 0.0f;
            block_sum = warp_reduce_sum(block_sum);
            if (threadIdx.x == 0) {
                atomicAdd(rssi, block_sum);
            }
        }
    }

    int batch = local_sequence * batches_per_sequence + batch_in_sequence;
    if (root_re < 0) {
        if (freq_bin < cfg.dft_size) {
            idft[batch * cfg.dft_size + freq_bin] = make_cuFloatComplex(0.0f, 0.0f);
        }
        return;
    }

    cuFloatComplex no_root = cmul_conj(sample, roots[local_sequence * cfg.sequence_length + root_re]);
    idft[batch * cfg.dft_size + freq_bin] = no_root;
}

__global__ void kernel_find_candidates(const cuFloatComplex* __restrict__ idft,
                                       prach_detector_candidate_t* __restrict__ candidates,
                                       prach_detector_config_t cfg,
                                       int batches_per_sequence)
{
    __shared__ float best_metric[THREADS];
    __shared__ float best_power[THREADS];
    __shared__ float reference_power[MAX_BATCHES_PER_SEQUENCE];
    __shared__ float reduction_scratch[THREADS];
    __shared__ int best_delay[THREADS];

    int local_sequence = blockIdx.y;
    int sequence = cfg.sequence_start + local_sequence;
    int window = blockIdx.x;
    int preamble_index = sequence * cfg.nof_shifts + window;
    int out_index = preamble_index;

    float local_metric = -1.0f;
    float local_power = 0.0f;
    int local_delay = 0;

    if (preamble_index < MAX_PRACH_CANDIDATES && preamble_index >= cfg.start_preamble_index &&
        preamble_index < cfg.start_preamble_index + cfg.nof_preamble_indices) {
        // Match host generic detector exactly (true IDFT time axis):
        //   window_start = (dft_size - (N_cs * i_window * dft_size) / L_ra) % dft_size
        //   delay d samples host index (window_start + d) % dft_size
        const int window_start =
            (cfg.dft_size - ((cfg.n_cs * window * cfg.dft_size) / cfg.sequence_length)) % cfg.dft_size;
        const float mod_scale = 1.0f / static_cast<float>(cfg.dft_size * cfg.sequence_length);
        const float window_scale = static_cast<float>(cfg.dft_size) / static_cast<float>(cfg.sequence_length);
        // Host: i_start = ((window_start + dft_size) - win_margin) % dft_size
        const int ref_start = (window_start + cfg.dft_size - cfg.win_margin) % cfg.dft_size;
        const int ref_len = 2 * cfg.win_margin + cfg.win_width;

        for (int b = 0; b != batches_per_sequence; ++b) {
            const cuFloatComplex* corr = idft + (local_sequence * batches_per_sequence + b) * cfg.dft_size;
            float partial = 0.0f;
            for (int i = threadIdx.x; i < ref_len; i += blockDim.x) {
                int idx = (ref_start + i) % cfg.dft_size;
                partial += cabs2(corr[idx]) * mod_scale;
            }

            float warp_sum = warp_reduce_sum(partial);
            if ((threadIdx.x & 31) == 0) {
                reduction_scratch[threadIdx.x >> 5] = warp_sum;
            }
            __syncthreads();
            if (threadIdx.x < 32) {
                constexpr int nof_warps = THREADS / 32;
                float block_sum = (threadIdx.x < nof_warps) ? reduction_scratch[threadIdx.x] : 0.0f;
                block_sum = warp_reduce_sum(block_sum);
                if (threadIdx.x == 0) {
                    reference_power[b] = block_sum;
                }
            }
            __syncthreads();
        }

        for (int delay = threadIdx.x; delay < cfg.win_width; delay += blockDim.x) {
            float numerator = 0.0f;
            float denominator = 0.0f;

            for (int b = 0; b != batches_per_sequence; ++b) {
                const cuFloatComplex* corr = idft + (local_sequence * batches_per_sequence + b) * cfg.dft_size;
                // Host: mod_square.subspan(window_start, win_width)[delay] (wrap if needed).
                int sample_idx = window_start + delay;
                if (sample_idx >= cfg.dft_size) {
                    sample_idx -= cfg.dft_size;
                }

                float window_power = cabs2(corr[sample_idx]) * mod_scale * window_scale;
                // Match host vector_noise_estimation. Use device-safe FLT_MIN floor instead of
                // isnormal(): CUDA device isnormal() is not reliable here and was flooring
                // almost every den to 1e-9, exploding metrics (~1e15) and false alarms.
                float diff = reference_power[b] - window_power;
                constexpr float k_flt_min = 1.1754943508222875e-38f;
                if (!isfinite(diff) || diff == 0.0f || fabsf(diff) < k_flt_min) {
                    diff = 1e-9f;
                }
                numerator += window_power;
                denominator += diff;
            }

            denominator = fabsf(denominator);
            if (!isfinite(denominator) || denominator <= 0.0f) {
                denominator = 1e-9f;
            }
            float metric = numerator / denominator;
            if (metric > local_metric) {
                local_metric = metric;
                local_power = numerator;
                local_delay = delay;
            }
        }
    }

    // Intra-warp max, then one shared step across warps.
    warp_reduce_max_metric(local_metric, local_power, local_delay);
    if ((threadIdx.x & 31) == 0) {
        best_metric[threadIdx.x >> 5] = local_metric;
        best_power[threadIdx.x >> 5] = local_power;
        best_delay[threadIdx.x >> 5] = local_delay;
    }
    __syncthreads();

    if (threadIdx.x < 32) {
        constexpr int nof_warps = THREADS / 32;
        float m = (threadIdx.x < nof_warps) ? best_metric[threadIdx.x] : -1.0f;
        float p = (threadIdx.x < nof_warps) ? best_power[threadIdx.x] : 0.0f;
        int d = (threadIdx.x < nof_warps) ? best_delay[threadIdx.x] : 0;
        warp_reduce_max_metric(m, p, d);
        if (threadIdx.x == 0) {
            best_metric[0] = m;
            best_power[0] = p;
            best_delay[0] = d;
        }
    }
    __syncthreads();

    if (threadIdx.x == 0 && out_index < MAX_PRACH_CANDIDATES) {
        prach_detector_candidate_t out{};
        float max_delay_gate = static_cast<float>(cfg.max_delay_samples) * 0.8f;
        if (preamble_index < MAX_PRACH_CANDIDATES && best_metric[0] > cfg.threshold &&
            static_cast<float>(best_delay[0]) < max_delay_gate) {
            int power_norm = cfg.nof_rx_ports * cfg.sequence_length * cfg.nof_symbols;
            if (cfg.combine_symbols) {
                power_norm *= cfg.nof_symbols;
            }
            out.valid = 1;
            out.preamble_index = preamble_index;
            out.delay_samples = best_delay[0];
            out.detection_metric = best_metric[0] / cfg.threshold;
            out.preamble_power = best_power[0] / static_cast<float>(power_norm);
        }
        candidates[out_index] = out;
    }
}

bool check_cuda(cudaError_t status)
{
    return status == cudaSuccess;
}

bool ensure_capacity(void** ptr, size_t* capacity, size_t bytes)
{
    if (*capacity >= bytes) {
        return true;
    }
    if (*ptr != nullptr) {
        cudaFree(*ptr);
        *ptr = nullptr;
        *capacity = 0;
    }
    if (bytes == 0) {
        return true;
    }
    if (cudaMalloc(ptr, bytes) != cudaSuccess) {
        return false;
    }
    *capacity = bytes;
    return true;
}

bool device_is_integrated()
{
    static const bool integrated = []() {
        int value = 0;
        return (cudaDeviceGetAttribute(&value, cudaDevAttrIntegrated, 0) == cudaSuccess) && (value != 0);
    }();
    return integrated;
}

// Allocate:
//  - dedicated device result block (best for heavy multiport / long PRACH)
//  - mapped host result block when possible (best for light single-port / short PRACH)
// Selection is per-detect based on workload size (see prefer_mapped_results).
bool allocate_result_buffers(prach_detector_ctx* ctx)
{
    ctx->results_zero_copy = false;
    ctx->mapped_results_available = false;
    ctx->h_result_pinned = false;
    ctx->h_results = nullptr;
    ctx->d_results = nullptr;
    ctx->d_results_device = nullptr;
    ctx->d_results_mapped = nullptr;

    if (cudaMalloc(reinterpret_cast<void**>(&ctx->d_results_device), sizeof(prach_detector_result_block)) !=
        cudaSuccess) {
        return false;
    }

    // Try mapped host (optional zero-copy path). Force with OCUDU_PRACH_MAPPED_RESULTS=1,
    // disable with =0; default on for integrated GPUs.
    const bool try_mapped = !env_flag_disabled("OCUDU_PRACH_MAPPED_RESULTS") &&
                            (env_flag_enabled("OCUDU_PRACH_MAPPED_RESULTS", false) || device_is_integrated());
    if (try_mapped &&
        cudaHostAlloc(reinterpret_cast<void**>(&ctx->h_results),
                      sizeof(prach_detector_result_block),
                      cudaHostAllocMapped) == cudaSuccess) {
        void* device_ptr = nullptr;
        if (cudaHostGetDevicePointer(&device_ptr, ctx->h_results, 0) == cudaSuccess && device_ptr != nullptr) {
            ctx->d_results_mapped = static_cast<prach_detector_result_block*>(device_ptr);
            ctx->h_result_pinned = true;
            ctx->mapped_results_available = true;
        } else {
            (void)cudaGetLastError();
            cudaFreeHost(ctx->h_results);
            ctx->h_results = nullptr;
        }
    } else {
        (void)cudaGetLastError();
    }

    if (ctx->h_results == nullptr) {
        if (cudaMallocHost(reinterpret_cast<void**>(&ctx->h_results), sizeof(prach_detector_result_block)) ==
            cudaSuccess) {
            ctx->h_result_pinned = true;
        } else {
            (void)cudaGetLastError();
            ctx->h_results = new (std::nothrow) prach_detector_result_block;
            if (ctx->h_results == nullptr) {
                cudaFree(ctx->d_results_device);
                ctx->d_results_device = nullptr;
                return false;
            }
            ctx->h_result_pinned = false;
        }
    }

    ctx->d_results = ctx->d_results_device;
    return true;
}

bool covers_all_candidate_slots(const prach_detector_config_t& cfg)
{
    // When every candidate slot is rewritten by the search kernel each call, a full
    // result-block clear is unnecessary (only RSSI must be zeroed before atomic adds).
    return cfg.start_preamble_index == 0 && cfg.nof_preamble_indices >= MAX_PRACH_CANDIDATES &&
           cfg.sequence_start == 0 && (cfg.nof_sequences * cfg.nof_shifts) >= MAX_PRACH_CANDIDATES;
}

// Light workloads benefit from mapped results (no D2H). Heavy multiport long-PRACH
// IDFTs are faster when candidates stay in device memory then copy once.
bool prefer_mapped_results(const prach_detector_ctx* ctx, const prach_detector_config_t& cfg, int total_batches)
{
    if (ctx == nullptr || !ctx->mapped_results_available) {
        return false;
    }
    if (env_flag_enabled("OCUDU_PRACH_MAPPED_RESULTS", false)) {
        return true; // force always-on
    }
    if (env_flag_disabled("OCUDU_PRACH_MAPPED_RESULTS")) {
        return false;
    }
    // Threshold: ~one short-PRACH 4-port / 64-seq IDFT volume (64*4*256).
    const size_t work = static_cast<size_t>(total_batches) * static_cast<size_t>(cfg.dft_size);
    return work <= (64u * 4u * 256u);
}

bool ensure_plan(prach_detector_ctx* ctx, int dft_size, int batch, cudaStream_t stream)
{
    if (ctx->vkfft_valid && ctx->vkfft_dft_size == dft_size && ctx->vkfft_batch == batch &&
        ctx->vkfft_stream == stream && ctx->vkfft_buffer == ctx->d_idft) {
        return true;
    }
    if (ctx->vkfft_valid) {
        deleteVkFFT(&ctx->vkfft_app);
        std::memset(&ctx->vkfft_app, 0, sizeof(ctx->vkfft_app));
        ctx->vkfft_valid = false;
    }

    int cuda_device = 0;
    if (!check_cuda(cudaGetDevice(&cuda_device)) || (cuDeviceGet(&ctx->vkfft_device, cuda_device) != CUDA_SUCCESS)) {
        return false;
    }

    ctx->vkfft_stream = stream;
    ctx->vkfft_buffer = ctx->d_idft;
    ctx->vkfft_buffer_size = static_cast<pfUINT>(static_cast<size_t>(batch) * dft_size * sizeof(cuFloatComplex));

    VkFFTConfiguration configuration = {};
    configuration.FFTdim = 1;
    configuration.size[0] = static_cast<pfUINT>(dft_size);
    configuration.device = &ctx->vkfft_device;
    configuration.stream = &ctx->vkfft_stream;
    configuration.num_streams = 1;
    configuration.numberBatches = batch;
    configuration.bufferSize = &ctx->vkfft_buffer_size;
    configuration.buffer = &ctx->vkfft_buffer;
    // True inverse plan matches host dft_processor::INVERSE (IDFT correlation).
    configuration.makeInversePlanOnly = 1;
    configuration.normalize = 0;
    configuration.disableReorderFourStep = 1;
    configuration.useLUT = 1;
    // Larger DFTs (format-0, 1024) benefit from more threads on GB10-class GPUs.
    configuration.aimThreads = (dft_size >= 512) ? 256 : 128;

    if (initializeVkFFT(&ctx->vkfft_app, configuration) != VKFFT_SUCCESS) {
        std::memset(&ctx->vkfft_app, 0, sizeof(ctx->vkfft_app));
        return false;
    }
    ctx->vkfft_dft_size = dft_size;
    ctx->vkfft_batch = batch;
    ctx->vkfft_valid = true;
    // Plan rebuild invalidates captured graphs that embed the old VkFFT launches.
    for (auto& entry : ctx->graphs) {
        if (entry.exec != nullptr) {
            cudaGraphExecDestroy(entry.exec);
            entry.exec = nullptr;
        }
        if (entry.graph != nullptr) {
            cudaGraphDestroy(entry.graph);
            entry.graph = nullptr;
        }
    }
    ctx->graphs.clear();
    ctx->next_graph_replacement = 0;
    ctx->graph_capture_failed = false;
    return true;
}

void destroy_graph_entry(prach_detector_ctx::graph_entry& entry)
{
    if (entry.exec != nullptr) {
        cudaGraphExecDestroy(entry.exec);
        entry.exec = nullptr;
    }
    if (entry.graph != nullptr) {
        cudaGraphDestroy(entry.graph);
        entry.graph = nullptr;
    }
    entry.d_input = nullptr;
    entry.stream = nullptr;
}

void destroy_graphs(prach_detector_ctx* ctx)
{
    if (ctx == nullptr) {
        return;
    }
    for (auto& entry : ctx->graphs) {
        destroy_graph_entry(entry);
    }
    ctx->graphs.clear();
    ctx->next_graph_replacement = 0;
    ctx->graph_capture_failed = false;
}

bool graph_matches(const prach_detector_ctx::graph_entry& entry,
                   const uint32_t* d_input,
                   cudaStream_t stream,
                   const prach_detector_config_t& cfg,
                   int batches_per_sequence,
                   bool zero_copy_results)
{
    return entry.exec != nullptr && entry.d_input == d_input && entry.stream == stream &&
           entry.dft_size == cfg.dft_size && entry.nof_sequences == cfg.nof_sequences &&
           entry.batches_per_sequence == batches_per_sequence && entry.nof_shifts == cfg.nof_shifts &&
           entry.nof_rx_ports == cfg.nof_rx_ports && entry.nof_symbols == cfg.nof_symbols &&
           entry.combine_symbols == cfg.combine_symbols && entry.sequence_length == cfg.sequence_length &&
           entry.zero_copy_results == (zero_copy_results ? 1 : 0);
}

prach_detector_ctx::graph_entry* find_graph(prach_detector_ctx* ctx,
                                            const uint32_t* d_input,
                                            cudaStream_t stream,
                                            const prach_detector_config_t& cfg,
                                            int batches_per_sequence,
                                            bool zero_copy_results)
{
    for (auto& entry : ctx->graphs) {
        if (graph_matches(entry, d_input, stream, cfg, batches_per_sequence, zero_copy_results)) {
            return &entry;
        }
    }
    return nullptr;
}

prach_detector_ctx::graph_entry* reserve_graph_slot(prach_detector_ctx* ctx)
{
    if (ctx->graphs.size() < GRAPH_CACHE_LIMIT) {
        ctx->graphs.push_back({});
        return &ctx->graphs.back();
    }
    auto& entry = ctx->graphs[ctx->next_graph_replacement % ctx->graphs.size()];
    ctx->next_graph_replacement = (ctx->next_graph_replacement + 1) % ctx->graphs.size();
    destroy_graph_entry(entry);
    return &entry;
}

bool validate_config(const prach_detector_config_t* cfg)
{
    int batches_per_sequence = cfg != nullptr ? cfg->nof_rx_ports * (cfg->combine_symbols ? 1 : cfg->nof_symbols) : 0;
    return cfg != nullptr && cfg->sequence_length > 0 && cfg->dft_size >= cfg->sequence_length &&
           cfg->nof_rx_ports > 0 && cfg->nof_symbols > 0 && cfg->nof_sequences > 0 && cfg->nof_shifts > 0 &&
           batches_per_sequence > 0 && batches_per_sequence <= MAX_BATCHES_PER_SEQUENCE &&
           cfg->sequence_start >= 0 && cfg->sequence_start + cfg->nof_sequences <= 64 && cfg->win_width > 0 &&
           cfg->threshold > 0.0f &&
           cfg->input_symbol_stride >= cfg->sequence_length &&
           cfg->input_port_stride >= cfg->input_symbol_stride * cfg->nof_symbols;
}

nr_ldpc_status_t enqueue_detector_work(prach_detector_ctx* ctx,
                                       const uint32_t* d_input,
                                       const prach_detector_config_t& cfg,
                                       int batches_per_sequence,
                                       int total_batches,
                                       cudaStream_t stream)
{
    (void)total_batches;
    // Full-range 64-preamble searches rewrite every candidate slot; only RSSI needs a clear.
    // Partial ranges keep a full block clear so stale candidate.valid bits cannot leak.
    if (covers_all_candidate_slots(cfg)) {
        if (!check_cuda(cudaMemsetAsync(&ctx->d_results->rssi, 0, sizeof(float), stream))) {
            return NR_LDPC_ERROR_CUDA_FAILED;
        }
    } else if (!check_cuda(cudaMemsetAsync(ctx->d_results, 0, sizeof(prach_detector_result_block), stream))) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }

    dim3 prep_grid(batches_per_sequence, cfg.nof_sequences, (cfg.dft_size + THREADS - 1) / THREADS);
    kernel_prepare_idft<<<prep_grid, THREADS, 0, stream>>>(
        d_input, ctx->d_roots, ctx->d_idft, &ctx->d_results->rssi, cfg, batches_per_sequence);
    if (!check_cuda(cudaGetLastError())) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }

    VkFFTLaunchParams launch_params = {};
    launch_params.buffer = &ctx->vkfft_buffer;
    // inverse=1 with makeInversePlanOnly: true IDFT (host-equivalent time axis).
    if (VkFFTAppend(&ctx->vkfft_app, 1, &launch_params) != VKFFT_SUCCESS) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }

    dim3 candidate_grid(cfg.nof_shifts, cfg.nof_sequences, 1);
    kernel_find_candidates<<<candidate_grid, THREADS, 0, stream>>>(
        ctx->d_idft, ctx->d_results->candidates, cfg, batches_per_sequence);
    if (!check_cuda(cudaGetLastError())) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }

    return NR_LDPC_SUCCESS;
}

nr_ldpc_status_t enqueue_detector_and_copy(prach_detector_ctx* ctx,
                                           const uint32_t* d_input,
                                           const prach_detector_config_t& cfg,
                                           int batches_per_sequence,
                                           int total_batches,
                                           cudaStream_t stream)
{
    nr_ldpc_status_t status =
        enqueue_detector_work(ctx, d_input, cfg, batches_per_sequence, total_batches, stream);
    if (status != NR_LDPC_SUCCESS) {
        return status;
    }
    // Mapped zero-copy results: kernels already wrote host-visible memory.
    if (!ctx->results_zero_copy) {
        if (!check_cuda(cudaMemcpyAsync(ctx->h_results,
                                        ctx->d_results,
                                        sizeof(prach_detector_result_block),
                                        cudaMemcpyDeviceToHost,
                                        stream))) {
            return NR_LDPC_ERROR_CUDA_FAILED;
        }
    }
    return NR_LDPC_SUCCESS;
}

bool capture_detector_graph(prach_detector_ctx* ctx,
                            const uint32_t* d_input,
                            const prach_detector_config_t& cfg,
                            int batches_per_sequence,
                            int total_batches,
                            cudaStream_t stream)
{
    cudaGraph_t captured = nullptr;
    cudaGraphExec_t exec = nullptr;

    if (cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal) != cudaSuccess) {
        (void)cudaGetLastError();
        ctx->graph_capture_failed = true;
        return false;
    }

    nr_ldpc_status_t status =
        enqueue_detector_and_copy(ctx, d_input, cfg, batches_per_sequence, total_batches, stream);
    cudaError_t end_status = cudaStreamEndCapture(stream, &captured);
    if (status != NR_LDPC_SUCCESS || end_status != cudaSuccess || captured == nullptr) {
        if (captured != nullptr) {
            cudaGraphDestroy(captured);
        }
        (void)cudaGetLastError();
        ctx->graph_capture_failed = true;
        return false;
    }

    if (cudaGraphInstantiate(&exec, captured, nullptr, nullptr, 0) != cudaSuccess) {
        cudaGraphDestroy(captured);
        (void)cudaGetLastError();
        ctx->graph_capture_failed = true;
        return false;
    }

    auto* entry = reserve_graph_slot(ctx);
    entry->d_input = d_input;
    entry->stream = stream;
    entry->dft_size = cfg.dft_size;
    entry->nof_sequences = cfg.nof_sequences;
    entry->batches_per_sequence = batches_per_sequence;
    entry->nof_shifts = cfg.nof_shifts;
    entry->nof_rx_ports = cfg.nof_rx_ports;
    entry->nof_symbols = cfg.nof_symbols;
    entry->combine_symbols = cfg.combine_symbols;
    entry->sequence_length = cfg.sequence_length;
    entry->zero_copy_results = ctx->results_zero_copy ? 1 : 0;
    entry->graph = captured;
    entry->exec = exec;
    return true;
}

void fill_host_result(const prach_detector_result_block& h_results,
                      const prach_detector_config_t& cfg,
                      prach_detector_result_t* result)
{
    std::memset(result, 0, sizeof(*result));
    result->rssi =
        h_results.rssi / static_cast<float>(cfg.nof_rx_ports * cfg.nof_symbols * cfg.sequence_length);
    const int last_requested_preamble = cfg.start_preamble_index + cfg.nof_preamble_indices;
    for (int i = 0; i != MAX_PRACH_CANDIDATES; ++i) {
        const prach_detector_candidate_t& candidate = h_results.candidates[i];
        if (!candidate.valid) {
            continue;
        }
        if (candidate.preamble_index < cfg.start_preamble_index ||
            candidate.preamble_index >= last_requested_preamble) {
            continue;
        }
        if (result->nof_candidates == 64) {
            break;
        }
        result->candidates[result->nof_candidates++] = candidate;
    }
}

} // namespace

extern "C" nr_ldpc_status_t prach_detector_create(prach_detector_handle_t* handle)
{
    if (handle == nullptr) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    auto* ctx = new (std::nothrow) prach_detector_ctx();
    if (ctx == nullptr) {
        return NR_LDPC_ERROR_ALLOC_FAILED;
    }
    if (!allocate_result_buffers(ctx) || (create_realtime_stream(&ctx->owned_stream) != cudaSuccess) ||
        (cudaEventCreateWithFlags(&ctx->done_event, cudaEventDisableTiming) != cudaSuccess)) {
        prach_detector_destroy(ctx);
        return NR_LDPC_ERROR_ALLOC_FAILED;
    }
    *handle = ctx;
    return NR_LDPC_SUCCESS;
}

extern "C" void prach_detector_destroy(prach_detector_handle_t handle)
{
    if (handle == nullptr) {
        return;
    }
    destroy_graphs(handle);
    if (handle->vkfft_valid) {
        deleteVkFFT(&handle->vkfft_app);
        handle->vkfft_valid = false;
    }
    if (handle->done_event != nullptr) {
        cudaEventDestroy(handle->done_event);
        handle->done_event = nullptr;
    }
    if (handle->owned_stream != nullptr) {
        cudaStreamDestroy(handle->owned_stream);
        handle->owned_stream = nullptr;
    }
    cudaFree(handle->d_input);
    cudaFree(handle->d_roots);
    cudaFree(handle->d_idft);
    if (handle->d_results_device != nullptr) {
        cudaFree(handle->d_results_device);
        handle->d_results_device = nullptr;
    }
    handle->d_results_mapped = nullptr;
    handle->d_results = nullptr;
    if (handle->h_result_pinned) {
        cudaFreeHost(handle->h_results);
    } else {
        delete handle->h_results;
    }
    handle->h_results = nullptr;
    delete handle;
}

extern "C" nr_ldpc_status_t prach_detector_detect(prach_detector_handle_t         handle,
                                                   const void*                    prach_cbf16,
                                                   const void*                    roots_cf32,
                                                   const prach_detector_config_t* config,
                                                   prach_detector_result_t*       result,
                                                   void*                          external_stream)
{
    if (handle == nullptr || prach_cbf16 == nullptr || roots_cf32 == nullptr || result == nullptr ||
        !validate_config(config)) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    cudaStream_t stream = (external_stream != nullptr) ? static_cast<cudaStream_t>(external_stream)
                                                       : handle->owned_stream;
    if (stream == nullptr) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }

    prach_detector_config_t cfg = *config;
    const int batches_per_sequence = cfg.nof_rx_ports * (cfg.combine_symbols ? 1 : cfg.nof_symbols);
    const int total_batches = batches_per_sequence * cfg.nof_sequences;

    if (!ensure_capacity(reinterpret_cast<void**>(&handle->d_idft),
                         &handle->d_idft_capacity,
                         static_cast<size_t>(total_batches) * cfg.dft_size * sizeof(cuFloatComplex)) ||
        !ensure_capacity(reinterpret_cast<void**>(&handle->d_roots),
                         &handle->d_roots_capacity,
                         static_cast<size_t>(cfg.nof_sequences) * cfg.sequence_length * sizeof(cuFloatComplex))) {
        return NR_LDPC_ERROR_ALLOC_FAILED;
    }

    const uint32_t* d_input = static_cast<const uint32_t*>(prach_cbf16);
    if (!cfg.input_is_device) {
        size_t input_bytes =
            static_cast<size_t>(cfg.nof_rx_ports) * cfg.nof_symbols * cfg.sequence_length * sizeof(uint32_t);
        if (!ensure_capacity(reinterpret_cast<void**>(&handle->d_input), &handle->d_input_capacity, input_bytes)) {
            return NR_LDPC_ERROR_ALLOC_FAILED;
        }
        if (!check_cuda(cudaMemcpyAsync(handle->d_input, prach_cbf16, input_bytes, cudaMemcpyHostToDevice, stream))) {
            return NR_LDPC_ERROR_CUDA_FAILED;
        }
        d_input = handle->d_input;
        cfg.input_symbol_stride = cfg.sequence_length;
        cfg.input_port_stride = cfg.nof_symbols * cfg.sequence_length;
    }

    // Roots upload cannot live inside the graph (host pointer may change); keep it outside.
    if (handle->root_cache_key != cfg.root_cache_key || handle->cached_nof_sequences != cfg.nof_sequences ||
        handle->cached_sequence_length != cfg.sequence_length) {
        size_t roots_bytes = static_cast<size_t>(cfg.nof_sequences) * cfg.sequence_length * sizeof(cuFloatComplex);
        if (!check_cuda(cudaMemcpyAsync(handle->d_roots, roots_cf32, roots_bytes, cudaMemcpyHostToDevice, stream))) {
            return NR_LDPC_ERROR_CUDA_FAILED;
        }
        handle->root_cache_key = cfg.root_cache_key;
        handle->cached_nof_sequences = cfg.nof_sequences;
        handle->cached_sequence_length = cfg.sequence_length;
        // Root contents changed: invalidate graphs that sampled the old roots via VkFFT buffer.
        destroy_graphs(handle);
    }

    if (!ensure_plan(handle, cfg.dft_size, total_batches, stream)) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }

    // Select result backing for this workload (mapped light / device heavy).
    handle->results_zero_copy = prefer_mapped_results(handle, cfg, total_batches);
    handle->d_results =
        handle->results_zero_copy ? handle->d_results_mapped : handle->d_results_device;
    if (handle->d_results == nullptr) {
        return NR_LDPC_ERROR_ALLOC_FAILED;
    }

    bool enqueued = false;
    if (cuda_graphs_enabled() && !handle->graph_capture_failed) {
        if (auto* entry = find_graph(
                handle, d_input, stream, cfg, batches_per_sequence, handle->results_zero_copy);
            entry != nullptr) {
            enqueued = (cudaGraphLaunch(entry->exec, stream) == cudaSuccess);
            if (!enqueued) {
                destroy_graph_entry(*entry);
                (void)cudaGetLastError();
            }
        }
        if (!enqueued) {
            std::lock_guard<std::mutex> lock(graph_capture_mutex());
            if (auto* entry = find_graph(
                    handle, d_input, stream, cfg, batches_per_sequence, handle->results_zero_copy);
                entry != nullptr) {
                enqueued = (cudaGraphLaunch(entry->exec, stream) == cudaSuccess);
                if (!enqueued) {
                    destroy_graph_entry(*entry);
                    (void)cudaGetLastError();
                }
            }
            if (!enqueued &&
                capture_detector_graph(handle, d_input, cfg, batches_per_sequence, total_batches, stream)) {
                if (auto* entry = find_graph(
                        handle, d_input, stream, cfg, batches_per_sequence, handle->results_zero_copy);
                    entry != nullptr) {
                    enqueued = (cudaGraphLaunch(entry->exec, stream) == cudaSuccess);
                    if (!enqueued) {
                        destroy_graph_entry(*entry);
                        (void)cudaGetLastError();
                    }
                }
            }
        }
    }

    if (!enqueued) {
        nr_ldpc_status_t enqueue_status =
            enqueue_detector_and_copy(handle, d_input, cfg, batches_per_sequence, total_batches, stream);
        if (enqueue_status != NR_LDPC_SUCCESS) {
            return enqueue_status;
        }
    }

    // Event sync is cheaper than full stream sync for this short detector path.
    if (!check_cuda(cudaEventRecord(handle->done_event, stream)) ||
        !check_cuda(cudaEventSynchronize(handle->done_event))) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }

    fill_host_result(*handle->h_results, cfg, result);
    return NR_LDPC_SUCCESS;
}
