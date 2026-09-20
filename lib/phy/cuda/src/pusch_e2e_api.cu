// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/**
 * @file pusch_e2e_api.cu
 * @brief CUDA-accelerated CRC computation for 5G NR transport blocks and code blocks
 * 
 * Implements CRC-24A, CRC-24B, and CRC-16 per 3GPP TS 38.212.
 * OCUDU PHY CUDA - PUSCH end-to-end: host API orchestration. 
  * Kernel launches go through the wrappers in pusch_e2e_launch.cuh
 */

#include "pusch_e2e.h"
#include "pusch_pilot_mask.h"
#include "pusch_sinr_readback.h"
#include "pusch_e2e_internal.cuh"
#include "pusch_e2e_preallocation.h"
#include "pusch_e2e_launch.cuh"
#include "pusch_e2e_low_papr.cuh"   // NOF_LOW_PAPR_GROUPS
#include "scrambling.h"
#include "dmrs_utils.h"
#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <cuComplex.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <new>
#include <mutex>
#include <cstring>
#include <cstdlib>
#ifdef OCUDU_PHY_CUDA_ENABLE_VKFFT
    #include "vkFFT.h"

struct pusch_vkfft_deprecoder_plan {
    VkFFTApplication app;
    pfUINT buffer_size;
    int dft_size;
    bool initialized;
};
#endif

/* Forward decls: defined later in this TU, used by pusch_e2e_create(). */
static uint64_t pack_deprecoding_fft_factors(int n, int* nof_factors);
// Used for create-path rollback after partial allocation.
void pusch_e2e_destroy(pusch_e2e_handle_t handle);

static nr_ldpc_status_t sync_pusch_precompute_stream(cudaStream_t stream, const char* phase)
{
    cudaError_t err = cudaGetLastError();
    if (err == cudaSuccess) {
        err = cudaStreamSynchronize(stream);
    }
    if (err != cudaSuccess) {
        std::fprintf(stderr,
                     "[OCUDU PHY CUDA] PUSCH E2E configure %s failed: %s\n",
                     phase ? phase : "precompute",
                     cudaGetErrorString(err));
    }
    return (err == cudaSuccess) ? NR_LDPC_SUCCESS : NR_LDPC_ERROR_CUDA_FAILED;
}

static bool pusch_e2e_env_flag_disabled(const char* name)
{
    const char* value = std::getenv(name);
    return value && (std::strcmp(value, "0") == 0 ||
                     std::strcmp(value, "false") == 0 ||
                     std::strcmp(value, "off") == 0 ||
                     std::strcmp(value, "no") == 0);
}

static unsigned pusch_e2e_env_unsigned(const char* name, unsigned default_value)
{
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return default_value;
    }
    char* end = nullptr;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) {
        return default_value;
    }
    return static_cast<unsigned>(parsed);
}

static int pusch_e2e_get_dmrs_symbol_indices_checked(int dmrs_symbol_mask, int* dmrs_symbol_indices)
{
    if ((dmrs_symbol_mask & ~((1 << MAX_SYMBOLS) - 1)) != 0) {
        return -1;
    }

    int nof_dmrs_symbols = 0;
    for (int s = 0; s < MAX_SYMBOLS; s++) {
        if (dmrs_symbol_mask & (1 << s)) {
            if (nof_dmrs_symbols == MAX_DMRS_SYMBOLS) {
                return -1;
            }
            if (dmrs_symbol_indices) {
                dmrs_symbol_indices[nof_dmrs_symbols] = s;
            }
            nof_dmrs_symbols++;
        }
    }

    return nof_dmrs_symbols;
}

nr_ldpc_status_t pusch_e2e_create(pusch_e2e_handle_t* handle)
{
    if (!handle) return NR_LDPC_ERROR_INVALID_CONFIG;

    /* Initialize LFSR jump tables for this module (once per process) */
    cudaError_t jump_err = initialize_pusch_e2e_jump_tables();
    if (jump_err != cudaSuccess) {
        fprintf(stderr, "[OCUDU PHY CUDA] Failed to initialize PUSCH E2E LFSR jump tables: %s\n",
                cudaGetErrorString(jump_err));
        return NR_LDPC_ERROR_CUDA_FAILED;
    }

    // Value-initialize so partial-construction failure can safely call pusch_e2e_destroy().
    pusch_e2e_ctx* ctx = new (std::nothrow) pusch_e2e_ctx{};
    if (!ctx) return NR_LDPC_ERROR_ALLOC_FAILED;

    ctx->configured = false;
    ctx->d_dmrs_pilots = nullptr;
    ctx->dmrs_pilots_capacity = 0;
    ctx->d_ch_estimates = nullptr;
    ctx->ch_estimates_capacity = 0;
    ctx->d_dmrs_estimates = nullptr;
    ctx->dmrs_estimates_capacity = 0;
    ctx->nof_estimates = 0;
    ctx->d_noise_vars = nullptr;
    ctx->noise_vars_capacity = 0;
    ctx->d_noise_var_partial = nullptr;
    ctx->noise_var_partial_capacity = 0;
    ctx->d_eq_noise_var_sum = nullptr;
    ctx->d_eq_noise_var_count = nullptr;
    ctx->d_evm_error_sum = nullptr;
    ctx->d_evm_symbol_count = nullptr;
    ctx->h_sinr_eq_block = nullptr;
    ctx->h_evm_error_sum = nullptr;
    ctx->h_evm_symbol_count = nullptr;
    ctx->d_rsrp_accum = nullptr;
    ctx->d_epre_accum = nullptr;
    ctx->d_sinr_noise_accum = nullptr;
    ctx->d_sinr_rsrp_accum = nullptr;
    ctx->d_dmrs_c_inits = nullptr;
    ctx->d_sinr_epre_result = nullptr;
    ctx->h_sinr_epre_result = nullptr;
    ctx->d_estimates_fp16 = nullptr;
    ctx->estimates_fp16_capacity = 0;

    /* Pre-allocated buffers for per-slot reuse (avoid malloc in hot path!) */
    ctx->d_dmrs_indices = nullptr;
    ctx->dmrs_indices_capacity = 0;
    ctx->d_freq_interp_estimates = nullptr;
    ctx->freq_interp_capacity = 0;
    ctx->d_lse_averaged = nullptr;
    ctx->lse_averaged_capacity = 0;
    ctx->d_llrs_half_temp = nullptr;
    ctx->llrs_half_temp_capacity = 0;
    ctx->d_dmrs_received = nullptr;
    ctx->dmrs_received_capacity = 0;
    ctx->d_noise_count = nullptr;
    ctx->d_scrambling_seq = nullptr;
    ctx->scrambling_seq_capacity = 0;
    ctx->cached_data_scrambling_c_init = UINT32_MAX;
    ctx->cached_data_scrambling_bits = 0;
    ctx->precompute_stream = nullptr;
    ctx->d_dmrs_pilot_bits = nullptr;
    ctx->dmrs_pilot_bits_capacity = 0;
    ctx->d_dmrs_mask = nullptr;
    ctx->dmrs_mask_capacity = 0;

    /* Low-PAPR DMRS pilots for transform precoding */
    ctx->d_low_papr_pilots = nullptr;
    ctx->low_papr_pilots_capacity = 0;
    ctx->cached_low_papr_nof_prb = -1;
    ctx->cached_low_papr_n_rs_id = -1;
    ctx->d_deprecode_symbols = nullptr;
    ctx->d_deprecode_noise_vars = nullptr;
    ctx->deprecode_symbols_capacity = 0;
    ctx->deprecode_noise_capacity = 0;
#ifdef OCUDU_PHY_CUDA_ENABLE_VKFFT
    constexpr bool vkfft_compiled = true;
#else
    constexpr bool vkfft_compiled = false;
#endif
    ctx->transform_deprecoder_backend =
        pusch_select_transform_deprecoder(std::getenv(PUSCH_TRANSFORM_DEPRECODER_ENV), vkfft_compiled);
    ctx->vkfft_auto_min_dft_size = 864;  /* 72 PRB measured crossover on DGX Spark. */
    const char* vkfft_min_env = std::getenv("OCUDU_PHY_CUDA_PUSCH_VKFFT_MIN_DFT_SIZE");
    if (vkfft_min_env) {
        int threshold = std::atoi(vkfft_min_env);
        if (threshold > 0 && threshold <= PUSCH_DEPRECODE_MAX_DFT_SIZE) {
            ctx->vkfft_auto_min_dft_size = threshold;
        }
    }
#ifdef OCUDU_PHY_CUDA_ENABLE_VKFFT
    std::memset(ctx->vkfft_plans, 0, sizeof(ctx->vkfft_plans));
    ctx->vkfft_buffer_ptr = nullptr;
    ctx->vkfft_device = 0;
    ctx->vkfft_stream = nullptr;
    ctx->vkfft_preplanned = false;
#endif
    memset(ctx->deprecoding_fft_factors, 0, sizeof(ctx->deprecoding_fft_factors));
    memset(ctx->deprecoding_fft_nof_factors, 0, sizeof(ctx->deprecoding_fft_nof_factors));
    for (int n = 12; n <= PUSCH_DEPRECODE_MAX_DFT_SIZE; n += 12) {
        int nof_factors = 0;
        uint64_t packed = pack_deprecoding_fft_factors(n, &nof_factors);
        if (nof_factors > 0) {
            ctx->deprecoding_fft_factors[n] = packed;
            ctx->deprecoding_fft_nof_factors[n] = nof_factors;
        }
    }

    /* OFDM symbol start times for CFO compensation */
    ctx->d_symbol_start_times = nullptr;
    ctx->symbol_times_scs_khz = 0;
    ctx->d_mimo_cfo_phasors = nullptr;
    memset(ctx->h_symbol_start_times, 0, sizeof(ctx->h_symbol_start_times));

    /* Precomputed DMRS pilots for all 14 symbols × all slots */
    ctx->d_precomputed_dmrs_pilots = nullptr;
    ctx->precomputed_dmrs_pilots_capacity = 0;
    ctx->precomputed_nof_slots = 0;
    ctx->precomputed_pilots_per_symbol = 0;
    ctx->precomputed_words_per_slot = 0;
    ctx->precomputed_words_per_sym = 0;
    ctx->dmrs_pilots_precomputed = false;
    ctx->cached_dmrs_symbol_mask = -1;  /* Force first upload */
    ctx->cached_nof_dmrs_symbols = 0;
    ctx->cached_dmrs_scrambling_id = UINT32_MAX;  /* Force first DMRS precompute */
    ctx->cached_dmrs_n_scid = -1;
    ctx->cached_dmrs_nof_prb = -1;
    ctx->cached_dmrs_type = -1;

    /* Allocate consolidated per-iteration accumulator block (single memset zeros all).
     * Layout: noise_count(4) | rsrp(32) | epre(32) | sinr_noise(32) | sinr_rsrp(32) |
     *         eq_sum(4) | eq_count(4) | evm_sum(4) | evm_count(4) |
     *         ta_cfo_accum(24) | cv_noise_accum(32) | cv_done_counter(4). */
    {
        size_t off = 0;
        /* noise_count: unsigned int */
        off += sizeof(unsigned int);                        /* 4 */
        size_t off_rsrp = off;
        off += 8 * sizeof(float);                           /* +32 */
        size_t off_epre = off;
        off += 8 * sizeof(float);                           /* +32 */
        size_t off_sinr_noise = off;
        off += 8 * sizeof(float);                           /* +32 */
        size_t off_sinr_rsrp = off;
        off += 8 * sizeof(float);                           /* +32 */
        size_t off_eq_sum = off;
        off += sizeof(float);                               /* +4 */
        size_t off_eq_count = off;
        off += sizeof(unsigned int);                        /* +4 = 140 */
        size_t off_evm_sum = off;
        off += sizeof(float);                               /* +4 = 144 */
        size_t off_evm_count = off;
        off += sizeof(unsigned int);                        /* +4 = 148 */
        size_t off_ta_cfo = off;
        off += 6 * sizeof(float);                           /* +24 = 164 (ta_r, ta_i, cfo_r, cfo_i, rsrp, epre) */
        size_t off_cv_noise = off;
        off += 8 * sizeof(float);                           /* +32 = 196 (per-port CV noise accum) */
        size_t off_cv_done = off;
        off += sizeof(unsigned int);                        /* +4 = 200 (CV last-block done counter) */

        ctx->accum_block_size = off;
        /* OCUDU_PUSCH_SINR_MANAGED=1: allocate the accumulator and result blocks as
         * managed memory so the host reads results directly after the completion sync
         * instead of via D2H copies. On GB10 a D2H that lands before a later kernel in
         * the dispatch order costs a ~100-180 us stream-dispatch boundary; managed
         * readback removes the copies entirely. */
        {
            const char* v = std::getenv("OCUDU_PUSCH_SINR_MANAGED");
            ctx->sinr_managed = (v != nullptr) && (v[0] != '0');
        }
        cudaError_t aerr = ctx->sinr_managed ? cudaMallocManaged(&ctx->d_accum_block, off)
                                             : cudaMalloc(&ctx->d_accum_block, off);
        if (aerr != cudaSuccess) {
            delete ctx;
            return NR_LDPC_ERROR_ALLOC_FAILED;
        }
        cudaMemset(ctx->d_accum_block, 0, off);

        /* Set pointer aliases into the contiguous block */
        char* base = (char*)ctx->d_accum_block;
        ctx->d_noise_count      = (unsigned int*)(base);
        ctx->d_rsrp_accum       = (float*)(base + off_rsrp);
        ctx->d_epre_accum       = (float*)(base + off_epre);
        ctx->d_sinr_noise_accum = (float*)(base + off_sinr_noise);
        ctx->d_sinr_rsrp_accum  = (float*)(base + off_sinr_rsrp);
        ctx->d_eq_noise_var_sum = (float*)(base + off_eq_sum);
        ctx->d_eq_noise_var_count = (unsigned int*)(base + off_eq_count);
        ctx->d_evm_error_sum = (float*)(base + off_evm_sum);
        ctx->d_evm_symbol_count = (unsigned int*)(base + off_evm_count);
        ctx->d_ta_cfo_accum     = (float*)(base + off_ta_cfo);
        ctx->d_cv_noise_accum   = (float*)(base + off_cv_noise);
        ctx->d_cv_done_counter  = (unsigned int*)(base + off_cv_done);
    }

    ctx->deprecode_symbols_capacity = PUSCH_DEPRECODE_MAX_SYMBOLS * PUSCH_DEPRECODE_MAX_DFT_SIZE;
    cudaError_t derr = cudaMalloc(&ctx->d_deprecode_symbols,
                                  ctx->deprecode_symbols_capacity * sizeof(cuFloatComplex));
    if (derr != cudaSuccess) {
        cudaFree(ctx->d_accum_block);
        delete ctx;
        return NR_LDPC_ERROR_ALLOC_FAILED;
    }
    ctx->deprecode_noise_capacity = PUSCH_DEPRECODE_MAX_SYMBOLS;
    derr = cudaMalloc(&ctx->d_deprecode_noise_vars, ctx->deprecode_noise_capacity * sizeof(float));
    if (derr != cudaSuccess) {
        cudaFree(ctx->d_deprecode_symbols);
        cudaFree(ctx->d_accum_block);
        delete ctx;
        return NR_LDPC_ERROR_ALLOC_FAILED;
    }

    /* Persistent precompute stream — avoid create/destroy per configure(). */
    if (cudaStreamCreateWithFlags(&ctx->precompute_stream, cudaStreamNonBlocking) != cudaSuccess) {
        ctx->precompute_stream = nullptr; /* fall back to default stream on configure */
    }

    /* ----------------------------------------------------------------
     * Pre-allocate max FR1 100 MHz / 4RX working sets so multi-sector
     * configure() does not cudaMalloc on the critical path.
     * Sizes match the grow-to-fit checks later in pusch_e2e_configure.
     * ---------------------------------------------------------------- */
    {
        constexpr int k_max_prb     = 273;
        constexpr int k_max_ports   = 4;
        constexpr int k_max_layers  = 4;
        constexpr int k_max_symbols = MAX_SYMBOLS; // 14
        constexpr int k_max_mod     = 8;           // 256QAM
        constexpr int k_max_dmrs    = MAX_DMRS_SYMBOLS;

        // Rollback via destroy (context is value-initialized / null-checked).
        auto fail_prealloc = [&]() {
            pusch_e2e_destroy(ctx);
            return NR_LDPC_ERROR_ALLOC_FAILED;
        };

        // Data scrambling: worst-case all REs are data.
        const int max_data_re       = k_max_prb * k_max_symbols * 12;
        const int max_scramble_bits = max_data_re * k_max_layers * k_max_mod;
        const size_t scramble_bytes =
            static_cast<size_t>((max_scramble_bits + 31) / 32) * sizeof(uint32_t);
        if (cudaMalloc(&ctx->d_scrambling_seq, scramble_bytes) != cudaSuccess) {
            return fail_prealloc();
        }
        ctx->scrambling_seq_capacity = scramble_bytes;

        // DMRS LSE: max DMRS symbols × ports × pilots/PRB (type1: 6) × PRBs
        const size_t lse_bytes = static_cast<size_t>(k_max_dmrs) * k_max_ports * k_max_prb * 6 *
                                 sizeof(cuFloatComplex);
        if (cudaMalloc(&ctx->d_dmrs_estimates, lse_bytes) != cudaSuccess) {
            return fail_prealloc();
        }
        ctx->dmrs_estimates_capacity = lse_bytes;

        // Full-grid channel estimates
        const size_t ch_bytes = static_cast<size_t>(k_max_ports) * k_max_prb * 12 * k_max_symbols *
                                sizeof(cuFloatComplex);
        if (cudaMalloc(&ctx->d_ch_estimates, ch_bytes) != cudaSuccess) {
            return fail_prealloc();
        }
        ctx->ch_estimates_capacity = ch_bytes;

        // The common-pilot admission query runs before any receiver kernels.
        // Its FP16 scratch must exist even when the first PUSCH is neural;
        // depending on a prior conventional decode rejects that first grant
        // and leaves a grow-to-fit allocation in the baseline hot path.
        if (pusch_preallocate_fp16_estimates(*ctx) != NR_LDPC_SUCCESS ||
            pusch_preallocate_dmrs_c_inits(*ctx) != NR_LDPC_SUCCESS) {
            return fail_prealloc();
        }

        // Noise vars / partials
        const size_t noise_bytes = static_cast<size_t>(k_max_ports) * sizeof(float);
        if (cudaMalloc(&ctx->d_noise_vars, noise_bytes) != cudaSuccess) {
            return fail_prealloc();
        }
        ctx->noise_vars_capacity = noise_bytes;
        // Partial reduction workspace: generous for multi-block reductions
        const size_t partial_bytes = static_cast<size_t>(k_max_ports) * 256 * sizeof(float);
        if (cudaMalloc(&ctx->d_noise_var_partial, partial_bytes) != cudaSuccess) {
            return fail_prealloc();
        }
        ctx->noise_var_partial_capacity = partial_bytes;

        // DMRS indices + freq-interp workspace
        const size_t dmrs_idx_bytes = static_cast<size_t>(k_max_dmrs) * sizeof(int);
        if (cudaMalloc(&ctx->d_dmrs_indices, dmrs_idx_bytes) != cudaSuccess) {
            return fail_prealloc();
        }
        ctx->dmrs_indices_capacity = dmrs_idx_bytes;

        const size_t freq_interp_bytes =
            static_cast<size_t>(k_max_dmrs) * k_max_prb * 12 * k_max_ports * sizeof(cuFloatComplex);
        if (cudaMalloc(&ctx->d_freq_interp_estimates, freq_interp_bytes) != cudaSuccess) {
            return fail_prealloc();
        }
        ctx->freq_interp_capacity = freq_interp_bytes;

        // Precomputed Gold DMRS pilots: all slots × all OFDM symbols × words_per_sym (type1).
        const int words_per_sym =
            (k_max_prb * 6 * 2 + 31) / 32; // 2 bits/pilot
        const int total_words =
            words_per_sym * MAX_SYMBOLS * MAX_SLOTS_PER_FRAME;
        const size_t pre_dmrs_bytes = static_cast<size_t>(total_words) * sizeof(uint32_t);
        if (cudaMalloc(&ctx->d_precomputed_dmrs_pilots, pre_dmrs_bytes) != cudaSuccess) {
            return fail_prealloc();
        }
        ctx->precomputed_dmrs_pilots_capacity = pre_dmrs_bytes;

        const size_t gather_bytes =
            static_cast<size_t>(k_max_dmrs) * words_per_sym * sizeof(uint32_t);
        if (cudaMalloc(&ctx->d_dmrs_pilot_bits, gather_bytes) != cudaSuccess) {
            return fail_prealloc();
        }
        ctx->dmrs_pilot_bits_capacity = gather_bytes;

        // Low-PAPR pilots (transform precoding): M_zc = nof_prb * 6
        const size_t low_papr_bytes = static_cast<size_t>(k_max_prb * 6) * sizeof(cuFloatComplex);
        if (cudaMalloc(&ctx->d_low_papr_pilots, low_papr_bytes) != cudaSuccess) {
            return fail_prealloc();
        }
        ctx->low_papr_pilots_capacity = low_papr_bytes;
    }
#ifdef OCUDU_PHY_CUDA_ENABLE_VKFFT
    ctx->vkfft_buffer_ptr = ctx->d_deprecode_symbols;
#endif

    /* Host-visible SINR/EVM readback. Managed mode: the h_* pointers alias the managed
     * device accumulators directly (device layout at d_eq_noise_var_sum is contiguous
     * [sum, count, evm_sum, evm_count], matching the former pinned-block layout).
     * Otherwise: pinned host memory filled by async D2H. */
    const size_t sinr_evm_host_bytes = 2 * sizeof(float) + 2 * sizeof(unsigned int);
    cudaError_t err;
    if (ctx->sinr_managed) {
        ctx->h_sinr_eq_block      = (void*)ctx->d_eq_noise_var_sum;
        ctx->h_sinr_noise_var_sum = ctx->d_eq_noise_var_sum;
        ctx->h_sinr_count         = ctx->d_eq_noise_var_count;
        ctx->h_evm_error_sum      = ctx->d_evm_error_sum;
        ctx->h_evm_symbol_count   = ctx->d_evm_symbol_count;
    } else {
        err = cudaMallocHost(&ctx->h_sinr_eq_block, sinr_evm_host_bytes);
        if (err == cudaSuccess) {
            char* host_base = (char*)ctx->h_sinr_eq_block;
            ctx->h_sinr_noise_var_sum = (float*)host_base;
            ctx->h_sinr_count = (unsigned int*)(host_base + sizeof(float));
            ctx->h_evm_error_sum = (float*)(host_base + sizeof(float) + sizeof(unsigned int));
            ctx->h_evm_symbol_count = (unsigned int*)(host_base + 2 * sizeof(float) + sizeof(unsigned int));
        } else {
            ctx->h_sinr_eq_block = nullptr;
            ctx->h_sinr_noise_var_sum = nullptr;
            ctx->h_sinr_count = nullptr;
            ctx->h_evm_error_sum = nullptr;
            ctx->h_evm_symbol_count = nullptr;
        }
        /* Initialize host SINR/EVM buffers to safe defaults (avoid stale reads) */
        if (ctx->h_sinr_eq_block) {
            memset(ctx->h_sinr_eq_block, 0, sinr_evm_host_bytes);
        }
    }

    err = ctx->sinr_managed ? cudaMallocManaged(&ctx->d_sinr_epre_result, 5 * sizeof(float))
                            : cudaMalloc(&ctx->d_sinr_epre_result, 5 * sizeof(float));
    if (err != cudaSuccess) { ctx->d_sinr_epre_result = nullptr; }
    if (ctx->sinr_managed) {
        ctx->h_sinr_epre_result = ctx->d_sinr_epre_result;
    } else {
        err = cudaMallocHost(&ctx->h_sinr_epre_result, 5 * sizeof(float));
        if (err != cudaSuccess) { ctx->h_sinr_epre_result = nullptr; }
    }
    if (ctx->h_sinr_epre_result) {
        for (int i = 0; i < 5; i++) ctx->h_sinr_epre_result[i] = -INFINITY;
    }

    /* Allocate OFDM symbol start times for CFO compensation */
    err = cudaMalloc(&ctx->d_symbol_start_times, 14 * sizeof(float));
    if (err != cudaSuccess) { ctx->d_symbol_start_times = nullptr; }
    err = cudaMalloc(&ctx->d_mimo_cfo_phasors, MIMO_CFO_PHASOR_COUNT * sizeof(float2));
    if (err != cudaSuccess) { ctx->d_mimo_cfo_phasors = nullptr; }

    /* External eq-output (disabled by default) */
    ctx->d_eq_symbols_out = nullptr;
    ctx->d_eq_noise_var_out = nullptr;

    /* Create scramblers */
    nr_ldpc_status_t status = scrambler_create(&ctx->data_scrambler);
    if (status != NR_LDPC_SUCCESS) {
        ctx->data_scrambler = nullptr;
        pusch_e2e_destroy(ctx);
        return status;
    }

    status = scrambler_create(&ctx->dmrs_scrambler);
    if (status != NR_LDPC_SUCCESS) {
        ctx->dmrs_scrambler = nullptr;
        pusch_e2e_destroy(ctx);
        return status;
    }

    *handle = ctx;
    return NR_LDPC_SUCCESS;
}

