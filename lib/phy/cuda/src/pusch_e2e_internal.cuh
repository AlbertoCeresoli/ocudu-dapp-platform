// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/**
 * @file pusch_e2e_internal.cuh
 * @brief Shared private internal definitions for the PUSCH E2E translation units (kernels TU + host API TU). Not public API.
 */

#pragma once
#include "pusch_e2e.h"
#include "pusch_transform_deprecoder_selection.h"
#include "scrambling.h"
#include <cuda.h>
#include <cuda_runtime.h>
#include <cuComplex.h>
#include <cuda_fp16.h>

/* Transform-deprecoder (DFT-s-OFDM) bounds */
#define PUSCH_DEPRECODE_MAX_DFT_SIZE 3240
#define PUSCH_DEPRECODE_MAX_SYMBOLS 14

/* Maximum supported configuration */
#define MAX_PRB 273
#define MAX_SYMBOLS 14
#define MAX_PORTS 8
#define MAX_DMRS_SYMBOLS 4
#define MAX_SLOTS_PER_FRAME 20  /* 30kHz SCS: 20 slots per 10ms frame */

#define MIMO_CFO_DATA_PHASOR_COUNT (MAX_SYMBOLS * MAX_DMRS_SYMBOLS)
#define MIMO_CFO_RESIDUAL_PHASOR_COUNT (MAX_DMRS_SYMBOLS * MAX_DMRS_SYMBOLS)
#define MIMO_CFO_PHASOR_COUNT (MIMO_CFO_DATA_PHASOR_COUNT + MIMO_CFO_RESIDUAL_PHASOR_COUNT)
#define MIMO_CFO_RESIDUAL_PHASOR_OFFSET MIMO_CFO_DATA_PHASOR_COUNT

// Keep the context layout independent of private backend compile definitions.
// Only the host API translation unit owns or dereferences these opaque plans.
struct pusch_vkfft_deprecoder_plan;

/* Jump-table init lives in the kernels TU (writes module-local __constant__);
 * exposed so the host API TU (create) can trigger it. */
cudaError_t initialize_pusch_e2e_jump_tables();

/* PUSCH E2E context */
struct pusch_e2e_ctx {
    pusch_e2e_config_t config;
    bool configured;

    /* Scrambler for data descrambling */
    scrambler_handle_t data_scrambler;

    /* Scrambler for DMRS pilot generation */
    scrambler_handle_t dmrs_scrambler;

    /* Device memory for DMRS pilots */
    cuFloatComplex* d_dmrs_pilots;
    size_t dmrs_pilots_capacity;

    /* Device memory for channel estimates (all REs, all ports) */
    cuFloatComplex* d_ch_estimates;
    size_t ch_estimates_capacity;
    int nof_estimates;

    /* Device memory for DMRS symbol estimates (before time interpolation) */
    cuFloatComplex* d_dmrs_estimates;
    size_t dmrs_estimates_capacity;

    /* Device memory for GPU-computed noise variances (per port) */
    float* d_noise_vars;
    size_t noise_vars_capacity;

    /* Device memory for noise variance reduction (per port, per block) */
    float* d_noise_var_partial;
    size_t noise_var_partial_capacity;

    /* Device memory for accumulated post-equalization noise variance (for SINR) */
    float* d_eq_noise_var_sum;
    unsigned int* d_eq_noise_var_count;

    /* Device memory for CPU-style hard-decision EVM accumulation */
    float* d_evm_error_sum;
    unsigned int* d_evm_symbol_count;

    /* Pinned host memory for async SINR/EVM readback */
    bool sinr_managed;                /* OCUDU_PUSCH_SINR_MANAGED: accum block + result block are
                                       * managed memory; h_* readback pointers alias device memory
                                       * directly and sinr_async_launch is a no-op (no D2H). */
    void* h_sinr_eq_block;            /* Single pinned alloc: [sinr sum/count, evm sum/count] */
    float* h_sinr_noise_var_sum;      /* Alias: h_sinr_eq_block + 0 */
    unsigned int* h_sinr_count;       /* Alias: h_sinr_eq_block + sizeof(float) */
    float* h_evm_error_sum;           /* Alias: h_sinr_eq_block + 8 */
    unsigned int* h_evm_symbol_count; /* Alias: h_sinr_eq_block + 12 */