nr_ldpc_status_t pusch_e2e_preplan_transform_deprecoder(pusch_e2e_handle_t handle,
                                                         cudaStream_t stream)
{
    if (!handle) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

#ifdef OCUDU_PHY_CUDA_ENABLE_VKFFT
    if ((handle->transform_deprecoder_backend != PUSCH_DEPRECODER_VKFFT) &&
        (handle->transform_deprecoder_backend != PUSCH_DEPRECODER_AUTO)) {
        return NR_LDPC_SUCCESS;
    }
    if (handle->vkfft_preplanned && handle->vkfft_stream == stream) {
        return NR_LDPC_SUCCESS;
    }

    int cuda_device = 0;
    cudaError_t cerr = cudaGetDevice(&cuda_device);
    if (cerr != cudaSuccess) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }
    CUresult cu_res = cuDeviceGet(&handle->vkfft_device, cuda_device);
    if (cu_res != CUDA_SUCCESS) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }

    handle->vkfft_stream = stream;
    for (int n = 12; n <= PUSCH_DEPRECODE_MAX_DFT_SIZE; n += 12) {
        if (handle->deprecoding_fft_nof_factors[n] <= 0) {
            continue;
        }
        if ((handle->transform_deprecoder_backend == PUSCH_DEPRECODER_AUTO) &&
            (n < handle->vkfft_auto_min_dft_size)) {
            continue;
        }

        auto* plan = new (std::nothrow) pusch_vkfft_deprecoder_plan();
        if (!plan) {
            return NR_LDPC_ERROR_ALLOC_FAILED;
        }
        std::memset(plan, 0, sizeof(*plan));
        plan->dft_size = n;
        plan->buffer_size = static_cast<pfUINT>(PUSCH_DEPRECODE_MAX_SYMBOLS * n * sizeof(cuFloatComplex));

        VkFFTConfiguration configuration = {};
        configuration.FFTdim = 1;
        configuration.size[0] = static_cast<pfUINT>(n);
        configuration.device = &handle->vkfft_device;
        configuration.stream = &handle->vkfft_stream;
        configuration.num_streams = 1;
        configuration.numberBatches = PUSCH_DEPRECODE_MAX_SYMBOLS;
        configuration.bufferSize = &plan->buffer_size;
        configuration.buffer = &handle->vkfft_buffer_ptr;
        configuration.makeInversePlanOnly = 1;
        configuration.normalize = 0;
        configuration.disableReorderFourStep = 1;
        configuration.useLUT = 1;
        configuration.aimThreads = 128;

        VkFFTResult result = initializeVkFFT(&plan->app, configuration);
        if (result != VKFFT_SUCCESS) {
            delete plan;
            if (handle->transform_deprecoder_backend == PUSCH_DEPRECODER_VKFFT) {
                return NR_LDPC_ERROR_CUDA_FAILED;
            }
            continue;
        }
        plan->initialized = true;
        handle->vkfft_plans[n] = plan;
    }

    handle->vkfft_preplanned = true;
    return NR_LDPC_SUCCESS;
#else
    (void)stream;
    if (handle->transform_deprecoder_backend == PUSCH_DEPRECODER_VKFFT) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    return NR_LDPC_SUCCESS;
#endif
}

void pusch_e2e_destroy(pusch_e2e_handle_t handle)
{
    if (handle) {
#ifdef OCUDU_PHY_CUDA_ENABLE_VKFFT
        for (int n = 0; n <= PUSCH_DEPRECODE_MAX_DFT_SIZE; ++n) {
            if (handle->vkfft_plans[n]) {
                deleteVkFFT(&handle->vkfft_plans[n]->app);
                delete handle->vkfft_plans[n];
            }
        }
#endif
        if (handle->d_dmrs_pilots) cudaFree(handle->d_dmrs_pilots);
        if (handle->d_ch_estimates) cudaFree(handle->d_ch_estimates);
        if (handle->d_dmrs_estimates) cudaFree(handle->d_dmrs_estimates);
        if (handle->d_noise_vars) cudaFree(handle->d_noise_vars);
        if (handle->d_noise_var_partial) cudaFree(handle->d_noise_var_partial);
        /* Free consolidated accumulator block (contains d_noise_count, d_rsrp_accum,
         * d_epre_accum, d_sinr_noise_accum, d_sinr_rsrp_accum, d_eq_noise_var_sum,
         * d_eq_noise_var_count as aliases — do NOT free them individually) */
        if (handle->d_accum_block) cudaFree(handle->d_accum_block);
        /* Managed mode: h_sinr_eq_block / h_sinr_epre_result alias managed device
         * memory freed above/below — do not free them separately. */
        if (!handle->sinr_managed && handle->h_sinr_eq_block) cudaFreeHost(handle->h_sinr_eq_block);
        if (handle->d_dmrs_c_inits) cudaFree(handle->d_dmrs_c_inits);
        if (handle->d_sinr_epre_result) cudaFree(handle->d_sinr_epre_result);
        if (!handle->sinr_managed && handle->h_sinr_epre_result) cudaFreeHost(handle->h_sinr_epre_result);
        /* Free pre-allocated per-slot buffers */
        if (handle->d_dmrs_indices) cudaFree(handle->d_dmrs_indices);
        if (handle->d_freq_interp_estimates) cudaFree(handle->d_freq_interp_estimates);
        if (handle->d_lse_averaged) cudaFree(handle->d_lse_averaged);
        if (handle->d_llrs_half_temp) cudaFree(handle->d_llrs_half_temp);
        if (handle->d_estimates_fp16) cudaFree(handle->d_estimates_fp16);
        if (handle->d_dmrs_received) cudaFree(handle->d_dmrs_received);
        /* d_noise_count: alias into d_accum_block unless batch path reallocated it */
        if (handle->d_noise_count && handle->d_accum_block &&
            !((char*)handle->d_noise_count >= (char*)handle->d_accum_block &&
              (char*)handle->d_noise_count < (char*)handle->d_accum_block + handle->accum_block_size)) {
            cudaFree(handle->d_noise_count);
        }
        if (handle->d_scrambling_seq) cudaFree(handle->d_scrambling_seq);
        if (handle->d_dmrs_pilot_bits) cudaFree(handle->d_dmrs_pilot_bits);
        if (handle->d_dmrs_mask) cudaFree(handle->d_dmrs_mask);
        if (handle->d_precomputed_dmrs_pilots) cudaFree(handle->d_precomputed_dmrs_pilots);
        if (handle->d_low_papr_pilots) cudaFree(handle->d_low_papr_pilots);
        if (handle->d_deprecode_symbols) cudaFree(handle->d_deprecode_symbols);
        if (handle->d_deprecode_noise_vars) cudaFree(handle->d_deprecode_noise_vars);
        if (handle->d_symbol_start_times) cudaFree(handle->d_symbol_start_times);
        if (handle->d_mimo_cfo_phasors) cudaFree(handle->d_mimo_cfo_phasors);
        if (handle->precompute_stream) {
            cudaStreamDestroy(handle->precompute_stream);
            handle->precompute_stream = nullptr;
        }
        if (handle->data_scrambler) scrambler_destroy(handle->data_scrambler);
        if (handle->dmrs_scrambler) scrambler_destroy(handle->dmrs_scrambler);
        delete handle;
    }
}

void pusch_e2e_set_eq_output(pusch_e2e_handle_t handle,
                              float2* d_eq_symbols_out,
                              float* d_eq_noise_var_out)
{
    if (!handle) return;
    handle->d_eq_symbols_out = d_eq_symbols_out;
    handle->d_eq_noise_var_out = d_eq_noise_var_out;
}

const void* pusch_e2e_get_estimates_fp16(pusch_e2e_handle_t handle)
{
    return handle ? handle->d_estimates_fp16 : nullptr;
}

nr_ldpc_status_t pusch_e2e_configure(pusch_e2e_handle_t handle,
                                      const pusch_e2e_config_t* cfg)
{
    if (!handle || !cfg) return NR_LDPC_ERROR_INVALID_CONFIG;

    /* Validate nof_tx_layers: E2E GPU kernels currently only support single-layer.
     * For 2/4-layer MIMO, the caller should use the batch GPU path (CPU equalization
     * + GPU soft demod), which leverages the multi-layer kernels in equalization.cu. */
    int nof_tx_layers = cfg->nof_tx_layers;
    if (nof_tx_layers == 0) nof_tx_layers = 1;  /* Default to 1 if not set */
    if (nof_tx_layers != 1 && nof_tx_layers != 2 && nof_tx_layers != 3 && nof_tx_layers != 4) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    if (cfg->dmrs_type != DMRS_TYPE_1 && cfg->dmrs_type != DMRS_TYPE_2) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    if (pusch_e2e_get_dmrs_symbol_indices_checked(cfg->dmrs_symbol_mask, nullptr) <= 0) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    /* Multi-layer requires appropriate port configuration */
    if (nof_tx_layers == 2 && cfg->nof_rx_ports < 2) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    if (nof_tx_layers == 3 && cfg->nof_rx_ports < 3) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    if (nof_tx_layers == 4 && cfg->nof_rx_ports < 4) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    handle->config = *cfg;
    handle->config.nof_tx_layers = nof_tx_layers;  /* Store validated value */
    handle->configured = true;

    /* Configure data scrambler
     * Per TS 38.211 Section 6.3.1.1 (PUSCH) and 7.3.1.1 (PDSCH):
     * c_init = n_RNTI × 2^15 + q × 2^14 + n_ID
     * Slot number is NOT used for data scrambling (only for DMRS). */
    nr_scrambling_config_t scr_cfg = {};
    scr_cfg.n_RNTI = cfg->rnti;
    scr_cfg.n_ID = cfg->n_id;
    scr_cfg.q = 0;
    scr_cfg.n_s = 0;  // Not used for data scrambling per spec

    nr_ldpc_status_t status = scrambler_configure(handle->data_scrambler, &scr_cfg);
    if (status != NR_LDPC_SUCCESS) return status;

    /* ================================================================
     * PRE-COMPUTE SCRAMBLING SEQUENCE AT CONFIGURE TIME
     *
     * Since c_init = (rnti << 15) + n_id is fixed per configuration,
     * we can pre-generate the entire scrambling sequence once here.
     * This eliminates ~1229 O(log N) LFSR jumps per slot at runtime!
     *
     * Size for 256QAM (mod_order=8) even when current MCS is lower so MCS
     * adaptation does not re-enter this path and stream-sync on the RT slot.
     * ================================================================ */
    {
        /* Calculate max data REs for this configuration. Keep this tied to the
         * actual DMRS mask so single-DMRS MIMO allocations do not under-size the
         * cached scrambling sequence. */
        int nof_dmrs_symbols_for_capacity =
            pusch_e2e_get_dmrs_symbol_indices_checked(cfg->dmrs_symbol_mask, nullptr);
        int dmrs_re_per_prb = dmrs_get_re_per_prb(cfg->dmrs_type) * cfg->nof_cdm_groups_without_data;
        if (dmrs_re_per_prb > 12) return NR_LDPC_ERROR_INVALID_CONFIG;
        int data_re_per_prb =
            cfg->nof_symbols * 12 - nof_dmrs_symbols_for_capacity * dmrs_re_per_prb;
        if (data_re_per_prb <= 0) return NR_LDPC_ERROR_INVALID_CONFIG;
        int max_data_re = cfg->nof_prb * data_re_per_prb;
        /* Always size for 256QAM worst-case so QPSK→64/256QAM MCS steps do not
         * free+malloc or re-precompute scramble under RT scheduling. */
        constexpr int MAX_MOD_ORDER = 8;
        int scramble_mod_order = cfg->mod_order > MAX_MOD_ORDER ? cfg->mod_order : MAX_MOD_ORDER;
        int nof_scramble_bits = max_data_re * nof_tx_layers * scramble_mod_order;
        int nof_scramble_words = (nof_scramble_bits + 31) / 32;
        size_t scramble_seq_size = nof_scramble_words * sizeof(uint32_t);

        /* Allocate if needed */
        if (scramble_seq_size > handle->scrambling_seq_capacity) {
            if (handle->d_scrambling_seq) cudaFree(handle->d_scrambling_seq);
            handle->d_scrambling_seq = nullptr;
            handle->scrambling_seq_capacity = 0;
            handle->cached_data_scrambling_c_init = UINT32_MAX;
            handle->cached_data_scrambling_bits = 0;
            cudaError_t err = cudaMalloc(&handle->d_scrambling_seq, scramble_seq_size);
            if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
            handle->scrambling_seq_capacity = scramble_seq_size;
        }

        /* Pre-generate scrambling sequence when the cached prefix is not sufficient. */
        uint32_t data_c_init = (cfg->rnti << 15) + cfg->n_id;
        if (handle->cached_data_scrambling_c_init != data_c_init ||
            handle->cached_data_scrambling_bits < nof_scramble_bits) {
            int scr_gen_threads = 256;
            int scr_gen_blocks = (nof_scramble_words + scr_gen_threads - 1) / scr_gen_threads;

            cudaStream_t precompute_stream = handle->precompute_stream;
            launch_kernel_generate_scrambling_sequence(scr_gen_blocks, scr_gen_threads, 0, precompute_stream,
                handle->d_scrambling_seq, data_c_init, nof_scramble_bits);
            nr_ldpc_status_t sync_status =
                sync_pusch_precompute_stream(precompute_stream, "scrambling-sequence precompute");
            if (sync_status != NR_LDPC_SUCCESS) return sync_status;

            handle->cached_data_scrambling_c_init = data_c_init;
            handle->cached_data_scrambling_bits = nof_scramble_bits;
        }
    }

    /* ================================================================
     * PRE-COMPUTE DMRS PILOTS FOR ALL 14 OFDM SYMBOLS × ALL SLOTS
     *
     * Since scrambling_id and n_scid are fixed per configuration,
     * we pre-generate pilots for ALL 14 symbol positions per slot.
     * This means ANY dmrs_symbol_mask works at runtime without
     * mask matching — the runtime path just gathers the needed
     * symbols via D2D copies.
     *
     * DMRS c_init depends ONLY on scrambling_id, n_scid, slot, and
     * symbol index (TS 38.211 6.4.1.1.1.1).  Skip the expensive
     * 280-kernel precompute when only RNTI/n_id/mod_order changed.
     *
     * Layout: [slot_idx * 14 * words_per_sym + ofdm_symbol * words_per_sym]
     * Memory: ~115 KB for 273 PRB (trivial).
     * ================================================================ */
    if (cfg->use_low_papr_dmrs != 0) {
        /* Transform-precoded PUSCH (Msg3 / DFT-s-OFDM) uses low-PAPR DMRS
         * pilots, not the pseudo-random Gold DMRS table below.  Avoid the
         * 20 slots x 14 symbols precompute in the timing-critical Msg3 path. */
        int M_zc = cfg->nof_prb * 6;
        if (M_zc <= 0) return NR_LDPC_ERROR_INVALID_CONFIG;

        size_t low_papr_size = (size_t)M_zc * sizeof(cuFloatComplex);
        if (low_papr_size > handle->low_papr_pilots_capacity) {
            if (handle->d_low_papr_pilots) cudaFree(handle->d_low_papr_pilots);
            cudaError_t err = cudaMalloc(&handle->d_low_papr_pilots, low_papr_size);
            if (err != cudaSuccess) {
                handle->d_low_papr_pilots = nullptr;
                handle->low_papr_pilots_capacity = 0;
                handle->cached_low_papr_nof_prb = -1;
                handle->cached_low_papr_n_rs_id = -1;
                return NR_LDPC_ERROR_ALLOC_FAILED;
            }
            handle->low_papr_pilots_capacity = low_papr_size;
            handle->cached_low_papr_nof_prb = -1;
            handle->cached_low_papr_n_rs_id = -1;
        }

        if (handle->cached_low_papr_nof_prb != cfg->nof_prb ||
            handle->cached_low_papr_n_rs_id != cfg->n_rs_id) {
            cudaStream_t precompute_stream = handle->precompute_stream;
            int u = cfg->n_rs_id % NOF_LOW_PAPR_GROUPS;
            if (u < 0) u += NOF_LOW_PAPR_GROUPS;
            int pilot_threads = 256;
            int pilot_blocks = (M_zc + pilot_threads - 1) / pilot_threads;
            launch_kernel_generate_low_papr_pilots(pilot_blocks, pilot_threads, 0, precompute_stream,
                handle->d_low_papr_pilots, u, M_zc);

            nr_ldpc_status_t sync_status =
                sync_pusch_precompute_stream(precompute_stream, "low-PAPR DMRS-pilot precompute");
            if (sync_status != NR_LDPC_SUCCESS) return sync_status;

            handle->cached_low_papr_nof_prb = cfg->nof_prb;
            handle->cached_low_papr_n_rs_id = cfg->n_rs_id;
        }
    } else if (cfg->scrambling_id != handle->cached_dmrs_scrambling_id ||
        cfg->n_scid        != handle->cached_dmrs_n_scid ||
        cfg->nof_prb       != handle->cached_dmrs_nof_prb ||
        (int)cfg->dmrs_type != handle->cached_dmrs_type)
    {
        int pilots_per_symbol = cfg->nof_prb * dmrs_get_re_per_prb(cfg->dmrs_type);
        int bits_per_sym = pilots_per_symbol * 2;   /* 2 bits per pilot (QPSK) */
        int words_per_sym = (bits_per_sym + 31) / 32;
        int words_per_slot = words_per_sym * 14;    /* all 14 OFDM symbols */
        int total_words = words_per_slot * MAX_SLOTS_PER_FRAME;
        size_t total_size = total_words * sizeof(uint32_t);

        /* Allocate if needed */
        if (total_size > handle->precomputed_dmrs_pilots_capacity) {
            if (handle->d_precomputed_dmrs_pilots) cudaFree(handle->d_precomputed_dmrs_pilots);
            cudaError_t err = cudaMalloc(&handle->d_precomputed_dmrs_pilots, total_size);
            if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
            handle->precomputed_dmrs_pilots_capacity = total_size;
        }

        /* Pre-generate DMRS pilots for all 14 symbols × all slots */
        int pilot_gen_threads = 256;
        int nof_threads_needed = (pilots_per_symbol + 31) / 32;
        int pilot_gen_blocks = (nof_threads_needed + pilot_gen_threads - 1) / pilot_gen_threads;

        {
            int total_pairs = MAX_SLOTS_PER_FRAME * 14;  /* 20 slots × 14 symbols = 280 */
            dim3 grid(pilot_gen_blocks, total_pairs);
            dim3 block(pilot_gen_threads);
            cudaStream_t precompute_stream = handle->precompute_stream;

            launch_kernel_generate_dmrs_pilots_batched(grid, block, 0, precompute_stream,
                handle->d_precomputed_dmrs_pilots,
                pilots_per_symbol, words_per_sym, words_per_slot,
                MAX_SLOTS_PER_FRAME, cfg->scrambling_id, cfg->n_scid);
            nr_ldpc_status_t sync_status = sync_pusch_precompute_stream(precompute_stream, "DMRS-pilot precompute");
            if (sync_status != NR_LDPC_SUCCESS) return sync_status;
        }

        /* Ensure d_dmrs_pilot_bits is large enough for D2D gather at runtime
         * (MAX_DMRS_SYMBOLS contiguous symbols for CV kernel) */
        size_t gather_size = (size_t)MAX_DMRS_SYMBOLS * words_per_sym * sizeof(uint32_t);
        if (gather_size > handle->dmrs_pilot_bits_capacity) {
            if (handle->d_dmrs_pilot_bits) cudaFree(handle->d_dmrs_pilot_bits);
            cudaError_t err = cudaMalloc(&handle->d_dmrs_pilot_bits, gather_size);
            if (err == cudaSuccess) {
                handle->dmrs_pilot_bits_capacity = gather_size;
            } else {
                handle->d_dmrs_pilot_bits = nullptr;
                handle->dmrs_pilot_bits_capacity = 0;
            }
        }

        /* Store precomputation metadata */
        handle->precomputed_nof_slots = MAX_SLOTS_PER_FRAME;
        handle->precomputed_pilots_per_symbol = pilots_per_symbol;
        handle->precomputed_words_per_slot = words_per_slot;
        handle->precomputed_words_per_sym = words_per_sym;
        handle->dmrs_pilots_precomputed = true;

        handle->cached_dmrs_scrambling_id = cfg->scrambling_id;
        handle->cached_dmrs_n_scid = cfg->n_scid;
        handle->cached_dmrs_nof_prb = cfg->nof_prb;
        handle->cached_dmrs_type = (int)cfg->dmrs_type;
    }

    /* ================================================================
     * PRECOMPUTE OFDM SYMBOL START TIMES FOR CFO COMPENSATION
     *
     * These are the start-of-FFT-window times (seconds from slot start)
     * for each of the 14 OFDM symbols. Only recompute when SCS changes.
     * Used by the E2E kernel to compute phase rotation dt between DMRS
     * and data symbols: exp(j * 2π * cfo * dt).
     * ================================================================ */
    if (cfg->scs_khz > 0 && cfg->scs_khz != handle->symbol_times_scs_khz &&
        handle->d_symbol_start_times) {
        int mu = 0;
        if (cfg->scs_khz == 30) mu = 1;
        else if (cfg->scs_khz == 60) mu = 2;
        else if (cfg->scs_khz == 120) mu = 3;
        float scs_hz = (float)cfg->scs_khz * 1000.0f;
        int half_slot_sym = 7 * (1 << mu);

        float t = 0.0f;
        for (int j = 0; j < 14; j++) {
            int cp_kappa = 144 >> mu;
            if (j == 0 || j == half_slot_sym) cp_kappa += 16;
            float cp_s = (float)cp_kappa / 30720000.0f;
            handle->h_symbol_start_times[j] = t + cp_s;  /* time at start of FFT window */
            t = handle->h_symbol_start_times[j] + 1.0f / scs_hz;  /* end of FFT window */
        }
        cudaMemcpy(handle->d_symbol_start_times, handle->h_symbol_start_times,
                   14 * sizeof(float), cudaMemcpyHostToDevice);
        handle->symbol_times_scs_khz = cfg->scs_khz;
    }

    return NR_LDPC_SUCCESS;
}

nr_ldpc_status_t pusch_e2e_update_slot_config(pusch_e2e_handle_t handle,
                                               int nof_prb,
                                               int start_prb,
                                               int slot_idx,
                                               int dmrs_symbol_mask)
{
    if (!handle || !handle->configured) return NR_LDPC_ERROR_INVALID_CONFIG;
    if (pusch_e2e_get_dmrs_symbol_indices_checked(dmrs_symbol_mask, nullptr) <= 0) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    /* Update per-slot fields in the handle's config.
     * These values change per PUSCH transmission but don't require
     * a full reconfiguration (which would trigger GPU synchronization). */
    handle->config.nof_prb = nof_prb;
    handle->config.start_prb = start_prb;
    handle->config.slot_idx = slot_idx;
    handle->config.dmrs_symbol_mask = dmrs_symbol_mask;

    /* NOTE: For loopback tests, we use slot=0 to match the transmitter.
     * The scrambling was already configured correctly at configure time with
     * slot=0, so we don't need to reconfigure it here. In real deployments,
     * this would need to be updated per-slot. */

    return NR_LDPC_SUCCESS;
}

nr_ldpc_status_t pusch_e2e_update_data_params(pusch_e2e_handle_t handle,
                                              uint16_t           rnti,
                                              uint16_t           n_id,
                                              int                mod_order)
{
    if (!handle || !handle->configured) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    if (mod_order != 1 && mod_order != 2 && mod_order != 4 && mod_order != 6 && mod_order != 8) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    const pusch_e2e_config_t& cfg = handle->config;
    const int nof_tx_layers =
        (cfg.nof_tx_layers == 0) ? 1 : cfg.nof_tx_layers;

    handle->config.rnti      = rnti;
    handle->config.n_id      = n_id;
    handle->config.mod_order = mod_order;

    /* Keep data scrambler c_init in sync (slot not part of PUSCH data scramble). */
    nr_scrambling_config_t scr_cfg = {};
    scr_cfg.n_RNTI                 = rnti;
    scr_cfg.n_ID                   = n_id;
    scr_cfg.q                      = 0;
    scr_cfg.n_s                    = 0;
    nr_ldpc_status_t status        = scrambler_configure(handle->data_scrambler, &scr_cfg);
    if (status != NR_LDPC_SUCCESS) {
        return status;
    }

    int nof_dmrs_symbols_for_capacity =
        pusch_e2e_get_dmrs_symbol_indices_checked(cfg.dmrs_symbol_mask, nullptr);
    int dmrs_re_per_prb = dmrs_get_re_per_prb(cfg.dmrs_type) * cfg.nof_cdm_groups_without_data;
    if (dmrs_re_per_prb > 12 || nof_dmrs_symbols_for_capacity <= 0) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    int data_re_per_prb = cfg.nof_symbols * 12 - nof_dmrs_symbols_for_capacity * dmrs_re_per_prb;
    if (data_re_per_prb <= 0) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    int max_data_re = cfg.nof_prb * data_re_per_prb;
    /* Match configure(): size for 256QAM so MCS up does not re-precompute. */
    constexpr int MAX_MOD_ORDER = 8;
    int scramble_mod_order      = mod_order > MAX_MOD_ORDER ? mod_order : MAX_MOD_ORDER;
    int nof_scramble_bits       = max_data_re * nof_tx_layers * scramble_mod_order;
    int nof_scramble_words      = (nof_scramble_bits + 31) / 32;
    size_t scramble_seq_size    = static_cast<size_t>(nof_scramble_words) * sizeof(uint32_t);

    if (scramble_seq_size > handle->scrambling_seq_capacity) {
        if (handle->d_scrambling_seq) {
            cudaFree(handle->d_scrambling_seq);
        }
        handle->d_scrambling_seq              = nullptr;
        handle->scrambling_seq_capacity      = 0;
        handle->cached_data_scrambling_c_init = UINT32_MAX;
        handle->cached_data_scrambling_bits   = 0;
        cudaError_t err = cudaMalloc(&handle->d_scrambling_seq, scramble_seq_size);
        if (err != cudaSuccess) {
            return NR_LDPC_ERROR_ALLOC_FAILED;
        }
        handle->scrambling_seq_capacity = scramble_seq_size;
    }

    uint32_t data_c_init = (static_cast<uint32_t>(rnti) << 15) + static_cast<uint32_t>(n_id);
    if (handle->cached_data_scrambling_c_init != data_c_init ||
        handle->cached_data_scrambling_bits < nof_scramble_bits) {
        int scr_gen_threads = 256;
        int scr_gen_blocks  = (nof_scramble_words + scr_gen_threads - 1) / scr_gen_threads;
        cudaStream_t precompute_stream = handle->precompute_stream;
        launch_kernel_generate_scrambling_sequence(scr_gen_blocks,
                                                   scr_gen_threads,
                                                   0,
                                                   precompute_stream,
                                                   handle->d_scrambling_seq,
                                                   data_c_init,
                                                   nof_scramble_bits);
        nr_ldpc_status_t sync_status =
            sync_pusch_precompute_stream(precompute_stream, "scrambling-sequence data-params update");
        if (sync_status != NR_LDPC_SUCCESS) {
            return sync_status;
        }
        handle->cached_data_scrambling_c_init = data_c_init;
        handle->cached_data_scrambling_bits   = nof_scramble_bits;
    }

    return NR_LDPC_SUCCESS;
}

const void* pusch_e2e_get_estimates(pusch_e2e_handle_t handle)
{
    if (!handle) return nullptr;
    return handle->d_ch_estimates;
}