    /* DMRS-based RSRP accumulation (per port, for SINR matching CPU ch.est) */
    float* d_rsrp_accum;        /* [nof_ports] — sum of |H|² at DMRS positions */

    /* DMRS-based EPRE accumulation (per port) */
    float* d_epre_accum;        /* [nof_ports] — sum of |y|² at DMRS positions */

    /* Residual SINR accumulators (per port, for fused CV+SINR kernel) */
    float* d_sinr_noise_accum;  /* [8] — per-port residual noise accumulator */
    float* d_sinr_rsrp_accum;   /* [8] — per-port signal power accumulator */

    /* DMRS c_init values for inline gold sequence generation (per DMRS symbol) */
    uint32_t* d_dmrs_c_inits;  /* [MAX_DMRS_SYMBOLS] */

    /* Results: DMRS-based SINR and EPRE computed on GPU */
    float* d_sinr_epre_result;  /* [5] — {sinr_db, epre_db, rsrp_db, ta_seconds, cfo_hz} */
    float* h_sinr_epre_result;  /* [5] — pinned host for async D2H */

    /* Pre-allocated buffers to avoid per-slot malloc (HUGE efficiency win!) */
    int* d_dmrs_indices;
    size_t dmrs_indices_capacity;
    cuFloatComplex* d_freq_interp_estimates;
    size_t freq_interp_capacity;
    cuFloatComplex* d_lse_averaged;
    size_t lse_averaged_capacity;
    __half* d_llrs_half_temp;  /* Temp buffer for INT8 conversion path */
    size_t llrs_half_temp_capacity;

    /* Optimized FP16 channel estimate buffers */
    __half2* d_estimates_fp16;
    size_t estimates_fp16_capacity;

    /* Buffer for DMRS received symbols (avoids grid re-read in noise var kernel) */
    cuFloatComplex* d_dmrs_received;
    size_t dmrs_received_capacity;

    /* Buffer for fused LSE+noise variance kernel (atomic counter) */
    unsigned int* d_noise_count;

    /* Consolidated per-iteration accumulator block — single memset zeros all.
     * Layout: [noise_count(4) | rsrp(32) | epre(32) | sinr_noise(32) | sinr_rsrp(32) |
     *          eq_sum(4) | eq_count(4) | evm_sum(4) | evm_count(4) |
     *          ta_cfo_accum(24) | cv_noise_accum(32) | cv_done_counter(4)]
     * Total: 208 bytes (max 8 ports). Pointer aliases point into this block. */
    void*  d_accum_block;
    size_t accum_block_size;

    /* TA/CFO/RSRP/EPRE accumulator (alias into d_accum_block, auto-zeroed).
     * Layout: [ta_r, ta_i, cfo_r, cfo_i, rsrp_sum, epre_sum] — 6 floats. */
    float* d_ta_cfo_accum;

    /* CV noise accumulator + done counter (aliases into d_accum_block, auto-zeroed).
     * Used by fused CV+finalize kernel. Separate from d_noise_vars so no mid-processing
     * re-zero is needed — the initial d_accum_block memset covers it. */
    float* d_cv_noise_accum;            /* [8] — per-port CV noise accumulator */
    unsigned int* d_cv_done_counter;    /* [1] — last-block atomic counter */

    /* Pre-computed scrambling sequence buffer (packed bits, 32 bits per word) */
    uint32_t* d_scrambling_seq;
    size_t scrambling_seq_capacity;
    uint32_t cached_data_scrambling_c_init;
    int cached_data_scrambling_bits;

    /* Pre-computed DMRS pilots (packed bits: 2 bits per pilot = real_sign, imag_sign)
     * Format: bit 2n = real sign, bit 2n+1 = imag sign for pilot n
     * Size: ceil(nof_prb * 6 * 2 * 2 / 32) words per DMRS symbol */
    uint32_t* d_dmrs_pilot_bits;
    size_t dmrs_pilot_bits_capacity;