static nr_ldpc_status_t pusch_e2e_process_full_gpu_impl(
    pusch_e2e_handle_t handle,
    const void* d_grid_cbf16,
    void* d_llrs_half,
    const int* d_re_indices,
    int nof_data_re,
    cudaStream_t stream,
    bool run_demod)
{
    if (!handle || !handle->configured) return NR_LDPC_ERROR_INVALID_CONFIG;
    if (!d_grid_cbf16 || !d_re_indices || (run_demod && !d_llrs_half)) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    const pusch_e2e_config_t& cfg = handle->config;

    /* MIMO support: 1, 2, or 4 layers are supported.
     * - 1 layer: MRC combining (existing path)
     * - 2 layers: 2x2 MIMO with ZF/MMSE equalization
     * - 4 layers: 4x4 MIMO with ZF/MMSE equalization
     * Layer count must not exceed port count. */
    if (cfg.nof_tx_layers > cfg.nof_rx_ports) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    if (cfg.nof_tx_layers != 1 && cfg.nof_tx_layers != 2 && cfg.nof_tx_layers != 3 && cfg.nof_tx_layers != 4) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    /* Calculate dimensions */
    int nof_dmrs_re_per_symbol = cfg.nof_prb * dmrs_get_re_per_prb(cfg.dmrs_type);
    int nof_re_per_symbol = cfg.nof_prb * 12;
    int grid_stride = cfg.grid_nof_symbols * cfg.grid_nof_subcarriers;
    int symbol_stride = cfg.grid_nof_subcarriers;

    int dmrs_symbol_indices[MAX_DMRS_SYMBOLS];
    int nof_dmrs_symbols = pusch_e2e_get_dmrs_symbol_indices_checked(cfg.dmrs_symbol_mask, dmrs_symbol_indices);
    if (nof_dmrs_symbols <= 0) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    cudaError_t err;
    nr_ldpc_status_t status;

    /* Allocate device memory for DMRS LSE estimates */
    size_t lse_size = nof_dmrs_symbols * nof_dmrs_re_per_symbol * cfg.nof_rx_ports * sizeof(cuFloatComplex);
    if (lse_size > handle->dmrs_estimates_capacity) {
        if (handle->d_dmrs_estimates) cudaFree(handle->d_dmrs_estimates);
        err = cudaMalloc(&handle->d_dmrs_estimates, lse_size);
        if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
        handle->dmrs_estimates_capacity = lse_size;
    }

    /* Allocate device memory for full channel estimates */
    size_t est_size = cfg.nof_symbols * nof_re_per_symbol * cfg.nof_rx_ports * sizeof(cuFloatComplex);
    if (est_size > handle->ch_estimates_capacity) {
        if (handle->d_ch_estimates) cudaFree(handle->d_ch_estimates);
        err = cudaMalloc(&handle->d_ch_estimates, est_size);
        if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
        handle->ch_estimates_capacity = est_size;
    }
    handle->nof_estimates = cfg.nof_symbols * nof_re_per_symbol * cfg.nof_rx_ports;

    /* Allocate device memory for GPU-computed noise variances */
    size_t noise_var_size = cfg.nof_rx_ports * sizeof(float);
    if (noise_var_size > handle->noise_vars_capacity) {
        if (handle->d_noise_vars) cudaFree(handle->d_noise_vars);
        err = cudaMalloc(&handle->d_noise_vars, noise_var_size);
        if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
        cudaMemset(handle->d_noise_vars, 0, noise_var_size);  /* Zero once at alloc */
        handle->noise_vars_capacity = noise_var_size;
    }

    /* Allocate device memory for partial noise variance sums */
    size_t partial_size = nof_dmrs_symbols * cfg.nof_rx_ports * sizeof(float);
    if (partial_size > handle->noise_var_partial_capacity) {
        if (handle->d_noise_var_partial) cudaFree(handle->d_noise_var_partial);
        err = cudaMalloc(&handle->d_noise_var_partial, partial_size);
        if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
        handle->noise_var_partial_capacity = partial_size;
    }

    /* Use pre-allocated buffers (avoid per-slot malloc - HUGE efficiency win!) */
    size_t dmrs_indices_needed = nof_dmrs_symbols * sizeof(int);
    if (dmrs_indices_needed > handle->dmrs_indices_capacity) {
        if (handle->d_dmrs_indices) cudaFree(handle->d_dmrs_indices);
        err = cudaMalloc(&handle->d_dmrs_indices, dmrs_indices_needed);
        if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
        handle->dmrs_indices_capacity = dmrs_indices_needed;
    }
    int* d_dmrs_indices = handle->d_dmrs_indices;
    /* CRITICAL FIX: Use synchronous cudaMemcpy - dmrs_symbol_indices is a local
     * stack variable that may be overwritten before async copy completes. */
    cudaMemcpy(d_dmrs_indices, dmrs_symbol_indices, nof_dmrs_symbols * sizeof(int),
               cudaMemcpyHostToDevice);

    /* Use pre-allocated buffer for frequency-interpolated DMRS estimates */
    size_t freq_interp_size = nof_dmrs_symbols * nof_re_per_symbol * cfg.nof_rx_ports * sizeof(cuFloatComplex);
    if (freq_interp_size > handle->freq_interp_capacity) {
        if (handle->d_freq_interp_estimates) cudaFree(handle->d_freq_interp_estimates);
        err = cudaMalloc(&handle->d_freq_interp_estimates, freq_interp_size);
        if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
        handle->freq_interp_capacity = freq_interp_size;
    }
    cuFloatComplex* d_freq_interp_estimates = handle->d_freq_interp_estimates;

    /* ================================================================
     * Phase 1a: Generate DMRS and compute LSE for ALL symbols first
     * (Mirroring CPU algorithm: collect all estimates before averaging)
     * ================================================================ */

    /* Store DMRS sequence pointers for reuse in noise variance estimation.
     * Keep this on the stack to avoid allocator jitter in the Msg3 hot path. */
    unsigned int* h_dmrs_seqs[MAX_DMRS_SYMBOLS] = {};

    /* CRITICAL FIX: cfg.slot_idx is absolute slot number, but DMRS c_init uses slot within frame */
    int slot_in_frame_for_dmrs = cfg.slot_idx % MAX_SLOTS_PER_FRAME;

    /* Low-PAPR DMRS path for transform precoding (MSG3/DFT-s-OFDM) */
    bool use_low_papr = (cfg.use_low_papr_dmrs != 0);
    cuFloatComplex* d_low_papr_pilots = nullptr;

    if (use_low_papr) {
        /* Allocate buffer for low-PAPR pilot sequence if needed.
         * For transform precoding, DMRS has 6 REs per PRB (no CDM, all pilots).
         * Sequence length M_zc = nof_prb * 6. */
        int M_zc = cfg.nof_prb * 6;
        size_t low_papr_size = M_zc * sizeof(cuFloatComplex);

        if (low_papr_size > handle->low_papr_pilots_capacity) {
            if (handle->d_low_papr_pilots) cudaFree(handle->d_low_papr_pilots);
            err = cudaMalloc(&handle->d_low_papr_pilots, low_papr_size);
            if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
            handle->low_papr_pilots_capacity = low_papr_size;
            handle->cached_low_papr_nof_prb = -1;
            handle->cached_low_papr_n_rs_id = -1;
        }
        d_low_papr_pilots = handle->d_low_papr_pilots;

        if (handle->cached_low_papr_nof_prb != cfg.nof_prb ||
            handle->cached_low_papr_n_rs_id != cfg.n_rs_id) {
            /* Generate low-PAPR pilots: u = n_rs_id % 30 */
            int u = cfg.n_rs_id % NOF_LOW_PAPR_GROUPS;
            if (u < 0) u += NOF_LOW_PAPR_GROUPS;
            int pilot_threads = 256;
            int pilot_blocks = (M_zc + pilot_threads - 1) / pilot_threads;
            launch_kernel_generate_low_papr_pilots(pilot_blocks, pilot_threads, 0, stream, 
                d_low_papr_pilots, u, M_zc);
            handle->cached_low_papr_nof_prb = cfg.nof_prb;
            handle->cached_low_papr_n_rs_id = cfg.n_rs_id;
        }
    }

    for (int d = 0; d < nof_dmrs_symbols; d++) {
        int dmrs_sym = dmrs_symbol_indices[d];

        if (!use_low_papr) {
            /* Standard pseudo-random Gold sequence DMRS (TS 38.211 Section 6.4.1.1.1) */

            /* Compute c_init for this DMRS symbol - use slot within frame per 3GPP TS 38.211 */
            uint32_t c_init = dmrs_compute_c_init(slot_in_frame_for_dmrs, dmrs_sym, cfg.scrambling_id, cfg.n_scid);

            /* Configure DMRS scrambler with direct c_init value */
            status = scrambler_configure_c_init(handle->dmrs_scrambler, c_init);
            if (status != NR_LDPC_SUCCESS) {
                return status;
            }

            /* Generate sequence bits (accounting for start_prb offset from point A) */
            int nof_seq_bits = (cfg.start_prb + cfg.nof_prb) * 6 * 2;
            status = scrambler_generate_sequence(handle->dmrs_scrambler, nof_seq_bits, stream);
            if (status != NR_LDPC_SUCCESS) {
                return status;
            }

            h_dmrs_seqs[d] = (unsigned int*)scrambler_get_sequence_ptr(handle->dmrs_scrambler);
        } else {
            /* Low-PAPR DMRS uses same sequence for all symbols (only depends on n_rs_id).
             * Set to NULL to indicate the LSE kernel should use the complex pilot buffer. */
            h_dmrs_seqs[d] = nullptr;
        }

        /* Output buffer for this DMRS symbol's LSE estimates */
        cuFloatComplex* d_lse_out = handle->d_dmrs_estimates +
                                     d * (cfg.nof_rx_ports * nof_dmrs_re_per_symbol);

        /* Launch LSE kernel */
        if (cfg.dmrs_type == DMRS_TYPE_1) {
            int blocks = cfg.nof_prb;
            int threads = cfg.nof_rx_ports;

            if (!use_low_papr) {
                /* Standard pseudo-random DMRS LSE */
                launch_kernel_dmrs_lse_type1(blocks, threads, 0, stream, 
                    static_cast<const unsigned int*>(d_grid_cbf16),
                    h_dmrs_seqs[d],
                    d_lse_out,
                    cfg.nof_prb,
                    cfg.nof_rx_ports,
                    grid_stride,
                    symbol_stride,
                    cfg.start_prb,
                    dmrs_sym,
                    cfg.start_symbol,
                    cfg.dmrs_scaling);
            } else {
                /* Low-PAPR DMRS LSE - uses complex pilot buffer directly.
                 * For transform precoding, all 6 REs per PRB carry pilots (no CDM). */
                launch_kernel_dmrs_lse_type1_low_papr(blocks, threads, 0, stream, 
                    static_cast<const unsigned int*>(d_grid_cbf16),
                    d_low_papr_pilots,
                    d_lse_out,
                    cfg.nof_prb,
                    cfg.nof_rx_ports,
                    grid_stride,
                    symbol_stride,
                    cfg.start_prb,
                    dmrs_sym,
                    cfg.start_symbol,
                    cfg.dmrs_scaling);
            }
        }

        /* Frequency interpolation for this DMRS symbol */
        cuFloatComplex* d_freq_out = d_freq_interp_estimates +
                                      d * (cfg.nof_rx_ports * nof_re_per_symbol);

        int total_re = cfg.nof_prb * cfg.nof_rx_ports * 12;
        int threads_fi = 256;
        int blocks_fi = (total_re + threads_fi - 1) / threads_fi;
        launch_kernel_freq_interpolate_type1(blocks_fi, threads_fi, 0, stream, 
            d_lse_out,
            d_freq_out,
            cfg.nof_prb,
            cfg.nof_rx_ports);
    }

    /* ================================================================
     * Phase 1b: Average LSE estimates across ALL DMRS symbols
     * (Mirroring CPU algorithm: avg_h = beta × Σ h_est / nof_dmrs_symbols)
     * ================================================================ */

    /* Use pre-allocated buffer for averaged LSE estimates */
    size_t avg_lse_size = nof_dmrs_re_per_symbol * cfg.nof_rx_ports * sizeof(cuFloatComplex);
    if (avg_lse_size > handle->lse_averaged_capacity) {
        if (handle->d_lse_averaged) cudaFree(handle->d_lse_averaged);
        err = cudaMalloc(&handle->d_lse_averaged, avg_lse_size);
        if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
        handle->lse_averaged_capacity = avg_lse_size;
    }
    cuFloatComplex* d_lse_averaged = handle->d_lse_averaged;

    /* Launch averaging kernel */
    int avg_total = cfg.nof_prb * cfg.nof_rx_ports * 6;
    int avg_threads = 256;
    int avg_blocks = (avg_total + avg_threads - 1) / avg_threads;
    launch_kernel_average_lse_estimates(avg_blocks, avg_threads, 0, stream, 
        handle->d_dmrs_estimates,
        d_lse_averaged,
        nof_dmrs_symbols,
        cfg.nof_prb,
        cfg.nof_rx_ports,
        nof_dmrs_re_per_symbol,
        cfg.dmrs_scaling);

    /* ================================================================
     * Phase 1c: Estimate noise variance using AVERAGED channel estimates
     * (Mirroring CPU algorithm: compute residuals with averaged h_est)
     * ================================================================ */

    /* Use slot within frame for DMRS c_init (3GPP TS 38.211 Section 6.4.1.1.1.1) */
    int slot_in_frame_for_dmrs_nv = cfg.slot_idx % MAX_SLOTS_PER_FRAME;

    for (int d = 0; d < nof_dmrs_symbols; d++) {
        int dmrs_sym = dmrs_symbol_indices[d];

        /* Launch noise variance estimation kernel using AVERAGED estimates */
        if (cfg.dmrs_type == DMRS_TYPE_1) {
            int nv_threads = 256;
            int nv_blocks = cfg.nof_rx_ports;
            int shared_mem = nv_threads * sizeof(float);
            float* d_partial_out = handle->d_noise_var_partial + d * cfg.nof_rx_ports;

            if (!use_low_papr) {
                /* Regenerate DMRS sequence for this symbol (needed for noise estimation) */
                uint32_t c_init = dmrs_compute_c_init(slot_in_frame_for_dmrs_nv, dmrs_sym, cfg.scrambling_id, cfg.n_scid);
                status = scrambler_configure_c_init(handle->dmrs_scrambler, c_init);
                if (status != NR_LDPC_SUCCESS) {
                    return status;
                }

                int nof_seq_bits = (cfg.start_prb + cfg.nof_prb) * 6 * 2;
                status = scrambler_generate_sequence(handle->dmrs_scrambler, nof_seq_bits, stream);
                if (status != NR_LDPC_SUCCESS) {
                    return status;
                }

                const unsigned int* d_dmrs_seq = scrambler_get_sequence_ptr(handle->dmrs_scrambler);

                launch_kernel_noise_variance_from_dmrs(nv_blocks, nv_threads, shared_mem, stream, 
                    static_cast<const unsigned int*>(d_grid_cbf16),
                    d_dmrs_seq,
                    d_lse_averaged,  /* Use AVERAGED estimates, not per-symbol */
                    d_partial_out,
                    cfg.nof_prb,
                    cfg.nof_rx_ports,
                    grid_stride,
                    symbol_stride,
                    cfg.start_prb,
                    dmrs_sym,
                    cfg.start_symbol,
                    cfg.dmrs_scaling,
                    nof_dmrs_re_per_symbol);
            } else {
                launch_kernel_noise_variance_from_low_papr_dmrs(nv_blocks, nv_threads, shared_mem, stream, 
                    static_cast<const unsigned int*>(d_grid_cbf16),
                    d_low_papr_pilots,
                    d_lse_averaged,
                    d_partial_out,
                    cfg.nof_prb,
                    cfg.nof_rx_ports,
                    grid_stride,
                    symbol_stride,
                    cfg.start_prb,
                    dmrs_sym,
                    cfg.dmrs_scaling,
                    nof_dmrs_re_per_symbol);
            }
        }
    }

    /* Finalize noise variance (average across all DMRS symbols) */
    if (!use_low_papr) {
        launch_kernel_noise_variance_finalize(1, cfg.nof_rx_ports, 0, stream, 
            handle->d_noise_vars,
            handle->d_noise_var_partial,
            cfg.nof_rx_ports,
            nof_dmrs_symbols,
            nof_dmrs_re_per_symbol,
            cfg.dmrs_scaling);
    } else {
        launch_kernel_noise_variance_finalize_low_papr(1, cfg.nof_rx_ports, 0, stream, 
            handle->d_noise_vars,
            handle->d_noise_var_partial,
            cfg.nof_rx_ports,
            nof_dmrs_symbols,
            nof_dmrs_re_per_symbol);
    }

    /* ================================================================
     * Phase 2: Time interpolation to all data symbols
     * ================================================================ */

    int total_time = cfg.nof_symbols * cfg.nof_rx_ports * nof_re_per_symbol;
    int threads_ti = 256;
    int blocks_ti = (total_time + threads_ti - 1) / threads_ti;

    /* Scale factor for time interpolation.
     * We keep the full magnitude (1.0) for equalization stability, and compensate
     * for the scaling mismatch in the noise variance calculation instead.
     * This avoids reducing channel estimate SNR by N² which would hurt weak signals. */
    float time_interp_scale = 1.0f;

    launch_kernel_time_interpolate(blocks_ti, threads_ti, 0, stream, 
        d_freq_interp_estimates,
        handle->d_ch_estimates,
        d_dmrs_indices,
        nof_dmrs_symbols,
        cfg.nof_symbols,
        cfg.nof_rx_ports,
        nof_re_per_symbol,
        cfg.start_symbol,
        time_interp_scale);

    if (!run_demod) {
        err = cudaGetLastError();
        return (err == cudaSuccess) ? NR_LDPC_SUCCESS : NR_LDPC_ERROR_CUDA_FAILED;
    }

    /* ================================================================
     * Phase 3: Generate data scrambling sequence
     * ================================================================ */

    int num_llrs = nof_data_re * cfg.mod_order;
    status = scrambler_generate_sequence(handle->data_scrambler, num_llrs, stream);
    if (status != NR_LDPC_SUCCESS) {
        /* Pre-allocated, persist */
        /* Pre-allocated, persist */
        return status;
    }

    const unsigned int* d_scramble_seq = scrambler_get_sequence_ptr(handle->data_scrambler);

    /* ================================================================
     * Phase 4: Fused equalization + demodulation + descrambling
     *          (using GPU-computed noise variances!)
     * ================================================================ */

    int threads_e2e = 256;
    int blocks_e2e = (nof_data_re + threads_e2e - 1) / threads_e2e;

    /* Reset SINR accumulators before kernel launch */
    cudaMemsetAsync(handle->d_eq_noise_var_sum, 0, sizeof(float) + sizeof(unsigned int), stream);

    /* Dispatch kernel based on nof_rx_ports and equalizer_algorithm */
    {
        nr_ldpc_status_t _ls = launch_kernel_pusch_e2e_with_estimates(
            blocks_e2e, threads_e2e, 0, stream,
            cfg.nof_rx_ports, cfg.equalizer_algorithm,
            static_cast<const unsigned int*>(d_grid_cbf16),
            handle->d_ch_estimates,
            d_re_indices,
            handle->d_noise_vars,
            static_cast<__half*>(d_llrs_half),
            d_scramble_seq,
            handle->d_eq_noise_var_sum,
            handle->d_eq_noise_var_count,
            nof_data_re, cfg.mod_order, grid_stride, symbol_stride, nof_re_per_symbol,
            cfg.start_symbol, cfg.start_prb * 12, cfg.tx_scaling, false);
        if (_ls != NR_LDPC_SUCCESS) return _ls;
    }

    /* Cleanup temporary buffers */
    /* Pre-allocated, persist */
    /* Pre-allocated, persist */

    err = cudaGetLastError();
    return (err == cudaSuccess) ? NR_LDPC_SUCCESS : NR_LDPC_ERROR_CUDA_FAILED;
}

const float* pusch_e2e_get_noise_vars(pusch_e2e_handle_t handle)
{
    if (!handle) return nullptr;
    return handle->d_noise_vars;
}

void pusch_e2e_sinr_async_launch(pusch_e2e_handle_t handle, cudaStream_t stream)
{
    (void)pusch_e2e_sinr_async_launch_checked(handle, stream);
}

nr_ldpc_status_t pusch_e2e_sinr_async_launch_checked(pusch_e2e_handle_t handle, cudaStream_t stream)
{
    if (!handle || !handle->d_eq_noise_var_sum || !handle->d_eq_noise_var_count ||
        !handle->h_sinr_eq_block || !handle->h_sinr_noise_var_sum || !handle->h_sinr_count) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    // These are owned allocations with fixed contiguous accumulator layouts.
    // Check aliases before either copy; do not accept a valid-looking sum with
    // a stale/mismatched count or an unpaired pilot-result buffer.
    if (reinterpret_cast<uintptr_t>(handle->d_eq_noise_var_count) !=
            reinterpret_cast<uintptr_t>(handle->d_eq_noise_var_sum) + sizeof(float) ||
        handle->h_sinr_eq_block != handle->h_sinr_noise_var_sum ||
        reinterpret_cast<uintptr_t>(handle->h_sinr_count) !=
            reinterpret_cast<uintptr_t>(handle->h_sinr_noise_var_sum) + sizeof(float) ||
        (handle->d_sinr_epre_result == nullptr) != (handle->h_sinr_epre_result == nullptr)) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    if (handle->sinr_managed) {
        /* Host readback pointers alias managed device memory; results are directly
         * readable after the caller's completion sync. No copies to enqueue. */
        return handle->h_sinr_eq_block == handle->d_eq_noise_var_sum &&
               handle->h_sinr_count == handle->d_eq_noise_var_count &&
               handle->h_sinr_epre_result == handle->d_sinr_epre_result
                   ? NR_LDPC_SUCCESS : NR_LDPC_ERROR_INVALID_CONFIG;
    }

    /* Single 16-byte D2H copy for adjacent SINR and EVM accumulators in d_accum_block. */
    if (cudaMemcpyAsync(handle->h_sinr_eq_block, handle->d_eq_noise_var_sum,
                        2 * sizeof(float) + 2 * sizeof(unsigned int), cudaMemcpyDeviceToHost, stream) != cudaSuccess) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }

    /* Launch async D2H copy of DMRS-based SINR/EPRE/RSRP/TA/CFO result (20 bytes) */
    if (handle->d_sinr_epre_result && handle->h_sinr_epre_result) {
        if (cudaMemcpyAsync(handle->h_sinr_epre_result, handle->d_sinr_epre_result,
                            5 * sizeof(float), cudaMemcpyDeviceToHost, stream) != cudaSuccess) {
            return NR_LDPC_ERROR_CUDA_FAILED;
        }
    }
    return NR_LDPC_SUCCESS;
}

float pusch_e2e_sinr_get_result(pusch_e2e_handle_t handle)
{
    // Compatibility accessor for callers that explicitly accept mixed provenance.
    const float post_eq = pusch_e2e_post_eq_sinr_get_result(handle);
    return std::isfinite(post_eq) ? post_eq : pusch_e2e_channel_estimator_sinr_get_result(handle);
}

float pusch_e2e_post_eq_sinr_get_result(pusch_e2e_handle_t handle)
{
    if (handle && handle->h_sinr_noise_var_sum && handle->h_sinr_count) {
        return ocudu::detail::pusch_post_eq_sinr_from_accumulator(*handle->h_sinr_noise_var_sum,
                                                                 *handle->h_sinr_count);
    }
    return -INFINITY;
}

float pusch_e2e_channel_estimator_sinr_get_result(pusch_e2e_handle_t handle)
{
    // DM-RS pilot-ratio estimate. Its calibration limitations remain independent
    // of equalizer noise or decoding performance; this accessor does not fix them.
    if (handle && handle->h_sinr_epre_result) {
        float sinr_db = handle->h_sinr_epre_result[0];
        if (std::isfinite(sinr_db)) {
            return sinr_db;
        }
    }

    return -INFINITY;
}

float pusch_e2e_epre_get_result(pusch_e2e_handle_t handle)
{
    if (!handle || !handle->h_sinr_epre_result) {
        return -INFINITY;
    }
    return handle->h_sinr_epre_result[1];
}

float pusch_e2e_rsrp_get_result(pusch_e2e_handle_t handle)
{
    if (!handle || !handle->h_sinr_epre_result) {
        return -INFINITY;
    }
    return handle->h_sinr_epre_result[2];
}

float pusch_e2e_ta_get_result(pusch_e2e_handle_t handle)
{
    if (!handle || !handle->h_sinr_epre_result) {
        return NAN;
    }
    return handle->h_sinr_epre_result[3];
}

float pusch_e2e_cfo_get_result(pusch_e2e_handle_t handle)
{
    if (!handle || !handle->h_sinr_epre_result) {
        return NAN;
    }
    return handle->h_sinr_epre_result[4];
}

float pusch_e2e_evm_get_result(pusch_e2e_handle_t handle)
{
    if (!handle || !handle->config.enable_evm_metric || !handle->h_evm_error_sum || !handle->h_evm_symbol_count) {
        return NAN;
    }

    float error_sum = *handle->h_evm_error_sum;
    unsigned int count = *handle->h_evm_symbol_count;
    if (count == 0 || !std::isfinite(error_sum) || error_sum < 0.0f) {
        return NAN;
    }

    return std::sqrt(error_sum / static_cast<float>(count));
}

void pusch_e2e_print_diagnostics(pusch_e2e_handle_t handle, cudaStream_t stream)
{
    if (!handle || !handle->configured) {
        fprintf(stderr, "[GPU E2E DIAG] Handle not configured\n");
        return;
    }

    cudaStreamSynchronize(stream);

    const pusch_e2e_config_t& cfg = handle->config;

    /* Print configuration */
    fprintf(stderr, "[GPU E2E DIAG] Configuration:\n");
    fprintf(stderr, "  nof_prb=%d, start_prb=%d, nof_symbols=%d, start_symbol=%d\n",
            cfg.nof_prb, cfg.start_prb, cfg.nof_symbols, cfg.start_symbol);
    fprintf(stderr, "  grid: %dx%d, nof_ports=%d, dmrs_mask=0x%x\n",
            cfg.grid_nof_subcarriers, cfg.grid_nof_symbols, cfg.nof_rx_ports, cfg.dmrs_symbol_mask);
    fprintf(stderr, "  dmrs_scaling=%.4f, tx_scaling=%.4f\n", cfg.dmrs_scaling, cfg.tx_scaling);

    /* Compute and print scaling factor used for noise variance */
    int nof_dmrs_symbols = __builtin_popcount(cfg.dmrs_symbol_mask);
    int nof_dmrs_re_per_symbol = cfg.nof_prb * 6;  /* Type 1 DMRS */
    int total_dmrs_re = nof_dmrs_symbols * nof_dmrs_re_per_symbol;
    float N = (float)nof_dmrs_symbols;
    float base_scale = 4.0f / sqrtf(N);  /* 2x original (4.0 instead of 2.0) */
    float alloc_factor = (total_dmrs_re < 50) ? 1.2f : 1.0f;
    float effective_scale = base_scale * alloc_factor;
    fprintf(stderr, "  nof_dmrs_symbols=%d, nof_dmrs_re=%d, noise_scale=%.3f (base=%.3f, alloc=%.3f)\n",
            nof_dmrs_symbols, total_dmrs_re, effective_scale, base_scale, alloc_factor);

    /* Print raw LSE estimates from the FP16 buffer used by the E2E kernel. */
    if (handle->d_estimates_fp16 && nof_dmrs_re_per_symbol > 0) {
        /* FP16 buffer layout: [dmrs_sym, prb, port, 12 subcarriers] */
        int sample_count = std::min(12, cfg.nof_prb * 12);  /* First PRB worth of estimates */
        std::vector<__half2> h_lse_fp16(sample_count);
        cudaMemcpy(h_lse_fp16.data(), handle->d_estimates_fp16,
                   sample_count * sizeof(__half2), cudaMemcpyDeviceToHost);
        fprintf(stderr, "[GPU E2E DIAG] First %d FP16 channel estimates (DMRS sym 0, port 0):\n", sample_count);
        float sum_mag = 0.0f;
        for (int i = 0; i < sample_count; i++) {
            float h_real = __half2float(h_lse_fp16[i].x);
            float h_imag = __half2float(h_lse_fp16[i].y);
            float mag = sqrtf(h_real * h_real + h_imag * h_imag);
            sum_mag += mag;
            if (i < 6) {
                fprintf(stderr, "  [%d] (%.4f, %.4f) mag=%.4f\n", i, h_real, h_imag, mag);
            }
        }
        fprintf(stderr, "  fp16_avg_mag=%.4f (first %d samples)\n", sum_mag / sample_count, sample_count);
    }

    /* Get noise variances */
    if (handle->d_noise_vars) {
        std::vector<float> h_noise_vars(cfg.nof_rx_ports);
        cudaMemcpy(h_noise_vars.data(), handle->d_noise_vars,
                   cfg.nof_rx_ports * sizeof(float), cudaMemcpyDeviceToHost);
        fprintf(stderr, "[GPU E2E DIAG] Pre-EQ noise variances per port:\n");
        for (int p = 0; p < cfg.nof_rx_ports; p++) {
            fprintf(stderr, "  port %d: %.6f (%.2f dB)\n", p, h_noise_vars[p],
                    -10.0f * log10f(h_noise_vars[p]));
        }
    }

    /* Get post-EQ noise variance accumulators */
    if (handle->d_eq_noise_var_sum && handle->d_eq_noise_var_count) {
        float noise_var_sum = 0.0f;
        unsigned int count = 0;
        cudaMemcpy(&noise_var_sum, handle->d_eq_noise_var_sum, sizeof(float), cudaMemcpyDeviceToHost);
        cudaMemcpy(&count, handle->d_eq_noise_var_count, sizeof(unsigned int), cudaMemcpyDeviceToHost);
        fprintf(stderr, "[GPU E2E DIAG] Post-EQ: sum=%.6f, count=%u, mean=%.6f\n",
                noise_var_sum, count, count > 0 ? noise_var_sum / count : 0.0f);
    }

    /* Sample a few channel estimates to check magnitude */
    if (handle->d_ch_estimates && handle->nof_estimates > 0) {
        int sample_count = std::min(10, handle->nof_estimates);
        std::vector<cuFloatComplex> h_samples(sample_count);
        cudaMemcpy(h_samples.data(), handle->d_ch_estimates,
                   sample_count * sizeof(cuFloatComplex), cudaMemcpyDeviceToHost);
        fprintf(stderr, "[GPU E2E DIAG] First %d channel estimate samples:\n", sample_count);
        float sum_mag = 0.0f;
        for (int i = 0; i < sample_count; i++) {
            float mag = sqrtf(h_samples[i].x * h_samples[i].x + h_samples[i].y * h_samples[i].y);
            sum_mag += mag;
            fprintf(stderr, "  [%d] (%.4f, %.4f) mag=%.4f\n", i, h_samples[i].x, h_samples[i].y, mag);
        }
        fprintf(stderr, "  avg_mag=%.4f\n", sum_mag / sample_count);
    }

    /* For SMALL allocations only: sample equalized symbols by computing a few manually.
     * This helps inspect constellation quality. Only do for nof_prb <= 5 to limit output. */
    if (cfg.nof_prb <= 5 && handle->d_ch_estimates && handle->nof_estimates >= 12) {
        /* Sample first symbol's data (skip DMRS positions).
         * For Type 1 DMRS, data REs are at subcarriers 1,3,5,7,9,11 in DMRS symbols,
         * or all 12 in non-DMRS symbols. Symbol 0 is usually data if start_symbol=0. */
        int first_data_symbol = cfg.start_symbol;
        int sample_re_count = std::min(6, cfg.nof_prb * 12);

        std::vector<cuFloatComplex> h_first(sample_re_count);
        std::vector<float> h_grid(sample_re_count * 2);  /* cbf16 packed as uint32 */

        /* Get channel estimates for first symbol */
        cudaMemcpy(h_first.data(), handle->d_ch_estimates,
                   sample_re_count * sizeof(cuFloatComplex), cudaMemcpyDeviceToHost);

        fprintf(stderr, "[GPU E2E DIAG] First %d equalized symbol samples (manual compute):\n", sample_re_count);
        fprintf(stderr, "  nof_prb=%d, start_prb=%d, first_data_symbol=%d\n",
                cfg.nof_prb, cfg.start_prb, first_data_symbol);
    }
}

static uint64_t pack_deprecoding_fft_factors(int n, int* nof_factors)
{
    uint64_t packed = 0;
    int count = 0;
    auto push_factor = [&](int factor) {
        packed |= (static_cast<uint64_t>(factor) << (4 * count));
        ++count;
        n /= factor;
    };

    while ((n % 5) == 0) {
        push_factor(5);
    }
    while ((n % 4) == 0) {
        push_factor(4);
    }
    while ((n % 3) == 0) {
        push_factor(3);
    }
    while ((n % 2) == 0) {
        push_factor(2);
    }

    if (n != 1) {
        *nof_factors = 0;
        return 0;
    }
    *nof_factors = count;
    return packed;
}

#ifdef OCUDU_PHY_CUDA_ENABLE_VKFFT
static nr_ldpc_status_t launch_vkfft_deprecoder(
    pusch_e2e_handle_t handle,
    const void* d_grid_cbf16,
    void* d_llrs_half,
    const int* d_re_indices,
    int nof_data_re,
    int dft_size,
    int nof_ofdm_symbols,
    int grid_stride,
    int symbol_stride,
    int est_stride,
    int start_subcarrier,
    uint32_t scramble_c_init,
    cudaStream_t stream)
{
    if (!handle->vkfft_preplanned || handle->vkfft_stream != stream ||
        !handle->vkfft_plans[dft_size] || !handle->vkfft_plans[dft_size]->initialized) {
        if (handle->transform_deprecoder_backend == PUSCH_DEPRECODER_VKFFT) {
            return NR_LDPC_ERROR_INVALID_CONFIG;
        }
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    if (nof_ofdm_symbols > PUSCH_DEPRECODE_MAX_SYMBOLS ||
        static_cast<size_t>(nof_ofdm_symbols * dft_size) > handle->deprecode_symbols_capacity) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    const pusch_e2e_config_t& cfg = handle->config;
    int threads = min(dft_size, 256);
    size_t shared_mem_size = threads * (sizeof(float) + sizeof(unsigned int));

    {
        nr_ldpc_status_t _ls = launch_kernel_pusch_e2e_deprecode_equalize_gather(
            nof_ofdm_symbols, threads, shared_mem_size, stream,
            cfg.nof_rx_ports, cfg.equalizer_algorithm,
            static_cast<const unsigned int*>(d_grid_cbf16),
            handle->d_ch_estimates, d_re_indices, handle->d_noise_vars,
            handle->d_deprecode_symbols, handle->d_deprecode_noise_vars,
            handle->d_eq_noise_var_sum, handle->d_eq_noise_var_count,
            nof_data_re, grid_stride, symbol_stride, est_stride,
            cfg.start_symbol, start_subcarrier, cfg.tx_scaling, dft_size, nof_ofdm_symbols);
        if (_ls != NR_LDPC_SUCCESS) return _ls;
    }

    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }

    VkFFTLaunchParams launch_params = {};
    launch_params.buffer = &handle->vkfft_buffer_ptr;
    VkFFTResult fft_result = VkFFTAppend(&handle->vkfft_plans[dft_size]->app, 1, &launch_params);
    if (fft_result != VKFFT_SUCCESS) {
        return NR_LDPC_ERROR_CUDA_FAILED;
    }

    int demod_threads = 256;
    int demod_blocks = (nof_data_re + demod_threads - 1) / demod_threads;
    launch_kernel_pusch_e2e_deprecode_demod_descramble(demod_blocks, demod_threads, 0, stream, 
        handle->d_deprecode_symbols,
        handle->d_deprecode_noise_vars,
        static_cast<__half*>(d_llrs_half),
        scramble_c_init,
        nof_data_re,
        cfg.mod_order,
        dft_size);

    err = cudaGetLastError();
    return (err == cudaSuccess) ? NR_LDPC_SUCCESS : NR_LDPC_ERROR_CUDA_FAILED;
}
#endif

nr_ldpc_status_t pusch_e2e_process_full_gpu_with_deprecoding(
    pusch_e2e_handle_t handle,
    const void* d_grid_cbf16,
    void* d_llrs_half,
    const int* d_re_indices,
    int nof_data_re,
    int dft_size,
    int nof_ofdm_symbols,
    cudaStream_t stream)
{
    /* First run the CE/noise portion of the full GPU path. Transform mode
     * must not generate normal CP-OFDM LLRs only to overwrite them below. */
    nr_ldpc_status_t status = pusch_e2e_process_full_gpu_impl(
        handle, d_grid_cbf16, d_llrs_half, d_re_indices, nof_data_re, stream, false);

    if (status != NR_LDPC_SUCCESS) {
        return status;
    }

    /* Now launch the transform deprecoder. The previous call already set up:
     * - Channel estimates in handle->d_ch_estimates
     * - Noise variances in handle->d_noise_vars
     *
     * The transform kernel emits the final LLRs directly.
     */

    const pusch_e2e_config_t& cfg = handle->config;
    int grid_stride = cfg.grid_nof_symbols * cfg.grid_nof_subcarriers;
    int symbol_stride = cfg.grid_nof_subcarriers;
    int est_stride = cfg.nof_prb * 12;  /* nof_re_per_symbol */
    int start_subcarrier = cfg.start_prb * 12;
    if (dft_size < 0 || dft_size > PUSCH_DEPRECODE_MAX_DFT_SIZE) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    uint64_t packed_fft_factors = handle->deprecoding_fft_factors[dft_size];
    int nof_fft_factors = handle->deprecoding_fft_nof_factors[dft_size];
    if ((packed_fft_factors == 0) || (nof_fft_factors <= 0)) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    /* Match the main E2E data descrambler c_init exactly. Do not use
     * scrambler_get_c_init() here: scrambler_configure() masks n_ID to 10 bits
     * for legacy TB usage, while the resident PUSCH path uses the full
     * configured data n_id when generating the on-device sequence. */
    uint32_t scramble_c_init = (static_cast<uint32_t>(cfg.rnti) << 15) + static_cast<uint32_t>(cfg.n_id);

#ifdef OCUDU_PHY_CUDA_ENABLE_VKFFT
    bool try_vkfft =
        (handle->transform_deprecoder_backend == PUSCH_DEPRECODER_VKFFT) ||
        ((handle->transform_deprecoder_backend == PUSCH_DEPRECODER_AUTO) &&
         (dft_size >= handle->vkfft_auto_min_dft_size));
    if (try_vkfft) {
        nr_ldpc_status_t vkfft_status = launch_vkfft_deprecoder(handle,
                                                                d_grid_cbf16,
                                                                d_llrs_half,
                                                                d_re_indices,
                                                                nof_data_re,
                                                                dft_size,
                                                                nof_ofdm_symbols,
                                                                grid_stride,
                                                                symbol_stride,
                                                                est_stride,
                                                                start_subcarrier,
                                                                scramble_c_init,
                                                                stream);
        if (vkfft_status == NR_LDPC_SUCCESS ||
            handle->transform_deprecoder_backend == PUSCH_DEPRECODER_VKFFT) {
            return vkfft_status;
        }
    }
#else
    if (handle->transform_deprecoder_backend == PUSCH_DEPRECODER_VKFFT) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
#endif

    /* Launch one block per OFDM symbol */
    int threads = min(dft_size, 256);
    /* Shared memory: two complex work buffers plus per-thread noise reductions. */
    size_t shared_mem_size = 2 * dft_size * sizeof(cuFloatComplex) +
                             threads * (sizeof(float) + sizeof(unsigned int));

    /* Dispatch kernel based on nof_rx_ports and equalizer_algorithm */
    {
        nr_ldpc_status_t _ls = launch_kernel_pusch_e2e_with_estimates_deprecode(
            nof_ofdm_symbols, threads, shared_mem_size, stream,
            cfg.nof_rx_ports, cfg.equalizer_algorithm,
            static_cast<const unsigned int*>(d_grid_cbf16),
            handle->d_ch_estimates,
            d_re_indices,
            handle->d_noise_vars,
            static_cast<__half*>(d_llrs_half),
            scramble_c_init,
            handle->d_eq_noise_var_sum, handle->d_eq_noise_var_count,
            nof_data_re, cfg.mod_order, grid_stride, symbol_stride, est_stride,
            cfg.start_symbol, start_subcarrier, cfg.tx_scaling,
            dft_size, packed_fft_factors, nof_fft_factors, nof_ofdm_symbols);
        if (_ls != NR_LDPC_SUCCESS) return _ls;
    }

    cudaError_t err = cudaGetLastError();
    return (err == cudaSuccess) ? NR_LDPC_SUCCESS : NR_LDPC_ERROR_CUDA_FAILED;
}