    /* DMRS symbol mask for mega-fused kernel (14 ints, 1 per symbol slot) */
    int* d_dmrs_mask;
    size_t dmrs_mask_capacity;

    /* Pre-computed DMRS pilots for ALL 14 OFDM symbols × ALL slots
     * Layout: [slot_idx * 14 * words_per_sym + ofdm_symbol * words_per_sym]
     * This eliminates runtime kernel_generate_dmrs_pilots regardless of DMRS mask.
     * Precomputed at configure() time since scrambling_id/n_scid are fixed. */
    uint32_t* d_precomputed_dmrs_pilots;
    size_t precomputed_dmrs_pilots_capacity;
    int precomputed_nof_slots;           /* Number of slots precomputed */
    int precomputed_pilots_per_symbol;   /* Pilots per DMRS symbol */
    int precomputed_words_per_slot;      /* uint32_t words per slot (14 * words_per_sym) */
    int precomputed_words_per_sym;       /* uint32_t words per single OFDM symbol */
    bool dmrs_pilots_precomputed;        /* Flag indicating pilots are ready */

    /* Low-PAPR DMRS support for transform precoding */
    cuFloatComplex* d_low_papr_pilots;   /* Device buffer for low-PAPR pilot symbols */
    size_t low_papr_pilots_capacity;
    int cached_low_papr_nof_prb;
    int cached_low_papr_n_rs_id;

    /* Bounded transform-deprecoding plans for all valid 5G NR DFT sizes.
     * Index by dft_size; invalid entries have nof_factors == 0. */
    uint64_t deprecoding_fft_factors[PUSCH_DEPRECODE_MAX_DFT_SIZE + 1];
    int      deprecoding_fft_nof_factors[PUSCH_DEPRECODE_MAX_DFT_SIZE + 1];
    int      transform_deprecoder_backend;
    int      vkfft_auto_min_dft_size;
    cuFloatComplex* d_deprecode_symbols;
    float*   d_deprecode_noise_vars;
    size_t   deprecode_symbols_capacity;
    size_t   deprecode_noise_capacity;
    pusch_vkfft_deprecoder_plan* vkfft_plans[PUSCH_DEPRECODE_MAX_DFT_SIZE + 1];
    void* vkfft_buffer_ptr;
    CUdevice vkfft_device;
    cudaStream_t vkfft_stream;
    bool vkfft_preplanned;

    /* Precomputed OFDM symbol start times (seconds from slot start) */
    float* d_symbol_start_times;         /* Device memory [14] */
    float  h_symbol_start_times[14];     /* Host cache */
    int    symbol_times_scs_khz;         /* SCS used for last precomputation (0 = not yet computed) */
    float2* d_mimo_cfo_phasors;          /* [data symbol x DMRS] + [DMRS x DMRS] CFO rotation cache */

    /* Cached DMRS indices to avoid per-slot sync cudaMemcpy.
     * d_dmrs_indices is already uploaded when cached_dmrs_symbol_mask matches. */
    int    cached_dmrs_symbol_mask;      /* Mask used for last d_dmrs_indices upload */
    int    cached_nof_dmrs_symbols;      /* Number of DMRS symbols in cached indices */

    /* Cached DMRS pilot precompute parameters — skip 280-kernel precompute
     * when only RNTI/n_id changes (DMRS c_init depends only on scrambling_id + n_scid). */
    uint32_t cached_dmrs_scrambling_id;
    int      cached_dmrs_n_scid;
    int      cached_dmrs_nof_prb;
    int      cached_dmrs_type;

    /* Persistent non-blocking stream for configure-time precomputes.
     * Reused across configures to avoid cudaStreamCreate/Destroy on the hot path
     * (multi-sector nsys showed heavy stream churn). */
    cudaStream_t precompute_stream;

    /* External eq-output buffers (set via pusch_e2e_set_eq_output, default NULL) */
    float2* d_eq_symbols_out;
    float*  d_eq_noise_var_out;
};