nr_ldpc_status_t pusch_e2e_warmup_transform_deprecoding(pusch_e2e_handle_t handle,
                                                         cudaStream_t stream,
                                                         int nof_prb,
                                                         int nof_rx_ports)
{
    if (!handle) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    nof_prb = std::max(1, std::min(nof_prb, PUSCH_DEPRECODE_MAX_DFT_SIZE / 12));
    while (nof_prb > 1 && handle->deprecoding_fft_nof_factors[nof_prb * 12] <= 0) {
        --nof_prb;
    }
    if (handle->deprecoding_fft_nof_factors[nof_prb * 12] <= 0) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    if (nof_rx_ports <= 1) {
        nof_rx_ports = 1;
    } else if (nof_rx_ports <= 2) {
        nof_rx_ports = 2;
    } else if (nof_rx_ports <= 4) {
        nof_rx_ports = 4;
    } else {
        nof_rx_ports = 8;
    }

    const int nof_symbols = 14;
    const int dmrs_symbol_mask = 0x0004;
    const int dft_size = nof_prb * 12;
    const int nof_data_symbols = nof_symbols - 1;
    const int nof_data_re = nof_data_symbols * dft_size;

    pusch_e2e_config_t warmup_cfg = {};
    warmup_cfg.nof_prb = nof_prb;
    warmup_cfg.start_prb = 0;
    warmup_cfg.nof_symbols = nof_symbols;
    warmup_cfg.start_symbol = 0;
    warmup_cfg.dmrs_symbol_mask = dmrs_symbol_mask;
    warmup_cfg.dmrs_type = DMRS_TYPE_1;
    warmup_cfg.nof_cdm_groups_without_data = 2;
    warmup_cfg.mod_order = 2;
    warmup_cfg.nof_rx_ports = nof_rx_ports;
    warmup_cfg.nof_tx_layers = 1;
    warmup_cfg.n_id = 0;
    warmup_cfg.n_scid = 0;
    warmup_cfg.slot_idx = 0;
    warmup_cfg.dmrs_scaling = 1.0f;
    warmup_cfg.tx_scaling = 1.0f;
    warmup_cfg.grid_nof_subcarriers = dft_size;
    warmup_cfg.grid_nof_symbols = nof_symbols;
    warmup_cfg.equalizer_algorithm = EQUALIZER_MMSE;
    warmup_cfg.scrambling_id = 0;
    warmup_cfg.rnti = 0;
    warmup_cfg.scs_khz = 30;
    warmup_cfg.use_low_papr_dmrs = 1;
    warmup_cfg.n_rs_id = 0;
    warmup_cfg.enable_evm_metric = 0;

    (void)cudaGetLastError();
    nr_ldpc_status_t status = pusch_e2e_configure(handle, &warmup_cfg);
    if (status != NR_LDPC_SUCCESS) {
        handle->configured = false;
        return status;
    }

    size_t grid_size = (size_t)nof_rx_ports * nof_symbols * dft_size * sizeof(unsigned int);
    size_t re_indices_size = (size_t)nof_data_re * sizeof(int);
    size_t llrs_size = (size_t)nof_data_re * warmup_cfg.mod_order * sizeof(__half);

    void* d_grid = nullptr;
    int* d_re_indices = nullptr;
    void* d_llrs = nullptr;

    cudaError_t err = cudaMalloc(&d_grid, grid_size);
    if (err != cudaSuccess) {
        handle->configured = false;
        return NR_LDPC_ERROR_ALLOC_FAILED;
    }
    err = cudaMalloc(&d_re_indices, re_indices_size);
    if (err != cudaSuccess) {
        cudaFree(d_grid);
        handle->configured = false;
        return NR_LDPC_ERROR_ALLOC_FAILED;
    }
    err = cudaMalloc(&d_llrs, llrs_size);
    if (err != cudaSuccess) {
        cudaFree(d_re_indices);
        cudaFree(d_grid);
        handle->configured = false;
        return NR_LDPC_ERROR_ALLOC_FAILED;
    }

    cudaMemsetAsync(d_grid, 0, grid_size, stream);
    cudaMemsetAsync(d_re_indices, 0, re_indices_size, stream);
    cudaMemsetAsync(d_llrs, 0, llrs_size, stream);

    status = pusch_e2e_process_full_gpu_with_deprecoding(
        handle, d_grid, d_llrs, d_re_indices, nof_data_re, dft_size, nof_data_symbols, stream);
    cudaError_t sync_err = cudaStreamSynchronize(stream);

    cudaFree(d_llrs);
    cudaFree(d_re_indices);
    cudaFree(d_grid);
    handle->configured = false;

    if (status != NR_LDPC_SUCCESS) {
        (void)cudaGetLastError();
        return status;
    }
    if (sync_err != cudaSuccess) {
        (void)cudaGetLastError();
        return NR_LDPC_ERROR_CUDA_FAILED;
    }

    (void)cudaGetLastError();
    return NR_LDPC_SUCCESS;
}

static nr_ldpc_status_t pusch_e2e_process_optimized_impl(
    pusch_e2e_handle_t handle,
    const void* d_grid_cbf16,
    void* d_llrs_half,
    const int* d_re_indices,
    int nof_data_re,
    cudaStream_t stream,
    bool pilot_metrics_only,
    int erased_sc = -1,
    bool preserve_data_metrics = false)
{
    if (!handle || !handle->configured) return NR_LDPC_ERROR_INVALID_CONFIG;
    if (!d_grid_cbf16 || (!pilot_metrics_only && (!d_llrs_half || !d_re_indices))) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    const pusch_e2e_config_t& cfg = handle->config;

    /* MIMO support: 1, 2, or 4 layers are supported.
     * Layer count must not exceed port count. */
    if (cfg.nof_tx_layers > cfg.nof_rx_ports) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    if (cfg.nof_tx_layers != 1 && cfg.nof_tx_layers != 2 && cfg.nof_tx_layers != 4) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    /* Calculate dimensions */
    int nof_re_per_symbol = cfg.nof_prb * 12;
    int grid_stride = cfg.grid_nof_symbols * cfg.grid_nof_subcarriers;
    int symbol_stride = cfg.grid_nof_subcarriers;

    int dmrs_symbol_indices[MAX_DMRS_SYMBOLS];
    int nof_dmrs_symbols = pusch_e2e_get_dmrs_symbol_indices_checked(cfg.dmrs_symbol_mask, dmrs_symbol_indices);
    if (nof_dmrs_symbols <= 0) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    cudaError_t err;

    /* ================================================================
     * ULTRA-OPTIMIZED PATH: Fused LSE+NoiseVar + On-the-fly Scrambling
     *
     * This path eliminates:
     * - Separate noise variance kernel (fused into LSE)
     * - Data scrambling sequence generation kernel (on-the-fly in E2E)
     * - DMRS received buffer (noise computed inline)
     *
     * Kernel launches: DMRS gen → Fused LSE+Noise → Finalize → E2E
     * (4 kernels vs 5 in previous optimized path)
     * ================================================================ */

    /* Allocate FP16 estimate buffer [dmrs_sym, prb, port, layer, 12] for MIMO */
    int nof_layers_fused = (cfg.nof_tx_layers > 0) ? cfg.nof_tx_layers : 1;
    size_t fp16_est_size = nof_dmrs_symbols * nof_re_per_symbol * cfg.nof_rx_ports * nof_layers_fused * sizeof(__half2);
    if (fp16_est_size > handle->estimates_fp16_capacity) {
        if (handle->d_estimates_fp16) cudaFree(handle->d_estimates_fp16);
        err = cudaMalloc(&handle->d_estimates_fp16, fp16_est_size);
        if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
        handle->estimates_fp16_capacity = fp16_est_size;
    }

    /* Allocate noise variance buffer (reused as accumulator) */
    size_t noise_size = cfg.nof_rx_ports * sizeof(float);
    if (noise_size > handle->noise_vars_capacity) {
        if (handle->d_noise_vars) cudaFree(handle->d_noise_vars);
        err = cudaMalloc(&handle->d_noise_vars, noise_size);
        if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
        handle->noise_vars_capacity = noise_size;
    }

    /* d_noise_count is now part of d_accum_block (allocated in pusch_e2e_create) */

    /* Upload DMRS symbol indices to device (cached — skip if mask unchanged) */
    size_t dmrs_indices_needed = nof_dmrs_symbols * sizeof(int);
    if (dmrs_indices_needed > handle->dmrs_indices_capacity) {
        if (handle->d_dmrs_indices) cudaFree(handle->d_dmrs_indices);
        err = cudaMalloc(&handle->d_dmrs_indices, dmrs_indices_needed);
        if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
        handle->dmrs_indices_capacity = dmrs_indices_needed;
        handle->cached_dmrs_symbol_mask = -1;  /* Force re-upload after realloc */
    }
    if (!pilot_metrics_only && cfg.dmrs_symbol_mask != handle->cached_dmrs_symbol_mask) {
        cudaMemcpyAsync(handle->d_dmrs_indices, dmrs_symbol_indices,
                        nof_dmrs_symbols * sizeof(int), cudaMemcpyHostToDevice, stream);
        handle->cached_dmrs_symbol_mask = cfg.dmrs_symbol_mask;
        handle->cached_nof_dmrs_symbols = nof_dmrs_symbols;
    }

    /* ================================================================
     * ULTRA-OPTIMIZED: On-the-fly DMRS + Fused LSE + Noise Variance
     *
     * This eliminates the separate DMRS sequence generation kernel!
     * DMRS pilots are computed on-the-fly using LFSR advancement.
     *
     * Kernel launches: Ultra-Fused LSE → Finalize → E2E (3 kernels!)
     * ================================================================ */
    /* Zero ALL per-iteration accumulators in a single memset (140 bytes).
     * Covers: d_noise_count, d_rsrp_accum, d_epre_accum, d_sinr_noise_accum,
     * d_sinr_rsrp_accum, d_eq_noise_var_sum, d_eq_noise_var_count.
     * d_noise_vars is NOT in this block — it needs a separate mid-processing re-zero
     * before the CV kernel (line ~9955). */
    if (preserve_data_metrics) {
        // Admission has checked this owned contiguous layout. Preserve only
        // [post-EQ sum/count, EVM sum/count]; never reuse pilot accumulators.
        constexpr size_t data_begin = sizeof(unsigned) + 4 * 8 * sizeof(float);
        constexpr size_t data_end = data_begin + 2 * (sizeof(float) + sizeof(unsigned));
        err = cudaMemsetAsync(handle->d_accum_block, 0, data_begin, stream);
        if (err != cudaSuccess) return NR_LDPC_ERROR_CUDA_FAILED;
        err = cudaMemsetAsync(static_cast<char*>(handle->d_accum_block) + data_end, 0,
                              handle->accum_block_size - data_end, stream);
    } else {
        err = cudaMemsetAsync(handle->d_accum_block, 0, handle->accum_block_size, stream);
    }
    if (pilot_metrics_only && err != cudaSuccess) return NR_LDPC_ERROR_CUDA_FAILED;

    /* Allocate d_dmrs_c_inits buffer if needed (small: MAX_DMRS_SYMBOLS × 4 bytes) */
    if (!handle->d_dmrs_c_inits) {
        cudaMalloc(&handle->d_dmrs_c_inits, MAX_DMRS_SYMBOLS * sizeof(uint32_t));
    }

    {
        int slot_in_frame = cfg.slot_idx % MAX_SLOTS_PER_FRAME;

        /* Compute and upload c_init values for all DMRS symbols */
        uint32_t h_dmrs_c_inits[MAX_DMRS_SYMBOLS];
        for (int d = 0; d < nof_dmrs_symbols; d++) {
            int dmrs_sym = dmrs_symbol_indices[d];
            h_dmrs_c_inits[d] = dmrs_compute_c_init(slot_in_frame, dmrs_sym, cfg.scrambling_id, cfg.n_scid);
        }
        if (pilot_metrics_only) {
            launch_kernel_prepare_pilot_metrics(stream, handle->d_dmrs_indices, handle->d_dmrs_c_inits,
                dmrs_symbol_indices[0], dmrs_symbol_indices[1], h_dmrs_c_inits[0], h_dmrs_c_inits[1]);
            if (cudaGetLastError() != cudaSuccess) return NR_LDPC_ERROR_CUDA_FAILED;
            handle->cached_dmrs_symbol_mask = cfg.dmrs_symbol_mask;
            handle->cached_nof_dmrs_symbols = nof_dmrs_symbols;
        } else {
            cudaMemcpyAsync(handle->d_dmrs_c_inits, h_dmrs_c_inits,
                         nof_dmrs_symbols * sizeof(uint32_t),
                         cudaMemcpyHostToDevice, stream);
        }

        /* Single 2D launch for all DMRS symbols — eliminates per-symbol launch overhead */
        dim3 lse_grid(cfg.nof_prb, nof_dmrs_symbols);
        launch_kernel_fused_lse_freq_interp_noise_fp16(lse_grid, cfg.nof_rx_ports, 0, stream, 
            static_cast<const unsigned int*>(d_grid_cbf16),
            handle->d_dmrs_c_inits,
            handle->d_dmrs_indices,
            handle->d_estimates_fp16,
            handle->d_noise_vars,
            handle->d_noise_count,
            handle->d_rsrp_accum,
            handle->d_epre_accum,
            cfg.nof_prb,
            cfg.nof_rx_ports,
            nof_dmrs_symbols,
            grid_stride,
            symbol_stride,
            cfg.start_prb,
            cfg.dmrs_scaling,
            (int)cfg.dmrs_type,
            erased_sc);
    }

    /* ================================================================
     * Phase 1b: Frequency-domain smoothing
     *
     * Apply raised cosine FIR filter across frequency to denoise the
     * LSE channel estimates. Operates on pilot positions and
     * re-interpolates to all 12 subcarriers. Must run before noise
     * estimation so cross-validation sees smoothed estimates.
     * ================================================================ */
    if (cfg.nof_prb > 1) {
        dim3 smooth_grid(cfg.nof_prb, nof_dmrs_symbols);
        launch_kernel_fd_smoothing_fp16(smooth_grid, cfg.nof_rx_ports, 0, stream, 
            handle->d_estimates_fp16,
            cfg.nof_prb,
            cfg.nof_rx_ports,
            nof_dmrs_symbols,
            (int)cfg.dmrs_type,
            erased_sc);
    }

    /* ================================================================
     * Phase 2: Cross-validation noise estimation + fused SINR/EPRE
     *
     * The LSE residuals give near-zero noise because h = y / pilot, so
     * |y - h*pilot| ≈ 0. Use cross-validation across DMRS symbols instead:
     * variance of h estimates gives true noise variance.
     *
     * SINR residual computation is fused into the CV kernel at pilot
     * positions (sc % 2 == 0) to eliminate a separate SINR kernel launch.
     * ================================================================ */

    /* Resolve pilot bits for fused SINR (needed before CV kernel).
     * With all-14-symbols precomputation, no mask matching is needed — just
     * gather the DMRS symbols for this slot's mask via D2D copies. */
    int sinr_words_per_dmrs_sym = 0;
    uint32_t* sinr_pilot_bits = nullptr;
    if (handle->d_sinr_epre_result && handle->d_epre_accum && handle->d_sinr_noise_accum) {
        int sinr_slot_in_frame = cfg.slot_idx % MAX_SLOTS_PER_FRAME;
        int sinr_max_pilot_prb = cfg.start_prb + cfg.nof_prb;
        int sinr_precomputed_max_prb = handle->precomputed_pilots_per_symbol / dmrs_get_re_per_prb(cfg.dmrs_type);
        bool sinr_can_precompute = handle->dmrs_pilots_precomputed &&
                                   sinr_slot_in_frame < handle->precomputed_nof_slots &&
                                   sinr_max_pilot_prb <= sinr_precomputed_max_prb &&
                                   handle->d_dmrs_pilot_bits;

        if (sinr_can_precompute) {
            /* Gather needed DMRS symbols from all-14 layout via single kernel launch */
            sinr_words_per_dmrs_sym = handle->precomputed_words_per_sym;
            {
                const uint32_t* slot_base = handle->d_precomputed_dmrs_pilots +
                                            sinr_slot_in_frame * handle->precomputed_words_per_slot;
                int gather_threads = ((sinr_words_per_dmrs_sym + 31) / 32) * 32;
                if (gather_threads < 32) gather_threads = 32;
                launch_kernel_gather_dmrs_pilot_bits(nof_dmrs_symbols, gather_threads, 0, stream, 
                    handle->d_dmrs_pilot_bits, slot_base, handle->d_dmrs_indices,
                    sinr_words_per_dmrs_sym);
            }
            sinr_pilot_bits = handle->d_dmrs_pilot_bits;
        } else if (handle->d_dmrs_c_inits) {
            /* Fallback: generate pilots via kernel (should not happen in normal operation) */
            int nof_pilots_per_sym = cfg.nof_prb * dmrs_get_re_per_prb(cfg.dmrs_type);
            int nof_pilot_words = (nof_pilots_per_sym * nof_dmrs_symbols * 2 + 31) / 32;
            size_t pilot_bits_size = nof_pilot_words * sizeof(uint32_t);
            if (pilot_bits_size > handle->dmrs_pilot_bits_capacity) {
                if (handle->d_dmrs_pilot_bits) cudaFree(handle->d_dmrs_pilot_bits);
                err = cudaMalloc(&handle->d_dmrs_pilot_bits, pilot_bits_size);
                if (err == cudaSuccess) {
                    handle->dmrs_pilot_bits_capacity = pilot_bits_size;
                } else {
                    handle->d_dmrs_pilot_bits = nullptr;
                    handle->dmrs_pilot_bits_capacity = 0;
                }
            }
            if (handle->d_dmrs_pilot_bits) {
                sinr_words_per_dmrs_sym = (nof_pilots_per_sym * 2 + 31) / 32;
                for (int d = 0; d < nof_dmrs_symbols; d++) {
                    int sinr_slot_for_dmrs = cfg.slot_idx % MAX_SLOTS_PER_FRAME;
                    uint64_t term1 = (1ULL << 17) * (14ULL * sinr_slot_for_dmrs + dmrs_symbol_indices[d] + 1) * (2ULL * cfg.scrambling_id + 1);
                    uint64_t term2 = 2ULL * cfg.scrambling_id + cfg.n_scid;
                    uint32_t c_init = static_cast<uint32_t>((term1 + term2) & 0x7FFFFFFF);
                    int nof_threads_needed = (nof_pilots_per_sym + 31) / 32;
                    int pilot_gen_threads = 256;
                    int pilot_gen_blocks = (nof_threads_needed + pilot_gen_threads - 1) / pilot_gen_threads;
                    uint32_t* pilot_bits_offset = handle->d_dmrs_pilot_bits + d * sinr_words_per_dmrs_sym;
                    launch_kernel_generate_dmrs_pilots(pilot_gen_blocks, pilot_gen_threads, 0, stream, 
                        pilot_bits_offset, c_init, nof_pilots_per_sym);
                }
                sinr_pilot_bits = handle->d_dmrs_pilot_bits;
            }
        }
    }

    /* The normal two-DMRS CSI path overwrites all five result fields:
     * SINR/EPRE/RSRP in the CV/finalize kernel and TA/CFO in
     * kernel_compute_ta_and_cfo. Keep the reset for reduced metrics paths
     * where some fields may otherwise retain stale values. */
    const bool sinr_epre_result_fully_overwritten =
        sinr_pilot_bits && cfg.scs_khz > 0 && nof_dmrs_symbols >= 2;
    static const bool skip_redundant_result_reset =
        !pusch_e2e_env_flag_disabled("OCUDU_PUSCH_SKIP_REDUNDANT_RESULT_RESET");
    if (handle->d_sinr_epre_result &&
        !(skip_redundant_result_reset && sinr_epre_result_fully_overwritten)) {
        launch_kernel_reset_sinr_epre_result(1, 1, 0, stream, handle->d_sinr_epre_result);
    }

    static const bool enable_cfo_phasors =
        !pusch_e2e_env_flag_disabled("OCUDU_PUSCH_CFO_PHASORS");
    const bool compute_ta_cfo = sinr_pilot_bits && handle->d_sinr_epre_result && cfg.scs_khz > 0;
    const bool use_cfo_phasors =
        compute_ta_cfo && enable_cfo_phasors && cfg.compensate_cfo && nof_dmrs_symbols >= 2 &&
        handle->d_mimo_cfo_phasors != nullptr && handle->d_symbol_start_times != nullptr;
    float2* cfo_phasors = use_cfo_phasors ? handle->d_mimo_cfo_phasors : nullptr;

    /* TA + CFO via fused phase estimation kernel — launched BEFORE CV noise
     * so that CFO estimate (d_sinr_epre_result[4]) is available for coherent
     * de-rotation in kernel_cross_validation_noise_variance. */
    if (compute_ta_cfo) {
        int pilot_spacing = (cfg.dmrs_type == DMRS_TYPE_1) ? 2 : 3;
        float pilot_spacing_hz = (float)pilot_spacing * (float)cfg.scs_khz * 1000.0f;
        int nof_pilots = cfg.nof_prb * dmrs_get_re_per_prb(cfg.dmrs_type);
        /* 3 pilot arrays × nof_pilots × 2 floats + 8 warps × 4 accumulators for reduction */
        size_t ta_cfo_smem = nof_pilots * 6 * sizeof(float) + 8 * 4 * sizeof(float);

        /* Compute time gap between first and last DMRS symbols (for CFO).
         * Matches CPU port_channel_estimator_average_impl::initialize_symbol_start_epochs():
         *   epoch[j] - epoch[j-1] = cp_j_seconds * SCS_Hz + 1.0  (in SCS periods)
         *   delta_t = (epoch[last] - epoch[first]) / SCS_Hz       (in seconds)
         * Normal CP: cp_len_kappa(j) = (144 >> mu) for regular, + 16 for j=0 or j=7*2^mu.
         * cp_seconds = cp_len_kappa * kappa * T_c = cp_len_kappa / 30720000. */
        float* d_cfo_ptr = nullptr;
        float delta_t_s = 0.0f;
        if (nof_dmrs_symbols >= 2) {
            int mu = 0;
            if (cfg.scs_khz == 30) mu = 1;
            else if (cfg.scs_khz == 60) mu = 2;
            else if (cfg.scs_khz == 120) mu = 3;
            float scs_hz = (float)cfg.scs_khz * 1000.0f;
            int half_slot_sym = 7 * (1 << mu);  /* Extended CP symbol index */
            for (int j = dmrs_symbol_indices[0] + 1; j <= dmrs_symbol_indices[nof_dmrs_symbols - 1]; j++) {
                int cp_kappa = 144 >> mu;
                if (j == 0 || j == half_slot_sym) cp_kappa += 16;
                float cp_s = (float)cp_kappa / 30720000.0f;
                delta_t_s += cp_s + 1.0f / scs_hz;
            }
            d_cfo_ptr = handle->d_sinr_epre_result + 4;
        }

        /* d_ta_cfo_accum already zeroed by d_accum_block memset above.
         * Launch one block per port for parallel execution across SMs. */
        launch_kernel_compute_ta_and_cfo(cfg.nof_rx_ports, 256, ta_cfo_smem, stream, 
            handle->d_estimates_fp16,
            handle->d_sinr_epre_result + 3,   /* TA output */
            d_cfo_ptr,                         /* CFO output (NULL if < 2 DMRS) */
            handle->d_ta_cfo_accum,            /* cross-port accumulators */
            cfg.nof_prb, cfg.nof_rx_ports, nof_dmrs_symbols,
            pilot_spacing_hz,
            delta_t_s,
            handle->d_symbol_start_times,
            handle->d_dmrs_indices,
            cfo_phasors,
            cfg.start_symbol,
            (int)cfg.dmrs_type);
    }

    /* d_cv_noise_accum and d_cv_done_counter are already zeroed by the d_accum_block
     * memset above — no separate re-zero of d_noise_vars needed for the CV path. */

    if (nof_dmrs_symbols >= 2) {
        int cv_threads = 12 * cfg.nof_rx_ports;
        int cv_blocks = cfg.nof_prb;

        const float* cv_cfo_ptr = (cfg.compensate_cfo && nof_dmrs_symbols >= 2) ? (handle->d_sinr_epre_result + 4) : nullptr;

        /* Fused CV + finalize: last block normalizes noise and computes SINR/EPRE.
         * Eliminates separate cudaMemsetAsync + kernel_finalize_cross_validation_noise launch. */
        launch_kernel_cross_validation_noise_variance(cv_blocks, cv_threads, 0, stream, 
            handle->d_estimates_fp16,
            handle->d_cv_noise_accum,
            static_cast<const unsigned int*>(d_grid_cbf16),
            sinr_pilot_bits,
            handle->d_dmrs_indices,
            sinr_pilot_bits ? handle->d_sinr_noise_accum : nullptr,
            sinr_pilot_bits ? handle->d_sinr_rsrp_accum : nullptr,
            cfg.nof_prb,
            cfg.nof_rx_ports,
            nof_dmrs_symbols,
            grid_stride,
            symbol_stride,
            cfg.start_prb,
            cfg.dmrs_scaling,
            sinr_words_per_dmrs_sym,
            cv_cfo_ptr,
            handle->d_symbol_start_times,
            cfo_phasors,
            /* Fused finalize params */
            handle->d_noise_vars,
            handle->d_cv_done_counter,
            sinr_pilot_bits ? handle->d_epre_accum : nullptr,
            sinr_pilot_bits ? handle->d_sinr_epre_result : nullptr,
            cfg.noise_mode,
            (int)cfg.dmrs_type,
            erased_sc);
    } else {
        /* Single DMRS symbol — cannot cross-validate across time. Match the CPU
         * estimator by computing pilot-residual noise from the smoothed channel
         * estimates instead of using the raw LSE self-residual/floor path. */
        if (sinr_pilot_bits) {
            dim3 residual_grid(cfg.nof_prb);
            int residual_threads = cfg.nof_rx_ports * dmrs_get_re_per_prb(cfg.dmrs_type);
            launch_kernel_compute_residual_sinr_epre_v3(residual_grid, residual_threads, 0, stream, 
                static_cast<const unsigned int*>(d_grid_cbf16),
                handle->d_estimates_fp16,
                sinr_pilot_bits,
                handle->d_dmrs_indices,
                handle->d_sinr_noise_accum,
                handle->d_sinr_rsrp_accum,
                cfg.nof_prb,
                cfg.nof_rx_ports,
                nof_dmrs_symbols,
                grid_stride,
                symbol_stride,
                cfg.start_prb,
                cfg.dmrs_scaling,
                sinr_words_per_dmrs_sym,
                (int)cfg.dmrs_type);
            launch_kernel_finalize_sinr_epre(1, 1, 0, stream, 
                handle->d_sinr_noise_accum,
                handle->d_sinr_rsrp_accum,
                handle->d_epre_accum,
                handle->d_noise_vars,
                handle->d_sinr_epre_result,
                cfg.nof_prb,
                cfg.nof_rx_ports,
                nof_dmrs_symbols,
                cfg.dmrs_scaling,
                (int)cfg.dmrs_type);
        } else {
            cudaMemsetAsync(handle->d_noise_vars, 0, cfg.nof_rx_ports * sizeof(float), stream);
            launch_kernel_finalize_noise_variance(1, cfg.nof_rx_ports, 0, stream, 
                handle->d_noise_vars,
                handle->d_noise_count,
                cfg.nof_rx_ports,
                cfg.dmrs_scaling,
                nof_dmrs_symbols);
        }
    }

    if (pilot_metrics_only) {
        // No data-RE outputs are touched. Accumulators were cleared above, so
        // a pilot-only transaction cannot resurrect old post-EQ SINR or EVM.
        return cudaGetLastError() == cudaSuccess ? NR_LDPC_SUCCESS : NR_LDPC_ERROR_CUDA_FAILED;
    }

    /* ================================================================
     * Phase 3: OPTIMIZED E2E kernel with WARP-COOPERATIVE scrambling
     *
     * Optimizations over previous version:
     * 1. Warp-cooperative scrambling: Lane 0 generates bits for 32 threads
     * 2. Block-level noise variance reduction (fewer global atomics)
     * 3. Template on NOF_DMRS for compile-time loop unrolling
     * 4. Vectorized half2 LLR writes
     * ================================================================ */
    /* Compute data scrambling c_init per TS 38.211 section 6.3.1.1:
     * c_init = n_RNTI * 2^15 + q * 2^14 + n_ID
     * Note: Slot index is NOT part of PUSCH/PDSCH data scrambling c_init.
     * Slot is only used for DMRS sequence generation (different section). */
    uint32_t data_c_init = (cfg.rnti << 15) + (0 << 14) + cfg.n_id;
    int nof_data_bits = nof_data_re * cfg.mod_order;
    static const bool enable_scramble_cache =
        !pusch_e2e_env_flag_disabled("OCUDU_PUSCH_SCRAMBLE_CACHE");
    static const unsigned scramble_cache_min_re =
        pusch_e2e_env_unsigned("OCUDU_PUSCH_SCRAMBLE_CACHE_MIN_RE", 10000);
    const uint32_t* data_scramble_seq =
        (enable_scramble_cache && nof_data_re >= static_cast<int>(scramble_cache_min_re) &&
         handle->d_scrambling_seq &&
         handle->cached_data_scrambling_c_init == data_c_init &&
         handle->cached_data_scrambling_bits >= nof_data_bits) ? handle->d_scrambling_seq : nullptr;

    int threads_e2e = 256;
    int blocks_e2e = (nof_data_re + threads_e2e - 1) / threads_e2e;

    /* d_eq_noise_var_sum and d_eq_noise_var_count already zeroed by d_accum_block memset */
    const float* e2e_cfo_ptr =
        (cfg.compensate_cfo && nof_dmrs_symbols >= 2) ? (handle->d_sinr_epre_result + 4) : nullptr;
    const float2* e2e_cfo_phasors = (e2e_cfo_ptr != nullptr) ? cfo_phasors : nullptr;

    /* Use optimized warp-cooperative kernel with templated DMRS count and time interp mode.
     * TIME_AVG=true: average all DMRS estimates (default, matches CPU).
     * TIME_AVG=false: linear interpolation between bracketing DMRS symbols. */
    {
        nr_ldpc_status_t _ls = launch_kernel_fused_e2e_warp_cooperative_fp16(
            blocks_e2e, threads_e2e, 0, stream,
            cfg.nof_rx_ports, cfg.equalizer_algorithm, nof_dmrs_symbols, cfg.mod_order, cfg.time_interp_mode,
            static_cast<const unsigned int*>(d_grid_cbf16),
            handle->d_estimates_fp16,
            d_re_indices,
            handle->d_noise_vars,
            static_cast<__half*>(d_llrs_half),
            data_scramble_seq,
            data_c_init,
            handle->d_eq_noise_var_sum,
            handle->d_eq_noise_var_count,
            cfg.enable_evm_metric ? handle->d_evm_error_sum : nullptr,
            cfg.enable_evm_metric ? handle->d_evm_symbol_count : nullptr,
            nof_data_re, cfg.nof_prb,
            grid_stride, symbol_stride, cfg.start_symbol, cfg.start_prb * 12, cfg.tx_scaling,
            handle->d_dmrs_indices,
            e2e_cfo_ptr,
            handle->d_symbol_start_times,
            e2e_cfo_phasors,
            handle->d_eq_symbols_out,
            handle->d_eq_noise_var_out);
        if (_ls != NR_LDPC_SUCCESS) return _ls;
    }

    err = cudaGetLastError();
    return (err == cudaSuccess) ? NR_LDPC_SUCCESS : NR_LDPC_ERROR_CUDA_FAILED;
}

nr_ldpc_status_t pusch_e2e_process_full_gpu_optimized(
    pusch_e2e_handle_t handle, const void* d_grid_cbf16, void* d_llrs_half,
    const int* d_re_indices, int nof_data_re, cudaStream_t stream)
{
    return pusch_e2e_process_optimized_impl(handle, d_grid_cbf16, d_llrs_half, d_re_indices,
                                           nof_data_re, stream, false);
}

nr_ldpc_status_t pusch_e2e_process_pilot_metrics(pusch_e2e_handle_t handle,
                                              const void* d_grid_cbf16, cudaStream_t stream)
{
    return pusch_e2e_process_pilot_metrics_with_dc(handle, d_grid_cbf16, -1, stream);
}

nr_ldpc_status_t pusch_e2e_process_pilot_metrics_with_dc(pusch_e2e_handle_t handle,
                                                      const void* d_grid_cbf16,
                                                      int dc_subcarrier, cudaStream_t stream)
{
    if (pusch_e2e_validate_pilot_metrics(handle, d_grid_cbf16, dc_subcarrier) != NR_LDPC_SUCCESS)
        return NR_LDPC_ERROR_INVALID_CONFIG;
    const auto& cfg = handle->config;
    const int erased_sc = ocudu::cuda::pilot_mask::erased_type1_pilot(dc_subcarrier,
                                                                   cfg.start_prb * 12, cfg.nof_prb * 12);
    return pusch_e2e_process_optimized_impl(handle, d_grid_cbf16, nullptr, nullptr, 0, stream, true, erased_sc);
}

nr_ldpc_status_t pusch_e2e_refresh_pilot_metrics_with_dc(pusch_e2e_handle_t handle,
                                                      const void* d_grid_cbf16,
                                                      int dc_subcarrier, cudaStream_t stream)
{
    if (pusch_e2e_validate_pilot_metrics(handle, d_grid_cbf16, dc_subcarrier) != NR_LDPC_SUCCESS)
        return NR_LDPC_ERROR_INVALID_CONFIG;
    const uintptr_t base = reinterpret_cast<uintptr_t>(handle->d_accum_block);
    constexpr size_t data_begin = sizeof(unsigned) + 4 * 8 * sizeof(float);
    // A corrupt layout must not turn the preserving reset into an out-of-bounds
    // write or accidentally clear/read an unrelated allocation.
    if (handle->accum_block_size != 208 || base > UINTPTR_MAX - handle->accum_block_size ||
        reinterpret_cast<uintptr_t>(handle->d_eq_noise_var_sum) != base + data_begin ||
        reinterpret_cast<uintptr_t>(handle->d_eq_noise_var_count) != base + data_begin + sizeof(float) ||
        reinterpret_cast<uintptr_t>(handle->d_evm_error_sum) != base + data_begin + sizeof(float) + sizeof(unsigned) ||
        reinterpret_cast<uintptr_t>(handle->d_evm_symbol_count) != base + data_begin + 2 * sizeof(float) + sizeof(unsigned) ||
        reinterpret_cast<uintptr_t>(handle->d_ta_cfo_accum) != base + data_begin + 2 * (sizeof(float) + sizeof(unsigned)))
        return NR_LDPC_ERROR_INVALID_CONFIG;
    const auto& cfg = handle->config;
    const int erased_sc = ocudu::cuda::pilot_mask::erased_type1_pilot(dc_subcarrier,
                                                                   cfg.start_prb * 12, cfg.nof_prb * 12);
    return pusch_e2e_process_optimized_impl(handle, d_grid_cbf16, nullptr, nullptr, 0, stream, true, erased_sc, true);
}

nr_ldpc_status_t pusch_e2e_validate_pilot_metrics(pusch_e2e_handle_t handle,
                                                const void* d_grid_cbf16,
                                                int dc_subcarrier)
{
    // All admission checks precede the first CUDA call or handle mutation.
    if (!handle || !handle->configured || !d_grid_cbf16 ||
        reinterpret_cast<uintptr_t>(d_grid_cbf16) % alignof(uint32_t) != 0) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    const auto& cfg = handle->config;
    // The host resource grid may span 275 RB (3300 subcarriers), independently
    // of this backend's 273-RB grant/pilot-cache capacity. Keep those bounds
    // separate: later checks still require the complete allocation to fit the
    // prepared pilots. Never reject a small supported grant just because its
    // producer supplies the full legal host grid.
    constexpr int max_input_grid_subcarriers = 275 * 12;
    int symbols[MAX_DMRS_SYMBOLS];
    const int count = pusch_e2e_get_dmrs_symbol_indices_checked(cfg.dmrs_symbol_mask, symbols);
    if (cfg.nof_rx_ports != 1 || cfg.nof_tx_layers != 1 || cfg.use_low_papr_dmrs != 0 ||
        cfg.dmrs_type != DMRS_TYPE_1 || cfg.scs_khz != 30 || count != 2 ||
        cfg.grid_nof_symbols != MAX_SYMBOLS || cfg.grid_nof_subcarriers < 12 ||
        cfg.grid_nof_subcarriers > max_input_grid_subcarriers || cfg.grid_nof_subcarriers % 12 != 0 ||
        dc_subcarrier < -1 || dc_subcarrier >= cfg.grid_nof_subcarriers ||
        cfg.nof_prb < 1 || cfg.nof_prb > MAX_PRB || cfg.start_prb < 0 ||
        cfg.start_prb > cfg.grid_nof_subcarriers / 12 - cfg.nof_prb ||
        cfg.start_symbol < 0 || cfg.start_symbol >= MAX_SYMBOLS || cfg.nof_symbols < 1 ||
        cfg.nof_symbols > MAX_SYMBOLS - cfg.start_symbol || symbols[0] < cfg.start_symbol ||
        symbols[1] >= cfg.start_symbol + cfg.nof_symbols || cfg.slot_idx < 0 ||
        !std::isfinite(cfg.dmrs_scaling) || cfg.dmrs_scaling <= 0 ||
        cfg.scrambling_id > 65535 || (cfg.n_scid != 0 && cfg.n_scid != 1) ||
        (cfg.noise_mode != 0 && cfg.noise_mode != 1) ||
        (cfg.compensate_cfo != 0 && cfg.compensate_cfo != 1)) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    const int words = handle->precomputed_words_per_sym;
    const int slot = cfg.slot_idx % MAX_SLOTS_PER_FRAME;
    if (!handle->dmrs_pilots_precomputed || words < 1 || words > (MAX_PRB * 12 + 31) / 32 ||
        handle->precomputed_words_per_slot != MAX_SYMBOLS * words ||
        handle->precomputed_nof_slots < 1 || handle->precomputed_nof_slots > MAX_SLOTS_PER_FRAME ||
        slot >= handle->precomputed_nof_slots ||
        handle->precomputed_pilots_per_symbol < (cfg.start_prb + cfg.nof_prb) * 6 ||
        handle->precomputed_pilots_per_symbol > MAX_PRB * 6 ||
        words * 16 < handle->precomputed_pilots_per_symbol ||
        handle->cached_dmrs_scrambling_id != cfg.scrambling_id || handle->cached_dmrs_n_scid != cfg.n_scid ||
        handle->cached_dmrs_type != static_cast<int>(cfg.dmrs_type) ||
        !handle->d_precomputed_dmrs_pilots || !handle->d_dmrs_pilot_bits ||
        handle->precomputed_dmrs_pilots_capacity <
            static_cast<size_t>(handle->precomputed_nof_slots) * MAX_SYMBOLS * words * sizeof(uint32_t) ||
        handle->dmrs_pilot_bits_capacity < 2 * words * sizeof(uint32_t) ||
        !handle->d_estimates_fp16 || handle->estimates_fp16_capacity < 2 * cfg.nof_prb * 12 * sizeof(__half2) ||
        !handle->d_noise_vars || handle->noise_vars_capacity < sizeof(float) ||
        !handle->d_dmrs_indices || handle->dmrs_indices_capacity < 2 * sizeof(int) ||
        !handle->d_dmrs_c_inits || !handle->d_accum_block || handle->accum_block_size < 208 ||
        !handle->d_noise_count || !handle->d_rsrp_accum || !handle->d_epre_accum ||
        !handle->d_sinr_noise_accum || !handle->d_sinr_rsrp_accum || !handle->d_sinr_epre_result ||
        !handle->d_ta_cfo_accum || !handle->d_cv_noise_accum || !handle->d_cv_done_counter ||
        !handle->d_symbol_start_times || !handle->d_mimo_cfo_phasors) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    return NR_LDPC_SUCCESS;
}

nr_ldpc_status_t pusch_e2e_process_full_gpu_optimized_mimo_half(
    pusch_e2e_handle_t handle,
    const void* d_grid_cbf16,
    void* d_llrs_half,
    const int* d_re_indices,
    int nof_data_re,
    cudaStream_t stream)
{
    if (!handle || !handle->configured) return NR_LDPC_ERROR_INVALID_CONFIG;
    if (!d_grid_cbf16 || !d_llrs_half || !d_re_indices) return NR_LDPC_ERROR_INVALID_CONFIG;

    const pusch_e2e_config_t& cfg = handle->config;
    if ((cfg.nof_tx_layers != 2 && cfg.nof_tx_layers != 3 && cfg.nof_tx_layers != 4) ||
        cfg.nof_tx_layers > cfg.nof_rx_ports) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    if (cfg.mod_order != 2 && cfg.mod_order != 4 && cfg.mod_order != 6 && cfg.mod_order != 8) {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    int nof_re_per_symbol = cfg.nof_prb * 12;
    int grid_stride = cfg.grid_nof_symbols * cfg.grid_nof_subcarriers;
    int symbol_stride = cfg.grid_nof_subcarriers;

    int dmrs_symbol_indices[MAX_DMRS_SYMBOLS];
    int nof_dmrs_symbols = pusch_e2e_get_dmrs_symbol_indices_checked(cfg.dmrs_symbol_mask, dmrs_symbol_indices);
    if (nof_dmrs_symbols <= 0) return NR_LDPC_ERROR_INVALID_CONFIG;

    cudaError_t err;
    int nof_layers = cfg.nof_tx_layers;
    size_t fp16_est_size = nof_dmrs_symbols * nof_re_per_symbol * cfg.nof_rx_ports * nof_layers * sizeof(__half2);
    if (fp16_est_size > handle->estimates_fp16_capacity) {
        if (handle->d_estimates_fp16) cudaFree(handle->d_estimates_fp16);
        err = cudaMalloc(&handle->d_estimates_fp16, fp16_est_size);
        if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
        handle->estimates_fp16_capacity = fp16_est_size;
    }

    size_t noise_size = cfg.nof_rx_ports * sizeof(float);
    if (noise_size > handle->noise_vars_capacity) {
        if (handle->d_noise_vars) cudaFree(handle->d_noise_vars);
        err = cudaMalloc(&handle->d_noise_vars, noise_size);
        if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
        handle->noise_vars_capacity = noise_size;
    }
    /* The reset rides the pilot-bit gather kernel below by default (each host launch
     * costs ~5-8 us on GB10); OCUDU_PUSCH_FUSED_PROLOGUE=0 restores the separate launch. */
    static const bool fused_prologue = []() {
        const char* v = std::getenv("OCUDU_PUSCH_FUSED_PROLOGUE");
        return (v == nullptr) || (v[0] != '0');
    }();
    if (handle->d_sinr_epre_result && !fused_prologue) {
        launch_kernel_reset_sinr_epre_result(1, 1, 0, stream, handle->d_sinr_epre_result);
    }

    if (!handle->d_noise_count) {
        err = cudaMalloc(&handle->d_noise_count, sizeof(unsigned int));
        if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
    }

    /* Upload DMRS symbol indices to device (cached — skip if mask unchanged) */
    size_t dmrs_indices_needed = nof_dmrs_symbols * sizeof(int);
    if (dmrs_indices_needed > handle->dmrs_indices_capacity) {
        if (handle->d_dmrs_indices) cudaFree(handle->d_dmrs_indices);
        err = cudaMalloc(&handle->d_dmrs_indices, dmrs_indices_needed);
        if (err != cudaSuccess) return NR_LDPC_ERROR_ALLOC_FAILED;
        handle->dmrs_indices_capacity = dmrs_indices_needed;
        handle->cached_dmrs_symbol_mask = -1;  /* Force re-upload after realloc */
    }
    if (cfg.dmrs_symbol_mask != handle->cached_dmrs_symbol_mask) {
        cudaMemcpyAsync(handle->d_dmrs_indices, dmrs_symbol_indices,
                        dmrs_indices_needed, cudaMemcpyHostToDevice, stream);
        handle->cached_dmrs_symbol_mask = cfg.dmrs_symbol_mask;
        handle->cached_nof_dmrs_symbols = nof_dmrs_symbols;
    }

    int slot_in_frame = cfg.slot_idx % MAX_SLOTS_PER_FRAME;
    int max_pilot_prb = cfg.start_prb + cfg.nof_prb;
    int precomputed_max_prb = handle->precomputed_pilots_per_symbol / dmrs_get_re_per_prb(cfg.dmrs_type);
    bool can_use_precomputed = handle->dmrs_pilots_precomputed &&
                               slot_in_frame < handle->precomputed_nof_slots &&
                               max_pilot_prb <= precomputed_max_prb &&
                               handle->d_dmrs_pilot_bits;

    uint32_t* d_pilot_bits_for_slot = nullptr;
    int words_per_dmrs_sym_stride = 0;
    if (can_use_precomputed) {
        words_per_dmrs_sym_stride = handle->precomputed_words_per_sym;
        const uint32_t* slot_base = handle->d_precomputed_dmrs_pilots +
                                    slot_in_frame * handle->precomputed_words_per_slot;
        int gather_threads = ((words_per_dmrs_sym_stride + 31) / 32) * 32;
        if (gather_threads < 32) gather_threads = 32;
        if (fused_prologue) {
            launch_kernel_gather_dmrs_pilot_bits_reset(nof_dmrs_symbols, gather_threads, 0, stream,
                handle->d_dmrs_pilot_bits, slot_base, handle->d_dmrs_indices, words_per_dmrs_sym_stride,
                handle->d_sinr_epre_result);
        } else {
            launch_kernel_gather_dmrs_pilot_bits(nof_dmrs_symbols, gather_threads, 0, stream,
                handle->d_dmrs_pilot_bits, slot_base, handle->d_dmrs_indices, words_per_dmrs_sym_stride);
        }
        d_pilot_bits_for_slot = handle->d_dmrs_pilot_bits;
    } else {
        return NR_LDPC_ERROR_INVALID_CONFIG;
    }

    dim3 lse_grid(cfg.nof_prb, nof_dmrs_symbols);
    int lse_threads = cfg.nof_rx_ports;
    if (cfg.nof_tx_layers == 2) {
        launch_kernel_mimo_lse_2layer_fp16(lse_grid, lse_threads, 0, stream, 
            static_cast<const unsigned int*>(d_grid_cbf16),
            d_pilot_bits_for_slot,
            handle->d_dmrs_indices,
            handle->d_estimates_fp16,
            cfg.nof_prb,
            cfg.nof_rx_ports,
            nof_dmrs_symbols,
            grid_stride,
            symbol_stride,
            cfg.start_prb,
            cfg.dmrs_scaling,
            words_per_dmrs_sym_stride,
            (int)cfg.dmrs_type);
    } else if (cfg.nof_tx_layers == 3) {
        launch_kernel_mimo_lse_3layer_fp16(lse_grid, lse_threads, 0, stream, 
            static_cast<const unsigned int*>(d_grid_cbf16),
            d_pilot_bits_for_slot,
            handle->d_dmrs_indices,
            handle->d_estimates_fp16,
            cfg.nof_prb,
            cfg.nof_rx_ports,
            nof_dmrs_symbols,
            grid_stride,
            symbol_stride,
            cfg.start_prb,
            cfg.dmrs_scaling,
            words_per_dmrs_sym_stride,
            (int)cfg.dmrs_type);
    } else {
        launch_kernel_mimo_lse_4layer_fp16(lse_grid, lse_threads, 0, stream, 
            static_cast<const unsigned int*>(d_grid_cbf16),
            d_pilot_bits_for_slot,
            handle->d_dmrs_indices,
            handle->d_estimates_fp16,
            cfg.nof_prb,
            cfg.nof_rx_ports,
            nof_dmrs_symbols,
            grid_stride,
            symbol_stride,
            cfg.start_prb,
            cfg.dmrs_scaling,
            words_per_dmrs_sym_stride,
            (int)cfg.dmrs_type);
    }

    if (cfg.nof_prb > 1) {
        /* Wide-block smoothing variant (same arithmetic, efficient thread mapping).
         * Enabled by default; set OCUDU_MIMO_FD_SMOOTH_OPT=0 to fall back for A/B. */
        static const bool fd_smooth_opt = []() {
            const char* v = std::getenv("OCUDU_MIMO_FD_SMOOTH_OPT");
            return (v == nullptr) || (v[0] != '0');
        }();
        if (fd_smooth_opt) {
            launch_kernel_mimo_fd_smoothing_fp16_wide(stream,
                handle->d_estimates_fp16,
                cfg.nof_prb,
                cfg.nof_rx_ports,
                cfg.nof_tx_layers,
                nof_dmrs_symbols,
                (int)cfg.dmrs_type);
        } else {
            dim3 smooth_grid(cfg.nof_prb, nof_dmrs_symbols, cfg.nof_tx_layers);
            launch_kernel_mimo_fd_smoothing_fp16(smooth_grid, cfg.nof_rx_ports, 0, stream,
                handle->d_estimates_fp16,
                cfg.nof_prb,
                cfg.nof_rx_ports,
                cfg.nof_tx_layers,
                nof_dmrs_symbols,
                (int)cfg.dmrs_type);
        }
    }

    /* Reset the contiguous MIMO accumulator tail once:
     * eq SINR sum/count, EVM sum/count, TA/CFO accum, CV noise accum, CV done counter.
     * d_noise_vars is fully overwritten by kernel_mimo_dmrs_residual_noise_variance. */
    const size_t mimo_accum_reset_bytes =
        2 * sizeof(float) + 2 * sizeof(unsigned int) + 6 * sizeof(float) +
        8 * sizeof(float) + sizeof(unsigned int);
    cudaMemsetAsync(handle->d_eq_noise_var_sum, 0, mimo_accum_reset_bytes, stream);

    static const bool enable_mimo_csi_metrics =
        !pusch_e2e_env_flag_disabled("OCUDU_PUSCH_MIMO_CSI_METRICS");
    const bool compute_mimo_csi =
        enable_mimo_csi_metrics && handle->d_sinr_epre_result && handle->d_ta_cfo_accum && cfg.scs_khz > 0;
    const float* mimo_cfo_ptr =
        (compute_mimo_csi && cfg.compensate_cfo && nof_dmrs_symbols >= 2) ? (handle->d_sinr_epre_result + 4) : nullptr;
    static const bool enable_mimo_cfo_phasors =
        !pusch_e2e_env_flag_disabled("OCUDU_PUSCH_CFO_PHASORS");
    const bool use_mimo_cfo_phasors =
        enable_mimo_cfo_phasors && mimo_cfo_ptr != nullptr &&
        handle->d_mimo_cfo_phasors != nullptr && handle->d_symbol_start_times != nullptr;
    float2* mimo_cfo_phasors = use_mimo_cfo_phasors ? handle->d_mimo_cfo_phasors : nullptr;

    if (compute_mimo_csi) {
        int pilot_spacing = (cfg.dmrs_type == DMRS_TYPE_1) ? 2 : 3;
        float pilot_spacing_hz = (float)pilot_spacing * (float)cfg.scs_khz * 1000.0f;
        float delta_t_s = 0.0f;
        if (nof_dmrs_symbols >= 2) {
            int mu = 0;
            if (cfg.scs_khz == 30) mu = 1;
            else if (cfg.scs_khz == 60) mu = 2;
            else if (cfg.scs_khz == 120) mu = 3;
            float scs_hz = (float)cfg.scs_khz * 1000.0f;
            int half_slot_sym = 7 * (1 << mu);
            for (int j = dmrs_symbol_indices[0] + 1; j <= dmrs_symbol_indices[nof_dmrs_symbols - 1]; j++) {
                int cp_kappa = 144 >> mu;
                if (j == 0 || j == half_slot_sym) cp_kappa += 16;
                float cp_s = (float)cp_kappa / 30720000.0f;
                delta_t_s += cp_s + 1.0f / scs_hz;
            }
        }

        /* Multi-block CSI: one pilot-group per thread across SMs (was a single
         * 256-thread block). Done-counter is the residual path's counter, reset
         * by the CSI last block after finalize.
         * OCUDU_PUSCH_MIMO_CSI_BLOCKS=1 forces the old single-block launch for A/B. */
        int pilots_per_prb_csi = dmrs_get_re_per_prb(cfg.dmrs_type);
        int total_pilots_csi = cfg.nof_prb * pilots_per_prb_csi;
        int csi_threads = 256;
        int csi_blocks = (total_pilots_csi + csi_threads - 1) / csi_threads;
        if (csi_blocks < 1) {
            csi_blocks = 1;
        }
        {
            unsigned forced = pusch_e2e_env_unsigned("OCUDU_PUSCH_MIMO_CSI_BLOCKS", 0);
            if (forced > 0) {
                csi_blocks = (int)forced;
            }
        }
        launch_kernel_compute_mimo_csi_from_estimates(csi_blocks, csi_threads, 0, stream, 
            static_cast<const unsigned int*>(d_grid_cbf16),
            handle->d_estimates_fp16,
            handle->d_dmrs_indices,
            handle->d_sinr_epre_result,
            handle->d_ta_cfo_accum,
            handle->d_cv_done_counter,
            cfg.nof_prb,
            cfg.nof_rx_ports,
            cfg.nof_tx_layers,
            nof_dmrs_symbols,
            grid_stride,
            symbol_stride,
            cfg.start_prb,
            pilot_spacing_hz,
            delta_t_s,
            handle->d_symbol_start_times,
            mimo_cfo_phasors,
            cfg.start_symbol,
            cfg.dmrs_scaling,
            (int)cfg.dmrs_type);
    }

    int nof_cdm = (cfg.nof_tx_layers + 1) / 2;
    int nof_dmrs_pilots_per_cdm = dmrs_get_re_per_prb(cfg.dmrs_type);
    /* Wide-block (per-PRB) variant, enabled by default; OCUDU_MIMO_NOISE_VAR_OPT=0 falls back for A/B. */
    static const bool noise_var_opt = []() {
        const char* v = std::getenv("OCUDU_MIMO_NOISE_VAR_OPT");
        return (v == nullptr) || (v[0] != '0');
    }();
    if (noise_var_opt) {
        launch_kernel_mimo_dmrs_residual_noise_variance_wide(stream,
            static_cast<const unsigned int*>(d_grid_cbf16),
            d_pilot_bits_for_slot,
            handle->d_dmrs_indices,
            handle->d_estimates_fp16,
            handle->d_cv_noise_accum,
            cfg.nof_prb,
            cfg.nof_rx_ports,
            cfg.nof_tx_layers,
            nof_dmrs_symbols,
            grid_stride,
            symbol_stride,
            cfg.start_prb,
            cfg.dmrs_scaling,
            words_per_dmrs_sym_stride,
            handle->d_noise_vars,
            handle->d_cv_done_counter,
            mimo_cfo_ptr,
            handle->d_symbol_start_times,
            mimo_cfo_phasors,
            (int)cfg.dmrs_type);
    } else {
    dim3 residual_grid(cfg.nof_prb, cfg.nof_rx_ports);
    launch_kernel_mimo_dmrs_residual_noise_variance(residual_grid, nof_dmrs_pilots_per_cdm * nof_cdm, 0, stream,
        static_cast<const unsigned int*>(d_grid_cbf16),
        d_pilot_bits_for_slot,
        handle->d_dmrs_indices,
        handle->d_estimates_fp16,
        handle->d_cv_noise_accum,
        cfg.nof_prb,
        cfg.nof_rx_ports,
        cfg.nof_tx_layers,
        nof_dmrs_symbols,
        grid_stride,
        symbol_stride,
        cfg.start_prb,
        cfg.dmrs_scaling,
        words_per_dmrs_sym_stride,
        handle->d_noise_vars,
        handle->d_cv_done_counter,
        mimo_cfo_ptr,
        handle->d_symbol_start_times,
        mimo_cfo_phasors,
        (int)cfg.dmrs_type);
    }

    if (!handle->d_scrambling_seq) return NR_LDPC_ERROR_INVALID_CONFIG;

    int threads_e2e = 256;
    int blocks_e2e = (nof_data_re + threads_e2e - 1) / threads_e2e;

    {
        nr_ldpc_status_t _ls;
        if (cfg.nof_tx_layers == 2) {
            _ls = launch_kernel_mimo_2layer_fp16(
                blocks_e2e, threads_e2e, 0, stream, cfg.nof_rx_ports, cfg.equalizer_algorithm,
                static_cast<const unsigned int*>(d_grid_cbf16),
                handle->d_estimates_fp16, d_re_indices, handle->d_noise_vars,
                static_cast<__half*>(d_llrs_half), handle->d_scrambling_seq,
                handle->d_eq_noise_var_sum, handle->d_eq_noise_var_count,
                cfg.enable_evm_metric ? handle->d_evm_error_sum : nullptr,
                cfg.enable_evm_metric ? handle->d_evm_symbol_count : nullptr,
                nof_data_re, nof_dmrs_symbols, cfg.nof_prb, cfg.mod_order,
                grid_stride, symbol_stride, cfg.start_symbol, cfg.start_prb * 12, cfg.tx_scaling,
                handle->d_dmrs_indices, mimo_cfo_ptr, handle->d_symbol_start_times, mimo_cfo_phasors,
                handle->d_eq_symbols_out, handle->d_eq_noise_var_out);
        } else if (cfg.nof_tx_layers == 3) {
            _ls = launch_kernel_mimo_3layer_fp16(
                blocks_e2e, threads_e2e, 0, stream, cfg.nof_rx_ports, cfg.equalizer_algorithm,
                static_cast<const unsigned int*>(d_grid_cbf16),
                handle->d_estimates_fp16, d_re_indices, handle->d_noise_vars,
                static_cast<__half*>(d_llrs_half), handle->d_scrambling_seq,
                handle->d_eq_noise_var_sum, handle->d_eq_noise_var_count,
                cfg.enable_evm_metric ? handle->d_evm_error_sum : nullptr,
                cfg.enable_evm_metric ? handle->d_evm_symbol_count : nullptr,
                nof_data_re, nof_dmrs_symbols, cfg.nof_prb, cfg.mod_order,
                grid_stride, symbol_stride, cfg.start_symbol, cfg.start_prb * 12, cfg.tx_scaling,
                handle->d_dmrs_indices, mimo_cfo_ptr, handle->d_symbol_start_times, mimo_cfo_phasors,
                handle->d_eq_symbols_out, handle->d_eq_noise_var_out);
        } else {
            _ls = launch_kernel_mimo_4layer_fp16(
                blocks_e2e, threads_e2e, 0, stream, cfg.nof_rx_ports, cfg.equalizer_algorithm,
                static_cast<const unsigned int*>(d_grid_cbf16),
                handle->d_estimates_fp16, d_re_indices, handle->d_noise_vars,
                static_cast<__half*>(d_llrs_half), handle->d_scrambling_seq,
                handle->d_eq_noise_var_sum, handle->d_eq_noise_var_count,
                cfg.enable_evm_metric ? handle->d_evm_error_sum : nullptr,
                cfg.enable_evm_metric ? handle->d_evm_symbol_count : nullptr,
                nof_data_re, nof_dmrs_symbols, cfg.nof_prb, cfg.mod_order,
                grid_stride, symbol_stride, cfg.start_symbol, cfg.start_prb * 12, cfg.tx_scaling,
                handle->d_dmrs_indices, mimo_cfo_ptr, handle->d_symbol_start_times, mimo_cfo_phasors,
                handle->d_eq_symbols_out, handle->d_eq_noise_var_out);
        }
        if (_ls != NR_LDPC_SUCCESS) return _ls;
    }

    err = cudaGetLastError();
    if (err != cudaSuccess) {
        fprintf(stderr, "[OCUDU PHY CUDA] E2E MIMO FP16 kernel error: %s (%d)\n",
                cudaGetErrorString(err), static_cast<int>(err));
        return NR_LDPC_ERROR_CUDA_FAILED;
    }

    return NR_LDPC_SUCCESS;
}
