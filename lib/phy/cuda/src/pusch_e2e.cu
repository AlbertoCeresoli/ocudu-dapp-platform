// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/**
 * @file pusch_e2e.cu
 * @brief Fused PUSCH end-to-end processing with integrated channel estimation
 * 
 */

#include "pusch_e2e.h"
#include "scrambling.h"
#include "ocudu_phy_cuda_mimo_math.cuh"
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
#endif

/* Concern-separated private headers (extracted from this file) */
#include "pusch_e2e_equalize.cuh"
#include "pusch_e2e_low_papr.cuh"
#include "pusch_e2e_qam_demod.cuh"
#include "pusch_e2e_internal.cuh"
#include "pusch_e2e_launch.cuh"

/* ============================================================================
 * LFSR Jump Tables for on-the-fly Gold sequence generation
 * NOTE: These are defined here (not extern) because CUDA __constant__
 * memory is module-local and cannot be linked across compilation units.
 * ============================================================================ */
#define NC_SKIP 1600
#define LFSR_BITS 31
#define MAX_JUMP_POWER 24

/* Define the jump tables in this module (not extern!) */
__constant__ uint32_t d_pusch_e2e_x1_jump[MAX_JUMP_POWER + 1][LFSR_BITS];
__constant__ uint32_t d_pusch_e2e_x2_jump[MAX_JUMP_POWER + 1][LFSR_BITS];

/* CRITICAL FIX: Thread-safe initialization for jump tables
 * Multiple PUSCH processor instances created in parallel were racing to initialize
 * the same constant memory symbols, causing "invalid resource handle" errors that
 * contaminated all subsequent CUDA operations.
 */
static std::once_flag g_pusch_e2e_jump_tables_init_flag;

/* ============================================================================
 * Raised Cosine FIR Filter for Frequency-Domain Smoothing
 *
 * Matches CPU port_channel_estimator_helpers.cpp filter_type constructor:
 *   RC_FILTER with roll-off=0.2, span=3 symbols, resampled with stride=2
 *   (DMRS Type 1: pilots on every other subcarrier), nof_rbs=min(nof_prb, 3).
 *
 * For nof_rbs >= 3: 15 taps operating on pilot positions (6 per PRB).
 * Filter center is at index 7, so it looks 7 pilot positions each side.
 *
 * Tail correction factors compensate for truncation at band edges.
 * Applied symmetrically: left edge uses reversed tail_correction,
 * right edge uses tail_correction directly.
 * ============================================================================ */
#define FD_SMOOTH_FILTER_LEN 15

__constant__ float d_rc_filter[FD_SMOOTH_FILTER_LEN] = {
    -0.0391585909f, -0.0287990728f,  0.0000000000f,  0.0445245383f,
     0.0970724536f,  0.1467045930f,  0.1821531663f,  0.1950058248f,
     0.1821531663f,  0.1467045930f,  0.0970724536f,  0.0445245383f,
     0.0000000000f, -0.0287990728f, -0.0391585909f
};

/* QAM piecewise-linear demod LUTs -> pusch_e2e_qam_demod.cuh */

/* Host-side matrix operations for computing jump tables */
static void matrix_multiply_gf2_local(const uint32_t A[LFSR_BITS],
                                       const uint32_t B[LFSR_BITS],
                                       uint32_t C[LFSR_BITS]) {
    uint32_t B_T[LFSR_BITS] = {0};
    for (int i = 0; i < LFSR_BITS; i++) {
        for (int j = 0; j < LFSR_BITS; j++) {
            if (B[j] & (1u << i)) {
                B_T[i] |= (1u << j);
            }
        }
    }
    for (int i = 0; i < LFSR_BITS; i++) {
        C[i] = 0;
        for (int j = 0; j < LFSR_BITS; j++) {
            uint32_t dot = A[i] & B_T[j];
            if (__builtin_popcount(dot) & 1) {
                C[i] |= (1u << j);
            }
        }
    }
}

static void init_x1_base_matrix_local(uint32_t M[LFSR_BITS]) {
    for (int i = 0; i < LFSR_BITS; i++) M[i] = 0;
    for (int i = 0; i < LFSR_BITS - 1; i++) M[i] = 1u << (i + 1);
    M[LFSR_BITS - 1] = (1u << 0) | (1u << 3);
}

static void init_x2_base_matrix_local(uint32_t M[LFSR_BITS]) {
    for (int i = 0; i < LFSR_BITS; i++) M[i] = 0;
    for (int i = 0; i < LFSR_BITS - 1; i++) M[i] = 1u << (i + 1);
    M[LFSR_BITS - 1] = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3);
}

// Helper to do the actual initialization (called once via std::call_once)
static cudaError_t do_initialize_jump_tables() {
    uint32_t h_x1_jump[MAX_JUMP_POWER + 1][LFSR_BITS];
    uint32_t h_x2_jump[MAX_JUMP_POWER + 1][LFSR_BITS];

    init_x1_base_matrix_local(h_x1_jump[0]);
    init_x2_base_matrix_local(h_x2_jump[0]);

    for (int p = 1; p <= MAX_JUMP_POWER; p++) {
        matrix_multiply_gf2_local(h_x1_jump[p-1], h_x1_jump[p-1], h_x1_jump[p]);
        matrix_multiply_gf2_local(h_x2_jump[p-1], h_x2_jump[p-1], h_x2_jump[p]);
    }

    cudaError_t err;
    err = cudaMemcpyToSymbol(d_pusch_e2e_x1_jump, h_x1_jump, sizeof(h_x1_jump));
    if (err != cudaSuccess) return err;

    err = cudaMemcpyToSymbol(d_pusch_e2e_x2_jump, h_x2_jump, sizeof(h_x2_jump));
    if (err != cudaSuccess) return err;

    return cudaSuccess;
}

cudaError_t initialize_pusch_e2e_jump_tables() {
    // CRITICAL FIX: Use std::call_once to ensure thread-safe initialization
    // This prevents multiple threads from racing to write to the same constant memory
    static cudaError_t init_result = cudaSuccess;
    std::call_once(g_pusch_e2e_jump_tables_init_flag, [&]() {
        init_result = do_initialize_jump_tables();
    });
    return init_result;
}

/* Apply a pre-computed jump matrix to LFSR state */
__device__ __forceinline__ uint32_t apply_jump_matrix_local(uint32_t state,
                                                             const uint32_t jump[LFSR_BITS]) {
    uint32_t result = 0;
    #pragma unroll
    for (int i = 0; i < LFSR_BITS; i++) {
        uint32_t masked = state & jump[i];
        if (__popc(masked) & 1) {
            result |= (1u << i);
        }
    }
    return result;
}

/* Advance x1 LFSR by N steps using matrix exponentiation - O(log N) */
__device__ __forceinline__ uint32_t advance_x1_local(uint32_t x1, int n) {
    while (n) {
        int p = __ffs(n) - 1;
        x1 = apply_jump_matrix_local(x1, d_pusch_e2e_x1_jump[p]);
        n &= (n - 1);
    }
    return x1;
}

/* Advance x2 LFSR by N steps using matrix exponentiation - O(log N) */
__device__ __forceinline__ uint32_t advance_x2_local(uint32_t x2, int n) {
    while (n) {
        int p = __ffs(n) - 1;
        x2 = apply_jump_matrix_local(x2, d_pusch_e2e_x2_jump[p]);
        n &= (n - 1);
    }
    return x2;
}

/* Single-step LFSR advancement for sequential bit generation */
__device__ __forceinline__ uint32_t step_x1_local(uint32_t x1) {
    uint32_t new_bit = ((x1 >> 3) ^ x1) & 1;
    return (x1 >> 1) | (new_bit << 30);
}

__device__ __forceinline__ uint32_t step_x2_local(uint32_t x2) {
    uint32_t new_bit = ((x2 >> 3) ^ (x2 >> 2) ^ (x2 >> 1) ^ x2) & 1;
    return (x2 >> 1) | (new_bit << 30);
}


__device__ __host__ __forceinline__ int pusch_dmrs_pilots_per_cdm(int dmrs_type)
{
    return (dmrs_type == DMRS_TYPE_2) ? 4 : 6;
}

__device__ __host__ __forceinline__ int pusch_dmrs_sc_offset(int dmrs_type, int cdm_group, int pilot_re)
{
    if (dmrs_type == DMRS_TYPE_2) {
        return cdm_group * 2 + (pilot_re & 1) + 6 * (pilot_re >> 1);
    }
    return cdm_group + 2 * pilot_re;
}

__device__ __forceinline__ __half2 interpolate_dmrs_pilots_half2(
    const __half2* __restrict__ pilots,
    int nof_pilots,
    int dmrs_type,
    int cdm_group,
    int sc)
{
    int left = 0;
    int right = 0;
    int left_sc = pusch_dmrs_sc_offset(dmrs_type, cdm_group, 0);
    int right_sc = left_sc;

    #pragma unroll
    for (int p = 0; p < 6; p++) {
        if (p >= nof_pilots) break;
        int pilot_sc = pusch_dmrs_sc_offset(dmrs_type, cdm_group, p);
        if (pilot_sc == sc) {
            return pilots[p];
        }
        if (pilot_sc < sc) {
            left = p;
            left_sc = pilot_sc;
        } else {
            right = p;
            right_sc = pilot_sc;
            break;
        }
    }

    if (sc <= left_sc) {
        return pilots[left];
    }
    if (right_sc <= left_sc) {
        return pilots[left];
    }

    float alpha = (float)(sc - left_sc) / (float)(right_sc - left_sc);
    __half2 left_h = pilots[left];
    __half2 right_h = pilots[right];
    return __hadd2(__hmul2(left_h, __float2half2_rn(1.0f - alpha)),
                   __hmul2(right_h, __float2half2_rn(alpha)));
}


/* PUSCH E2E context */
/* pusch_e2e_ctx, MAX_* / deprecoder defs -> pusch_e2e_internal.cuh */

/* Low-PAPR DMRS sequence tables & ZC helpers -> pusch_e2e_low_papr.cuh */

/**
 * Generate low-PAPR DMRS pilot sequence for transform precoding.
 * Implements TS 38.211 Section 6.4.1.1.2 (PUSCH DMRS with transform precoding).
 *
 * @param d_pilots Output: complex pilot symbols [M_zc]
 * @param u Sequence group = n_rs_id % 30
 * @param M_zc Sequence length = nof_prb * 6 (DMRS RE per symbol)
 */
__global__ void kernel_generate_low_papr_pilots(
    cuFloatComplex* __restrict__ d_pilots,
    int u,
    int M_zc)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= M_zc) return;

    int arg;
    int N_zc;

    if (M_zc == 6) {
        arg = d_phi_M_sc_6[u][idx];
        N_zc = 4;
    } else if (M_zc == 12) {
        arg = d_phi_M_sc_12[u][idx];
        N_zc = 4;
    } else if (M_zc == 18) {
        arg = d_phi_M_sc_18[u][idx];
        N_zc = 4;
    } else if (M_zc == 24) {
        arg = d_phi_M_sc_24[u][idx];
        N_zc = 4;
    } else if (M_zc == 30) {
        /* TS 38.211 formula for M_zc = 30 */
        N_zc = 31;
        /* arg[n] = -((u+1)*(n+1)*(n+2)) mod (2*N_zc) */
        int64_t prod = (int64_t)(u + 1) * (int64_t)(idx + 1) * (int64_t)(idx + 2);
        arg = -((int)(prod % (2 * N_zc)));
    } else {
        /* Zadoff-Chu for M_zc >= 36 */
        N_zc = get_prime_less_than_device(M_zc);
        int q = zc_sequence_q_device(u, 0, N_zc);  /* v = 0 for low-PAPR DMRS */
        int m = idx % N_zc;
        /* arg[n] = -(q*m*(m+1)) mod (2*N_zc) */
        int64_t prod = (int64_t)q * (int64_t)m * (int64_t)(m + 1);
        arg = -((int)(prod % (2 * N_zc)));
    }

    /* Compute exp(j * π * arg / N_zc) */
    float phase = M_PI * (float)arg / (float)N_zc;
    d_pilots[idx] = make_cuFloatComplex(cosf(phase), sinf(phase));
}

/* ============================================================================
 * Inline device functions
 * ============================================================================ */

__device__ __forceinline__ cuFloatComplex cbf16_to_fp32(unsigned int packed) {
    unsigned int real_bits = packed & 0xFFFF;
    unsigned int imag_bits = (packed >> 16) & 0xFFFF;
    unsigned int real_fp32 = real_bits << 16;
    unsigned int imag_fp32 = imag_bits << 16;
    float real_f = *reinterpret_cast<float*>(&real_fp32);
    float imag_f = *reinterpret_cast<float*>(&imag_fp32);
    return make_cuFloatComplex(real_f, imag_f);
}

__device__ __forceinline__ cuFloatComplex cuCmulf_fast(cuFloatComplex a, cuFloatComplex b) {
    return make_cuFloatComplex(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

__device__ __forceinline__ cuFloatComplex cuConjf_fast(cuFloatComplex a) {
    return make_cuFloatComplex(a.x, -a.y);
}

/* EQ_NOISE_VAR_DEEP_FADE -> pusch_e2e_equalize.cuh */

/* The CPU post-equalization SINR path drops infinities, but keeps large
 * finite equalizer noise variances. The GPU uses EQ_NOISE_VAR_DEEP_FADE as
 * its infinity-equivalent erasure value, so only that exact sentinel is
 * excluded from the average. */
__device__ __forceinline__ bool sinr_noise_var_is_valid(float eq_noise_var)
{
    return isfinite(eq_noise_var) && (eq_noise_var != EQ_NOISE_VAR_DEEP_FADE);
}

/* Accumulate eq_noise_var for SINR reporting, excluding deep-fade REs. */
__device__ __forceinline__ void sinr_accumulate(
    float* d_sum, unsigned int* d_count, float eq_noise_var)
{
    if (sinr_noise_var_is_valid(eq_noise_var)) {
        atomicAdd(d_sum, eq_noise_var);
        atomicAdd(d_count, 1u);
    }
}

__device__ __forceinline__ void warp_accumulate_sum_count(
    float* d_sum, unsigned int* d_count, float value, unsigned int count)
{
    unsigned mask = __activemask();
    int lane = threadIdx.x & 31;

    #pragma unroll
    for (int offset = 16; offset > 0; offset >>= 1) {
        value += __shfl_down_sync(mask, value, offset);
        count += __shfl_down_sync(mask, count, offset);
    }

    int leader = __ffs(mask) - 1;
    if ((lane == leader) && (count > 0)) {
        atomicAdd(d_sum, value);
        atomicAdd(d_count, count);
    }
}

__device__ __forceinline__ void sinr_accumulate_warp(
    float* d_sum, unsigned int* d_count, float eq_noise_var)
{
    unsigned int valid_count = sinr_noise_var_is_valid(eq_noise_var) ? 1u : 0u;
    warp_accumulate_sum_count(d_sum, d_count, valid_count ? eq_noise_var : 0.0f, valid_count);
}

__device__ __forceinline__ float nearest_qam_axis(float value, int max_abs_level, float scale)
{
    float clipped = fminf((float)max_abs_level, fmaxf((float)-max_abs_level, value / scale));
    int idx = (int)floorf((clipped + (float)max_abs_level) * 0.5f + 0.5f);
    idx = max(0, min(max_abs_level, idx));
    return ((float)(-max_abs_level + 2 * idx)) * scale;
}

__device__ __forceinline__ float evm_error_power(cuFloatComplex eq, int mod_order)
{
    float ref_re;
    float ref_im;
    if (mod_order == 2) {
        constexpr float scale = 0.7071067811865475f;
        ref_re = (eq.x >= 0.0f) ? scale : -scale;
        ref_im = (eq.y >= 0.0f) ? scale : -scale;
    } else if (mod_order == 4) {
        constexpr float scale = 0.31622776601683794f;
        ref_re = nearest_qam_axis(eq.x, 3, scale);
        ref_im = nearest_qam_axis(eq.y, 3, scale);
    } else if (mod_order == 6) {
        constexpr float scale = 0.1543033499620919f;
        ref_re = nearest_qam_axis(eq.x, 7, scale);
        ref_im = nearest_qam_axis(eq.y, 7, scale);
    } else {
        constexpr float scale = 0.07669649888473704f;
        ref_re = nearest_qam_axis(eq.x, 15, scale);
        ref_im = nearest_qam_axis(eq.y, 15, scale);
    }

    float err_re = eq.x - ref_re;
    float err_im = eq.y - ref_im;
    return err_re * err_re + err_im * err_im;
}

__device__ __forceinline__ void evm_accumulate(
    float* d_sum, unsigned int* d_count, cuFloatComplex eq, int mod_order)
{
    if (d_sum && d_count && isfinite(eq.x) && isfinite(eq.y)) {
        atomicAdd(d_sum, evm_error_power(eq, mod_order));
        atomicAdd(d_count, 1u);
    }
}

__device__ __forceinline__ void evm_accumulate_warp(
    float* d_sum, unsigned int* d_count, float evm_sum, unsigned int symbol_count)
{
    if (d_sum && d_count) {
        warp_accumulate_sum_count(d_sum, d_count, evm_sum, symbol_count);
    }
}

/* Equalization & MIMO matrix-inversion helpers -> pusch_e2e_equalize.cuh */

/* ============================================================================
 * MIMO CHANNEL ESTIMATION WITH OCC DESPREADING
 *
 * For multi-layer MIMO, DMRS layers are separated using OCC (Orthogonal Cover
 * Codes) within CDM groups. For Type 1 DMRS:
 *
 * 2-layer MIMO (CDM Group 0 only):
 *   - Layer 0: OCC [+1, +1] on consecutive DMRS RE pairs
 *   - Layer 1: OCC [+1, -1] on consecutive DMRS RE pairs
 *   - h0 = (lse[2k] + lse[2k+1]) / 2
 *   - h1 = (lse[2k] - lse[2k+1]) / 2
 *
 * 4-layer MIMO (CDM Groups 0 and 1):
 *   - CDM Group 0 (subcarriers 0,2,4,6,8,10): Layers 0,1
 *   - CDM Group 1 (subcarriers 1,3,5,7,9,11): Layers 2,3
 *   - Same OCC patterns within each group
 * ============================================================================ */

/**
 * 2-layer MIMO LSE kernel with OCC despreading.
 *
 * This kernel processes all DMRS symbols in parallel and outputs per-layer
 * channel estimates for 2-layer MIMO configurations.
 *
 * Grid: (nof_prb, nof_dmrs_symbols)
 * Block: (nof_ports)
 *
 * Output layout: [dmrs_sym, prb, port, layer, 6_dmrs_re] -> interpolated to 12 RE
 * For efficiency, we output interpolated [dmrs_sym, prb, port, layer, 12_re]
 */
__global__ void kernel_mimo_lse_2layer_fp16(
    const unsigned int* __restrict__ d_grid_cbf16,
    const uint32_t* __restrict__ d_dmrs_seq,       /* Packed Gold sequence bits */
    const int* __restrict__ dmrs_symbol_indices,   /* Array of DMRS symbol indices */
    __half2* __restrict__ d_estimates_fp16,        /* FP16 output [dmrs_sym, prb, port, layer, 12] */
    int nof_prb,
    int nof_ports,
    int nof_dmrs_symbols,
    int grid_stride,
    int symbol_stride,
    int start_prb,
    float dmrs_scaling,
    int words_per_dmrs_sym_stride,
    int dmrs_type)
{
    int prb_idx = blockIdx.x;
    int dmrs_sym_idx = blockIdx.y;
    int port = threadIdx.x;

    if (prb_idx >= nof_prb || dmrs_sym_idx >= nof_dmrs_symbols || port >= nof_ports) return;

    int dmrs_symbol = dmrs_symbol_indices[dmrs_sym_idx];
    int prb_start_sc = (start_prb + prb_idx) * 12;
    const uint32_t* d_dmrs_seq_symbol = d_dmrs_seq + dmrs_sym_idx * words_per_dmrs_sym_stride;

    int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);

    float inv_sqrt2 = 0.70710678118f;
    float h_normalizer = 1.0f / dmrs_scaling;

    /* Compute raw LSE at all 6 DMRS positions first */
    cuFloatComplex lse[6];

    #pragma unroll
    for (int d = 0; d < nof_pilots; d++) {
        int sc = prb_start_sc + pusch_dmrs_sc_offset(dmrs_type, 0, d);
        int grid_idx = port * grid_stride + dmrs_symbol * symbol_stride + sc;
        cuFloatComplex y = cbf16_to_fp32(d_grid_cbf16[grid_idx]);

        int pilot_idx = (start_prb + prb_idx) * nof_pilots + d;
        int word_idx = pilot_idx / 16;
        int bit_offset = (pilot_idx % 16) * 2;
        uint32_t pilot_word = d_dmrs_seq_symbol[word_idx];
        int c_real = (pilot_word >> bit_offset) & 1;
        int c_imag = (pilot_word >> (bit_offset + 1)) & 1;

        /* QPSK pilot generation */
        float p_real = (1 - 2 * c_real) * inv_sqrt2;
        float p_imag = (1 - 2 * c_imag) * inv_sqrt2;

        /* LSE: h = y × conj(pilot) / dmrs_scaling */
        lse[d] = make_cuFloatComplex(
            (y.x * p_real + y.y * p_imag) * h_normalizer,
            (y.y * p_real - y.x * p_imag) * h_normalizer);
    }

    /* OCC despreading for 2-layer MIMO.
     * DMRS Type 1, 2-layer uses OCC in frequency domain:
     *   - Layer 0: OCC [+1, +1] → h0[k] = (lse[2k] + lse[2k+1]) / 2
     *   - Layer 1: OCC [+1, -1] → h1[k] = (lse[2k] - lse[2k+1]) / 2
     * where k = 0,1,2 (3 OCC pairs from 6 DMRS REs) */
    __half2 h_layer0[6], h_layer1[6];  /* Per-layer estimates at DMRS positions */

    #pragma unroll
    for (int k = 0; k < 3; k++) {
        if (2 * k + 1 >= nof_pilots) break;
        cuFloatComplex lse_even = lse[2 * k];
        cuFloatComplex lse_odd = lse[2 * k + 1];

        /* Layer 0: (even + odd) / 2 */
        cuFloatComplex h0 = make_cuFloatComplex(
            (lse_even.x + lse_odd.x) * 0.5f,
            (lse_even.y + lse_odd.y) * 0.5f);

        /* Layer 1: (even - odd) / 2 */
        cuFloatComplex h1 = make_cuFloatComplex(
            (lse_even.x - lse_odd.x) * 0.5f,
            (lse_even.y - lse_odd.y) * 0.5f);

        /* Store at both DMRS positions (OCC gives same estimate for paired REs) */
        __half2 h0_fp16 = __halves2half2(__float2half(h0.x), __float2half(h0.y));
        __half2 h1_fp16 = __halves2half2(__float2half(h1.x), __float2half(h1.y));

        h_layer0[2 * k] = h0_fp16;
        h_layer0[2 * k + 1] = h0_fp16;
        h_layer1[2 * k] = h1_fp16;
        h_layer1[2 * k + 1] = h1_fp16;
    }

    /* Output layout: [dmrs_sym, prb, port, layer, 12]
     * Total per dmrs_sym: nof_prb * nof_ports * 2_layers * 12 */
    int re_per_layer = 12;
    int re_per_port = 2 * re_per_layer;  /* 2 layers */
    int re_per_prb = nof_ports * re_per_port;
    int re_per_dmrs_sym = nof_prb * re_per_prb;

    int out_base = dmrs_sym_idx * re_per_dmrs_sym +
                   prb_idx * re_per_prb +
                   port * re_per_port;

    /* Output with frequency interpolation for odd subcarriers */
    #pragma unroll
    for (int sc = 0; sc < 12; sc++) {
        __half2 h0_out, h1_out;

        h0_out = interpolate_dmrs_pilots_half2(h_layer0, nof_pilots, dmrs_type, 0, sc);
        h1_out = interpolate_dmrs_pilots_half2(h_layer1, nof_pilots, dmrs_type, 0, sc);

        d_estimates_fp16[out_base + sc] = h0_out;              /* Layer 0 */
        d_estimates_fp16[out_base + re_per_layer + sc] = h1_out;  /* Layer 1 */
    }
}

__device__ __forceinline__ __half2 interpolate_mimo_dmrs_pilot_half2(
    const __half2* __restrict__ pilots,
    int sc,
    int cdm_offset)
{
    if (cdm_offset == 0) {
        if ((sc & 1) == 0) {
            return pilots[sc / 2];
        }
        int left = sc / 2;
        int right = (left + 1 < 6) ? left + 1 : 5;
        return __hmul2(__hadd2(pilots[left], pilots[right]), __float2half2_rn(0.5f));
    }

    if (sc <= 1) {
        return pilots[0];
    }
    if ((sc & 1) == 1) {
        return pilots[sc / 2];
    }
    int left = sc / 2 - 1;
    int right = sc / 2;
    return __hmul2(__hadd2(pilots[left], pilots[right]), __float2half2_rn(0.5f));
}

/**
 * 3-layer MIMO LSE kernel with OCC despreading.
 *
 * Layers 0 and 1 use CDM group 0. Layer 2 uses CDM group 1 with the +,+ OCC
 * branch; layer 3 from the 4-layer pattern is absent.
 */
__global__ void kernel_mimo_lse_3layer_fp16(
    const unsigned int* __restrict__ d_grid_cbf16,
    const uint32_t* __restrict__ d_dmrs_seq,
    const int* __restrict__ dmrs_symbol_indices,
    __half2* __restrict__ d_estimates_fp16,
    int nof_prb,
    int nof_ports,
    int nof_dmrs_symbols,
    int grid_stride,
    int symbol_stride,
    int start_prb,
    float dmrs_scaling,
    int words_per_dmrs_sym_stride,
    int dmrs_type)
{
    int prb_idx = blockIdx.x;
    int dmrs_sym_idx = blockIdx.y;
    int port = threadIdx.x;

    if (prb_idx >= nof_prb || dmrs_sym_idx >= nof_dmrs_symbols || port >= nof_ports) return;

    int dmrs_symbol = dmrs_symbol_indices[dmrs_sym_idx];
    int prb_start_sc = (start_prb + prb_idx) * 12;
    const uint32_t* d_dmrs_seq_symbol = d_dmrs_seq + dmrs_sym_idx * words_per_dmrs_sym_stride;

    float inv_sqrt2 = 0.70710678118f;
    float h_normalizer = 1.0f / dmrs_scaling;

    int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);
    cuFloatComplex lse_cdm0[6], lse_cdm1[6];

    #pragma unroll
    for (int d = 0; d < 6; d++) {
        if (d >= nof_pilots) break;
        int pilot_idx = (start_prb + prb_idx) * nof_pilots + d;
        int word_idx = pilot_idx / 16;
        int bit_offset = (pilot_idx % 16) * 2;
        uint32_t pilot_word = d_dmrs_seq_symbol[word_idx];
        int c_real = (pilot_word >> bit_offset) & 1;
        int c_imag = (pilot_word >> (bit_offset + 1)) & 1;

        float p_real = (1 - 2 * c_real) * inv_sqrt2;
        float p_imag = (1 - 2 * c_imag) * inv_sqrt2;

        int sc0 = prb_start_sc + pusch_dmrs_sc_offset(dmrs_type, 0, d);
        int grid_idx0 = port * grid_stride + dmrs_symbol * symbol_stride + sc0;
        cuFloatComplex y0 = cbf16_to_fp32(d_grid_cbf16[grid_idx0]);
        lse_cdm0[d] = make_cuFloatComplex(
            (y0.x * p_real + y0.y * p_imag) * h_normalizer,
            (y0.y * p_real - y0.x * p_imag) * h_normalizer);

        int sc1 = prb_start_sc + pusch_dmrs_sc_offset(dmrs_type, 1, d);
        int grid_idx1 = port * grid_stride + dmrs_symbol * symbol_stride + sc1;
        cuFloatComplex y1 = cbf16_to_fp32(d_grid_cbf16[grid_idx1]);
        lse_cdm1[d] = make_cuFloatComplex(
            (y1.x * p_real + y1.y * p_imag) * h_normalizer,
            (y1.y * p_real - y1.x * p_imag) * h_normalizer);
    }

    __half2 h_layer[3][6];
    #pragma unroll
    for (int k = 0; k < 3; k++) {
        if (2 * k + 1 >= nof_pilots) break;
        cuFloatComplex lse0_even = lse_cdm0[2 * k];
        cuFloatComplex lse0_odd = lse_cdm0[2 * k + 1];
        cuFloatComplex h0 = make_cuFloatComplex((lse0_even.x + lse0_odd.x) * 0.5f,
                                                (lse0_even.y + lse0_odd.y) * 0.5f);
        cuFloatComplex h1 = make_cuFloatComplex((lse0_even.x - lse0_odd.x) * 0.5f,
                                                (lse0_even.y - lse0_odd.y) * 0.5f);

        cuFloatComplex lse1_even = lse_cdm1[2 * k];
        cuFloatComplex lse1_odd = lse_cdm1[2 * k + 1];
        cuFloatComplex h2 = make_cuFloatComplex((lse1_even.x + lse1_odd.x) * 0.5f,
                                                (lse1_even.y + lse1_odd.y) * 0.5f);

        __half2 h0_fp16 = __halves2half2(__float2half(h0.x), __float2half(h0.y));
        __half2 h1_fp16 = __halves2half2(__float2half(h1.x), __float2half(h1.y));
        __half2 h2_fp16 = __halves2half2(__float2half(h2.x), __float2half(h2.y));

        h_layer[0][2 * k] = h0_fp16;
        h_layer[0][2 * k + 1] = h0_fp16;
        h_layer[1][2 * k] = h1_fp16;
        h_layer[1][2 * k + 1] = h1_fp16;
        h_layer[2][2 * k] = h2_fp16;
        h_layer[2][2 * k + 1] = h2_fp16;
    }

    int re_per_layer = 12;
    int re_per_port = 3 * re_per_layer;
    int re_per_prb = nof_ports * re_per_port;
    int re_per_dmrs_sym = nof_prb * re_per_prb;
    int out_base = dmrs_sym_idx * re_per_dmrs_sym + prb_idx * re_per_prb + port * re_per_port;

    #pragma unroll
    for (int layer = 0; layer < 3; layer++) {
        int layer_base = out_base + layer * re_per_layer;
        int cdm_offset = layer / 2;
        #pragma unroll
        for (int sc = 0; sc < 12; sc++) {
            d_estimates_fp16[layer_base + sc] =
                interpolate_dmrs_pilots_half2(h_layer[layer], nof_pilots, dmrs_type, cdm_offset, sc);
        }
    }
}

/**
 * 4-layer MIMO LSE kernel with OCC despreading.
 *
 * For 4-layer MIMO, we use both CDM groups:
 *   - CDM Group 0 (subcarriers 0,2,4,6,8,10): Layers 0,1
 *   - CDM Group 1 (subcarriers 1,3,5,7,9,11): Layers 2,3
 *
 * Each CDM group uses the same OCC pattern as 2-layer.
 *
 * Output layout: [dmrs_sym, prb, port, layer, 12_re]
 */
__global__ void kernel_mimo_lse_4layer_fp16(
    const unsigned int* __restrict__ d_grid_cbf16,
    const uint32_t* __restrict__ d_dmrs_seq,       /* Packed Gold sequence bits */
    const int* __restrict__ dmrs_symbol_indices,   /* Array of DMRS symbol indices */
    __half2* __restrict__ d_estimates_fp16,        /* FP16 output [dmrs_sym, prb, port, layer, 12] */
    int nof_prb,
    int nof_ports,
    int nof_dmrs_symbols,
    int grid_stride,
    int symbol_stride,
    int start_prb,
    float dmrs_scaling,
    int words_per_dmrs_sym_stride,
    int dmrs_type)
{
    int prb_idx = blockIdx.x;
    int dmrs_sym_idx = blockIdx.y;
    int port = threadIdx.x;

    if (prb_idx >= nof_prb || dmrs_sym_idx >= nof_dmrs_symbols || port >= nof_ports) return;

    int dmrs_symbol = dmrs_symbol_indices[dmrs_sym_idx];
    int prb_start_sc = (start_prb + prb_idx) * 12;
    const uint32_t* d_dmrs_seq_symbol = d_dmrs_seq + dmrs_sym_idx * words_per_dmrs_sym_stride;

    float inv_sqrt2 = 0.70710678118f;
    float h_normalizer = 1.0f / dmrs_scaling;

    int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);

    /* Compute raw LSE at all 12 DMRS positions */
    cuFloatComplex lse_cdm0[6], lse_cdm1[6];

    /* CDM Group 0 (layers 0,1) */
    #pragma unroll
    for (int d = 0; d < 6; d++) {
        if (d >= nof_pilots) break;
        int sc = prb_start_sc + pusch_dmrs_sc_offset(dmrs_type, 0, d);
        int grid_idx = port * grid_stride + dmrs_symbol * symbol_stride + sc;
        cuFloatComplex y = cbf16_to_fp32(d_grid_cbf16[grid_idx]);

        /* CDM Group 0 pilots: indices 0,1,2,3,4,5 */
        int pilot_idx = (start_prb + prb_idx) * nof_pilots + d;
        int word_idx = pilot_idx / 16;
        int bit_offset = (pilot_idx % 16) * 2;
        uint32_t pilot_word = d_dmrs_seq_symbol[word_idx];
        int c_real = (pilot_word >> bit_offset) & 1;
        int c_imag = (pilot_word >> (bit_offset + 1)) & 1;

        float p_real = (1 - 2 * c_real) * inv_sqrt2;
        float p_imag = (1 - 2 * c_imag) * inv_sqrt2;

        lse_cdm0[d] = make_cuFloatComplex(
            (y.x * p_real + y.y * p_imag) * h_normalizer,
            (y.y * p_real - y.x * p_imag) * h_normalizer);
    }

    /* CDM Group 1 (layers 2,3) - uses different pilot sequence offset.
     * For Type 1 DMRS, CDM group 1 pilots are at indices 6-11 (offset by 6 from CDM0).
     * NOTE: This is a simplification; full 3GPP spec may require different scrambling. */
    #pragma unroll
    for (int d = 0; d < 6; d++) {
        if (d >= nof_pilots) break;
        int sc = prb_start_sc + pusch_dmrs_sc_offset(dmrs_type, 1, d);
        int grid_idx = port * grid_stride + dmrs_symbol * symbol_stride + sc;
        cuFloatComplex y = cbf16_to_fp32(d_grid_cbf16[grid_idx]);

        int pilot_idx = (start_prb + prb_idx) * nof_pilots + d;
        int word_idx = pilot_idx / 16;
        int bit_offset = (pilot_idx % 16) * 2;
        uint32_t pilot_word = d_dmrs_seq_symbol[word_idx];
        int c_real = (pilot_word >> bit_offset) & 1;
        int c_imag = (pilot_word >> (bit_offset + 1)) & 1;

        float p_real = (1 - 2 * c_real) * inv_sqrt2;
        float p_imag = (1 - 2 * c_imag) * inv_sqrt2;

        lse_cdm1[d] = make_cuFloatComplex(
            (y.x * p_real + y.y * p_imag) * h_normalizer,
            (y.y * p_real - y.x * p_imag) * h_normalizer);
    }

    /* OCC despreading for all 4 layers */
    __half2 h_layer[4][6];  /* [layer][dmrs_re] */

    /* CDM Group 0 → Layers 0,1 */
    #pragma unroll
    for (int k = 0; k < 3; k++) {
        if (2 * k + 1 >= nof_pilots) break;
        cuFloatComplex lse_even = lse_cdm0[2 * k];
        cuFloatComplex lse_odd = lse_cdm0[2 * k + 1];

        cuFloatComplex h0 = make_cuFloatComplex(
            (lse_even.x + lse_odd.x) * 0.5f,
            (lse_even.y + lse_odd.y) * 0.5f);
        cuFloatComplex h1 = make_cuFloatComplex(
            (lse_even.x - lse_odd.x) * 0.5f,
            (lse_even.y - lse_odd.y) * 0.5f);

        __half2 h0_fp16 = __halves2half2(__float2half(h0.x), __float2half(h0.y));
        __half2 h1_fp16 = __halves2half2(__float2half(h1.x), __float2half(h1.y));

        h_layer[0][2 * k] = h0_fp16;
        h_layer[0][2 * k + 1] = h0_fp16;
        h_layer[1][2 * k] = h1_fp16;
        h_layer[1][2 * k + 1] = h1_fp16;
    }

    /* CDM Group 1 → Layers 2,3 */
    #pragma unroll
    for (int k = 0; k < 3; k++) {
        if (2 * k + 1 >= nof_pilots) break;
        cuFloatComplex lse_even = lse_cdm1[2 * k];
        cuFloatComplex lse_odd = lse_cdm1[2 * k + 1];

        cuFloatComplex h2 = make_cuFloatComplex(
            (lse_even.x + lse_odd.x) * 0.5f,
            (lse_even.y + lse_odd.y) * 0.5f);
        cuFloatComplex h3 = make_cuFloatComplex(
            (lse_even.x - lse_odd.x) * 0.5f,
            (lse_even.y - lse_odd.y) * 0.5f);

        __half2 h2_fp16 = __halves2half2(__float2half(h2.x), __float2half(h2.y));
        __half2 h3_fp16 = __halves2half2(__float2half(h3.x), __float2half(h3.y));

        h_layer[2][2 * k] = h2_fp16;
        h_layer[2][2 * k + 1] = h2_fp16;
        h_layer[3][2 * k] = h3_fp16;
        h_layer[3][2 * k + 1] = h3_fp16;
    }

    /* Output layout: [dmrs_sym, prb, port, layer, 12] */
    int re_per_layer = 12;
    int re_per_port = 4 * re_per_layer;  /* 4 layers */
    int re_per_prb = nof_ports * re_per_port;
    int re_per_dmrs_sym = nof_prb * re_per_prb;

    int out_base = dmrs_sym_idx * re_per_dmrs_sym +
                   prb_idx * re_per_prb +
                   port * re_per_port;

    /* Output with frequency interpolation for odd subcarriers */
    #pragma unroll
    for (int layer = 0; layer < 4; layer++) {
        int layer_base = out_base + layer * re_per_layer;
        int cdm_offset = layer / 2;

        #pragma unroll
        for (int sc = 0; sc < 12; sc++) {
            d_estimates_fp16[layer_base + sc] =
                interpolate_dmrs_pilots_half2(h_layer[layer], nof_pilots, dmrs_type, cdm_offset, sc);
        }
    }
}

/* ============================================================================
 * ULTRA-OPTIMIZED KERNELS: Fused LSE+NoiseVar and On-the-fly Scrambling
 *
 * These kernels eliminate additional kernel launches and memory traffic:
 * 1. Fused LSE: Computes channel estimates AND noise variance in one pass
 * 2. On-the-fly scrambling: Generates Gold sequence bits directly instead
 *    of reading from pre-generated buffer
 * ============================================================================ */

/**
 * Compute DMRS c_init on device (matches dmrs_compute_c_init).
 * TS 38.211 6.4.1.1.1.1:
 *   c_init = (2^17 × (14 × n_s + l + 1) × (2×N_ID + 1) + 2×N_ID + n_SCID) mod 2^31
 */
__device__ __forceinline__ uint32_t compute_dmrs_c_init_device(
    int slot_idx, int symbol_idx, uint32_t scrambling_id, int n_scid)
{
    uint64_t term1 = (1ULL << 17) * (14ULL * slot_idx + symbol_idx + 1) * (2ULL * scrambling_id + 1);
    uint64_t term2 = 2ULL * scrambling_id + n_scid;
    return static_cast<uint32_t>((term1 + term2) & 0x7FFFFFFF);
}

/**
 * Pre-generate DMRS pilot bits in parallel.
 *
 * Each thread generates 16 pilot pairs (32 pilots = 64 bits = 2 words).
 * Uses LFSR jump to starting position, then sequential generation.
 *
 * Output format: packed bits where bit 2n = real_sign, bit 2n+1 = imag_sign
 * Pilot value = (1 - 2*real_sign) + j*(1 - 2*imag_sign)) / sqrt(2)
 *
 * Grid: (nof_words/2 + 255) / 256, Threads: 256
 */
__global__ void kernel_generate_dmrs_pilots(
    uint32_t* __restrict__ d_pilot_bits,
    uint32_t c_init,
    int nof_pilots)  /* Total pilots = nof_prb * 6 per DMRS symbol */
{
    int word_idx = (blockIdx.x * blockDim.x + threadIdx.x) * 2;  /* Each thread does 2 words (32 pilots) */
    int pilot_start = word_idx * 16;  /* 16 pilots per word (2 bits each) */

    if (pilot_start >= nof_pilots) return;

    /* Initialize LFSRs and jump to starting position */
    uint32_t x1 = 1;
    uint32_t x2 = c_init & 0x7FFFFFFF;
    int bit_start = pilot_start * 2;  /* 2 bits per pilot */
    int total_advance = NC_SKIP + bit_start;
    x1 = advance_x1_local(x1, total_advance);
    x2 = advance_x2_local(x2, total_advance);

    /* Generate up to 32 pilots (64 bits = 2 words) */
    uint32_t word0 = 0, word1 = 0;
    int pilots_remaining = nof_pilots - pilot_start;
    int pilots_to_gen = min(32, pilots_remaining);

    for (int p = 0; p < pilots_to_gen; p++) {
        /* Generate 2 bits: real_sign, imag_sign */
        uint32_t c_real = (x1 ^ x2) & 1;
        uint32_t new_x1 = ((x1 >> 3) ^ x1) & 1;
        x1 = (x1 >> 1) | (new_x1 << 30);
        uint32_t new_x2 = ((x2 >> 3) ^ (x2 >> 2) ^ (x2 >> 1) ^ x2) & 1;
        x2 = (x2 >> 1) | (new_x2 << 30);

        uint32_t c_imag = (x1 ^ x2) & 1;
        new_x1 = ((x1 >> 3) ^ x1) & 1;
        x1 = (x1 >> 1) | (new_x1 << 30);
        new_x2 = ((x2 >> 3) ^ (x2 >> 2) ^ (x2 >> 1) ^ x2) & 1;
        x2 = (x2 >> 1) | (new_x2 << 30);

        /* Pack into words: 2 bits per pilot, 16 pilots per word */
        int bit_pos = (p % 16) * 2;
        if (p < 16) {
            word0 |= (c_real << bit_pos) | (c_imag << (bit_pos + 1));
        } else {
            word1 |= (c_real << bit_pos) | (c_imag << (bit_pos + 1));
        }
    }

    d_pilot_bits[word_idx] = word0;
    if (word_idx + 1 < (nof_pilots * 2 + 31) / 32) {
        d_pilot_bits[word_idx + 1] = word1;
    }
}

/**
 * Batched version of kernel_generate_dmrs_pilots: generates pilots for ALL
 * (slot, symbol) pairs in a single kernel launch. blockIdx.y indexes the
 * flattened (slot * 14 + symbol) pair; blockIdx.x + threadIdx.x index
 * within a single symbol's pilots, identical to the original kernel.
 *
 * Eliminates 279 kernel launch overheads per configure call (280 → 1).
 */
__global__ void kernel_generate_dmrs_pilots_batched(
    uint32_t* __restrict__ d_pilot_bits,
    int nof_pilots,
    int words_per_sym,
    int words_per_slot,
    int nof_slots,
    uint32_t scrambling_id,
    int n_scid)
{
    int pair_idx = blockIdx.y;
    int slot = pair_idx / 14;
    int sym  = pair_idx % 14;
    if (slot >= nof_slots) return;

    /* Compute c_init per TS 38.211 §6.4.1.1.1.1 */
    uint64_t term1 = (1ULL << 17) * (14ULL * slot + sym + 1) * (2ULL * scrambling_id + 1);
    uint64_t term2 = 2ULL * scrambling_id + n_scid;
    uint32_t c_init = static_cast<uint32_t>((term1 + term2) & 0x7FFFFFFF);

    int word_idx = (blockIdx.x * blockDim.x + threadIdx.x) * 2;
    int pilot_start = word_idx * 16;
    if (pilot_start >= nof_pilots) return;

    /* Initialize LFSRs and jump to starting position */
    uint32_t x1 = 1;
    uint32_t x2 = c_init & 0x7FFFFFFF;
    int bit_start = pilot_start * 2;
    int total_advance = NC_SKIP + bit_start;
    x1 = advance_x1_local(x1, total_advance);
    x2 = advance_x2_local(x2, total_advance);

    /* Generate up to 32 pilots (64 bits = 2 words) */
    uint32_t word0 = 0, word1 = 0;
    int pilots_remaining = nof_pilots - pilot_start;
    int pilots_to_gen = min(32, pilots_remaining);

    for (int p = 0; p < pilots_to_gen; p++) {
        uint32_t c_real = (x1 ^ x2) & 1;
        uint32_t new_x1 = ((x1 >> 3) ^ x1) & 1;
        x1 = (x1 >> 1) | (new_x1 << 30);
        uint32_t new_x2 = ((x2 >> 3) ^ (x2 >> 2) ^ (x2 >> 1) ^ x2) & 1;
        x2 = (x2 >> 1) | (new_x2 << 30);

        uint32_t c_imag = (x1 ^ x2) & 1;
        new_x1 = ((x1 >> 3) ^ x1) & 1;
        x1 = (x1 >> 1) | (new_x1 << 30);
        new_x2 = ((x2 >> 3) ^ (x2 >> 2) ^ (x2 >> 1) ^ x2) & 1;
        x2 = (x2 >> 1) | (new_x2 << 30);

        int bit_pos = (p % 16) * 2;
        if (p < 16) {
            word0 |= (c_real << bit_pos) | (c_imag << (bit_pos + 1));
        } else {
            word1 |= (c_real << bit_pos) | (c_imag << (bit_pos + 1));
        }
    }

    uint32_t* dst = d_pilot_bits + slot * words_per_slot + sym * words_per_sym;
    dst[word_idx] = word0;
    if (word_idx + 1 < (nof_pilots * 2 + 31) / 32) {
        dst[word_idx + 1] = word1;
    }
}

/**
 * Gather DMRS pilot bits from the all-14-symbol precomputed layout into a
 * contiguous per-DMRS-symbol buffer.  Replaces 2-4 cudaMemcpyAsync D2D calls
 * per slot with a single kernel launch.
 *
 * Grid:  nof_dmrs_symbols blocks x 1
 * Block: words_per_sym threads (rounded up to warp multiple, max 128)
 */
__global__ void kernel_gather_dmrs_pilot_bits(
    uint32_t* __restrict__       d_dst,
    const uint32_t* __restrict__ d_src,
    const int* __restrict__      d_symbol_indices,
    int                          words_per_sym)
{
    int word = threadIdx.x;
    if (word >= words_per_sym) return;
    int sym = d_symbol_indices[blockIdx.x];
    d_dst[blockIdx.x * words_per_sym + word] = d_src[sym * words_per_sym + word];
}

/**
 * Gather variant that also resets the 5-float SINR/EPRE result block
 * (OCUDU_PUSCH_FUSED_PROLOGUE). Folds the former 1-thread reset launch into
 * the gather so the per-slot prologue is one kernel instead of two; the
 * reset values match kernel_reset_sinr_epre_result exactly. Runs before the
 * CSI kernel, which is the only writer of those fields, so ordering is
 * unchanged. d_sinr_epre_result may be null.
 */
__global__ void kernel_gather_dmrs_pilot_bits_reset(
    uint32_t* __restrict__       d_dst,
    const uint32_t* __restrict__ d_src,
    const int* __restrict__      d_symbol_indices,
    int                          words_per_sym,
    float* __restrict__          d_sinr_epre_result)
{
    if (blockIdx.x == 0 && threadIdx.x == 0 && d_sinr_epre_result != nullptr) {
        d_sinr_epre_result[0] = -INFINITY; /* SINR */
        d_sinr_epre_result[1] = -INFINITY; /* EPRE */
        d_sinr_epre_result[2] = -INFINITY; /* RSRP */
        d_sinr_epre_result[3] = NAN;       /* TA */
        d_sinr_epre_result[4] = NAN;       /* CFO */
    }
    int word = threadIdx.x;
    if (word >= words_per_sym) return;
    int sym = d_symbol_indices[blockIdx.x];
    d_dst[blockIdx.x * words_per_sym + word] = d_src[sym * words_per_sym + word];
}

/**
 * Fused LSE + Freq Interpolation + Noise Variance Accumulation
 *
 * This kernel performs channel estimation AND accumulates noise variance
 * residuals in a single pass, eliminating the separate noise variance kernel.
 *
 * Grid: (nof_prb, nof_dmrs_symbols), Threads: nof_ports
 * Output: FP16 estimates + atomically accumulated noise variance
 */
__global__ void kernel_fused_lse_freq_interp_noise_fp16(
    const unsigned int* __restrict__ d_grid_cbf16,
    const uint32_t* __restrict__ d_dmrs_c_inits,  /* Per-DMRS-symbol c_init array */
    const int* __restrict__ dmrs_symbol_indices,
    __half2* __restrict__ d_estimates_fp16,
    float* __restrict__ d_noise_var_accum,     /* Per-port atomic accumulator */
    unsigned int* __restrict__ d_noise_count,  /* Total DMRS REs processed (single atomic) */
    float* __restrict__ d_rsrp_accum,          /* Per-port RSRP accumulator (|H|²) */
    float* __restrict__ d_epre_accum,          /* Per-port EPRE accumulator (|y|²) */
    int nof_prb,
    int nof_ports,
    int nof_dmrs_symbols,
    int grid_stride,
    int symbol_stride,
    int start_prb,
    float dmrs_scaling,
    int dmrs_type)
{
    int prb_idx = blockIdx.x;
    int dmrs_sym_idx = blockIdx.y;
    int port = threadIdx.x;

    if (prb_idx >= nof_prb || dmrs_sym_idx >= nof_dmrs_symbols || port >= nof_ports) return;

    int dmrs_symbol = dmrs_symbol_indices[dmrs_sym_idx];
    int prb_start_sc = (start_prb + prb_idx) * 12;
    int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);

    /* DMRS pilot amplitude: Use the actual TX amplitude including power boost.
     * TX sends pilots with amplitude = dmrs_scaling * 1/sqrt(2).
     * For LSE: h = y × conj(p) / |p|^2 where |p|^2 = (dmrs_scaling * 0.707)^2 * 2 = dmrs_scaling^2.
     * Note: This matches the original code behavior when dmrs_scaling=1. */
    float pilot_amp = dmrs_scaling * 0.70710678118f;
    float pilot_pwr = dmrs_scaling * dmrs_scaling;  // |p|^2
    float h_normalizer = 1.0f / pilot_pwr;

    /* Compute LSE at all 6 DMRS positions, also accumulate noise, RSRP, EPRE */
    __half2 h_dmrs[6];
    float local_noise_sum = 0.0f;
    float local_rsrp_sum = 0.0f;
    float local_epre_sum = 0.0f;

    /* Inline DMRS gold sequence generation: advance LFSR to this PRB's first pilot bit */
    uint32_t dmrs_c_init = d_dmrs_c_inits[dmrs_sym_idx];
    int bit_start = (start_prb + prb_idx) * nof_pilots * 2;
    uint32_t x1 = advance_x1_local(1, NC_SKIP + bit_start);
    uint32_t x2 = advance_x2_local(dmrs_c_init, NC_SKIP + bit_start);

    #pragma unroll
    for (int d = 0; d < nof_pilots; d++) {
        int sc = prb_start_sc + pusch_dmrs_sc_offset(dmrs_type, 0, d);
        int grid_idx = port * grid_stride + dmrs_symbol * symbol_stride + sc;
        cuFloatComplex y = cbf16_to_fp32(d_grid_cbf16[grid_idx]);

        /* Generate DMRS pilot bits inline from LFSR */
        int c_real = (x1 ^ x2) & 1; x1 = step_x1_local(x1); x2 = step_x2_local(x2);
        int c_imag = (x1 ^ x2) & 1; x1 = step_x1_local(x1); x2 = step_x2_local(x2);

        float p_real = (1 - 2 * c_real) * pilot_amp;
        float p_imag = (1 - 2 * c_imag) * pilot_amp;

        /* LSE: h = y × conj(pilot) / |pilot|^2 */
        float h_real = (y.x * p_real + y.y * p_imag) * h_normalizer;
        float h_imag = (y.y * p_real - y.x * p_imag) * h_normalizer;

        h_dmrs[d] = __halves2half2(__float2half(h_real), __float2half(h_imag));

        /* Accumulate RSRP: |H|² per DMRS pilot */
        local_rsrp_sum += h_real * h_real + h_imag * h_imag;

        /* Accumulate EPRE: |y|² per DMRS pilot */
        local_epre_sum += y.x * y.x + y.y * y.y;

        /* Compute noise residual: |y - h * pilot|^2
         * Since h = y × conj(p) / |p|^2, we have h × p = y (ideally).
         * For noise estimation with multiple DMRS, use cross-symbol averaging.
         * For now, compute the residual which will be near-zero for pure LSE. */
        float y_ideal_real = h_real * p_real - h_imag * p_imag;
        float y_ideal_imag = h_real * p_imag + h_imag * p_real;
        float res_real = y.x - y_ideal_real;
        float res_imag = y.y - y_ideal_imag;
        local_noise_sum += res_real * res_real + res_imag * res_imag;
    }

    /* Output estimates: [dmrs_sym, prb, port, 12] */
    int re_per_dmrs_sym = nof_prb * nof_ports * 12;
    int out_base = dmrs_sym_idx * re_per_dmrs_sym + prb_idx * (nof_ports * 12) + port * 12;

    #pragma unroll
    for (int sc = 0; sc < 12; sc++) {
        __half2 h_out;
        h_out = interpolate_dmrs_pilots_half2(h_dmrs, nof_pilots, dmrs_type, 0, sc);
        d_estimates_fp16[out_base + sc] = h_out;
    }

    /* Atomic accumulate noise variance (per-port) */
    atomicAdd(&d_noise_var_accum[port], local_noise_sum);

    /* Atomic accumulate RSRP and EPRE (per-port) */
    atomicAdd(&d_rsrp_accum[port], local_rsrp_sum);
    atomicAdd(&d_epre_accum[port], local_epre_sum);

    /* Count total DMRS REs (only one thread per port needs to do this) */
    if (prb_idx == 0 && dmrs_sym_idx == 0) {
        atomicAdd(d_noise_count, nof_prb * nof_dmrs_symbols * nof_pilots);
    }
}

/**
 * Finalize noise variance after fused LSE kernel.
 * Simple kernel to divide accumulated sum by count and apply scaling.
 */
__global__ void kernel_finalize_noise_variance(
    float* __restrict__ d_noise_var_accum,
    const unsigned int* __restrict__ d_noise_count,
    int nof_ports,
    float dmrs_scaling,
    int nof_dmrs_symbols)
{
    int port = threadIdx.x;
    if (port >= nof_ports) return;

    unsigned int count = *d_noise_count;
    float sum = d_noise_var_accum[port];

    /* Normalize */
    float divisor = (count > 1) ? (float)(count - 1) : 1.0f;
    float noise_var = sum / divisor;

    /* Apply same scaling as original kernel */
    float N = (float)nof_dmrs_symbols;
    float beta_sq = dmrs_scaling * dmrs_scaling;
    float base_scale = beta_sq / sqrtf(N);
    float alloc_factor = (count < 50) ? 1.2f : 1.0f;

    /* Noise variance floor for fallback path (when CPU noise not provided).
     * The GPU DMRS-based noise estimation is unreliable because h is estimated from
     * the same y samples (so |y - h*pilot| ≈ 0). Use a conservative floor. */
    d_noise_var_accum[port] = fmaxf(noise_var * base_scale * alloc_factor, 0.001f);
}

/**
 * CROSS-VALIDATION NOISE ESTIMATION KERNEL
 *
 * Computes noise variance by comparing channel estimates across DMRS symbols.
 * This is the CORRECT way to estimate noise - the previous method computed
 * |y - h*pilot| where h = y/pilot, which gives zero!
 *
 * For N DMRS symbols with estimates h[0], h[1], ..., h[N-1]:
 *   h[i] = h_true + n[i]   where n[i] ~ CN(0, σ²)
 *   h_mean = mean(h[i])
 *   variance = mean(|h[i] - h_mean|²) = σ² * (N-1)/N
 *   Therefore: σ² = variance * N / (N-1)
 *
 * For the common case of N=2:
 *   |h[0] - h[1]|² = |n[0] - n[1]|² has E[·] = 2σ²
 *   So σ² = 0.5 * |h[0] - h[1]|²
 *
 * Grid: (nof_prb, 1), Threads: 12 * nof_ports
 * Each thread handles one (prb, port, sc) triplet, sums over DMRS symbols.
 */
__global__ void kernel_cross_validation_noise_variance(
    const __half2* __restrict__ d_estimates_fp16,
    float* __restrict__ d_noise_var_accum,
    const unsigned int* __restrict__ d_grid_cbf16,
    const uint32_t* __restrict__ d_dmrs_pilot_bits,
    const int* __restrict__ d_dmrs_symbol_indices,
    float* __restrict__ d_sinr_noise_accum,
    float* __restrict__ d_sinr_rsrp_accum,
    int nof_prb,
    int nof_ports,
    int nof_dmrs_symbols,
    int grid_stride,
    int symbol_stride,
    int start_prb,
    float dmrs_scaling,
    int words_per_dmrs_sym,
    const float* __restrict__ d_cfo_hz_ptr,
    const float* __restrict__ d_symbol_start_times,
    const float2* __restrict__ d_cfo_phasors,
    /* Fused finalize parameters (eliminates separate kernel launch) */
    float* __restrict__ d_noise_vars_out,
    unsigned int* __restrict__ d_cv_done_counter,
    const float* __restrict__ d_epre_accum,
    float* __restrict__ d_sinr_epre_result,
    int noise_mode,  /* 0=cross-validation (default), 1=pilot-residual */
    int dmrs_type)
{
    int prb_idx = blockIdx.x;
    int local_idx = threadIdx.x;  /* 0..12*nof_ports-1 */
    int port = local_idx / 12;
    int sc = local_idx % 12;

    if (prb_idx >= nof_prb || port >= nof_ports) return;

    /* CFO de-rotation phasors: exp(-j·2π·cfo·t_d) per DMRS symbol.
     * Thread 0 computes sincosf into shared memory; all threads use it. */
    __shared__ float s_derot_cos[4];  /* max 4 DMRS symbols */
    __shared__ float s_derot_sin[4];

    if (local_idx == 0) {
        float cfo_hz = (d_cfo_hz_ptr != nullptr) ? *d_cfo_hz_ptr : 0.0f;
        for (int d = 0; d < nof_dmrs_symbols && d < 4; d++) {
            if (d_cfo_hz_ptr != nullptr && d_cfo_phasors != nullptr) {
                float2 ph = d_cfo_phasors[MIMO_CFO_RESIDUAL_PHASOR_OFFSET + d];
                s_derot_cos[d] = ph.x;
                s_derot_sin[d] = ph.y;
            } else if (d_cfo_hz_ptr != nullptr && d_symbol_start_times != nullptr && d_dmrs_symbol_indices != nullptr) {
                float t_d = d_symbol_start_times[d_dmrs_symbol_indices[d]];
                sincosf(-6.2831853071795864f * cfo_hz * t_d, &s_derot_sin[d], &s_derot_cos[d]);
            } else {
                s_derot_cos[d] = 1.0f;
                s_derot_sin[d] = 0.0f;
            }
        }
    }
    __syncthreads();

    /* Estimate layout: [dmrs_sym, prb, port, 12] */
    int re_per_dmrs_sym = nof_prb * nof_ports * 12;
    int base_idx = prb_idx * (nof_ports * 12) + port * 12 + sc;

    /* FAST PATH: 2 DMRS symbols (most common case) */
    if (nof_dmrs_symbols == 2) {
        __half2 h0 = d_estimates_fp16[0 * re_per_dmrs_sym + base_idx];
        __half2 h1 = d_estimates_fp16[1 * re_per_dmrs_sym + base_idx];

        /* Convert to FP32 */
        float h0_real = __half2float(__low2half(h0));
        float h0_imag = __half2float(__high2half(h0));
        float h1_real = __half2float(__low2half(h1));
        float h1_imag = __half2float(__high2half(h1));

        /* De-rotate each estimate by exp(-j·2π·cfo·t_d) to remove CFO phase */
        float h0r_d = h0_real * s_derot_cos[0] - h0_imag * s_derot_sin[0];
        float h0i_d = h0_real * s_derot_sin[0] + h0_imag * s_derot_cos[0];
        float h1r_d = h1_real * s_derot_cos[1] - h1_imag * s_derot_sin[1];
        float h1i_d = h1_real * s_derot_sin[1] + h1_imag * s_derot_cos[1];

        /* Compute |h0_derot - h1_derot|² — CFO component removed */
        float diff_real = h0r_d - h1r_d;
        float diff_imag = h0i_d - h1i_d;
        float diff_sq = diff_real * diff_real + diff_imag * diff_imag;

        /* σ² = 0.5 * E[|h0 - h1|²] for two independent estimates */
        float noise_contribution = 0.5f * diff_sq;

        atomicAdd(&d_noise_var_accum[port], noise_contribution);

        /* Fused SINR: at pilot positions (sc % 2 == 0), compute residual noise */
        int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);
        int d_pilot = -1;
        for (int p = 0; p < nof_pilots; ++p) {
            if (pusch_dmrs_sc_offset(dmrs_type, 0, p) == sc) {
                d_pilot = p;
                break;
            }
        }

        if ((d_pilot >= 0) && d_sinr_noise_accum != nullptr) {
            /* Coherent average of de-rotated estimates */
            float h_avg_re = (h0r_d + h1r_d) * 0.5f;
            float h_avg_im = (h0i_d + h1i_d) * 0.5f;

            float local_rsrp = h_avg_re * h_avg_re + h_avg_im * h_avg_im;
            float local_noise = 0.0f;
            float pilot_amp = dmrs_scaling * 0.70710678118f;

            int pilot_idx = (start_prb + prb_idx) * nof_pilots + d_pilot;
            int pilot_word_idx = pilot_idx / 16;
            int pilot_bit_offset = (pilot_idx % 16) * 2;

            for (int d = 0; d < 2; d++) {
                int dmrs_symbol = d_dmrs_symbol_indices[d];
                int sc_grid = (start_prb + prb_idx) * 12 + sc;

                int grid_idx = port * grid_stride + dmrs_symbol * symbol_stride + sc_grid;
                cuFloatComplex y = cbf16_to_fp32(d_grid_cbf16[grid_idx]);

                const uint32_t* dmrs_seq = d_dmrs_pilot_bits + d * words_per_dmrs_sym;
                uint32_t pilot_word = dmrs_seq[pilot_word_idx];
                int c_real = (pilot_word >> pilot_bit_offset) & 1;
                int c_imag = (pilot_word >> (pilot_bit_offset + 1)) & 1;

                float p_real = (1 - 2 * c_real) * pilot_amp;
                float p_imag = (1 - 2 * c_imag) * pilot_amp;

                /* pred = h_avg_derot × pilot */
                float pred_real = h_avg_re * p_real - h_avg_im * p_imag;
                float pred_imag = h_avg_re * p_imag + h_avg_im * p_real;

                /* Re-rotate prediction to match received signal phase:
                 * pred *= exp(+j·2π·cfo·t_d) = conjugate of de-rotation.
                 * conj(cos_d + j·sin_d) = cos_d - j·sin_d where sin_d = sin(-2π·cfo·t_d)
                 * So: (a+jb)(cos_d - j·sin_d) = (a·cos_d + b·sin_d) + j(b·cos_d - a·sin_d) */
                float pr = pred_real * s_derot_cos[d] + pred_imag * s_derot_sin[d];
                float pi = pred_imag * s_derot_cos[d] - pred_real * s_derot_sin[d];

                float res_real = y.x - pr;
                float res_imag = y.y - pi;

                local_noise += res_real * res_real + res_imag * res_imag;
            }

            atomicAdd(&d_sinr_noise_accum[port], local_noise);
            atomicAdd(&d_sinr_rsrp_accum[port], local_rsrp);
        }
    }
    /* GENERAL PATH: N DMRS symbols */
    else if (nof_dmrs_symbols > 2) {
        /* First pass: compute de-rotated mean */
        float sum_real = 0.0f, sum_imag = 0.0f;
        for (int d = 0; d < nof_dmrs_symbols; d++) {
            __half2 h = d_estimates_fp16[d * re_per_dmrs_sym + base_idx];
            float hr = __half2float(__low2half(h));
            float hi = __half2float(__high2half(h));
            /* De-rotate by exp(-j·2π·cfo·t_d) */
            sum_real += hr * s_derot_cos[d] - hi * s_derot_sin[d];
            sum_imag += hr * s_derot_sin[d] + hi * s_derot_cos[d];
        }
        float mean_real = sum_real / nof_dmrs_symbols;
        float mean_imag = sum_imag / nof_dmrs_symbols;

        /* Second pass: compute variance from de-rotated estimates */
        float var_sum = 0.0f;
        for (int d = 0; d < nof_dmrs_symbols; d++) {
            __half2 h = d_estimates_fp16[d * re_per_dmrs_sym + base_idx];
            float hr = __half2float(__low2half(h));
            float hi = __half2float(__high2half(h));
            float dr = (hr * s_derot_cos[d] - hi * s_derot_sin[d]) - mean_real;
            float di = (hr * s_derot_sin[d] + hi * s_derot_cos[d]) - mean_imag;
            var_sum += dr * dr + di * di;
        }

        /* variance = sum / N, then σ² = variance * N / (N-1) */
        float variance = var_sum / nof_dmrs_symbols;
        float noise_est = variance * nof_dmrs_symbols / (nof_dmrs_symbols - 1);

        atomicAdd(&d_noise_var_accum[port], noise_est);

        int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);
        int d_pilot = -1;
        for (int p = 0; p < nof_pilots; ++p) {
            if (pusch_dmrs_sc_offset(dmrs_type, 0, p) == sc) {
                d_pilot = p;
                break;
            }
        }

        if ((d_pilot >= 0) && d_sinr_noise_accum != nullptr) {
            float local_rsrp = mean_real * mean_real + mean_imag * mean_imag;
            float local_noise = 0.0f;
            float pilot_amp = dmrs_scaling * 0.70710678118f;

            int pilot_idx = (start_prb + prb_idx) * nof_pilots + d_pilot;
            int pilot_word_idx = pilot_idx / 16;
            int pilot_bit_offset = (pilot_idx % 16) * 2;

            for (int d = 0; d < nof_dmrs_symbols; d++) {
                int dmrs_symbol = d_dmrs_symbol_indices[d];
                int sc_grid = (start_prb + prb_idx) * 12 + sc;

                int grid_idx = port * grid_stride + dmrs_symbol * symbol_stride + sc_grid;
                cuFloatComplex y = cbf16_to_fp32(d_grid_cbf16[grid_idx]);

                const uint32_t* dmrs_seq = d_dmrs_pilot_bits + d * words_per_dmrs_sym;
                uint32_t pilot_word = dmrs_seq[pilot_word_idx];
                int c_real = (pilot_word >> pilot_bit_offset) & 1;
                int c_imag = (pilot_word >> (pilot_bit_offset + 1)) & 1;

                float p_real = (1 - 2 * c_real) * pilot_amp;
                float p_imag = (1 - 2 * c_imag) * pilot_amp;

                /* pred = h_avg_derot × pilot */
                float pred_real = mean_real * p_real - mean_imag * p_imag;
                float pred_imag = mean_real * p_imag + mean_imag * p_real;

                /* Re-rotate: pred *= exp(+j·2π·cfo·t_d) = conjugate of de-rotation */
                float pr = pred_real * s_derot_cos[d] + pred_imag * s_derot_sin[d];
                float pi = pred_imag * s_derot_cos[d] - pred_real * s_derot_sin[d];

                float res_real = y.x - pr;
                float res_imag = y.y - pi;

                local_noise += res_real * res_real + res_imag * res_imag;
            }

            atomicAdd(&d_sinr_noise_accum[port], local_noise);
            atomicAdd(&d_sinr_rsrp_accum[port], local_rsrp);
        }
    }
    /* Edge case: only 1 DMRS symbol - cannot estimate noise, use floor */
    /* This will be handled by the standalone finalize kernel */

    /* ---- Fused finalize: last-block pattern (same as TA/CFO kernel) ----
     * After all blocks complete their atomicAdds into d_noise_var_accum,
     * the last block normalizes the accumulated noise and writes finalized
     * values to d_noise_vars_out. This eliminates a separate kernel launch. */
    __syncthreads();  /* Ensure all threads in this block are done with atomicAdds */

    if (threadIdx.x == 0 && d_cv_done_counter != nullptr) {
        __threadfence();  /* Make this block's atomicAdds visible to all SMs */
        unsigned int done = atomicAdd(d_cv_done_counter, 1u) + 1;

        if (done == (unsigned int)nof_prb) {
            /* Last block: all CV accumulations are globally visible.
             * Normalize per-port noise variance and write to output. */
            int nof_sc = nof_prb * 12;
            int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);
            int nof_pilot_positions = nof_prb * nof_pilots;
            int nof_dmrs_re_per_port = nof_prb * nof_dmrs_symbols * nof_pilots;

            if (noise_mode == 1 && d_sinr_noise_accum != nullptr && nof_dmrs_re_per_port > 1) {
                /* Pilot-residual noise: σ² = Σ|y - H_avg*pilot|² / (N-1)
                 * More robust under fading where CV overestimates noise. */
                for (int p = 0; p < nof_ports; p++) {
                    float noise_var = d_sinr_noise_accum[p] / (float)(nof_dmrs_re_per_port - 1);
                    d_noise_vars_out[p] = fmaxf(noise_var, 1e-6f);
                }
            } else {
                /* Cross-validation noise (default) */
                for (int p = 0; p < nof_ports; p++) {
                    float noise_var = (nof_sc > 0)
                        ? (d_noise_var_accum[p] / (float)nof_sc) : 0.001f;
                    /* Calibration: CV variance is ~40% of CPU noise */
                    noise_var *= 2.5f;
                    d_noise_vars_out[p] = fmaxf(noise_var, 1e-6f);
                }
            }

            /* Compute SINR/EPRE/RSRP in dB from fused CV accumulators */
            if (d_sinr_epre_result != nullptr && d_sinr_noise_accum != nullptr) {
                float total_signal = 0.0f, total_noise = 0.0f, total_epre = 0.0f;
                for (int p = 0; p < nof_ports; p++) {
                    float signal_power = d_sinr_rsrp_accum[p] / (float)nof_pilot_positions;
                    float sinr_noise_var = d_sinr_noise_accum[p] / (float)(nof_dmrs_re_per_port - 1);
                    sinr_noise_var = fmaxf(sinr_noise_var, signal_power * 1e-10f);
                    float mean_epre_port = d_epre_accum[p] / (float)nof_dmrs_re_per_port;
                    total_signal += signal_power;
                    total_noise += sinr_noise_var;
                    total_epre += mean_epre_port;
                }

                float mean_noise = total_noise / nof_ports;
                d_sinr_epre_result[0] = (mean_noise > 1e-10f)
                    ? 10.0f * log10f(total_signal / mean_noise) : 60.0f;
                float mean_epre = total_epre / nof_ports;
                d_sinr_epre_result[1] = (mean_epre > 1e-10f)
                    ? 10.0f * log10f(mean_epre) : -INFINITY;
                float mean_rsrp = (total_signal / nof_ports) * dmrs_scaling * dmrs_scaling;
                d_sinr_epre_result[2] = (mean_rsrp > 1e-10f)
                    ? 10.0f * log10f(mean_rsrp) : -INFINITY;
            }
        }
    }
}

__device__ __forceinline__ float mimo_dmrs_residual_pilot_sum(
    const unsigned int* __restrict__ d_grid_cbf16,
    const uint32_t* __restrict__ d_dmrs_pilot_bits,
    const int* __restrict__ d_dmrs_symbol_indices,
    const __half2* __restrict__ d_estimates_fp16,
    int nof_prb,
    int nof_ports,
    int nof_layers,
    int nof_dmrs_symbols,
    int grid_stride,
    int symbol_stride,
    int start_prb,
    float dmrs_scaling,
    int words_per_dmrs_sym,
    const float* __restrict__ d_cfo_hz_ptr,
    const float* __restrict__ d_symbol_start_times,
    const float2* __restrict__ d_cfo_phasors,
    int dmrs_type,
    int prb_idx,
    int port,
    int cdm,
    int pilot_re)
{
    int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);
    int re_per_layer = 12;
    int re_per_port = nof_layers * re_per_layer;
    int re_per_prb = nof_ports * re_per_port;
    int re_per_dmrs_sym = nof_prb * re_per_prb;
    int prb_base = prb_idx * re_per_prb + port * re_per_port;

    int sc_in_prb = pusch_dmrs_sc_offset(dmrs_type, cdm, pilot_re);
    float occ = ((pilot_re & 1) == 0) ? 1.0f : -1.0f;

    float residual_sum = 0.0f;
    float inv_sqrt2 = 0.70710678118f;
    float cfo_hz = (d_cfo_hz_ptr != nullptr) ? *d_cfo_hz_ptr : 0.0f;

    for (int d = 0; d < nof_dmrs_symbols; d++) {
        int dmrs_symbol = d_dmrs_symbol_indices[d];
        int grid_sc = (start_prb + prb_idx) * 12 + sc_in_prb;
        int grid_idx = port * grid_stride + dmrs_symbol * symbol_stride + grid_sc;
        cuFloatComplex y = cbf16_to_fp32(d_grid_cbf16[grid_idx]);

        int pilot_idx = (start_prb + prb_idx) * nof_pilots + pilot_re;
        int pilot_word_idx = pilot_idx / 16;
        int pilot_bit_offset = (pilot_idx % 16) * 2;
        const uint32_t* dmrs_seq = d_dmrs_pilot_bits + d * words_per_dmrs_sym;
        uint32_t pilot_word = dmrs_seq[pilot_word_idx];
        int c_real = (pilot_word >> pilot_bit_offset) & 1;
        int c_imag = (pilot_word >> (pilot_bit_offset + 1)) & 1;

        float p_real = (1 - 2 * c_real) * inv_sqrt2 * dmrs_scaling;
        float p_imag = (1 - 2 * c_imag) * inv_sqrt2 * dmrs_scaling;

        cuFloatComplex h_eff = make_cuFloatComplex(0.0f, 0.0f);
        int first_layer = cdm * 2;
        int second_layer = first_layer + 1;

        for (int l = first_layer; l < nof_layers && l <= second_layer; l++) {
            float h_re = 0.0f;
            float h_im = 0.0f;
            for (int avg_d = 0; avg_d < nof_dmrs_symbols; avg_d++) {
                int est_idx = avg_d * re_per_dmrs_sym + prb_base + l * re_per_layer + sc_in_prb;
                __half2 h = d_estimates_fp16[est_idx];
                float hr = __half2float(__low2half(h));
                float hi = __half2float(__high2half(h));
                if (cfo_hz != 0.0f) {
                    float cos_d = 1.0f;
                    float sin_d = 0.0f;
                    if (d_cfo_phasors != nullptr) {
                        float2 ph = d_cfo_phasors[MIMO_CFO_RESIDUAL_PHASOR_OFFSET +
                                                   d * MAX_DMRS_SYMBOLS + avg_d];
                        cos_d = ph.x;
                        sin_d = ph.y;
                    } else if (d_symbol_start_times != nullptr) {
                        float dt = d_symbol_start_times[dmrs_symbol] -
                                   d_symbol_start_times[d_dmrs_symbol_indices[avg_d]];
                        sincosf(6.2831853071795864f * cfo_hz * dt, &sin_d, &cos_d);
                    }
                    float r2 = hr * cos_d - hi * sin_d;
                    float i2 = hr * sin_d + hi * cos_d;
                    hr = r2;
                    hi = i2;
                }
                h_re += hr;
                h_im += hi;
            }
            h_re /= nof_dmrs_symbols;
            h_im /= nof_dmrs_symbols;

            float layer_occ = (l == second_layer) ? occ : 1.0f;
            h_eff.x += layer_occ * h_re;
            h_eff.y += layer_occ * h_im;
        }

        float pred_re = h_eff.x * p_real - h_eff.y * p_imag;
        float pred_im = h_eff.x * p_imag + h_eff.y * p_real;
        float err_re = y.x - pred_re;
        float err_im = y.y - pred_im;
        residual_sum += err_re * err_re + err_im * err_im;
    }

    return residual_sum;
}

__global__ void kernel_mimo_dmrs_residual_noise_variance(
    const unsigned int* __restrict__ d_grid_cbf16,
    const uint32_t* __restrict__ d_dmrs_pilot_bits,
    const int* __restrict__ d_dmrs_symbol_indices,
    const __half2* __restrict__ d_estimates_fp16,
    float* __restrict__ d_noise_var_accum,
    int nof_prb,
    int nof_ports,
    int nof_layers,
    int nof_dmrs_symbols,
    int grid_stride,
    int symbol_stride,
    int start_prb,
    float dmrs_scaling,
    int words_per_dmrs_sym,
    float* __restrict__ d_noise_vars_out,
    unsigned int* __restrict__ d_done_counter,
    const float* __restrict__ d_cfo_hz_ptr,
    const float* __restrict__ d_symbol_start_times,
    const float2* __restrict__ d_cfo_phasors,
    int dmrs_type)
{
    int prb_idx = blockIdx.x;
    int port = blockIdx.y;
    int local_idx = threadIdx.x;
    int nof_cdm = (nof_layers + 1) / 2;
    int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);
    int cdm = local_idx / nof_pilots;
    int pilot_re = local_idx % nof_pilots;

    if (prb_idx >= nof_prb || port >= nof_ports || cdm >= nof_cdm) return;

    float residual_sum = mimo_dmrs_residual_pilot_sum(
        d_grid_cbf16, d_dmrs_pilot_bits, d_dmrs_symbol_indices, d_estimates_fp16,
        nof_prb, nof_ports, nof_layers, nof_dmrs_symbols, grid_stride, symbol_stride,
        start_prb, dmrs_scaling, words_per_dmrs_sym, d_cfo_hz_ptr, d_symbol_start_times,
        d_cfo_phasors, dmrs_type, prb_idx, port, cdm, pilot_re);

    atomicAdd(&d_noise_var_accum[port], residual_sum);
    __syncthreads();

    if (threadIdx.x == 0 && d_done_counter != nullptr) {
        __threadfence();
        unsigned int done = atomicAdd(d_done_counter, 1u) + 1;
        if (done == (unsigned int)(nof_prb * nof_ports)) {
            int nof_dmrs_pilots = nof_prb * nof_dmrs_symbols * nof_pilots;
            int divisor = nof_dmrs_pilots * nof_cdm - 1;
            for (int p = 0; p < nof_ports; p++) {
                float noise_var = (divisor > 0) ? (d_noise_var_accum[p] / (float)divisor) : 0.001f;
                d_noise_vars_out[p] = fmaxf(noise_var, 1e-6f);
            }
        }
    }
}

/**
 * Wide-block variant of kernel_mimo_dmrs_residual_noise_variance
 * (OCUDU_MIMO_NOISE_VAR_OPT). The baseline launches (prb, port) blocks of only
 * pilots*cdm (<=12) threads; this variant folds all ports into one block per
 * PRB — (pilots*cdm, ports) threads — so warps are full. Per-thread residual
 * arithmetic is shared with the baseline via mimo_dmrs_residual_pilot_sum.
 *
 * Grid:  (nof_prb)
 * Block: (nof_pilots * nof_cdm, nof_ports)
 */
__global__ void kernel_mimo_dmrs_residual_noise_variance_wide(
    const unsigned int* __restrict__ d_grid_cbf16,
    const uint32_t* __restrict__ d_dmrs_pilot_bits,
    const int* __restrict__ d_dmrs_symbol_indices,
    const __half2* __restrict__ d_estimates_fp16,
    float* __restrict__ d_noise_var_accum,
    int nof_prb,
    int nof_ports,
    int nof_layers,
    int nof_dmrs_symbols,
    int grid_stride,
    int symbol_stride,
    int start_prb,
    float dmrs_scaling,
    int words_per_dmrs_sym,
    float* __restrict__ d_noise_vars_out,
    unsigned int* __restrict__ d_done_counter,
    const float* __restrict__ d_cfo_hz_ptr,
    const float* __restrict__ d_symbol_start_times,
    const float2* __restrict__ d_cfo_phasors,
    int dmrs_type)
{
    int prb_idx = blockIdx.x;
    int port = threadIdx.y;
    int local_idx = threadIdx.x;
    int nof_cdm = (nof_layers + 1) / 2;
    int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);
    int cdm = local_idx / nof_pilots;
    int pilot_re = local_idx % nof_pilots;

    if (prb_idx >= nof_prb || port >= nof_ports || cdm >= nof_cdm) {
        // Still participate in the block-wide done handshake below.
    } else {
        float residual_sum = mimo_dmrs_residual_pilot_sum(
            d_grid_cbf16, d_dmrs_pilot_bits, d_dmrs_symbol_indices, d_estimates_fp16,
            nof_prb, nof_ports, nof_layers, nof_dmrs_symbols, grid_stride, symbol_stride,
            start_prb, dmrs_scaling, words_per_dmrs_sym, d_cfo_hz_ptr, d_symbol_start_times,
            d_cfo_phasors, dmrs_type, prb_idx, port, cdm, pilot_re);
        atomicAdd(&d_noise_var_accum[port], residual_sum);
    }
    __syncthreads();

    if (threadIdx.x == 0 && threadIdx.y == 0 && d_done_counter != nullptr) {
        __threadfence();
        unsigned int done = atomicAdd(d_done_counter, 1u) + 1;
        if (done == (unsigned int)nof_prb) {
            int nof_dmrs_pilots = nof_prb * nof_dmrs_symbols * nof_pilots;
            int divisor = nof_dmrs_pilots * nof_cdm - 1;
            for (int p = 0; p < nof_ports; p++) {
                float noise_var = (divisor > 0) ? (d_noise_var_accum[p] / (float)divisor) : 0.001f;
                d_noise_vars_out[p] = fmaxf(noise_var, 1e-6f);
            }
        }
    }
}

/**
 * Multi-block residual-based SINR/EPRE kernel (v3).
 *
 * Same algorithm as v2 but parallelized across PRBs:
 * - Launch <<<nof_prb, nof_ports * 6>>> — one block per PRB
 * - Each thread handles one (port, pilot) pair
 * - Global atomics to per-port accumulators (max 8 ports, fine granularity)
 *
 * Requires separate kernel_finalize_sinr_epre to convert accumulators to dB.
 */
__global__ void kernel_compute_residual_sinr_epre_v3(
    const unsigned int* __restrict__ d_grid_cbf16,
    const __half2* __restrict__ d_estimates_fp16,
    const uint32_t* __restrict__ d_dmrs_pilot_bits,
    const int* __restrict__ d_dmrs_symbol_indices,
    float* __restrict__ d_sinr_noise_accum,
    float* __restrict__ d_sinr_rsrp_accum,
    int nof_prb,
    int nof_ports,
    int nof_dmrs_symbols,
    int grid_stride,
    int symbol_stride,
    int start_prb,
    float dmrs_scaling,
    int words_per_dmrs_sym,
    int dmrs_type)
{
    int prb_idx = blockIdx.x;
    int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);
    int port = threadIdx.x / nof_pilots;
    int d_pilot = threadIdx.x % nof_pilots;

    if (prb_idx >= nof_prb || port >= nof_ports) return;

    int re_per_dmrs_sym = nof_prb * nof_ports * 12;
    int sc_offset = pusch_dmrs_sc_offset(dmrs_type, 0, d_pilot);
    float pilot_amp = dmrs_scaling * 0.70710678118f;

    /* Time-average the smoothed channel estimates across DMRS symbols */
    float h_avg_re = 0.0f, h_avg_im = 0.0f;
    for (int d = 0; d < nof_dmrs_symbols; d++) {
        int est_idx = d * re_per_dmrs_sym + prb_idx * (nof_ports * 12) + port * 12 + sc_offset;
        __half2 h = d_estimates_fp16[est_idx];
        h_avg_re += __half2float(__low2half(h));
        h_avg_im += __half2float(__high2half(h));
    }
    float inv_n = 1.0f / (float)nof_dmrs_symbols;
    h_avg_re *= inv_n;
    h_avg_im *= inv_n;

    float local_rsrp = h_avg_re * h_avg_re + h_avg_im * h_avg_im;
    float local_noise = 0.0f;

    /* Compute residual noise for each DMRS symbol */
    int pilot_idx = (start_prb + prb_idx) * nof_pilots + d_pilot;
    int pilot_word_idx = pilot_idx / 16;
    int pilot_bit_offset = (pilot_idx % 16) * 2;

    for (int d = 0; d < nof_dmrs_symbols; d++) {
        int dmrs_symbol = d_dmrs_symbol_indices[d];
        int sc = (start_prb + prb_idx) * 12 + sc_offset;

        int grid_idx = port * grid_stride + dmrs_symbol * symbol_stride + sc;
        cuFloatComplex y = cbf16_to_fp32(d_grid_cbf16[grid_idx]);

        const uint32_t* dmrs_seq = d_dmrs_pilot_bits + d * words_per_dmrs_sym;
        uint32_t pilot_word = dmrs_seq[pilot_word_idx];
        int c_real = (pilot_word >> pilot_bit_offset) & 1;
        int c_imag = (pilot_word >> (pilot_bit_offset + 1)) & 1;

        float p_real = (1 - 2 * c_real) * pilot_amp;
        float p_imag = (1 - 2 * c_imag) * pilot_amp;

        float pred_real = h_avg_re * p_real - h_avg_im * p_imag;
        float pred_imag = h_avg_re * p_imag + h_avg_im * p_real;

        float res_real = y.x - pred_real;
        float res_imag = y.y - pred_imag;

        local_noise += res_real * res_real + res_imag * res_imag;
    }

    atomicAdd(&d_sinr_noise_accum[port], local_noise);
    atomicAdd(&d_sinr_rsrp_accum[port], local_rsrp);
}

/**
 * Finalize SINR/EPRE/RSRP from accumulated residual noise and signal power.
 * Launched <<<1, 1>>> — single thread doing dB conversion.
 */
__global__ void kernel_finalize_sinr_epre(
    const float* __restrict__ d_sinr_noise_accum,
    const float* __restrict__ d_sinr_rsrp_accum,
    const float* __restrict__ d_epre_accum,
    float* __restrict__ d_noise_vars_out,
    float* __restrict__ d_sinr_epre_result,
    int nof_prb,
    int nof_ports,
    int nof_dmrs_symbols,
    float dmrs_scaling,
    int dmrs_type)
{
    int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);
    int nof_pilot_positions = nof_prb * nof_pilots;
    int nof_dmrs_re_per_port = nof_prb * nof_dmrs_symbols * nof_pilots;

    float total_signal = 0.0f, total_noise = 0.0f, total_epre = 0.0f;
    for (int p = 0; p < nof_ports; p++) {
        float signal_power = d_sinr_rsrp_accum[p] / (float)nof_pilot_positions;
        float noise_var = d_sinr_noise_accum[p] / (float)(nof_dmrs_re_per_port - 1);
        noise_var = fmaxf(noise_var, signal_power * 1e-10f);
        if (d_noise_vars_out != nullptr) {
            d_noise_vars_out[p] = noise_var;
        }
        float mean_epre_port = d_epre_accum[p] / (float)nof_dmrs_re_per_port;
        total_signal += signal_power;
        total_noise += noise_var;
        total_epre += mean_epre_port;
    }

    if (d_sinr_epre_result == nullptr) {
        return;
    }

    float mean_noise = total_noise / nof_ports;
    d_sinr_epre_result[0] = (mean_noise > 1e-10f)
        ? 10.0f * log10f(total_signal / mean_noise) : 60.0f;
    float mean_epre = total_epre / nof_ports;
    d_sinr_epre_result[1] = (mean_epre > 1e-10f)
        ? 10.0f * log10f(mean_epre) : -INFINITY;
    /* RSRP: averaged per-port signal power in dB.
     * d_sinr_rsrp_accum stores |H|² where H = y/(pilot*dmrs_scaling),
     * so multiply by dmrs_scaling² to recover actual received signal power. */
    float mean_rsrp = (total_signal / nof_ports) * dmrs_scaling * dmrs_scaling;
    d_sinr_epre_result[2] = (mean_rsrp > 1e-10f)
        ? 10.0f * log10f(mean_rsrp) : -INFINITY;
}

__global__ void kernel_reset_sinr_epre_result(float* __restrict__ d_sinr_epre_result)
{
    if (d_sinr_epre_result == nullptr) {
        return;
    }
    d_sinr_epre_result[0] = -INFINITY; /* SINR */
    d_sinr_epre_result[1] = -INFINITY; /* EPRE */
    d_sinr_epre_result[2] = -INFINITY; /* RSRP */
    d_sinr_epre_result[3] = NAN;       /* TA */
    d_sinr_epre_result[4] = NAN;       /* CFO */
}

__device__ __forceinline__ float2 pusch_phasor_mul(float2 a, float2 b)
{
    return make_float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

__device__ __forceinline__ float2 pusch_phasor_conj(float2 a)
{
    return make_float2(a.x, -a.y);
}

__device__ void pusch_fill_cfo_phasor_table(float cfo_hz,
                                             const float* __restrict__ d_symbol_start_times,
                                             const int* __restrict__ d_dmrs_symbol_indices,
                                             float2* __restrict__ d_cfo_phasors,
                                             int nof_dmrs_symbols,
                                             int start_symbol)
{
    if (d_cfo_phasors == nullptr) {
        return;
    }

    float2 sym_phasors[MAX_SYMBOLS];
    if (cfo_hz == 0.0f || d_symbol_start_times == nullptr ||
        d_dmrs_symbol_indices == nullptr || nof_dmrs_symbols <= 0) {
        #pragma unroll
        for (int s = 0; s < MAX_SYMBOLS; ++s) {
            sym_phasors[s] = make_float2(1.0f, 0.0f);
        }
    } else {
        float omega = 6.2831853071795864f * cfo_hz;
        float sn, cs;
        sincosf(omega * d_symbol_start_times[0], &sn, &cs);
        sym_phasors[0] = make_float2(cs, sn);

        float cached_dt[3];
        float2 cached_step[3];
        int cached_count = 0;

        #pragma unroll
        for (int sidx = 1; sidx < MAX_SYMBOLS; ++sidx) {
            float dt = d_symbol_start_times[sidx] - d_symbol_start_times[sidx - 1];
            int cache_idx = -1;
            #pragma unroll
            for (int c = 0; c < 3; ++c) {
                if (c < cached_count && fabsf(dt - cached_dt[c]) < 1.0e-12f) {
                    cache_idx = c;
                }
            }
            if (cache_idx < 0) {
                float step_s, step_c;
                sincosf(omega * dt, &step_s, &step_c);
                cache_idx = cached_count;
                if (cached_count < 3) {
                    cached_dt[cached_count] = dt;
                    cached_step[cached_count] = make_float2(step_c, step_s);
                    cached_count++;
                } else {
                    cached_step[cache_idx - 1] = make_float2(step_c, step_s);
                    cache_idx--;
                }
            }
            sym_phasors[sidx] = pusch_phasor_mul(sym_phasors[sidx - 1], cached_step[cache_idx]);
        }
    }

    #pragma unroll
    for (int data_symbol = 0; data_symbol < MAX_SYMBOLS; ++data_symbol) {
        #pragma unroll
        for (int d = 0; d < MAX_DMRS_SYMBOLS; ++d) {
            float2 ph = make_float2(1.0f, 0.0f);
            if (d < nof_dmrs_symbols) {
                int dmrs_symbol = d_dmrs_symbol_indices[d] + start_symbol;
                if ((unsigned)dmrs_symbol < MAX_SYMBOLS) {
                    ph = pusch_phasor_mul(sym_phasors[data_symbol],
                                          pusch_phasor_conj(sym_phasors[dmrs_symbol]));
                }
            }
            d_cfo_phasors[data_symbol * MAX_DMRS_SYMBOLS + d] = ph;
        }
    }

    #pragma unroll
    for (int target_dmrs_idx = 0; target_dmrs_idx < MAX_DMRS_SYMBOLS; ++target_dmrs_idx) {
        #pragma unroll
        for (int source_dmrs_idx = 0; source_dmrs_idx < MAX_DMRS_SYMBOLS; ++source_dmrs_idx) {
            float2 ph = make_float2(1.0f, 0.0f);
            if (target_dmrs_idx < nof_dmrs_symbols && source_dmrs_idx < nof_dmrs_symbols) {
                int target_symbol = d_dmrs_symbol_indices[target_dmrs_idx];
                int source_symbol = d_dmrs_symbol_indices[source_dmrs_idx];
                if ((unsigned)target_symbol < MAX_SYMBOLS && (unsigned)source_symbol < MAX_SYMBOLS) {
                    ph = pusch_phasor_mul(sym_phasors[target_symbol],
                                          pusch_phasor_conj(sym_phasors[source_symbol]));
                }
            }
            d_cfo_phasors[MIMO_CFO_RESIDUAL_PHASOR_OFFSET +
                          target_dmrs_idx * MAX_DMRS_SYMBOLS + source_dmrs_idx] = ph;
        }
    }
}

/**
 * Fused Time Alignment (TA) + Carrier Frequency Offset (CFO) estimation
 * from DMRS channel estimates.
 *
 * TA algorithm (phase slope across frequency):
 * 1. Time-average pilot estimates into shared memory
 * 2. Cross-correlate adjacent pilots: sum_k H_avg[k+1] * conj(H_avg[k])
 * 3. ta_seconds = -atan2f(imag, real) / (2 * PI * pilot_spacing_hz)
 *
 * CFO algorithm (phase rotation across time, requires >= 2 DMRS symbols):
 * 1. Store first and last DMRS symbol estimates in shared memory
 * 2. Cross-correlate all pilots: sum_k H_last[k] * conj(H_first[k])
 * 3. cfo_hz = atan2f(imag, real) / (2 * PI * delta_t_s)
 *
 * Shared memory: 3 arrays × nof_pilots × 2 floats (avg, sym0, sym_last).
 * Launched <<<nof_ports, 256, smem>>> — one block per port for parallel execution.
 * Uses atomicAdd to accumulate cross-port results, last block computes atan2.
 */
__global__ void kernel_compute_ta_and_cfo(
    const __half2* __restrict__ d_estimates_fp16,
    float* __restrict__ d_ta_result,
    float* __restrict__ d_cfo_result,
    float* __restrict__ d_accum,   /* [ta_r, ta_i, cfo_r, cfo_i, done_counter] — pre-zeroed */
    int nof_prb,
    int nof_ports,
    int nof_dmrs_symbols,
    float pilot_spacing_hz,
    float delta_t_s,
    const float* __restrict__ d_symbol_start_times,
    const int* __restrict__ d_dmrs_symbol_indices,
    float2* __restrict__ d_cfo_phasors,
    int start_symbol,
    int dmrs_type)
{
    extern __shared__ float s_data[];

    int pilots_per_prb = pusch_dmrs_pilots_per_cdm(dmrs_type);
    int nof_pilots = nof_prb * pilots_per_prb;
    float* s_avg      = s_data;
    float* s_sym0     = s_data + nof_pilots * 2;
    float* s_sym_last = s_data + nof_pilots * 4;
    /* Cross-warp reduction scratch: 4 accumulators × up to 8 warps, after pilot arrays. */
    float* s_warp_reduce = s_data + nof_pilots * 6;
    int tid = threadIdx.x;
    int nthreads = blockDim.x;
    int warp_id = tid / 32;
    int lane    = tid % 32;
    int nwarps  = nthreads / 32;
    int port    = blockIdx.x;  /* Each block handles one port */

    /* Phase 1: Load all DMRS symbols for this port, store sym0, sym_last, and average.
     * Layout: [dmrs_sym][prb][port][12 subcarriers] as __half2. */
    for (int pilot = tid; pilot < nof_pilots; pilot += nthreads) {
        int prb = pilot / pilots_per_prb;
        int sc_in_prb = pusch_dmrs_sc_offset(dmrs_type, 0, pilot % pilots_per_prb);
        float sum_r = 0.0f, sum_i = 0.0f;
        for (int d = 0; d < nof_dmrs_symbols; d++) {
            int idx = d * nof_prb * nof_ports * 12
                    + prb * nof_ports * 12 + port * 12 + sc_in_prb;
            __half2 h = d_estimates_fp16[idx];
            float r = __half2float(h.x), i = __half2float(h.y);
            sum_r += r; sum_i += i;
            if (d == 0) {
                s_sym0[pilot * 2] = r; s_sym0[pilot * 2 + 1] = i;
            }
            if (d == nof_dmrs_symbols - 1) {
                s_sym_last[pilot * 2] = r; s_sym_last[pilot * 2 + 1] = i;
            }
        }
        s_avg[pilot * 2]     = sum_r / nof_dmrs_symbols;
        s_avg[pilot * 2 + 1] = sum_i / nof_dmrs_symbols;
    }
    __syncthreads();

    /* Phase 2a: TA — adjacent-frequency cross-correlation on averaged estimates. */
    float ta_lr = 0.0f, ta_li = 0.0f;
    for (int k = 1 + tid; k < nof_pilots; k += nthreads) {
        float cr = s_avg[k * 2],       ci = s_avg[k * 2 + 1];
        float pr = s_avg[(k - 1) * 2], pi = s_avg[(k - 1) * 2 + 1];
        ta_lr += cr * pr + ci * pi;   /* Re{H[k] · conj(H[k-1])} */
        ta_li += ci * pr - cr * pi;   /* Im{H[k] · conj(H[k-1])} */
    }

    /* Phase 2b: CFO — all-pilot time cross-correlation (sym_last vs sym0). */
    float cfo_lr = 0.0f, cfo_li = 0.0f;
    if (d_cfo_result) {
        for (int k = tid; k < nof_pilots; k += nthreads) {
            float h0r = s_sym0[k * 2],     h0i = s_sym0[k * 2 + 1];
            float h1r = s_sym_last[k * 2], h1i = s_sym_last[k * 2 + 1];
            cfo_lr += h1r * h0r + h1i * h0i;   /* Re{H_last[k] · conj(H_first[k])} */
            cfo_li += h1i * h0r - h1r * h0i;   /* Im{H_last[k] · conj(H_first[k])} */
        }
    }

    /* Phase 3: Intra-warp shuffle reduction (4 accumulators). */
    for (int off = 16; off > 0; off >>= 1) {
        ta_lr  += __shfl_down_sync(0xFFFFFFFF, ta_lr,  off);
        ta_li  += __shfl_down_sync(0xFFFFFFFF, ta_li,  off);
        cfo_lr += __shfl_down_sync(0xFFFFFFFF, cfo_lr, off);
        cfo_li += __shfl_down_sync(0xFFFFFFFF, cfo_li, off);
    }

    /* Phase 4: Cross-warp reduction via shared memory. */
    if (lane == 0) {
        s_warp_reduce[warp_id * 4 + 0] = ta_lr;
        s_warp_reduce[warp_id * 4 + 1] = ta_li;
        s_warp_reduce[warp_id * 4 + 2] = cfo_lr;
        s_warp_reduce[warp_id * 4 + 3] = cfo_li;
    }
    __syncthreads();

    /* Phase 5: Thread 0 reduces across warps, then atomicAdd to global accumulators. */
    if (tid == 0) {
        float ta_r = 0.0f, ta_i = 0.0f, cfo_r = 0.0f, cfo_i = 0.0f;
        for (int w = 0; w < nwarps; w++) {
            ta_r  += s_warp_reduce[w * 4 + 0];
            ta_i  += s_warp_reduce[w * 4 + 1];
            cfo_r += s_warp_reduce[w * 4 + 2];
            cfo_i += s_warp_reduce[w * 4 + 3];
        }
        atomicAdd(&d_accum[0], ta_r);
        atomicAdd(&d_accum[1], ta_i);
        atomicAdd(&d_accum[2], cfo_r);
        atomicAdd(&d_accum[3], cfo_i);

        /* Last block to finish computes atan2 and writes final results. */
        __threadfence();
        int done = atomicAdd((int*)&d_accum[4], 1) + 1;
        if (done == nof_ports) {
            float ta_phase = atan2f(d_accum[1], d_accum[0]);
            d_ta_result[0] = -ta_phase / (2.0f * 3.14159265358979f * pilot_spacing_hz);

            if (d_cfo_result && delta_t_s > 0.0f) {
                float cfo_phase = atan2f(d_accum[3], d_accum[2]);
                float cfo_hz = cfo_phase / (2.0f * 3.14159265358979f * delta_t_s);
                d_cfo_result[0] = cfo_hz;
                pusch_fill_cfo_phasor_table(cfo_hz,
                                             d_symbol_start_times,
                                             d_dmrs_symbol_indices,
                                             d_cfo_phasors,
                                             nof_dmrs_symbols,
                                             start_symbol);
            }
        }
    }
}

/**
 * MIMO CSI helper.
 *
 * Uses the layer-0 channel estimates to estimate TA/CFO. All layers use the
 * same propagation timing and oscillator offset, so layer 0 is enough and keeps
 * the kernel small. RSRP is averaged across all estimated layers and ports for
 * scheduler reporting.
 */
/*
 * MIMO CSI (TA/CFO/RSRP/EPRE) from channel estimates.
 *
 * Multi-block: each thread owns one pilot index across the allocation so large
 * 100 MHz / 8-port cases use many SMs instead of a single 256-thread block.
 * Partial sums reduce per warp → block → global atomics; the last block
 * finalizes atan2/log metrics and optional CFO phasor table. Metrics semantics
 * match the prior single-block kernel.
 *
 * d_accum layout: [ta_r, ta_i, cfo_r, cfo_i, rsrp_sum, epre_sum] (pre-zeroed).
 * d_done_counter: pre-zeroed unsigned used only for last-block finalize, then
 *                 reset to 0 so residual noise may reuse it.
 */
__global__ void kernel_compute_mimo_csi_from_estimates(
    const unsigned int* __restrict__ d_grid_cbf16,
    const __half2* __restrict__ d_estimates_fp16,
    const int* __restrict__ d_dmrs_symbol_indices,
    float* __restrict__ d_sinr_epre_result,
    float* __restrict__ d_accum,
    unsigned int* __restrict__ d_done_counter,
    int nof_prb,
    int nof_ports,
    int nof_layers,
    int nof_dmrs_symbols,
    int grid_stride,
    int symbol_stride,
    int start_prb,
    float pilot_spacing_hz,
    float delta_t_s,
    const float* __restrict__ d_symbol_start_times,
    float2* __restrict__ d_cfo_phasors,
    int start_symbol,
    float dmrs_scaling,
    int dmrs_type)
{
    int tid = threadIdx.x;
    int pilots_per_prb = pusch_dmrs_pilots_per_cdm(dmrs_type);
    int total_pilots = nof_prb * pilots_per_prb;
    int re_per_port = nof_layers * 12;
    int re_per_prb = nof_ports * re_per_port;
    int re_per_dmrs_sym = nof_prb * re_per_prb;

    float ta_r = 0.0f;
    float ta_i = 0.0f;
    float cfo_r = 0.0f;
    float cfo_i = 0.0f;
    float rsrp_sum = 0.0f;
    float epre_sum = 0.0f;

    for (int pilot = blockIdx.x * blockDim.x + tid; pilot < total_pilots;
         pilot += blockDim.x * gridDim.x) {
        int prb = pilot / pilots_per_prb;
        int sc_in_prb = pusch_dmrs_sc_offset(dmrs_type, 0, pilot % pilots_per_prb);

        /* TA and CFO from layer 0, accumulated across ports. */
        for (int port = 0; port < nof_ports; ++port) {
            int base = prb * re_per_prb + port * re_per_port + sc_in_prb;

            float avg_re = 0.0f;
            float avg_im = 0.0f;
            float first_re = 0.0f;
            float first_im = 0.0f;
            float last_re = 0.0f;
            float last_im = 0.0f;

            for (int d = 0; d < nof_dmrs_symbols; ++d) {
                int idx = d * re_per_dmrs_sym + base;
                __half2 h = d_estimates_fp16[idx];
                float re = __half2float(__low2half(h));
                float im = __half2float(__high2half(h));
                avg_re += re;
                avg_im += im;
                if (d == 0) {
                    first_re = re;
                    first_im = im;
                }
                if (d == nof_dmrs_symbols - 1) {
                    last_re = re;
                    last_im = im;
                }
            }

            avg_re /= (float)nof_dmrs_symbols;
            avg_im /= (float)nof_dmrs_symbols;

            if (pilot > 0) {
                int prev_pilot = pilot - 1;
                int prev_prb = prev_pilot / pilots_per_prb;
                int prev_sc = pusch_dmrs_sc_offset(dmrs_type, 0, prev_pilot % pilots_per_prb);
                int prev_base = prev_prb * re_per_prb + port * re_per_port + prev_sc;
                float prev_re = 0.0f;
                float prev_im = 0.0f;
                for (int d = 0; d < nof_dmrs_symbols; ++d) {
                    int prev_idx = d * re_per_dmrs_sym + prev_base;
                    __half2 prev_h = d_estimates_fp16[prev_idx];
                    prev_re += __half2float(__low2half(prev_h));
                    prev_im += __half2float(__high2half(prev_h));
                }
                prev_re /= (float)nof_dmrs_symbols;
                prev_im /= (float)nof_dmrs_symbols;

                ta_r += avg_re * prev_re + avg_im * prev_im;
                ta_i += avg_im * prev_re - avg_re * prev_im;
            }

            if (nof_dmrs_symbols >= 2) {
                cfo_r += last_re * first_re + last_im * first_im;
                cfo_i += last_im * first_re - last_re * first_im;
            }
        }

        /* RSRP from all layers and ports. */
        for (int d = 0; d < nof_dmrs_symbols; ++d) {
            int dmrs_symbol = d_dmrs_symbol_indices[d];
            for (int port = 0; port < nof_ports; ++port) {
                int grid_sc = (start_prb + prb) * 12 + sc_in_prb;
                int grid_idx = port * grid_stride + dmrs_symbol * symbol_stride + grid_sc;
                cuFloatComplex y = cbf16_to_fp32(d_grid_cbf16[grid_idx]);
                epre_sum += y.x * y.x + y.y * y.y;

                for (int layer = 0; layer < nof_layers; ++layer) {
                    int idx = d * re_per_dmrs_sym + prb * re_per_prb + port * re_per_port + layer * 12 + sc_in_prb;
                    __half2 h = d_estimates_fp16[idx];
                    float re = __half2float(__low2half(h));
                    float im = __half2float(__high2half(h));
                    rsrp_sum += re * re + im * im;
                }
            }
        }
    }

    /* Warp then block reduce before a single global atomic per accumulator. */
    auto warp_sum = [](float v) {
        for (int offset = 16; offset > 0; offset >>= 1) {
            v += __shfl_down_sync(0xffffffff, v, offset);
        }
        return v;
    };
    ta_r = warp_sum(ta_r);
    ta_i = warp_sum(ta_i);
    cfo_r = warp_sum(cfo_r);
    cfo_i = warp_sum(cfo_i);
    rsrp_sum = warp_sum(rsrp_sum);
    epre_sum = warp_sum(epre_sum);

    __shared__ float s_ta_r[8], s_ta_i[8], s_cfo_r[8], s_cfo_i[8], s_rsrp[8], s_epre[8];
    int warp = tid >> 5;
    int lane = tid & 31;
    int nwarps = (blockDim.x + 31) >> 5;
    if (lane == 0 && warp < 8) {
        s_ta_r[warp] = ta_r;
        s_ta_i[warp] = ta_i;
        s_cfo_r[warp] = cfo_r;
        s_cfo_i[warp] = cfo_i;
        s_rsrp[warp] = rsrp_sum;
        s_epre[warp] = epre_sum;
    }
    __syncthreads();

    if (tid == 0) {
        float b_ta_r = 0, b_ta_i = 0, b_cfo_r = 0, b_cfo_i = 0, b_rsrp = 0, b_epre = 0;
        for (int w = 0; w < nwarps && w < 8; ++w) {
            b_ta_r += s_ta_r[w];
            b_ta_i += s_ta_i[w];
            b_cfo_r += s_cfo_r[w];
            b_cfo_i += s_cfo_i[w];
            b_rsrp += s_rsrp[w];
            b_epre += s_epre[w];
        }
        atomicAdd(&d_accum[0], b_ta_r);
        atomicAdd(&d_accum[1], b_ta_i);
        atomicAdd(&d_accum[2], b_cfo_r);
        atomicAdd(&d_accum[3], b_cfo_i);
        atomicAdd(&d_accum[4], b_rsrp);
        atomicAdd(&d_accum[5], b_epre);
    }
    __syncthreads();
    __threadfence();

    __shared__ int s_is_last;
    if (tid == 0) {
        unsigned int done = atomicAdd(d_done_counter, 1u) + 1u;
        s_is_last = (done == (unsigned int)gridDim.x) ? 1 : 0;
    }
    __syncthreads();
    if (!s_is_last) {
        return;
    }

    if (tid == 0) {
        float ta_phase = atan2f(d_accum[1], d_accum[0]);
        d_sinr_epre_result[3] = -ta_phase / (2.0f * 3.14159265358979f * pilot_spacing_hz);

        if (nof_dmrs_symbols >= 2 && delta_t_s > 0.0f) {
            float cfo_phase = atan2f(d_accum[3], d_accum[2]);
            float cfo_hz = cfo_phase / (2.0f * 3.14159265358979f * delta_t_s);
            d_sinr_epre_result[4] = cfo_hz;
            pusch_fill_cfo_phasor_table(cfo_hz,
                                         d_symbol_start_times,
                                         d_dmrs_symbol_indices,
                                         d_cfo_phasors,
                                         nof_dmrs_symbols,
                                         start_symbol);
        } else {
            d_sinr_epre_result[4] = NAN;
        }

        int rsrp_count = total_pilots * nof_dmrs_symbols * nof_ports * nof_layers;
        float mean_rsrp = (rsrp_count > 0) ? (d_accum[4] / (float)rsrp_count) * dmrs_scaling * dmrs_scaling : 0.0f;
        d_sinr_epre_result[2] = (mean_rsrp > 1e-10f) ? 10.0f * log10f(mean_rsrp) : -INFINITY;

        int epre_count = total_pilots * nof_dmrs_symbols * nof_ports;
        float layer_norm = (float)nof_layers * 0.25f;
        float mean_epre =
            (epre_count > 0) ? (d_accum[5] / (float)epre_count) * dmrs_scaling * dmrs_scaling * layer_norm : 0.0f;
        d_sinr_epre_result[1] = (mean_epre > 1e-10f) ? 10.0f * log10f(mean_epre) : -INFINITY;

        /* Leave done counter clear for residual-noise kernel reuse. */
        *d_done_counter = 0u;
    }
}

/**
 * Pre-generate scrambling sequence kernel.
 *
 * Each thread generates 32 consecutive bits (one uint32_t word).
 * Thread i generates bits [i*32, (i+1)*32).
 *
 * This eliminates per-RE LFSR advancement in the E2E kernel, replacing
 * O(log N) matrix operations per RE with a single memory load.
 *
 * Memory: ceil(nof_bits / 32) * 4 bytes (e.g., 30 KB for 100 MHz 64-QAM)
 */
__global__ void kernel_generate_scrambling_sequence(
    uint32_t* __restrict__ d_scrambling_seq,
    uint32_t c_init,
    int nof_bits)
{
    int word_idx = blockIdx.x * blockDim.x + threadIdx.x;
    int bit_start = word_idx * 32;

    if (bit_start >= nof_bits) return;

    /* Initialize LFSRs */
    uint32_t x1 = 1;
    uint32_t x2 = c_init & 0x7FFFFFFF;

    /* Jump to starting position: Nc + word_idx*32 */
    int total_advance = NC_SKIP + bit_start;
    x1 = advance_x1_local(x1, total_advance);
    x2 = advance_x2_local(x2, total_advance);

    /* Generate 32 bits */
    uint32_t packed = 0;
    int bits_to_generate = min(32, nof_bits - bit_start);

    #pragma unroll
    for (int b = 0; b < 32; b++) {
        if (b < bits_to_generate) {
            uint32_t scr_bit = (x1 ^ x2) & 1;
            packed |= (scr_bit << b);

            /* Step LFSRs */
            uint32_t new_x1 = ((x1 >> 3) ^ x1) & 1;
            x1 = (x1 >> 1) | (new_x1 << 30);

            uint32_t new_x2 = ((x2 >> 3) ^ (x2 >> 2) ^ (x2 >> 1) ^ x2) & 1;
            x2 = (x2 >> 1) | (new_x2 << 30);
        }
    }

    d_scrambling_seq[word_idx] = packed;
}

/* soft_demod_64qam/256qam_piecewise -> pusch_e2e_qam_demod.cuh */

/* ============================================================================
 * ULTRA-OPTIMIZED E2E KERNEL v2: Cached scrambling + reductions
 *
 * Optimizations:
 * 1. Cached scrambling sequence: use configure-time Gold sequence precompute
 *    when available; keep warp-cooperative generation as a fallback.
 * 2. Block-level noise variance reduction via warp shuffle
 * 3. Template on NOF_DMRS for compile-time unrolling
 * 4. Vectorized half2 LLR writes
 * ============================================================================ */

/**
 * Warp-cooperative soft demod helper for 256QAM
 * Computes 8 LLRs per RE with proper max-log approximation
 */
__device__ __forceinline__ void soft_demod_256qam_optimized(
    cuFloatComplex eq, float inv_noise, float llr_vals[8])
{
    const float s = 0.07669650f;  // 1/sqrt(170)
    float abs_re = fabsf(eq.x);
    float abs_im = fabsf(eq.y);
    float scale = inv_noise * s * 2.0f;

    llr_vals[0] = eq.x * scale;
    llr_vals[1] = eq.y * scale;
    llr_vals[2] = (s * 8.0f - abs_re) * scale;
    llr_vals[3] = (s * 8.0f - abs_im) * scale;

    float level2 = s * 4.0f;
    llr_vals[4] = (level2 - fabsf(abs_re - level2 * 2.0f)) * scale;
    llr_vals[5] = (level2 - fabsf(abs_im - level2 * 2.0f)) * scale;

    float level3 = s * 2.0f;
    float d_re = fabsf(abs_re - level2 * 2.0f);
    float d_im = fabsf(abs_im - level2 * 2.0f);
    llr_vals[6] = (level3 - fabsf(d_re - level3 * 2.0f)) * scale;
    llr_vals[7] = (level3 - fabsf(d_im - level3 * 2.0f)) * scale;
}

/**
 * Piecewise-linear interval function for 64QAM soft demodulation
 * Implements optimal LLR approximation per TS 38.211
 *
 * @param value        Input symbol component (real or imag)
 * @param inv_noise    1/noise_variance scaling factor
 * @param interval_width  Width of each interval (2*M or 4*M)
 * @param num_intervals   Number of intervals (8 for bits 0-3, 4 for bits 4-5)
 * @param slopes       Slope lookup table
 * @param intercepts   Intercept lookup table
 * @return Scaled LLR value
 */
__device__ __forceinline__ float interval_function_64qam(
    float value,
    float inv_noise,
    float interval_width,
    int num_intervals,
    const float* slopes,
    const float* intercepts)
{
    /* Determine interval index: map value to [-num_intervals/2, num_intervals/2) range */
    float normalized = value / interval_width + (float)num_intervals * 0.5f;
    int idx = (int)floorf(normalized);

    /* Clamp to valid interval range [0, num_intervals-1] */
    idx = max(0, min(num_intervals - 1, idx));

    /* Apply piecewise-linear formula: LLR = (slope * value + intercept) / noise_var */
    return (slopes[idx] * value + intercepts[idx]) * inv_noise;
}

/**
 * Warp-level float reduction for noise variance accumulation
 * (Named with _f32 suffix to avoid conflict with later definition)
 */
__device__ __forceinline__ float warp_reduce_sum_f32(float val) {
    #pragma unroll
    for (int offset = 16; offset > 0; offset /= 2) {
        val += __shfl_down_sync(0xffffffff, val, offset);
    }
    return val;
}

/**
 * OPTIMIZED E2E kernel with cached scrambling and warp-cooperative fallback.
 * Template parameters: NOF_PORTS, ALGORITHM, NOF_DMRS (for unrolling)
 */
template <int NOF_PORTS, int ALGORITHM, int NOF_DMRS, bool TIME_AVG = false, int MOD_ORDER = 2>
__global__ void __launch_bounds__(256, 4) kernel_fused_e2e_warp_cooperative_fp16(
    const unsigned int* __restrict__ d_grid_cbf16,
    const __half2* __restrict__ d_dmrs_estimates_fp16,
    const int* __restrict__ re_indices,
    const float* __restrict__ noise_vars,
    __half* __restrict__ llrs_half,
    const uint32_t* __restrict__ scramble_seq,
    uint32_t scramble_c_init,
    float* __restrict__ d_eq_noise_var_sum,
    unsigned int* __restrict__ d_eq_noise_var_count,
    float* __restrict__ d_evm_error_sum,
    unsigned int* __restrict__ d_evm_symbol_count,
    int nof_re,
    int nof_prb,
    int grid_stride,
    int symbol_stride,
    int start_symbol,
    int start_subcarrier,
    float tx_scaling,
    const int* __restrict__ d_dmrs_indices,
    const float* __restrict__ d_cfo_hz_ptr,
    const float* __restrict__ d_symbol_start_times,
    const float2* __restrict__ d_cfo_phasors,
    float2* __restrict__ d_eq_symbols_out,
    float* __restrict__ d_eq_noise_var_out)
{
    const int WARP_SIZE = 32;
    const int WARPS_PER_BLOCK = 8;  // 256 threads / 32
    constexpr int mod_order = MOD_ORDER;

    int tid = threadIdx.x;
    int warp_id = tid / WARP_SIZE;
    int lane = tid & (WARP_SIZE - 1);
    int global_thread_id = blockIdx.x * blockDim.x + tid;

    /* ================================================================
     * Shared memory layout:
     * - Noise variances (8 floats)
     * - DMRS symbol indices (up to 4 ints)
     * - Fallback scrambling bits per warp (32 threads x 8 bits max = 256 bytes per warp)
     * - Partial sums for noise variance reduction
     * ================================================================ */
    __shared__ float s_noise_vars[8];
    __shared__ int s_dmrs_indices[4];
    __shared__ uint8_t s_scramble_bits[WARPS_PER_BLOCK][WARP_SIZE * 8];
    __shared__ float s_noise_var_sums[WARPS_PER_BLOCK];
    __shared__ int s_noise_var_counts[WARPS_PER_BLOCK];
    __shared__ float s_evm_error_sums[WARPS_PER_BLOCK];
    __shared__ int s_evm_symbol_counts[WARPS_PER_BLOCK];
    __shared__ float s_cfo_hz;
    __shared__ float s_sym_times[14];
    __shared__ float2 s_cfo_phasors[MIMO_CFO_DATA_PHASOR_COUNT];
    bool use_cfo_fallback =
        (d_cfo_hz_ptr != nullptr && d_cfo_phasors == nullptr && d_symbol_start_times != nullptr);

    /* Load noise variances, DMRS indices, CFO, and symbol times to shared memory */
    if (tid < NOF_PORTS) {
        s_noise_vars[tid] = noise_vars[tid];
    }
    if (tid < NOF_DMRS) {
        s_dmrs_indices[tid] = d_dmrs_indices[tid];
    }
    if (tid == 0) {
        s_cfo_hz = (d_cfo_hz_ptr != nullptr) ? *d_cfo_hz_ptr : 0.0f;
    }
    if (use_cfo_fallback && tid < 14) {
        s_sym_times[tid] = d_symbol_start_times[tid];
    }
    if (d_cfo_phasors != nullptr) {
        for (int i = tid; i < MIMO_CFO_DATA_PHASOR_COUNT; i += blockDim.x) {
            s_cfo_phasors[i] = d_cfo_phasors[i];
        }
    }
    __syncthreads();

    /* ================================================================
     * OPTIMIZATION #1: Cached scrambling sequence.
     * The normal path consumes the configure-time precomputed Gold sequence.
     * The fallback keeps the previous warp-cooperative generation for tests
     * and for configurations where the cached sequence is unavailable.
     * ================================================================ */
    int warp_re_base = (blockIdx.x * blockDim.x + warp_id * WARP_SIZE);

    if (scramble_seq == nullptr && lane == 0 && warp_re_base < nof_re) {
        /* Compute starting bit position for this warp */
        int bit_base = warp_re_base * mod_order + NC_SKIP;

        /* Advance LFSRs to starting position (O(log N)) - done once per warp! */
        uint32_t x1 = advance_x1_local(1, bit_base);
        uint32_t x2 = advance_x2_local(scramble_c_init, bit_base);

        /* Compute bits needed for this warp */
        int warp_end_re = min(warp_re_base + WARP_SIZE, nof_re);
        int bits_needed = (warp_end_re - warp_re_base) * mod_order;

        /* Generate scrambling bits for entire warp (max 256 bits) */
        for (int i = 0; i < bits_needed; i++) {
            s_scramble_bits[warp_id][i] = (x1 ^ x2) & 1;
            x1 = step_x1_local(x1);
            x2 = step_x2_local(x2);
        }
    }
    if (scramble_seq == nullptr) {
        __syncwarp();
    }

    /* ================================================================
     * Main processing - each thread handles one RE
     * ================================================================ */
    int re_idx = global_thread_id;
    float my_eq_noise_var = 0.0f;
    float my_evm_error = 0.0f;
    int my_evm_valid = 0;
    int my_valid = 0;

    if (re_idx < nof_re) {
        my_valid = 1;

        /* Get source RE index */
        int src_re = re_indices[re_idx];
        int subcarrier = src_re % symbol_stride;

        /* Compute relative indices for estimate lookup */
        int re_in_alloc = subcarrier - start_subcarrier;
        int prb_idx = re_in_alloc / 12;
        int sc_in_prb = re_in_alloc % 12;
        int re_per_dmrs_sym = nof_prb * NOF_PORTS * 12;

        /* ================================================================
         * Time interpolation of channel estimates
         *
         * TIME_AVG=true:  Average all DMRS symbol estimates (matches CPU
         *                 "average" strategy). Gives σ²/N noise per estimate.
         * TIME_AVG=false: Linear interpolation between bracketing DMRS
         *                 symbols + edge extrapolation. Higher noise at edges.
         * ================================================================ */
        int ofdm_symbol = src_re / symbol_stride;

        cuFloatComplex h[8];
        if constexpr (TIME_AVG) {
            /* Time-average all DMRS estimates (matches CPU "average" strategy) */
            float inv_n = 1.0f / (float)NOF_DMRS;
            #pragma unroll
            for (int p = 0; p < NOF_PORTS; p++) {
                float sum_re = 0.0f, sum_im = 0.0f;
                #pragma unroll
                for (int d = 0; d < NOF_DMRS; d++) {
                    int est_idx = d * re_per_dmrs_sym + prb_idx * (NOF_PORTS * 12) + p * 12 + sc_in_prb;
                    __half2 hd = d_dmrs_estimates_fp16[est_idx];
                    float hr = __half2float(__low2half(hd));
                    float hi = __half2float(__high2half(hd));
                    /* CFO compensation: rotate each DMRS estimate to data symbol time */
                    if (s_cfo_hz != 0.0f) {
                        float cos_d = 1.0f;
                        float sin_d = 0.0f;
                        if (d_cfo_phasors != nullptr && (unsigned)ofdm_symbol < MAX_SYMBOLS) {
                            float2 ph = s_cfo_phasors[ofdm_symbol * MAX_DMRS_SYMBOLS + d];
                            cos_d = ph.x;
                            sin_d = ph.y;
                        } else if (use_cfo_fallback) {
                            int dmrs_sym = s_dmrs_indices[d] + start_symbol;
                            float dt = s_sym_times[ofdm_symbol] - s_sym_times[dmrs_sym];
                            sincosf(6.2831853071795864f * s_cfo_hz * dt, &sin_d, &cos_d);
                        }
                        float r2 = hr * cos_d - hi * sin_d;
                        float i2 = hr * sin_d + hi * cos_d;
                        hr = r2; hi = i2;
                    }
                    sum_re += hr;
                    sum_im += hi;
                }
                h[p] = make_cuFloatComplex(sum_re * inv_n, sum_im * inv_n);
            }
        } else {
            /* Linear time interpolation between bracketing DMRS symbols */

            /* Find bracketing DMRS symbols */
            int d_before = -1, d_after = -1;
            #pragma unroll
            for (int d = 0; d < NOF_DMRS; d++) {
                int dmrs_sym = s_dmrs_indices[d] + start_symbol;
                if (dmrs_sym <= ofdm_symbol) { d_before = d; }
                if (dmrs_sym >= ofdm_symbol && d_after < 0) { d_after = d; }
            }

            /* Edge cases: extrapolate from nearest two DMRS */
            if (d_before < 0) {
                d_before = d_after;
                d_after = (d_before + 1 < NOF_DMRS) ? d_before + 1 : d_before;
            }
            if (d_after < 0 || d_after == d_before) {
                d_after = d_before;
                d_before = (d_after > 0) ? d_after - 1 : d_after;
            }

            int dmrs_before = s_dmrs_indices[d_before] + start_symbol;
            int dmrs_after = s_dmrs_indices[d_after] + start_symbol;

            /* Compute interpolation weight */
            float weight = 0.0f;
            if (dmrs_after != dmrs_before) {
                weight = (float)(ofdm_symbol - dmrs_before) / (float)(dmrs_after - dmrs_before);
            }

            /* CFO compensation phasors: rotate each DMRS estimate to the data symbol time */
            float cos_b = 1.0f, sin_b = 0.0f;
            float cos_a = 1.0f, sin_a = 0.0f;
            if (s_cfo_hz != 0.0f) {
                if (d_cfo_phasors != nullptr && (unsigned)ofdm_symbol < MAX_SYMBOLS) {
                    float2 ph_b = s_cfo_phasors[ofdm_symbol * MAX_DMRS_SYMBOLS + d_before];
                    float2 ph_a = s_cfo_phasors[ofdm_symbol * MAX_DMRS_SYMBOLS + d_after];
                    cos_b = ph_b.x;
                    sin_b = ph_b.y;
                    cos_a = ph_a.x;
                    sin_a = ph_a.y;
                } else if (use_cfo_fallback) {
                    float dt_b = s_sym_times[ofdm_symbol] - s_sym_times[dmrs_before];
                    float dt_a = s_sym_times[ofdm_symbol] - s_sym_times[dmrs_after];
                    sincosf(6.2831853071795864f * s_cfo_hz * dt_b, &sin_b, &cos_b);
                    sincosf(6.2831853071795864f * s_cfo_hz * dt_a, &sin_a, &cos_a);
                }
            }

            #pragma unroll
            for (int p = 0; p < NOF_PORTS; p++) {
                int est_idx = prb_idx * (NOF_PORTS * 12) + p * 12 + sc_in_prb;
                __half2 h_b = d_dmrs_estimates_fp16[d_before * re_per_dmrs_sym + est_idx];
                __half2 h_a = d_dmrs_estimates_fp16[d_after * re_per_dmrs_sym + est_idx];
                float rb = __half2float(__low2half(h_b)), ib = __half2float(__high2half(h_b));
                float ra = __half2float(__low2half(h_a)), ia = __half2float(__high2half(h_a));

                float rb2 = rb * cos_b - ib * sin_b;
                float ib2 = rb * sin_b + ib * cos_b;
                float ra2 = ra * cos_a - ia * sin_a;
                float ia2 = ra * sin_a + ia * cos_a;

                h[p] = make_cuFloatComplex(rb2 + weight * (ra2 - rb2),
                                            ib2 + weight * (ia2 - ib2));
            }
        }

        /* Load received symbols from grid */
        cuFloatComplex y[8];
        #pragma unroll
        for (int p = 0; p < NOF_PORTS; p++) {
            int grid_idx = p * grid_stride + src_re;
            y[p] = cbf16_to_fp32(d_grid_cbf16[grid_idx]);
        }

        /* Equalization */
        cuFloatComplex eq;
        float eq_noise_var;
        equalize_symbol<NOF_PORTS, ALGORITHM>(y, h, s_noise_vars, tx_scaling, eq, eq_noise_var);
        my_eq_noise_var = sinr_noise_var_is_valid(eq_noise_var) ? eq_noise_var : 0.0f;
        if (d_evm_error_sum && d_evm_symbol_count && isfinite(eq.x) && isfinite(eq.y)) {
            my_evm_error = evm_error_power(eq, mod_order);
            my_evm_valid = 1;
        }

        /* Write equalized symbols to external buffer if requested */
        if (d_eq_symbols_out) {
            d_eq_symbols_out[re_idx] = make_float2(eq.x, eq.y);
            d_eq_noise_var_out[re_idx] = eq_noise_var;
        }

        /* Soft demodulation */
        float llr_vals[8];
        float inv_noise = 1.0f / fmaxf(eq_noise_var, 1e-10f);

        if (mod_order == 2) {
            /* QPSK: L = 2*sqrt(2) * symbol / noise_var */
            float scale = 2.0f * 1.41421356f * inv_noise;
            llr_vals[0] = eq.x * scale;
            llr_vals[1] = eq.y * scale;
        } else if (mod_order == 4) {
            /* 16QAM: piecewise-linear soft demodulation per TS 38.211.
             * M = 1/sqrt(10), GAIN = 4*M, THRESHOLD = 2*M. */
            const float M_SQRT1_10 = 0.31622776601683794f;
            const float GAIN_FIRST = 4.0f * M_SQRT1_10;
            const float THRESHOLD = 2.0f * M_SQRT1_10;
            const float CONST_0_8 = 0.8f;

            float abs_re = fabsf(eq.x);
            float abs_im = fabsf(eq.y);

            float l_first_re = GAIN_FIRST * eq.x;
            float l_first_im = GAIN_FIRST * eq.y;

            float l_second_re = 2.0f * l_first_re - copysignf(CONST_0_8, eq.x);
            float l_second_im = 2.0f * l_first_im - copysignf(CONST_0_8, eq.y);

            float l_01_re = (abs_re > THRESHOLD) ? l_second_re : l_first_re;
            float l_01_im = (abs_im > THRESHOLD) ? l_second_im : l_first_im;

            float l_23_re = CONST_0_8 - fabsf(l_first_re);
            float l_23_im = CONST_0_8 - fabsf(l_first_im);

            llr_vals[0] = l_01_re * inv_noise;
            llr_vals[1] = l_01_im * inv_noise;
            llr_vals[2] = l_23_re * inv_noise;
            llr_vals[3] = l_23_im * inv_noise;
        } else if (mod_order == 6) {
            /* 64QAM: piecewise-linear approximation per TS 38.211.
             * Uses optimal interval functions matching CPU implementation for
             * accurate LLR computation at all SINR levels. */
            soft_demod_64qam_piecewise(eq, inv_noise, llr_vals);
        } else if (mod_order == 8) {
            /* 256QAM: piecewise-linear approximation matching CPU implementation */
            soft_demod_256qam_piecewise(eq, inv_noise, llr_vals);
        }

        /* ================================================================
         * OPTIMIZATION #1 cont'd: Apply descrambling from cached sequence
         * or from fallback shared-memory bits.
         * ================================================================ */
        int my_bit_offset = lane * mod_order;
        int llr_base = re_idx * mod_order;
        uint64_t scr_window = 0;
        if (scramble_seq != nullptr) {
            int word_idx = llr_base >> 5;
            int bit_offset = llr_base & 31;
            uint32_t scr_word = scramble_seq[word_idx];
            uint32_t scr_word_next = ((bit_offset + mod_order) > 32) ? scramble_seq[word_idx + 1] : 0;
            scr_window = (static_cast<uint64_t>(scr_word_next) << 32) | scr_word;
            scr_window >>= bit_offset;
        }

        /* ================================================================
         * OPTIMIZATION #4: Vectorized LLR writes using half2
         * ================================================================ */
        if (mod_order == 8) {
            /* 256QAM: 8 LLRs = 4 × half2 writes */
            half2 llr_pack[4];
            #pragma unroll
            for (int i = 0; i < 4; i++) {
                float llr0 = llr_vals[i*2];
                float llr1 = llr_vals[i*2 + 1];

                /* Apply descrambling */
                uint32_t scr0 = (scramble_seq != nullptr) ? ((scr_window >> (i*2)) & 1u) :
                                                           s_scramble_bits[warp_id][my_bit_offset + i*2];
                uint32_t scr1 = (scramble_seq != nullptr) ? ((scr_window >> (i*2 + 1)) & 1u) :
                                                           s_scramble_bits[warp_id][my_bit_offset + i*2 + 1];
                llr0 = scr0 ? -llr0 : llr0;
                llr1 = scr1 ? -llr1 : llr1;

                /* Clamp to FP16 range */
                llr0 = fmaxf(-65504.0f, fminf(65504.0f, llr0));
                llr1 = fmaxf(-65504.0f, fminf(65504.0f, llr1));

                llr_pack[i] = __floats2half2_rn(llr0, llr1);
            }
            /* Vectorized store - 4 × 4-byte writes */
            *((half2*)(llrs_half + llr_base + 0)) = llr_pack[0];
            *((half2*)(llrs_half + llr_base + 2)) = llr_pack[1];
            *((half2*)(llrs_half + llr_base + 4)) = llr_pack[2];
            *((half2*)(llrs_half + llr_base + 6)) = llr_pack[3];
        } else if (mod_order == 4) {
            /* 16QAM: 4 LLRs = 2 × half2 writes */
            half2 llr_pack[2];
            #pragma unroll
            for (int i = 0; i < 2; i++) {
                float llr0 = llr_vals[i*2];
                float llr1 = llr_vals[i*2 + 1];
                uint32_t scr0 = (scramble_seq != nullptr) ? ((scr_window >> (i*2)) & 1u) :
                                                           s_scramble_bits[warp_id][my_bit_offset + i*2];
                uint32_t scr1 = (scramble_seq != nullptr) ? ((scr_window >> (i*2 + 1)) & 1u) :
                                                           s_scramble_bits[warp_id][my_bit_offset + i*2 + 1];
                llr0 = scr0 ? -llr0 : llr0;
                llr1 = scr1 ? -llr1 : llr1;
                llr0 = fmaxf(-65504.0f, fminf(65504.0f, llr0));
                llr1 = fmaxf(-65504.0f, fminf(65504.0f, llr1));
                llr_pack[i] = __floats2half2_rn(llr0, llr1);
            }
            *((half2*)(llrs_half + llr_base + 0)) = llr_pack[0];
            *((half2*)(llrs_half + llr_base + 2)) = llr_pack[1];
        } else if (mod_order == 6) {
            /* 64QAM: 6 LLRs = 3 × half2 writes */
            half2 llr_pack[3];
            #pragma unroll
            for (int i = 0; i < 3; i++) {
                float llr0 = llr_vals[i*2];
                float llr1 = llr_vals[i*2 + 1];
                uint32_t scr0 = (scramble_seq != nullptr) ? ((scr_window >> (i*2)) & 1u) :
                                                           s_scramble_bits[warp_id][my_bit_offset + i*2];
                uint32_t scr1 = (scramble_seq != nullptr) ? ((scr_window >> (i*2 + 1)) & 1u) :
                                                           s_scramble_bits[warp_id][my_bit_offset + i*2 + 1];
                llr0 = scr0 ? -llr0 : llr0;
                llr1 = scr1 ? -llr1 : llr1;
                llr0 = fmaxf(-65504.0f, fminf(65504.0f, llr0));
                llr1 = fmaxf(-65504.0f, fminf(65504.0f, llr1));
                llr_pack[i] = __floats2half2_rn(llr0, llr1);
            }
            *((half2*)(llrs_half + llr_base + 0)) = llr_pack[0];
            *((half2*)(llrs_half + llr_base + 2)) = llr_pack[1];
            *((half2*)(llrs_half + llr_base + 4)) = llr_pack[2];
        } else {
            /* QPSK: 2 LLRs = 1 × half2 write */
            float llr0 = llr_vals[0];
            float llr1 = llr_vals[1];
            uint32_t scr0 = (scramble_seq != nullptr) ? (scr_window & 1u) :
                                                       s_scramble_bits[warp_id][my_bit_offset];
            uint32_t scr1 = (scramble_seq != nullptr) ? ((scr_window >> 1) & 1u) :
                                                       s_scramble_bits[warp_id][my_bit_offset + 1];
            llr0 = scr0 ? -llr0 : llr0;
            llr1 = scr1 ? -llr1 : llr1;
            llr0 = fmaxf(-65504.0f, fminf(65504.0f, llr0));
            llr1 = fmaxf(-65504.0f, fminf(65504.0f, llr1));
            *((half2*)(llrs_half + llr_base)) = __floats2half2_rn(llr0, llr1);
        }
    }

    /* ================================================================
     * OPTIMIZATION #2: Block-level noise variance reduction
     * Warp shuffle → shared memory → single atomic per block
     * ================================================================ */

    /* Warp-level reduction using shuffle (deep-fade REs contribute 0) */
    float warp_sum = warp_reduce_sum_f32(my_eq_noise_var);
    int sinr_valid = my_valid && (my_eq_noise_var > 0.0f);
    int warp_count = __popc(__ballot_sync(0xffffffff, sinr_valid));
    float warp_evm_sum = warp_reduce_sum_f32(my_evm_error);
    int warp_evm_count = __popc(__ballot_sync(0xffffffff, my_evm_valid != 0));

    /* Lane 0 stores warp result to shared memory */
    if (lane == 0) {
        s_noise_var_sums[warp_id] = warp_sum;
        s_noise_var_counts[warp_id] = warp_count;
        s_evm_error_sums[warp_id] = warp_evm_sum;
        s_evm_symbol_counts[warp_id] = warp_evm_count;
    }
    __syncthreads();

    /* Thread 0 does final block reduction and single global atomic */
    if (tid == 0) {
        float block_sum = 0.0f;
        int block_count = 0;
        float block_evm_sum = 0.0f;
        int block_evm_count = 0;
        #pragma unroll
        for (int w = 0; w < WARPS_PER_BLOCK; w++) {
            block_sum += s_noise_var_sums[w];
            block_count += s_noise_var_counts[w];
            block_evm_sum += s_evm_error_sums[w];
            block_evm_count += s_evm_symbol_counts[w];
        }
        if (block_count > 0) {
            atomicAdd(d_eq_noise_var_sum, block_sum);
            atomicAdd(d_eq_noise_var_count, (unsigned int)block_count);
        }
        if (block_evm_count > 0) {
            atomicAdd(d_evm_error_sum, block_evm_sum);
            atomicAdd(d_evm_symbol_count, (unsigned int)block_evm_count);
        }
    }
}

/* ============================================================================
 * ULTRA-OPTIMIZED E2E KERNELS with compile-time modulation order dispatch
 *
 * These kernels eliminate runtime conditionals by using template specialization
 * for modulation order. Combined with launch bounds for optimal GPU occupancy.
 *
 * Key optimizations:
 * - __launch_bounds__(256, 4) for optimal register allocation
 * - Compile-time soft demod constants (no runtime conditionals)
 * - Pre-computed QAM scaling factors
 * - Template dispatch by port count AND modulation order
 * - Vectorized LLR output where possible
 * ============================================================================ */

/* Pre-computed QAM scaling constants (compile-time) */
constexpr float QPSK_SCALE = 2.8284271247461903f;   /* 2 * sqrt(2) - matches srsRAN CPU demod */
[[maybe_unused]] constexpr float QAM16_SCALE = 0.6324555320336759f;  /* 1/sqrt(10) */
[[maybe_unused]] constexpr float QAM64_SCALE = 0.4082482904638631f;  /* 1/sqrt(42) */
[[maybe_unused]] constexpr float QAM256_SCALE = 0.2581988897471611f; /* 1/sqrt(170) */

__device__ __forceinline__ void store_descrambled_half_llr(
    __half* __restrict__ llrs_half,
    const uint32_t* __restrict__ scramble_seq,
    int bit_start,
    const float* llr_vals,
    int mod_order)
{
    int word_idx = bit_start / 32;
    int bit_off = bit_start % 32;
    uint32_t scr_word = scramble_seq[word_idx];
    uint32_t scr_word_next = scramble_seq[word_idx + 1];
    uint64_t scr_window = ((uint64_t)scr_word_next << 32) | scr_word;

    #pragma unroll
    for (int b = 0; b < 8; b++) {
        if (b < mod_order) {
            uint32_t scr = (scr_window >> (bit_off + b)) & 1;
            float llr = scr ? -llr_vals[b] : llr_vals[b];
            llr = fmaxf(-65504.0f, fminf(65504.0f, llr));
            llrs_half[bit_start + b] = __float2half(llr);
        }
    }
}

__device__ __forceinline__ void soft_demod_symbol_runtime(cuFloatComplex eq, float eq_noise_var, int mod_order, float* llr_vals)
{
    float inv_noise = 1.0f / fmaxf(eq_noise_var, 1e-10f);

    if (mod_order == 2) {
        float scale = QPSK_SCALE * inv_noise;
        llr_vals[0] = eq.x * scale;
        llr_vals[1] = eq.y * scale;
    } else if (mod_order == 4) {
        const float M_SQRT1_10 = 0.31622776601683794f;
        const float GAIN_FIRST = 4.0f * M_SQRT1_10;
        const float THRESHOLD = 2.0f * M_SQRT1_10;
        const float CONST_0_8 = 0.8f;

        float abs_re = fabsf(eq.x);
        float abs_im = fabsf(eq.y);

        float l_first_re = GAIN_FIRST * eq.x;
        float l_first_im = GAIN_FIRST * eq.y;
        float l_second_re = 2.0f * l_first_re - copysignf(CONST_0_8, eq.x);
        float l_second_im = 2.0f * l_first_im - copysignf(CONST_0_8, eq.y);

        llr_vals[0] = ((abs_re > THRESHOLD) ? l_second_re : l_first_re) * inv_noise;
        llr_vals[1] = ((abs_im > THRESHOLD) ? l_second_im : l_first_im) * inv_noise;
        llr_vals[2] = (CONST_0_8 - fabsf(l_first_re)) * inv_noise;
        llr_vals[3] = (CONST_0_8 - fabsf(l_first_im)) * inv_noise;
    } else if (mod_order == 6) {
        soft_demod_64qam_piecewise(eq, inv_noise, llr_vals);
    } else if (mod_order == 8) {
        soft_demod_256qam_piecewise(eq, inv_noise, llr_vals);
    }
}

/* ============================================================================
 * WARP REDUCTION HELPERS for efficient SINR accumulation
 *
 * These functions reduce per-thread values across a warp using shuffle
 * instructions, then use a single atomicAdd per warp instead of per-thread.
 * This reduces atomic contention by 32x.
 * ============================================================================ */

__device__ __forceinline__ float warp_reduce_sum(float val) {
    /* Butterfly reduction across 32-thread warp */
    #pragma unroll
    for (int offset = 16; offset > 0; offset >>= 1) {
        val += __shfl_down_sync(0xffffffff, val, offset);
    }
    return val;  /* Only lane 0 has the final sum */
}

__device__ __forceinline__ unsigned int warp_reduce_sum_uint(unsigned int val) {
    #pragma unroll
    for (int offset = 16; offset > 0; offset >>= 1) {
        val += __shfl_down_sync(0xffffffff, val, offset);
    }
    return val;
}

/* Block-level reduction using shared memory for inter-warp communication */
template<int BLOCK_SIZE>
__device__ __forceinline__ void block_reduce_atomic(
    float val, unsigned int count,
    float* __restrict__ d_sum, unsigned int* __restrict__ d_count)
{
    __shared__ float s_sum[32];    /* One slot per warp */
    __shared__ unsigned int s_count[32];

    int lane = threadIdx.x & 31;
    int warp_id = threadIdx.x >> 5;
    int num_warps = BLOCK_SIZE >> 5;

    /* Warp-level reduction */
    float warp_sum = warp_reduce_sum(val);
    unsigned int warp_count = warp_reduce_sum_uint(count);

    /* Lane 0 of each warp writes to shared memory */
    if (lane == 0) {
        s_sum[warp_id] = warp_sum;
        s_count[warp_id] = warp_count;
    }
    __syncthreads();

    /* First warp reduces across all warps */
    if (warp_id == 0) {
        float final_sum = (lane < num_warps) ? s_sum[lane] : 0.0f;
        unsigned int final_count = (lane < num_warps) ? s_count[lane] : 0;

        final_sum = warp_reduce_sum(final_sum);
        final_count = warp_reduce_sum_uint(final_count);

        /* Only thread 0 does the atomic */
        if (lane == 0) {
            atomicAdd(d_sum, final_sum);
            atomicAdd(d_count, final_count);
        }
    }
}

/* ============================================================================
 * 2-LAYER MIMO E2E KERNELS
 *
 * These kernels process 2-layer MIMO configurations using per-layer channel
 * estimates from the MIMO LSE kernels. Each RE outputs 2x the LLRs.
 *
 * Key differences from single-layer kernels:
 * - Channel estimates indexed as [dmrs_sym, prb, port, layer, 12]
 * - Uses equalize_symbol_2layer for MIMO equalization
 * - Outputs LLRs in codeword order: llrs[re][layer][bit]
 * ============================================================================ */

template <int NOF_PORTS, int ALGORITHM>
__global__ void __launch_bounds__(256, 4)
kernel_mimo_2layer_fp16(
    const unsigned int* __restrict__ d_grid_cbf16,
    const __half2* __restrict__ d_dmrs_estimates_fp16,
    const int* __restrict__ re_indices,
    const float* __restrict__ noise_vars,
    __half* __restrict__ llrs_half,
    const uint32_t* __restrict__ scramble_seq,
    float* __restrict__ d_eq_noise_var_sum,
    unsigned int* __restrict__ d_eq_noise_var_count,
    float* __restrict__ d_evm_error_sum,
    unsigned int* __restrict__ d_evm_symbol_count,
    int nof_re,
    int nof_dmrs_symbols,
    int nof_prb,
    int mod_order,
    int grid_stride,
    int symbol_stride,
    int start_symbol,
    int start_subcarrier,
    float tx_scaling,
    const int* __restrict__ d_dmrs_indices,
    const float* __restrict__ d_cfo_hz_ptr,
    const float* __restrict__ d_symbol_start_times,
    const float2* __restrict__ d_cfo_phasors,
    float2* __restrict__ d_eq_symbols_out,
    float* __restrict__ d_eq_noise_var_out)
{
    int re_idx = blockIdx.x * blockDim.x + threadIdx.x;

    __shared__ float s_noise_vars[8];
    __shared__ int s_dmrs_indices[4];
    __shared__ float s_cfo_hz;
    __shared__ float s_sym_times[14];
    __shared__ float2 s_cfo_phasors[MIMO_CFO_DATA_PHASOR_COUNT];
    bool use_cfo_fallback =
        (d_cfo_hz_ptr != nullptr && d_cfo_phasors == nullptr && d_symbol_start_times != nullptr);
    if (threadIdx.x < NOF_PORTS) {
        s_noise_vars[threadIdx.x] = noise_vars[threadIdx.x];
    }
    if (use_cfo_fallback && threadIdx.x < nof_dmrs_symbols && threadIdx.x < 4) {
        s_dmrs_indices[threadIdx.x] = d_dmrs_indices[threadIdx.x];
    }
    if (threadIdx.x == 0) {
        s_cfo_hz = (d_cfo_hz_ptr != nullptr) ? *d_cfo_hz_ptr : 0.0f;
    }
    if (use_cfo_fallback && threadIdx.x < 14) {
        s_sym_times[threadIdx.x] = d_symbol_start_times[threadIdx.x];
    }
    if (d_cfo_phasors != nullptr) {
        for (int i = threadIdx.x; i < MIMO_CFO_DATA_PHASOR_COUNT; i += blockDim.x) {
            s_cfo_phasors[i] = d_cfo_phasors[i];
        }
    }
    __syncthreads();

    if (re_idx >= nof_re) return;

    int src_re = re_indices[re_idx];
    int ofdm_symbol = src_re / symbol_stride;
    int subcarrier = src_re % symbol_stride;
    int re_in_alloc = subcarrier - start_subcarrier;
    int prb_idx = re_in_alloc / 12;
    int sc_in_prb = re_in_alloc % 12;

    constexpr int NOF_LAYERS = 2;
    int re_per_layer = 12;
    int re_per_port = NOF_LAYERS * re_per_layer;
    int re_per_prb = NOF_PORTS * re_per_port;
    int re_per_dmrs_sym = nof_prb * re_per_prb;

    cuFloatComplex H[NOF_PORTS][2];
    #pragma unroll
    for (int p = 0; p < NOF_PORTS; p++) {
        float h_real_sum[2] = {0.0f, 0.0f};
        float h_imag_sum[2] = {0.0f, 0.0f};
        #pragma unroll
        for (int d = 0; d < 4; d++) {
            if (d < nof_dmrs_symbols) {
                int base = d * re_per_dmrs_sym + prb_idx * re_per_prb + p * re_per_port;
                #pragma unroll
                for (int layer = 0; layer < NOF_LAYERS; layer++) {
                    __half2 h = d_dmrs_estimates_fp16[base + layer * re_per_layer + sc_in_prb];
                    float hr = __half2float(__low2half(h));
                    float hi = __half2float(__high2half(h));
                    if (s_cfo_hz != 0.0f) {
                        float cos_d = 1.0f;
                        float sin_d = 0.0f;
                        if (d_cfo_phasors != nullptr && (unsigned)ofdm_symbol < MAX_SYMBOLS) {
                            float2 ph = s_cfo_phasors[ofdm_symbol * MAX_DMRS_SYMBOLS + d];
                            cos_d = ph.x;
                            sin_d = ph.y;
                        } else if (use_cfo_fallback) {
                            int dmrs_sym = s_dmrs_indices[d] + start_symbol;
                            float dt = s_sym_times[ofdm_symbol] - s_sym_times[dmrs_sym];
                            sincosf(6.2831853071795864f * s_cfo_hz * dt, &sin_d, &cos_d);
                        }
                        float r2 = hr * cos_d - hi * sin_d;
                        float i2 = hr * sin_d + hi * cos_d;
                        hr = r2;
                        hi = i2;
                    }
                    h_real_sum[layer] += hr;
                    h_imag_sum[layer] += hi;
                }
            }
        }
        float inv_n = 1.0f / (float)nof_dmrs_symbols;
        H[p][0] = make_cuFloatComplex(h_real_sum[0] * inv_n, h_imag_sum[0] * inv_n);
        H[p][1] = make_cuFloatComplex(h_real_sum[1] * inv_n, h_imag_sum[1] * inv_n);
    }

    cuFloatComplex y[NOF_PORTS];
    #pragma unroll
    for (int p = 0; p < NOF_PORTS; p++) {
        y[p] = cbf16_to_fp32(d_grid_cbf16[p * grid_stride + src_re]);
    }

    cuFloatComplex eq_symbols[2];
    float eq_noise_vars[2];
    equalize_symbol_2layer<NOF_PORTS, ALGORITHM>(y, H, s_noise_vars, tx_scaling, eq_symbols, eq_noise_vars);

    sinr_accumulate_warp(d_eq_noise_var_sum, d_eq_noise_var_count,
                         (eq_noise_vars[0] + eq_noise_vars[1]) * 0.5f);
    if (d_evm_error_sum && d_evm_symbol_count) {
        evm_accumulate_warp(d_evm_error_sum,
                            d_evm_symbol_count,
                            evm_error_power(eq_symbols[0], mod_order) + evm_error_power(eq_symbols[1], mod_order),
                            2u);
    }

    if (d_eq_symbols_out) {
        int eq_base = re_idx * NOF_LAYERS;
        d_eq_symbols_out[eq_base + 0] = make_float2(eq_symbols[0].x, eq_symbols[0].y);
        d_eq_symbols_out[eq_base + 1] = make_float2(eq_symbols[1].x, eq_symbols[1].y);
        d_eq_noise_var_out[eq_base + 0] = eq_noise_vars[0];
        d_eq_noise_var_out[eq_base + 1] = eq_noise_vars[1];
    }

    #pragma unroll
    for (int layer = 0; layer < NOF_LAYERS; layer++) {
        float llr_vals[8];
        soft_demod_symbol_runtime(eq_symbols[layer], eq_noise_vars[layer], mod_order, llr_vals);
        int bit_start = re_idx * (NOF_LAYERS * mod_order) + layer * mod_order;
        store_descrambled_half_llr(llrs_half, scramble_seq, bit_start, llr_vals, mod_order);
    }
}

template <int NOF_PORTS, int ALGORITHM>
__global__ void __launch_bounds__(256, 2)
kernel_mimo_3layer_fp16(
    const unsigned int* __restrict__ d_grid_cbf16,
    const __half2* __restrict__ d_dmrs_estimates_fp16,
    const int* __restrict__ re_indices,
    const float* __restrict__ noise_vars,
    __half* __restrict__ llrs_half,
    const uint32_t* __restrict__ scramble_seq,
    float* __restrict__ d_eq_noise_var_sum,
    unsigned int* __restrict__ d_eq_noise_var_count,
    float* __restrict__ d_evm_error_sum,
    unsigned int* __restrict__ d_evm_symbol_count,
    int nof_re,
    int nof_dmrs_symbols,
    int nof_prb,
    int mod_order,
    int grid_stride,
    int symbol_stride,
    int start_symbol,
    int start_subcarrier,
    float tx_scaling,
    const int* __restrict__ d_dmrs_indices,
    const float* __restrict__ d_cfo_hz_ptr,
    const float* __restrict__ d_symbol_start_times,
    const float2* __restrict__ d_cfo_phasors,
    float2* __restrict__ d_eq_symbols_out,
    float* __restrict__ d_eq_noise_var_out)
{
    int re_idx = blockIdx.x * blockDim.x + threadIdx.x;

    __shared__ float s_noise_vars[8];
    __shared__ int s_dmrs_indices[4];
    __shared__ float s_cfo_hz;
    __shared__ float s_sym_times[14];
    __shared__ float2 s_cfo_phasors[MIMO_CFO_DATA_PHASOR_COUNT];
    bool use_cfo_fallback =
        (d_cfo_hz_ptr != nullptr && d_cfo_phasors == nullptr && d_symbol_start_times != nullptr);
    if (threadIdx.x < NOF_PORTS) {
        s_noise_vars[threadIdx.x] = noise_vars[threadIdx.x];
    }
    if (use_cfo_fallback && threadIdx.x < nof_dmrs_symbols && threadIdx.x < 4) {
        s_dmrs_indices[threadIdx.x] = d_dmrs_indices[threadIdx.x];
    }
    if (threadIdx.x == 0) {
        s_cfo_hz = (d_cfo_hz_ptr != nullptr) ? *d_cfo_hz_ptr : 0.0f;
    }
    if (use_cfo_fallback && threadIdx.x < 14) {
        s_sym_times[threadIdx.x] = d_symbol_start_times[threadIdx.x];
    }
    if (d_cfo_phasors != nullptr) {
        for (int i = threadIdx.x; i < MIMO_CFO_DATA_PHASOR_COUNT; i += blockDim.x) {
            s_cfo_phasors[i] = d_cfo_phasors[i];
        }
    }
    __syncthreads();

    if (re_idx >= nof_re) return;

    int src_re = re_indices[re_idx];
    int ofdm_symbol = src_re / symbol_stride;
    int subcarrier = src_re % symbol_stride;
    int re_in_alloc = subcarrier - start_subcarrier;
    int prb_idx = re_in_alloc / 12;
    int sc_in_prb = re_in_alloc % 12;

    constexpr int NOF_LAYERS = 3;
    int re_per_layer = 12;
    int re_per_port = NOF_LAYERS * re_per_layer;
    int re_per_prb = NOF_PORTS * re_per_port;
    int re_per_dmrs_sym = nof_prb * re_per_prb;

    cuFloatComplex H[NOF_PORTS][3];
    #pragma unroll
    for (int p = 0; p < NOF_PORTS; p++) {
        float h_real_sum[3] = {0.0f, 0.0f, 0.0f};
        float h_imag_sum[3] = {0.0f, 0.0f, 0.0f};
        #pragma unroll
        for (int d = 0; d < 4; d++) {
            if (d < nof_dmrs_symbols) {
                int base = d * re_per_dmrs_sym + prb_idx * re_per_prb + p * re_per_port;
                #pragma unroll
                for (int layer = 0; layer < NOF_LAYERS; layer++) {
                    __half2 h = d_dmrs_estimates_fp16[base + layer * re_per_layer + sc_in_prb];
                    float hr = __half2float(__low2half(h));
                    float hi = __half2float(__high2half(h));
                    if (s_cfo_hz != 0.0f) {
                        float cos_d = 1.0f;
                        float sin_d = 0.0f;
                        if (d_cfo_phasors != nullptr && (unsigned)ofdm_symbol < MAX_SYMBOLS) {
                            float2 ph = s_cfo_phasors[ofdm_symbol * MAX_DMRS_SYMBOLS + d];
                            cos_d = ph.x;
                            sin_d = ph.y;
                        } else if (use_cfo_fallback) {
                            int dmrs_sym = s_dmrs_indices[d] + start_symbol;
                            float dt = s_sym_times[ofdm_symbol] - s_sym_times[dmrs_sym];
                            sincosf(6.2831853071795864f * s_cfo_hz * dt, &sin_d, &cos_d);
                        }
                        float r2 = hr * cos_d - hi * sin_d;
                        float i2 = hr * sin_d + hi * cos_d;
                        hr = r2;
                        hi = i2;
                    }
                    h_real_sum[layer] += hr;
                    h_imag_sum[layer] += hi;
                }
            }
        }
        float inv_n = 1.0f / (float)nof_dmrs_symbols;
        #pragma unroll
        for (int layer = 0; layer < NOF_LAYERS; layer++) {
            H[p][layer] = make_cuFloatComplex(h_real_sum[layer] * inv_n, h_imag_sum[layer] * inv_n);
        }
    }

    cuFloatComplex y[NOF_PORTS];
    #pragma unroll
    for (int p = 0; p < NOF_PORTS; p++) {
        y[p] = cbf16_to_fp32(d_grid_cbf16[p * grid_stride + src_re]);
    }

    cuFloatComplex eq_symbols[3];
    float eq_noise_vars[3];
    equalize_symbol_3layer<NOF_PORTS, ALGORITHM>(y, H, s_noise_vars, tx_scaling, eq_symbols, eq_noise_vars);

    sinr_accumulate_warp(d_eq_noise_var_sum, d_eq_noise_var_count,
                         (eq_noise_vars[0] + eq_noise_vars[1] + eq_noise_vars[2]) / 3.0f);
    if (d_evm_error_sum && d_evm_symbol_count) {
        evm_accumulate_warp(d_evm_error_sum,
                            d_evm_symbol_count,
                            evm_error_power(eq_symbols[0], mod_order) + evm_error_power(eq_symbols[1], mod_order) +
                                evm_error_power(eq_symbols[2], mod_order),
                            3u);
    }

    if (d_eq_symbols_out) {
        int eq_base = re_idx * NOF_LAYERS;
        #pragma unroll
        for (int layer = 0; layer < NOF_LAYERS; layer++) {
            d_eq_symbols_out[eq_base + layer] = make_float2(eq_symbols[layer].x, eq_symbols[layer].y);
            d_eq_noise_var_out[eq_base + layer] = eq_noise_vars[layer];
        }
    }

    #pragma unroll
    for (int layer = 0; layer < NOF_LAYERS; layer++) {
        float llr_vals[8];
        soft_demod_symbol_runtime(eq_symbols[layer], eq_noise_vars[layer], mod_order, llr_vals);
        int bit_start = re_idx * (NOF_LAYERS * mod_order) + layer * mod_order;
        store_descrambled_half_llr(llrs_half, scramble_seq, bit_start, llr_vals, mod_order);
    }
}

/* ============================================================================
 * 4-LAYER MIMO E2E KERNELS
 *
 * These kernels process 4-layer MIMO configurations using per-layer channel
 * estimates from the MIMO LSE kernels. Each RE outputs 4x the LLRs.
 *
 * Requires >= 4 RX ports (4x4 or 8x4 configurations).
 * ============================================================================ */

template <int NOF_PORTS, int ALGORITHM>
__global__ void __launch_bounds__(256, 2)
kernel_mimo_4layer_fp16(
    const unsigned int* __restrict__ d_grid_cbf16,
    const __half2* __restrict__ d_dmrs_estimates_fp16,
    const int* __restrict__ re_indices,
    const float* __restrict__ noise_vars,
    __half* __restrict__ llrs_half,
    const uint32_t* __restrict__ scramble_seq,
    float* __restrict__ d_eq_noise_var_sum,
    unsigned int* __restrict__ d_eq_noise_var_count,
    float* __restrict__ d_evm_error_sum,
    unsigned int* __restrict__ d_evm_symbol_count,
    int nof_re,
    int nof_dmrs_symbols,
    int nof_prb,
    int mod_order,
    int grid_stride,
    int symbol_stride,
    int start_symbol,
    int start_subcarrier,
    float tx_scaling,
    const int* __restrict__ d_dmrs_indices,
    const float* __restrict__ d_cfo_hz_ptr,
    const float* __restrict__ d_symbol_start_times,
    const float2* __restrict__ d_cfo_phasors,
    float2* __restrict__ d_eq_symbols_out,
    float* __restrict__ d_eq_noise_var_out)
{
    int re_idx = blockIdx.x * blockDim.x + threadIdx.x;

    __shared__ float s_noise_vars[8];
    __shared__ int s_dmrs_indices[4];
    __shared__ float s_cfo_hz;
    __shared__ float s_sym_times[14];
    __shared__ float2 s_cfo_phasors[MIMO_CFO_DATA_PHASOR_COUNT];
    bool use_cfo_fallback =
        (d_cfo_hz_ptr != nullptr && d_cfo_phasors == nullptr && d_symbol_start_times != nullptr);
    if (threadIdx.x < NOF_PORTS) {
        s_noise_vars[threadIdx.x] = noise_vars[threadIdx.x];
    }
    if (use_cfo_fallback && threadIdx.x < nof_dmrs_symbols && threadIdx.x < 4) {
        s_dmrs_indices[threadIdx.x] = d_dmrs_indices[threadIdx.x];
    }
    if (threadIdx.x == 0) {
        s_cfo_hz = (d_cfo_hz_ptr != nullptr) ? *d_cfo_hz_ptr : 0.0f;
    }
    if (use_cfo_fallback && threadIdx.x < 14) {
        s_sym_times[threadIdx.x] = d_symbol_start_times[threadIdx.x];
    }
    if (d_cfo_phasors != nullptr) {
        for (int i = threadIdx.x; i < MIMO_CFO_DATA_PHASOR_COUNT; i += blockDim.x) {
            s_cfo_phasors[i] = d_cfo_phasors[i];
        }
    }
    __syncthreads();

    if (re_idx >= nof_re) return;

    int src_re = re_indices[re_idx];
    int ofdm_symbol = src_re / symbol_stride;
    int subcarrier = src_re % symbol_stride;
    int re_in_alloc = subcarrier - start_subcarrier;
    int prb_idx = re_in_alloc / 12;
    int sc_in_prb = re_in_alloc % 12;

    constexpr int NOF_LAYERS = 4;
    int re_per_layer = 12;
    int re_per_port = NOF_LAYERS * re_per_layer;
    int re_per_prb = NOF_PORTS * re_per_port;
    int re_per_dmrs_sym = nof_prb * re_per_prb;

    cuFloatComplex H[NOF_PORTS][4];
    #pragma unroll
    for (int p = 0; p < NOF_PORTS; p++) {
        float h_real_sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float h_imag_sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        #pragma unroll
        for (int d = 0; d < 4; d++) {
            if (d < nof_dmrs_symbols) {
                int base = d * re_per_dmrs_sym + prb_idx * re_per_prb + p * re_per_port;
                #pragma unroll
                for (int layer = 0; layer < NOF_LAYERS; layer++) {
                    __half2 h = d_dmrs_estimates_fp16[base + layer * re_per_layer + sc_in_prb];
                    float hr = __half2float(__low2half(h));
                    float hi = __half2float(__high2half(h));
                    if (s_cfo_hz != 0.0f) {
                        float cos_d = 1.0f;
                        float sin_d = 0.0f;
                        if (d_cfo_phasors != nullptr && (unsigned)ofdm_symbol < MAX_SYMBOLS) {
                            float2 ph = s_cfo_phasors[ofdm_symbol * MAX_DMRS_SYMBOLS + d];
                            cos_d = ph.x;
                            sin_d = ph.y;
                        } else if (use_cfo_fallback) {
                            int dmrs_sym = s_dmrs_indices[d] + start_symbol;
                            float dt = s_sym_times[ofdm_symbol] - s_sym_times[dmrs_sym];
                            sincosf(6.2831853071795864f * s_cfo_hz * dt, &sin_d, &cos_d);
                        }
                        float r2 = hr * cos_d - hi * sin_d;
                        float i2 = hr * sin_d + hi * cos_d;
                        hr = r2;
                        hi = i2;
                    }
                    h_real_sum[layer] += hr;
                    h_imag_sum[layer] += hi;
                }
            }
        }
        float inv_n = 1.0f / (float)nof_dmrs_symbols;
        #pragma unroll
        for (int layer = 0; layer < NOF_LAYERS; layer++) {
            H[p][layer] = make_cuFloatComplex(h_real_sum[layer] * inv_n, h_imag_sum[layer] * inv_n);
        }
    }

    cuFloatComplex y[NOF_PORTS];
    #pragma unroll
    for (int p = 0; p < NOF_PORTS; p++) {
        y[p] = cbf16_to_fp32(d_grid_cbf16[p * grid_stride + src_re]);
    }

    cuFloatComplex eq_symbols[4];
    float eq_noise_vars[4];
    equalize_symbol_4layer<NOF_PORTS, ALGORITHM>(y, H, s_noise_vars, tx_scaling, eq_symbols, eq_noise_vars);

    sinr_accumulate_warp(d_eq_noise_var_sum, d_eq_noise_var_count,
                         (eq_noise_vars[0] + eq_noise_vars[1] + eq_noise_vars[2] + eq_noise_vars[3]) * 0.25f);
    if (d_evm_error_sum && d_evm_symbol_count) {
        evm_accumulate_warp(d_evm_error_sum,
                            d_evm_symbol_count,
                            evm_error_power(eq_symbols[0], mod_order) + evm_error_power(eq_symbols[1], mod_order) +
                                evm_error_power(eq_symbols[2], mod_order) + evm_error_power(eq_symbols[3], mod_order),
                            4u);
    }

    if (d_eq_symbols_out) {
        int eq_base = re_idx * NOF_LAYERS;
        #pragma unroll
        for (int layer = 0; layer < NOF_LAYERS; layer++) {
            d_eq_symbols_out[eq_base + layer] = make_float2(eq_symbols[layer].x, eq_symbols[layer].y);
            d_eq_noise_var_out[eq_base + layer] = eq_noise_vars[layer];
        }
    }

    #pragma unroll
    for (int layer = 0; layer < NOF_LAYERS; layer++) {
        float llr_vals[8];
        soft_demod_symbol_runtime(eq_symbols[layer], eq_noise_vars[layer], mod_order, llr_vals);
        int bit_start = re_idx * (NOF_LAYERS * mod_order) + layer * mod_order;
        store_descrambled_half_llr(llrs_half, scramble_seq, bit_start, llr_vals, mod_order);
    }
}

/* ============================================================================
 * ORIGINAL CUDA Kernels (kept for compatibility)
 * ============================================================================ */

/**
 * Generate DMRS pilots and compute LSE channel estimates at DMRS positions.
 *
 * This kernel processes one PRB per thread block, with threads handling
 * different ports in parallel. For Type 1 DMRS, each PRB has 6 DMRS REs.
 *
 * Input grid layout: [port, symbol, subcarrier] in cbf16 format
 * Output: LSE estimates at DMRS positions [dmrs_symbol, port, prb, dmrs_re_in_prb]
 */
__global__ void kernel_dmrs_lse_type1(
    const unsigned int* __restrict__ d_grid_cbf16,
    const unsigned int* __restrict__ d_dmrs_seq,  /* Packed Gold sequence bits */
    cuFloatComplex* __restrict__ d_lse_estimates,
    int nof_prb,
    int nof_ports,
    int grid_stride,           /* nof_symbols * nof_subcarriers */
    int symbol_stride,         /* nof_subcarriers */
    int start_prb,
    int dmrs_symbol_idx,       /* Which symbol in the slot (absolute) */
    int start_symbol,          /* First symbol of allocation (for relative indexing) */
    float dmrs_scaling)
{
    int prb_idx = blockIdx.x;
    int port = threadIdx.x;

    if (prb_idx >= nof_prb || port >= nof_ports) return;

    /* Type 1 DMRS: REs at subcarriers 0, 2, 4, 6, 8, 10 within PRB */
    int prb_start_sc = (start_prb + prb_idx) * 12;

    /* DMRS RE indices within PRB for Type 1 */
    const int dmrs_sc_offset[6] = {0, 2, 4, 6, 8, 10};

    /* Match CPU channel estimation scaling exactly:
     * CPU generates pilots with amplitude 1/sqrt(2), giving |pilot|² = 0.5.
     * The received DMRS signal is: y = H × pilot_tx + noise
     * where pilot_tx = dmrs_scaling × (1/sqrt(2)) × QPSK.
     *
     * CPU LSE: h_raw = y × pilot* = H × dmrs_scaling × |pilot|² = H × dmrs_scaling × 0.5
     * CPU then divides by dmrs_scaling: h_cpu = H × 0.5
     *
     * The CPU MMSE formula is designed for this scaling, with |h|² = |H|² × 0.25.
     * The 0.5 factor affects both signal and noise variance such that SINR is correct.
     *
     * GPU must match: h = H × 0.5
     */
    float inv_sqrt2 = 0.70710678118f;  /* Unit amplitude, no dmrs_scaling */
    float h_normalizer = 1.0f / dmrs_scaling;  /* Divide by dmrs_scaling like CPU */

    #pragma unroll
    for (int d = 0; d < 6; d++) {
        int sc = prb_start_sc + dmrs_sc_offset[d];

        /* Load received symbol from grid.
         * Grid is uploaded with ABSOLUTE symbol indexing (0..13 for entire slot),
         * so use dmrs_symbol_idx directly without any offset. */
        int grid_idx = port * grid_stride + dmrs_symbol_idx * symbol_stride + sc;
        cuFloatComplex y = cbf16_to_fp32(d_grid_cbf16[grid_idx]);

        /* Generate DMRS pilot from Gold sequence.
         * Pilot index must account for start_prb offset since the sequence
         * starts from point A (RB 0) per 3GPP TS 38.211 Section 6.4.1.1.1.
         * CPU code uses prg.advance(start_prb * 6 * 2) to skip bits.
         */
        int pilot_idx = (start_prb + prb_idx) * 6 + d;
        int bit_idx_real = 2 * pilot_idx;
        int bit_idx_imag = 2 * pilot_idx + 1;

        int word_idx_real = bit_idx_real / 32;
        int bit_pos_real = 31 - (bit_idx_real % 32);  // MSB-first to match gold_sequence_generate_kernel
        int c_real = (d_dmrs_seq[word_idx_real] >> bit_pos_real) & 1;

        int word_idx_imag = bit_idx_imag / 32;
        int bit_pos_imag = 31 - (bit_idx_imag % 32);  // MSB-first to match gold_sequence_generate_kernel
        int c_imag = (d_dmrs_seq[word_idx_imag] >> bit_pos_imag) & 1;

        /* QPSK mapping: (1-2c) / sqrt(2) - unit amplitude like CPU */
        float p_real = (1 - 2 * c_real) * inv_sqrt2;
        float p_imag = (1 - 2 * c_imag) * inv_sqrt2;
        cuFloatComplex pilot = make_cuFloatComplex(p_real, p_imag);

        /* LSE: h_raw = y × conj(pilot) = H × dmrs_scaling × |pilot|² + noise_term
         *            = H × dmrs_scaling × 0.5 + noise_term
         * Divide by dmrs_scaling to match CPU: h = H × 0.5
         */
        cuFloatComplex h_raw = cuCmulf_fast(y, cuConjf_fast(pilot));
        cuFloatComplex h = make_cuFloatComplex(h_raw.x * h_normalizer,
                                               h_raw.y * h_normalizer);

        /* Store LSE estimate */
        /* Layout: [prb, port, dmrs_re] for coalesced access in interpolation */
        int out_idx = prb_idx * (nof_ports * 6) + port * 6 + d;
        d_lse_estimates[out_idx] = h;
    }
}

/**
 * Generate DMRS pilots and compute LSE channel estimates for LOW-PAPR DMRS.
 *
 * This kernel is used when transform precoding is enabled (MSG3/DFT-s-OFDM).
 * Low-PAPR DMRS uses pre-computed complex pilot symbols (Zadoff-Chu or lookup table)
 * instead of Gold sequence bits.
 *
 * For transform precoding DMRS (TS 38.211 Section 6.4.1.1.2):
 * - All 12 subcarriers per PRB carry DMRS (no data, no CDM)
 * - But pilots are mapped to even subcarriers (0, 2, 4, 6, 8, 10) per PRB
 * - Sequence length M_zc = nof_prb * 6
 *
 * Input grid layout: [port, symbol, subcarrier] in cbf16 format
 * Output: LSE estimates at DMRS positions [dmrs_symbol, port, prb, dmrs_re_in_prb]
 */
__global__ void kernel_dmrs_lse_type1_low_papr(
    const unsigned int* __restrict__ d_grid_cbf16,
    const cuFloatComplex* __restrict__ d_pilots,  /* Pre-computed low-PAPR pilots [M_zc] */
    cuFloatComplex* __restrict__ d_lse_estimates,
    int nof_prb,
    int nof_ports,
    int grid_stride,           /* nof_symbols * nof_subcarriers */
    int symbol_stride,         /* nof_subcarriers */
    int start_prb,
    int dmrs_symbol_idx,       /* Which symbol in the slot (absolute) */
    int start_symbol,          /* First symbol of allocation (for relative indexing) */
    float dmrs_scaling)
{
    int prb_idx = blockIdx.x;
    int port = threadIdx.x;

    if (prb_idx >= nof_prb || port >= nof_ports) return;

    /* Type 1 DMRS: REs at subcarriers 0, 2, 4, 6, 8, 10 within PRB */
    int prb_start_sc = (start_prb + prb_idx) * 12;

    /* DMRS RE indices within PRB for Type 1 */
    const int dmrs_sc_offset[6] = {0, 2, 4, 6, 8, 10};

    /* Normalization to match CPU scaling (see kernel_dmrs_lse_type1 for details) */
    float h_normalizer = 1.0f / dmrs_scaling;

    #pragma unroll
    for (int d = 0; d < 6; d++) {
        int sc = prb_start_sc + dmrs_sc_offset[d];

        /* Load received symbol from grid.
         * Grid is uploaded with ABSOLUTE symbol indexing (0..13 for entire slot),
         * so use dmrs_symbol_idx directly without any offset. */
        int grid_idx = port * grid_stride + dmrs_symbol_idx * symbol_stride + sc;
        cuFloatComplex y = cbf16_to_fp32(d_grid_cbf16[grid_idx]);

        /* Get low-PAPR pilot symbol.
         * For transform precoding, pilot index is within the allocation (no start_prb offset)
         * since the low-PAPR sequence is generated for the allocated PRBs only.
         * Pilot index = prb_idx * 6 + d (where d is DMRS RE index within PRB).
         */
        int pilot_idx = prb_idx * 6 + d;
        cuFloatComplex pilot = d_pilots[pilot_idx];

        /* LSE: h_raw = y × conj(pilot) = H × dmrs_scaling × |pilot|² + noise_term
         * Low-PAPR pilots have |pilot|² = 1 (unit amplitude).
         * Divide by dmrs_scaling to normalize like CPU does.
         */
        cuFloatComplex h_raw = cuCmulf_fast(y, cuConjf_fast(pilot));
        cuFloatComplex h = make_cuFloatComplex(h_raw.x * h_normalizer,
                                               h_raw.y * h_normalizer);

        /* Store LSE estimate */
        /* Layout: [prb, port, dmrs_re] for coalesced access in interpolation */
        int out_idx = prb_idx * (nof_ports * 6) + port * 6 + d;
        d_lse_estimates[out_idx] = h;
    }
}

/**
 * Average LSE channel estimates across all DMRS symbols (time-domain averaging).
 *
 * This mirrors the CPU algorithm which averages h_est FIRST before computing noise.
 * CPU: scaled_estimates = Σ h_est[sym] / (beta × nof_dmrs_symbols)
 *
 * Since the LSE kernel already divided by dmrs_scaling (beta), we only need to
 * divide by nof_dmrs_symbols here to complete the time-domain averaging.
 *
 * Input: LSE estimates for each DMRS symbol [dmrs_sym, prb, port, dmrs_re]
 * Output: Averaged estimates [prb, port, dmrs_re] (single buffer for all RE)
 */
__global__ void kernel_average_lse_estimates(
    const cuFloatComplex* __restrict__ d_lse_all_symbols,  /* [nof_dmrs_sym, nof_prb * nof_ports * 6] */
    cuFloatComplex* __restrict__ d_lse_averaged,           /* [nof_prb * nof_ports * 6] */
    int nof_dmrs_symbols,
    int nof_prb,
    int nof_ports,
    int total_dmrs_re_per_symbol,  /* nof_prb * 6 for Type 1 */
    float beta)                    /* dmrs_scaling factor from CPU (unused - LSE already applied it) */
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    int total = nof_prb * nof_ports * 6;
    if (idx >= total) return;

    /* Scale factor: 1 / nof_dmrs_symbols (LSE kernel already divided by beta) */
    float scale = 1.0f / (float)nof_dmrs_symbols;

    /* Accumulate across all DMRS symbols */
    cuFloatComplex sum = make_cuFloatComplex(0.0f, 0.0f);
    for (int d = 0; d < nof_dmrs_symbols; d++) {
        int sym_offset = d * (nof_ports * total_dmrs_re_per_symbol);
        cuFloatComplex h = d_lse_all_symbols[sym_offset + idx];
        sum.x += h.x;
        sum.y += h.y;
    }

    /* Apply scaling */
    d_lse_averaged[idx] = make_cuFloatComplex(sum.x * scale, sum.y * scale);
}

/**
 * Compute noise variance from DMRS residuals using TIME-AVERAGED channel estimates.
 *
 * This mirrors the CPU algorithm exactly:
 * 1. Use averaged h_est (computed by kernel_average_lse_estimates)
 * 2. For each DMRS symbol: residual = rx - avg_h_est × pilot
 * 3. Accumulate |residual|² across all DMRS symbols and REs
 *
 * Using averaged h_est prevents the noise cancellation problem that occurs
 * when using per-symbol estimates.
 *
 * Launch with one block per port, threads cooperatively reduce.
 */
__global__ void kernel_noise_variance_from_dmrs(
    const unsigned int* __restrict__ d_grid_cbf16,
    const unsigned int* __restrict__ d_dmrs_seq,
    const cuFloatComplex* __restrict__ d_lse_averaged,   /* AVERAGED estimates across DMRS symbols */
    float* __restrict__ d_noise_var_partial,             /* Per-block partial sums */
    int nof_prb,
    int nof_ports,
    int grid_stride,
    int symbol_stride,
    int start_prb,
    int dmrs_symbol_idx,
    int start_symbol,        /* First symbol of allocation (for relative indexing) */
    float dmrs_scaling,
    int total_dmrs_re)    /* nof_prb * 6 for Type 1 */
{
    extern __shared__ float s_partial[];

    int port = blockIdx.x;
    int tid = threadIdx.x;
    int nof_threads = blockDim.x;

    if (port >= nof_ports) return;

    /* Reconstruct the ideal received signal using AVERAGED channel estimates.
     *
     * CPU algorithm (from port_channel_estimator_average_impl.cpp lines 446-491):
     * - First averages h_est across all DMRS symbols with scaling: avg_h = beta × Σ h / N
     *   where h_lse = H × 0.5 (after dividing by beta in LSE)
     *   So: avg_h = beta × H × 0.5
     * - Then computes: predicted_obs = avg_h × pilot_unit (NOT scaled by 2!)
     * - And residual = rx_pilots - predicted_obs
     *
     * For received signal: y = H × pilot_tx = H × beta × pilot_unit
     * predicted = avg_h × pilot_unit = beta × H × 0.5 × pilot_unit
     * residual = y - predicted = H × beta × pilot_unit - beta × H × 0.5 × pilot_unit
     *          = 0.5 × beta × H × pilot_unit
     *
     * The CPU intentionally has a residual that's 0.5 × signal, not zero.
     * This residual's power scales with signal power, giving a practical SINR
     * estimate (around 5-7 dB in ZMQ). We match this exactly.
     */
    float inv_sqrt2 = 0.70710678118f;  /* Unit pilot amplitude */
    float pilot_scale = 1.0f;  /* NO compensation - match CPU which leaves 0.5× signal residual */
    float local_sum = 0.0f;

    /* Each thread processes multiple DMRS REs */
    for (int dmrs_idx = tid; dmrs_idx < total_dmrs_re; dmrs_idx += nof_threads) {
        int prb_idx = dmrs_idx / 6;
        int d = dmrs_idx % 6;

        /* Type 1 DMRS: REs at subcarriers 0, 2, 4, 6, 8, 10 */
        const int dmrs_sc_offset[6] = {0, 2, 4, 6, 8, 10};
        int prb_start_sc = (start_prb + prb_idx) * 12;
        int sc = prb_start_sc + dmrs_sc_offset[d];

        /* Load received symbol.
         * Grid is uploaded with ABSOLUTE symbol indexing (0..13 for entire slot),
         * so use dmrs_symbol_idx directly without any offset. */
        int grid_idx = port * grid_stride + dmrs_symbol_idx * symbol_stride + sc;
        cuFloatComplex y = cbf16_to_fp32(d_grid_cbf16[grid_idx]);

        /* Regenerate DMRS pilot (accounting for start_prb offset) */
        int pilot_idx = (start_prb + prb_idx) * 6 + d;
        int bit_idx_real = 2 * pilot_idx;
        int bit_idx_imag = 2 * pilot_idx + 1;

        int word_idx_real = bit_idx_real / 32;
        int bit_pos_real = 31 - (bit_idx_real % 32);  // MSB-first to match gold_sequence_generate_kernel
        int c_real = (d_dmrs_seq[word_idx_real] >> bit_pos_real) & 1;

        int word_idx_imag = bit_idx_imag / 32;
        int bit_pos_imag = 31 - (bit_idx_imag % 32);  // MSB-first to match gold_sequence_generate_kernel
        int c_imag = (d_dmrs_seq[word_idx_imag] >> bit_pos_imag) & 1;

        /* QPSK with pilot_scale to reconstruct transmitted pilot amplitude */
        float p_real = (1 - 2 * c_real) * inv_sqrt2 * pilot_scale;
        float p_imag = (1 - 2 * c_imag) * inv_sqrt2 * pilot_scale;
        cuFloatComplex pilot = make_cuFloatComplex(p_real, p_imag);

        /* Load AVERAGED channel estimate and scale by beta (dmrs_scaling).
         *
         * CPU algorithm (port_channel_estimator_average_impl.cpp lines 447-449):
         *   scaling_factor = beta / nof_lse_symbols
         *   scaled_estimates = estimates × scaling_factor
         *
         * For time-domain average (nof_lse_symbols=1), this gives:
         *   scaled_estimates = filtered_estimates × beta
         *
         * Our d_lse_averaged = (Σ y × conj(p) / beta) / N = (Σ y × conj(p)) / (beta × N)
         * We need to multiply by beta to match CPU's scaled_estimates.
         */
        int lse_idx = prb_idx * (nof_ports * 6) + port * 6 + d;
        cuFloatComplex h_avg_raw = d_lse_averaged[lse_idx];
        /* Scale by dmrs_scaling to match CPU's scaled_estimates = estimates × beta */
        cuFloatComplex h_avg = make_cuFloatComplex(h_avg_raw.x * dmrs_scaling, h_avg_raw.y * dmrs_scaling);

        /* Compute ideal received using scaled estimate:
         * y_ideal = h_avg × pilot
         *
         * With h_avg = (Σ y × conj(p)) / N and pilot = unit amplitude:
         * For unity channel y ≈ beta × pilot, so:
         * y_ideal = (Σ beta × pilot × conj(pilot)) / N × pilot = beta × |p|² / N × pilot = beta × pilot / N
         *
         * The actual received is: y = beta × pilot + noise
         * residual = y - y_ideal = beta × pilot - beta × pilot / N = beta × pilot × (1 - 1/N) + noise
         *
         * For N > 1, there's a residual signal component. This matches CPU behavior.
         */
        cuFloatComplex y_ideal = cuCmulf_fast(h_avg, pilot);

        /* Residual = received - ideal ≈ noise */
        float res_real = y.x - y_ideal.x;
        float res_imag = y.y - y_ideal.y;

        /* Accumulate |residual|^2 */
        local_sum += res_real * res_real + res_imag * res_imag;
    }

    /* Store to shared memory */
    s_partial[tid] = local_sum;
    __syncthreads();

    /* Parallel reduction in shared memory */
    for (int stride = nof_threads / 2; stride > 0; stride >>= 1) {
        if (tid < stride) {
            s_partial[tid] += s_partial[tid + stride];
        }
        __syncthreads();
    }

    /* Thread 0 writes result */
    if (tid == 0) {
        d_noise_var_partial[port] = s_partial[0];
    }
}

__global__ void kernel_noise_variance_from_low_papr_dmrs(
    const unsigned int* __restrict__ d_grid_cbf16,
    const cuFloatComplex* __restrict__ d_pilots,
    const cuFloatComplex* __restrict__ d_lse_averaged,
    float* __restrict__ d_noise_var_partial,
    int nof_prb,
    int nof_ports,
    int grid_stride,
    int symbol_stride,
    int start_prb,
    int dmrs_symbol_idx,
    float dmrs_scaling,
    int total_dmrs_re)
{
    extern __shared__ float s_partial[];

    int port = blockIdx.x;
    int tid = threadIdx.x;
    int nof_threads = blockDim.x;

    if (port >= nof_ports) return;

    const int dmrs_sc_offset[6] = {0, 2, 4, 6, 8, 10};
    float local_sum = 0.0f;

    for (int dmrs_idx = tid; dmrs_idx < total_dmrs_re; dmrs_idx += nof_threads) {
        int prb_idx = dmrs_idx / 6;
        int d = dmrs_idx - prb_idx * 6;
        int sc = (start_prb + prb_idx) * 12 + dmrs_sc_offset[d];

        int grid_idx = port * grid_stride + dmrs_symbol_idx * symbol_stride + sc;
        cuFloatComplex y = cbf16_to_fp32(d_grid_cbf16[grid_idx]);

        int pilot_idx = prb_idx * 6 + d;
        cuFloatComplex pilot = d_pilots[pilot_idx];

        int lse_idx = prb_idx * (nof_ports * 6) + port * 6 + d;
        cuFloatComplex h_avg_raw = d_lse_averaged[lse_idx];
        cuFloatComplex h_avg = make_cuFloatComplex(h_avg_raw.x * dmrs_scaling, h_avg_raw.y * dmrs_scaling);
        cuFloatComplex y_ideal = cuCmulf_fast(h_avg, pilot);

        float res_real = y.x - y_ideal.x;
        float res_imag = y.y - y_ideal.y;
        local_sum += res_real * res_real + res_imag * res_imag;
    }

    s_partial[tid] = local_sum;
    __syncthreads();

    for (int stride = nof_threads >> 1; stride > 0; stride >>= 1) {
        if (tid < stride) {
            s_partial[tid] += s_partial[tid + stride];
        }
        __syncthreads();
    }

    if (tid == 0) {
        d_noise_var_partial[port] = s_partial[0];
    }
}

/**
 * Finalize noise variance: divide accumulated sum by count with dynamic scaling.
 *
 * The scaling factor accounts for differences between GPU and CPU noise estimation:
 * - GPU: residual = y - (h_avg × dmrs_scaling × pilot)
 * - CPU: residual = y - (h_lse × dmrs_scaling² / N × pilot)
 *
 * The residual magnitudes differ, requiring a configuration-dependent scaling factor
 * to match the CPU's noise variance estimate and achieve consistent SINR.
 */
__global__ void kernel_noise_variance_finalize(
    float* __restrict__ d_noise_vars,
    const float* __restrict__ d_partial_sums,
    int nof_ports,
    int nof_dmrs_symbols,
    int total_dmrs_re_per_symbol,
    float dmrs_scaling)
{
    int port = threadIdx.x;
    if (port >= nof_ports) return;

    /* Sum across all DMRS symbols */
    float total_sum = 0.0f;
    for (int d = 0; d < nof_dmrs_symbols; d++) {
        total_sum += d_partial_sums[d * nof_ports + port];
    }

    /* Average: divide by total DMRS REs.
     * Use (total_dmrs_re - 1) to match CPU's unbiased variance estimator. */
    int total_dmrs_re = nof_dmrs_symbols * total_dmrs_re_per_symbol;
    float divisor = (total_dmrs_re > 1) ? (float)(total_dmrs_re - 1) : 1.0f;
    float noise_var = total_sum / divisor;

    /* Dynamic scaling based on DMRS configuration.
     *
     * The GPU and CPU use different formulas for the predicted signal, leading to
     * different residual magnitudes. The scaling factor corrects for this difference.
     *
     * GPU residual factor: (1 - 0.5/N) where N = nof_dmrs_symbols
     * CPU residual factor: (1 - beta × 0.5/N) where beta = dmrs_scaling
     *
     * For beta ≈ 1.414 (2 CDM groups):
     * - N=1: GPU/CPU ratio ≈ 2.9, but empirically 2.0 works better
     * - N=2: GPU/CPU ratio ≈ 1.35
     *
     * Use a scaling formula that interpolates between these cases:
     * scale = 2.0 / sqrt(N) provides reasonable balance.
     *
     * Additionally, for very small allocations (few DMRS REs), the statistical
     * noise in the variance estimate is higher, so we may need more regularization.
     */
    float N = (float)nof_dmrs_symbols;
    /* Dynamic noise variance scaling to match CPU behavior.
     *
     * The CPU uses DIFFERENT scaling for equalization vs noise estimation:
     * - Equalization: h = pilots_lse / beta / N
     * - Noise estimation: h = pilots_lse × beta / N
     *
     * The noise h is beta² larger than eq h. Since GPU uses the same scaling
     * (both divided by beta and N), the GPU noise variance is beta² times smaller.
     *
     * To compensate, we multiply noise_var by dmrs_scaling² (beta²).
     *
     * Additional factors:
     * - 2.0 / sqrt(N) provides empirical regularization
     */
    float beta_sq = dmrs_scaling * dmrs_scaling;
    /* Reduced scale factor to better match CPU noise variance estimation.
     * Previous formula (2.0 * beta_sq / sqrt(N)) was overestimating noise variance.
     * New formula: beta_sq / sqrt(N) to match CPU behavior more closely. */
    float base_scale = beta_sq / sqrtf(N);

    /* For small allocations (< 50 DMRS REs total), increase scaling slightly
     * to provide more regularization and improve decode stability. */
    float alloc_factor = 1.0f;
    if (total_dmrs_re < 50) {
        alloc_factor = 1.2f;  /* 20% more regularization for small allocations */
    }

    float noise_var_scaled = noise_var * base_scale * alloc_factor;

    /* Apply minimum floor to prevent numerical issues with clean channels. */
    d_noise_vars[port] = fmaxf(noise_var_scaled, 0.001f);
}

__global__ void kernel_noise_variance_finalize_low_papr(
    float* __restrict__ d_noise_vars,
    const float* __restrict__ d_partial_sums,
    int nof_ports,
    int nof_dmrs_symbols,
    int total_dmrs_re_per_symbol)
{
    int port = threadIdx.x;
    if (port >= nof_ports) return;

    float total_sum = 0.0f;
    for (int d = 0; d < nof_dmrs_symbols; d++) {
        total_sum += d_partial_sums[d * nof_ports + port];
    }

    int total_dmrs_re = nof_dmrs_symbols * total_dmrs_re_per_symbol;
    float divisor = (total_dmrs_re > 1) ? static_cast<float>(total_dmrs_re - 1) : 1.0f;
    float noise_var = total_sum / divisor;

    /* The averaged LSE includes the same DMRS noise that is present in the
     * residual. Correct the variance loss from subtracting an N-symbol mean. */
    if (nof_dmrs_symbols > 1) {
        noise_var *= static_cast<float>(nof_dmrs_symbols) / static_cast<float>(nof_dmrs_symbols - 1);
    }

    d_noise_vars[port] = fmaxf(noise_var, 1e-8f);
}

/**
 * Frequency interpolation: expand DMRS estimates to all subcarriers within PRB.
 *
 * Type 1 DMRS: estimates at SC 0,2,4,6,8,10 → interpolate to 1,3,5,7,9,11
 *
 * Input: LSE estimates at DMRS positions [prb, port, dmrs_re_in_prb]
 * Output: Full estimates for all subcarriers [port, prb, sc_in_prb]
 */
__global__ void kernel_freq_interpolate_type1(
    const cuFloatComplex* __restrict__ d_lse_estimates,
    cuFloatComplex* __restrict__ d_full_estimates,
    int nof_prb,
    int nof_ports)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    int total = nof_prb * nof_ports * 12;  /* All REs */
    if (idx >= total) return;

    int prb = idx / (nof_ports * 12);
    int port_sc = idx % (nof_ports * 12);
    int port = port_sc / 12;
    int sc_in_prb = port_sc % 12;

    /* Load LSE estimates for this PRB and port */
    int lse_base = prb * (nof_ports * 6) + port * 6;

    cuFloatComplex h;

    if (sc_in_prb % 2 == 0) {
        /* DMRS position - direct copy */
        int dmrs_idx = sc_in_prb / 2;
        h = d_lse_estimates[lse_base + dmrs_idx];
    } else {
        /* Interpolate between adjacent DMRS positions */
        int left_idx = sc_in_prb / 2;
        int right_idx = left_idx + 1;
        if (right_idx >= 6) right_idx = 5;  /* Edge case */

        cuFloatComplex h_left = d_lse_estimates[lse_base + left_idx];
        cuFloatComplex h_right = d_lse_estimates[lse_base + right_idx];

        /* Linear interpolation (alpha = 0.5 for midpoint) */
        h.x = 0.5f * (h_left.x + h_right.x);
        h.y = 0.5f * (h_left.y + h_right.y);
    }

    /* Store to output: [port, prb, sc_in_prb] layout */
    int out_idx = port * (nof_prb * 12) + prb * 12 + sc_in_prb;
    d_full_estimates[out_idx] = h;
}

/**
 * Frequency-domain smoothing kernel (FP16 in-place).
 *
 * Applies a 15-tap raised cosine FIR filter to denoise LSE channel estimates
 * across frequency, matching the CPU port_channel_estimator_helpers filter.
 *
 * The filter operates on DMRS pilot positions (6 per PRB for Type 1), then
 * re-interpolates to all 12 subcarriers per PRB.
 *
 * Grid:  (nof_prb, nof_dmrs_symbols)
 * Block: (nof_ports)
 *
 * Each thread handles one (prb, port) pair for one DMRS symbol, applying
 * the filter across 6 pilot positions and writing 12 smoothed subcarriers.
 */
__global__ void kernel_fd_smoothing_fp16(
    __half2* __restrict__ d_estimates,  /* [dmrs_sym, prb, port, 12] in-place */
    int nof_prb,
    int nof_ports,
    int nof_dmrs_symbols,
    int dmrs_type)
{
    int prb_idx = blockIdx.x;
    int dmrs_sym_idx = blockIdx.y;
    int port = threadIdx.x;

    if (prb_idx >= nof_prb || dmrs_sym_idx >= nof_dmrs_symbols || port >= nof_ports) return;

    const int HALF_FILTER = FD_SMOOTH_FILTER_LEN / 2;  /* 7 */
    int re_per_dmrs_sym = nof_prb * nof_ports * 12;
    int base = dmrs_sym_idx * re_per_dmrs_sym + prb_idx * (nof_ports * 12) + port * 12;
    int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);
    int total_pilots = nof_prb * nof_pilots;
    int pilot_base = prb_idx * nof_pilots;

    /* Apply FIR filter to each of the 6 pilot positions */
    float smoothed_re[6], smoothed_im[6];

    #pragma unroll
    for (int p = 0; p < 6; p++) {
        if (p >= nof_pilots) break;
        float sum_re = 0.0f;
        float sum_im = 0.0f;
        float coef_sum = 0.0f;
        int global_pilot = pilot_base + p;

        for (int t = 0; t < FD_SMOOTH_FILTER_LEN; t++) {
            int src_pilot = global_pilot + (t - HALF_FILTER);

            /* Skip out-of-range taps (don't clamp) */
            if (src_pilot < 0 || src_pilot >= total_pilots) continue;

            /* Read from the appropriate PRB's estimates */
            int src_prb = src_pilot / nof_pilots;
            int src_pos = src_pilot % nof_pilots;
            int src_idx = dmrs_sym_idx * re_per_dmrs_sym +
                          src_prb * (nof_ports * 12) + port * 12 +
                          pusch_dmrs_sc_offset(dmrs_type, 0, src_pos);

            __half2 h_src = d_estimates[src_idx];
            float r = __half2float(__low2half(h_src));
            float i = __half2float(__high2half(h_src));

            sum_re += d_rc_filter[t] * r;
            sum_im += d_rc_filter[t] * i;
            coef_sum += d_rc_filter[t];
        }

        /* Renormalize to compensate for truncated taps at band edges */
        if (coef_sum > 1e-6f) {
            float inv_coef = 1.0f / coef_sum;
            sum_re *= inv_coef;
            sum_im *= inv_coef;
        }

        smoothed_re[p] = sum_re;
        smoothed_im[p] = sum_im;
    }

    /* Write back smoothed estimates for all 12 subcarriers */
    #pragma unroll
    for (int sc = 0; sc < 12; sc++) {
        float r, i_val;
        int left = 0;
        int right = 0;
        int left_sc = pusch_dmrs_sc_offset(dmrs_type, 0, 0);
        int right_sc = left_sc;
        bool exact = false;
        for (int p = 0; p < nof_pilots; ++p) {
            int pilot_sc = pusch_dmrs_sc_offset(dmrs_type, 0, p);
            if (pilot_sc == sc) {
                left = right = p;
                exact = true;
                break;
            }
            if (pilot_sc < sc) {
                left = p;
                left_sc = pilot_sc;
            } else {
                right = p;
                right_sc = pilot_sc;
                break;
            }
        }
        if (exact || sc <= left_sc || right_sc <= left_sc) {
            r = smoothed_re[left];
            i_val = smoothed_im[left];
        } else {
            float alpha = (float)(sc - left_sc) / (float)(right_sc - left_sc);
            r = (1.0f - alpha) * smoothed_re[left] + alpha * smoothed_re[right];
            i_val = (1.0f - alpha) * smoothed_im[left] + alpha * smoothed_im[right];
        }
        d_estimates[base + sc] = __halves2half2(__float2half(r), __float2half(i_val));
    }
}

__global__ void kernel_mimo_fd_smoothing_fp16(
    __half2* __restrict__ d_estimates,  /* [dmrs_sym, prb, port, layer, 12] in-place */
    int nof_prb,
    int nof_ports,
    int nof_layers,
    int nof_dmrs_symbols,
    int dmrs_type)
{
    int prb_idx = blockIdx.x;
    int dmrs_sym_idx = blockIdx.y;
    int layer = blockIdx.z;
    int port = threadIdx.x;

    if (prb_idx >= nof_prb || dmrs_sym_idx >= nof_dmrs_symbols || port >= nof_ports || layer >= nof_layers) return;

    const int HALF_FILTER = FD_SMOOTH_FILTER_LEN / 2;
    int re_per_layer = 12;
    int re_per_port = nof_layers * re_per_layer;
    int re_per_prb = nof_ports * re_per_port;
    int re_per_dmrs_sym = nof_prb * re_per_prb;
    int base = dmrs_sym_idx * re_per_dmrs_sym + prb_idx * re_per_prb + port * re_per_port + layer * re_per_layer;
    int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);
    int total_pilots = nof_prb * nof_pilots;
    int pilot_base = prb_idx * nof_pilots;
    int cdm_offset = layer / 2;

    float smoothed_re[6], smoothed_im[6];

    #pragma unroll
    for (int p = 0; p < 6; p++) {
        if (p >= nof_pilots) break;
        float sum_re = 0.0f;
        float sum_im = 0.0f;
        float coef_sum = 0.0f;
        int global_pilot = pilot_base + p;

        for (int t = 0; t < FD_SMOOTH_FILTER_LEN; t++) {
            int src_pilot = global_pilot + (t - HALF_FILTER);
            if (src_pilot < 0 || src_pilot >= total_pilots) continue;

            int src_prb = src_pilot / nof_pilots;
            int src_pos = src_pilot % nof_pilots;
            int src_sc = pusch_dmrs_sc_offset(dmrs_type, cdm_offset, src_pos);
            int src_idx = dmrs_sym_idx * re_per_dmrs_sym +
                          src_prb * re_per_prb + port * re_per_port + layer * re_per_layer + src_sc;

            __half2 h_src = d_estimates[src_idx];
            sum_re += d_rc_filter[t] * __half2float(__low2half(h_src));
            sum_im += d_rc_filter[t] * __half2float(__high2half(h_src));
            coef_sum += d_rc_filter[t];
        }

        if (coef_sum > 1e-6f) {
            float inv_coef = 1.0f / coef_sum;
            sum_re *= inv_coef;
            sum_im *= inv_coef;
        }

        smoothed_re[p] = sum_re;
        smoothed_im[p] = sum_im;
    }

    __half2 smoothed_pilots[6];
    #pragma unroll
    for (int p = 0; p < 6; p++) {
        if (p >= nof_pilots) break;
        smoothed_pilots[p] = __halves2half2(__float2half(smoothed_re[p]), __float2half(smoothed_im[p]));
    }

    #pragma unroll
    for (int sc = 0; sc < 12; sc++) {
        d_estimates[base + sc] =
            interpolate_dmrs_pilots_half2(smoothed_pilots, nof_pilots, dmrs_type, cdm_offset, sc);
    }
}

/**
 * Wide-block variant of kernel_mimo_fd_smoothing_fp16 (OCUDU_MIMO_FD_SMOOTH_OPT).
 *
 * The baseline launches (prb, dmrs_sym, layer) blocks of only nof_ports (<=8)
 * threads, wasting most of every warp and serializing 6 pilot smoothings plus
 * 12 interpolations per thread. This variant covers one (prb, dmrs_sym) per
 * block with (12, ports, layers) threads: pilot smoothing is computed once per
 * (pilot, port, layer) by the sc<nof_pilots threads into shared memory, then
 * every thread interpolates exactly one output RE. Arithmetic per pilot/RE is
 * identical to the baseline; only the thread mapping changes.
 *
 * Grid:  (nof_prb, nof_dmrs_symbols)
 * Block: (12, nof_ports, nof_layers)
 * Shmem: nof_layers * nof_ports * 6 * sizeof(__half2)
 */
__global__ void kernel_mimo_fd_smoothing_fp16_wide(
    __half2* __restrict__ d_estimates,  /* [dmrs_sym, prb, port, layer, 12] in-place */
    int nof_prb,
    int nof_ports,
    int nof_layers,
    int nof_dmrs_symbols,
    int dmrs_type)
{
    int prb_idx = blockIdx.x;
    int dmrs_sym_idx = blockIdx.y;
    int sc = threadIdx.x;
    int port = threadIdx.y;
    int layer = threadIdx.z;

    if (prb_idx >= nof_prb || dmrs_sym_idx >= nof_dmrs_symbols) return;

    const int HALF_FILTER = FD_SMOOTH_FILTER_LEN / 2;
    int re_per_layer = 12;
    int re_per_port = nof_layers * re_per_layer;
    int re_per_prb = nof_ports * re_per_port;
    int re_per_dmrs_sym = nof_prb * re_per_prb;
    int base = dmrs_sym_idx * re_per_dmrs_sym + prb_idx * re_per_prb + port * re_per_port + layer * re_per_layer;
    int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);
    int total_pilots = nof_prb * nof_pilots;
    int pilot_base = prb_idx * nof_pilots;
    int cdm_offset = layer / 2;

    extern __shared__ __half2 s_smoothed[]; /* [layer][port][6] */
    __half2* s_pilots = s_smoothed + (layer * nof_ports + port) * 6;

    if (sc < nof_pilots) {
        int p = sc;
        float sum_re = 0.0f;
        float sum_im = 0.0f;
        float coef_sum = 0.0f;
        int global_pilot = pilot_base + p;

        for (int t = 0; t < FD_SMOOTH_FILTER_LEN; t++) {
            int src_pilot = global_pilot + (t - HALF_FILTER);
            if (src_pilot < 0 || src_pilot >= total_pilots) continue;

            int src_prb = src_pilot / nof_pilots;
            int src_pos = src_pilot % nof_pilots;
            int src_sc = pusch_dmrs_sc_offset(dmrs_type, cdm_offset, src_pos);
            int src_idx = dmrs_sym_idx * re_per_dmrs_sym +
                          src_prb * re_per_prb + port * re_per_port + layer * re_per_layer + src_sc;

            __half2 h_src = d_estimates[src_idx];
            sum_re += d_rc_filter[t] * __half2float(__low2half(h_src));
            sum_im += d_rc_filter[t] * __half2float(__high2half(h_src));
            coef_sum += d_rc_filter[t];
        }

        if (coef_sum > 1e-6f) {
            float inv_coef = 1.0f / coef_sum;
            sum_re *= inv_coef;
            sum_im *= inv_coef;
        }

        s_pilots[p] = __halves2half2(__float2half(sum_re), __float2half(sum_im));
    }

    __syncthreads();

    d_estimates[base + sc] = interpolate_dmrs_pilots_half2(s_pilots, nof_pilots, dmrs_type, cdm_offset, sc);
}

/**
 * Frequency-domain smoothing kernel (FP32 cuFloatComplex, in-place).
 *
 * Same algorithm as kernel_fd_smoothing_fp16 but for the separate-kernel path
 * which uses cuFloatComplex with layout [dmrs_sym][port][prb * 12].
 *
 * Grid:  (nof_prb, nof_dmrs_symbols)
 * Block: (nof_ports)
 */
__global__ void kernel_fd_smoothing_fp32(
    cuFloatComplex* __restrict__ d_estimates,  /* [dmrs_sym][port][nof_prb * 12] in-place */
    int nof_prb,
    int nof_ports,
    int nof_dmrs_symbols)
{
    int prb_idx = blockIdx.x;
    int dmrs_sym_idx = blockIdx.y;
    int port = threadIdx.x;

    if (prb_idx >= nof_prb || dmrs_sym_idx >= nof_dmrs_symbols || port >= nof_ports) return;

    const int HALF_FILTER = FD_SMOOTH_FILTER_LEN / 2;  /* 7 */
    int re_per_sym = nof_ports * nof_prb * 12;
    int port_base = dmrs_sym_idx * re_per_sym + port * (nof_prb * 12) + prb_idx * 12;
    int total_pilots = nof_prb * 6;
    int pilot_base = prb_idx * 6;

    /* Apply FIR filter to each of the 6 pilot positions */
    float smoothed_re[6], smoothed_im[6];

    #pragma unroll
    for (int p = 0; p < 6; p++) {
        float sum_re = 0.0f;
        float sum_im = 0.0f;
        float coef_sum = 0.0f;
        int global_pilot = pilot_base + p;

        for (int t = 0; t < FD_SMOOTH_FILTER_LEN; t++) {
            int src_pilot = global_pilot + (t - HALF_FILTER);

            /* Skip out-of-range taps (don't clamp) */
            if (src_pilot < 0 || src_pilot >= total_pilots) continue;

            int src_prb = src_pilot / 6;
            int src_pos = src_pilot % 6;
            int src_idx = dmrs_sym_idx * re_per_sym +
                          port * (nof_prb * 12) + src_prb * 12 + src_pos * 2;

            cuFloatComplex h_src = d_estimates[src_idx];
            sum_re += d_rc_filter[t] * h_src.x;
            sum_im += d_rc_filter[t] * h_src.y;
            coef_sum += d_rc_filter[t];
        }

        /* Renormalize to compensate for truncated taps at band edges */
        if (coef_sum > 1e-6f) {
            float inv_coef = 1.0f / coef_sum;
            sum_re *= inv_coef;
            sum_im *= inv_coef;
        }

        smoothed_re[p] = sum_re;
        smoothed_im[p] = sum_im;
    }

    /* Write back smoothed estimates for all 12 subcarriers */
    #pragma unroll
    for (int sc = 0; sc < 12; sc++) {
        float r, i_val;
        if (sc % 2 == 0) {
            r = smoothed_re[sc / 2];
            i_val = smoothed_im[sc / 2];
        } else {
            int left = sc / 2;
            int right = (left + 1 < 6) ? left + 1 : 5;
            r = 0.5f * (smoothed_re[left] + smoothed_re[right]);
            i_val = 0.5f * (smoothed_im[left] + smoothed_im[right]);
        }
        d_estimates[port_base + sc] = make_cuFloatComplex(r, i_val);
    }
}

/**
 * Time interpolation: interpolate estimates from DMRS symbols to data symbols.
 *
 * For symbols between two DMRS symbols: linear interpolation
 * For symbols before first DMRS: copy from first DMRS
 * For symbols after last DMRS: copy from last DMRS
 *
 * Input: Frequency-interpolated estimates for each DMRS symbol
 * Output: Estimates for all symbols in the allocation
 */
/**
 * Time interpolation kernel with optional scaling.
 *
 * CPU algorithm applies: total_scaling = 1 / (beta × N) to the LSE
 * Our LSE kernel already applies 1/beta, so we need to apply 1/N here.
 *
 * The scale factor ensures GPU channel estimates match CPU magnitude:
 * - GPU without scale: h = y × conj(p) / beta
 * - GPU with scale: h = y × conj(p) / beta × scale = y × conj(p) / (beta × N)
 * - CPU: h = y × conj(p) / (beta × N)
 */
__global__ void kernel_time_interpolate(
    const cuFloatComplex* __restrict__ d_dmrs_estimates,  /* [dmrs_sym_idx, port, nof_re] */
    cuFloatComplex* __restrict__ d_full_estimates,        /* [symbol, port, nof_re] */
    const int* __restrict__ dmrs_symbol_indices,          /* Array of DMRS symbol indices */
    int nof_dmrs_symbols,
    int nof_symbols,
    int nof_ports,
    int nof_re_per_symbol,
    int start_symbol,
    float output_scale)   /* Scale factor: 1/nof_dmrs_symbols to match CPU averaging */
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    int total = nof_symbols * nof_ports * nof_re_per_symbol;
    if (idx >= total) return;

    int sym_rel = idx / (nof_ports * nof_re_per_symbol);
    int port_re = idx % (nof_ports * nof_re_per_symbol);
    int port = port_re / nof_re_per_symbol;
    int re = port_re % nof_re_per_symbol;

    /* Time-average all DMRS estimates (matches CPU "average" strategy) */
    int est_idx = port * nof_re_per_symbol + re;
    int stride = nof_ports * nof_re_per_symbol;

    cuFloatComplex h = make_cuFloatComplex(0.0f, 0.0f);
    for (int d = 0; d < nof_dmrs_symbols; d++) {
        cuFloatComplex hd = d_dmrs_estimates[d * stride + est_idx];
        h.x += hd.x;
        h.y += hd.y;
    }
    float inv_n = output_scale / (float)nof_dmrs_symbols;
    h.x *= inv_n;
    h.y *= inv_n;

    /* Store output: [symbol, port, re] */
    int out_idx = sym_rel * (nof_ports * nof_re_per_symbol) + port * nof_re_per_symbol + re;
    d_full_estimates[out_idx] = h;
}

/**
 * Fused E2E kernel with channel estimate lookup.
 *
 * This version looks up pre-computed channel estimates from GPU memory
 * instead of receiving them as input. Supports ZF, MMSE, and MMSE-IRC.
 */
template <int NOF_PORTS, int ALGORITHM>
__global__ void kernel_pusch_e2e_with_estimates(
    const unsigned int* __restrict__ d_grid_cbf16,
    const cuFloatComplex* __restrict__ d_estimates,  /* GPU-computed estimates */
    const int* __restrict__ re_indices,
    const float* __restrict__ noise_vars,
    __half* __restrict__ llrs_half,
    const unsigned int* __restrict__ scramble_seq,
    float* __restrict__ d_eq_noise_var_sum,         /* Accumulator for SINR */
    unsigned int* __restrict__ d_eq_noise_var_count, /* RE counter for SINR */
    int nof_re,
    int mod_order,
    int grid_stride,           /* nof_symbols * nof_subcarriers */
    int symbol_stride,         /* grid_nof_subcarriers (for decoding src_re) */
    int est_stride,            /* nof_re_per_symbol for estimates */
    int start_symbol,          /* allocation start symbol */
    int start_subcarrier,      /* allocation start subcarrier (start_prb * 12) */
    float tx_scaling)
{
    int re_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (re_idx >= nof_re) return;

    /* Load noise variances to shared memory */
    __shared__ float s_noise_vars[8];
    if (threadIdx.x < NOF_PORTS) {
        s_noise_vars[threadIdx.x] = noise_vars[threadIdx.x];
    }
    __syncthreads();

    /* Get source RE index (in full grid) */
    int src_re = re_indices[re_idx];

    /* Decode symbol and subcarrier from src_re */
    int symbol = src_re / symbol_stride;
    int subcarrier = src_re % symbol_stride;

    /* Compute relative indices for estimate lookup */
    int symbol_rel = symbol - start_symbol;
    int re_in_symbol = subcarrier - start_subcarrier;

    /* Load channel data for all ports.
     * Grid is uploaded with ALL slot symbols (absolute indexing 0..13),
     * so use src_re directly for grid lookup.
     * Estimates are GPU-computed with relative indexing, so use symbol_rel/re_in_symbol. */
    cuFloatComplex y[8], h[8];

    /* Validate estimate indexing bounds */
    if (re_in_symbol < 0 || re_in_symbol >= est_stride || symbol_rel < 0) {
        /* Out of bounds - use identity channel to prevent crash */
        #pragma unroll
        for (int p = 0; p < NOF_PORTS; p++) {
            h[p] = make_cuFloatComplex(1.0f, 0.0f);
            y[p] = make_cuFloatComplex(0.0f, 0.0f);
        }
    } else {
        #pragma unroll
        for (int p = 0; p < NOF_PORTS; p++) {
            int grid_idx = p * grid_stride + src_re;
            y[p] = cbf16_to_fp32(d_grid_cbf16[grid_idx]);

            /* Load estimate from GPU-computed buffer */
            /* Estimate layout: [symbol_rel, port, re_in_symbol] */
            int est_idx = symbol_rel * (NOF_PORTS * est_stride) + p * est_stride + re_in_symbol;
            h[p] = d_estimates[est_idx];
        }
    }

    /* ---- Equalization (ZF, MMSE, or MMSE-IRC) ---- */
    cuFloatComplex eq;
    float eq_noise_var;
    equalize_symbol<NOF_PORTS, ALGORITHM>(y, h, s_noise_vars, tx_scaling, eq, eq_noise_var);

    /* Accumulate post-equalization noise variance for SINR calculation. */
    sinr_accumulate(d_eq_noise_var_sum, d_eq_noise_var_count, eq_noise_var);

    float inv_noise = 1.0f / eq_noise_var;

    /* ---- Soft Demodulation ---- */
    float llr_vals[8];

    if (mod_order == 2) {
        float scale = 2.0f * 1.41421356f * inv_noise;
        llr_vals[0] = eq.x * scale;
        llr_vals[1] = eq.y * scale;
    } else if (mod_order == 4) {
        /* 16QAM soft demodulation - piecewise linear to match CPU implementation.
         * M_SQRT1_10 = 1/sqrt(10) = 0.31622776
         * GAIN_FIRST = 4 * M_SQRT1_10 = 1.2649
         * THRESHOLD = 2 * M_SQRT1_10 = 0.6324555
         */
        const float M_SQRT1_10 = 0.31622776601683794f;
        const float GAIN_FIRST = 4.0f * M_SQRT1_10;
        const float THRESHOLD = 2.0f * M_SQRT1_10;
        const float CONST_0_8 = 0.8f;

        float abs_re = fabsf(eq.x);
        float abs_im = fabsf(eq.y);

        /* Bits 0,1: piecewise linear with threshold at 2*M_SQRT1_10 */
        float l_first_re = GAIN_FIRST * eq.x;
        float l_first_im = GAIN_FIRST * eq.y;

        /* Second interval: 2*first - copysign(0.8, symbol) */
        float l_second_re = 2.0f * l_first_re - copysignf(CONST_0_8, eq.x);
        float l_second_im = 2.0f * l_first_im - copysignf(CONST_0_8, eq.y);

        /* Select based on threshold */
        float l_01_re = (abs_re > THRESHOLD) ? l_second_re : l_first_re;
        float l_01_im = (abs_im > THRESHOLD) ? l_second_im : l_first_im;

        /* Bits 2,3: 0.8 - |first| */
        float l_23_re = CONST_0_8 - fabsf(l_first_re);
        float l_23_im = CONST_0_8 - fabsf(l_first_im);

        /* Scale by 1/noise_var */
        llr_vals[0] = l_01_re * inv_noise;
        llr_vals[1] = l_01_im * inv_noise;
        llr_vals[2] = l_23_re * inv_noise;
        llr_vals[3] = l_23_im * inv_noise;

    } else if (mod_order == 6) {
        // 64QAM: piecewise-linear approximation matching CPU implementation
        soft_demod_64qam_piecewise(eq, inv_noise, llr_vals);
    } else if (mod_order == 8) {
        // 256QAM: piecewise-linear approximation matching CPU implementation
        soft_demod_256qam_piecewise(eq, inv_noise, llr_vals);
    }

    /* ---- Descrambling ---- */
    int llr_base = re_idx * mod_order;
    int word_idx = llr_base / 32;
    int bit_offset = llr_base % 32;
    unsigned int scr_word = scramble_seq[word_idx];
    unsigned int scr_word_next = scramble_seq[word_idx + 1];

    for (int b = 0; b < mod_order; b++) {
        int bit_pos = bit_offset + b;
        unsigned int scr_bit;
        if (bit_pos < 32) {
            scr_bit = (scr_word >> bit_pos) & 1;
        } else {
            scr_bit = (scr_word_next >> (bit_pos - 32)) & 1;
        }

        float llr = scr_bit ? -llr_vals[b] : llr_vals[b];
        llr = fmaxf(-65504.0f, fminf(65504.0f, llr));
        llrs_half[llr_base + b] = __float2half(llr);
    }
}

/* ============================================================================
 * API Implementation
 * ============================================================================ */























/* ============================================================================
 * Transform Deprecoding (IDFT) Support for Full GPU Path
 * ============================================================================ */

__device__ __forceinline__ cuFloatComplex cmul_e2e(cuFloatComplex a, cuFloatComplex b) {
    return make_cuFloatComplex(a.x * b.x - a.y * b.y,
                               a.x * b.y + a.y * b.x);
}

__device__ __forceinline__ cuFloatComplex twiddle_idft_cycles_e2e(float cycles) {
    float angle = 2.0f * 3.14159265359f * cycles;
    float c, s;
    __sincosf(angle, &s, &c);
    return make_cuFloatComplex(c, s);
}


__device__ __forceinline__ cuFloatComplex mixed_radix_butterfly_idft_e2e(
    const cuFloatComplex* __restrict__ in,
    int radix,
    int n0)
{
    if (radix == 2) {
        return (n0 == 0) ? make_cuFloatComplex(in[0].x + in[1].x, in[0].y + in[1].y) :
                           make_cuFloatComplex(in[0].x - in[1].x, in[0].y - in[1].y);
    }

    if (radix == 3) {
        constexpr float c = -0.5f;
        constexpr float s = 0.8660254037844386f;
        if (n0 == 0) {
            return make_cuFloatComplex(in[0].x + in[1].x + in[2].x,
                                       in[0].y + in[1].y + in[2].y);
        }
        cuFloatComplex w1 = (n0 == 1) ? make_cuFloatComplex(c, s) : make_cuFloatComplex(c, -s);
        cuFloatComplex w2 = (n0 == 1) ? make_cuFloatComplex(c, -s) : make_cuFloatComplex(c, s);
        cuFloatComplex a = cmul_e2e(w1, in[1]);
        cuFloatComplex b = cmul_e2e(w2, in[2]);
        return make_cuFloatComplex(in[0].x + a.x + b.x, in[0].y + a.y + b.y);
    }

    if (radix == 4) {
        if (n0 == 0) {
            return make_cuFloatComplex(in[0].x + in[1].x + in[2].x + in[3].x,
                                       in[0].y + in[1].y + in[2].y + in[3].y);
        }
        if (n0 == 1) {
            return make_cuFloatComplex(in[0].x - in[1].y - in[2].x + in[3].y,
                                       in[0].y + in[1].x - in[2].y - in[3].x);
        }
        if (n0 == 2) {
            return make_cuFloatComplex(in[0].x - in[1].x + in[2].x - in[3].x,
                                       in[0].y - in[1].y + in[2].y - in[3].y);
        }
        return make_cuFloatComplex(in[0].x + in[1].y - in[2].x - in[3].y,
                                   in[0].y - in[1].x - in[2].y + in[3].x);
    }

    cuFloatComplex sum = make_cuFloatComplex(0.0f, 0.0f);
    cuFloatComplex w = make_cuFloatComplex(1.0f, 0.0f);
    float angle = 2.0f * 3.14159265359f * static_cast<float>(n0) / static_cast<float>(radix);
    float s, c;
    __sincosf(angle, &s, &c);
    cuFloatComplex w_step = make_cuFloatComplex(c, s);

    #pragma unroll
    for (int i = 0; i < 5; ++i) {
        if (i >= radix) {
            break;
        }
        cuFloatComplex x = cmul_e2e(w, in[i]);
        sum.x += x.x;
        sum.y += x.y;
        w = cmul_e2e(w, w_step);
    }
    return sum;
}

/**
 * Fused E2E kernel with GPU channel estimates AND transform deprecoding.
 *
 * Processing flow per symbol:
 * 1. Equalize REs using GPU-computed channel estimates
 * 2. Apply a shared-memory mixed-radix IDFT across DFT-size subcarriers
 * 3. Soft demodulate + descramble
 *
 * Each block processes one OFDM symbol. Threads cooperatively compute IDFT.
 */
template <int NOF_PORTS, int ALGORITHM>
__global__ void kernel_pusch_e2e_with_estimates_deprecode(
    const unsigned int* __restrict__ d_grid_cbf16,
    const cuFloatComplex* __restrict__ d_estimates,
    const int* __restrict__ re_indices,
    const float* __restrict__ noise_vars,
    __half* __restrict__ llrs_half,
    uint32_t scramble_c_init,  /* c_init for on-the-fly LFSR (matches non-transform path) */
    float* __restrict__ d_eq_noise_var_sum,
    unsigned int* __restrict__ d_eq_noise_var_count,
    int nof_re,
    int mod_order,
    int grid_stride,
    int symbol_stride,         /* nof_subcarriers (for decoding src_re) */
    int est_stride,            /* nof_re_per_symbol for estimates */
    int start_symbol,          /* allocation start symbol */
    int start_subcarrier,      /* allocation start subcarrier (start_prb * 12) */
    float tx_scaling,
    int dft_size,
    uint64_t packed_fft_factors,
    int nof_fft_factors,
    int nof_symbols)
{
    extern __shared__ char shared_mem[];
    cuFloatComplex* s_eq_symbols = reinterpret_cast<cuFloatComplex*>(shared_mem);
    cuFloatComplex* s_fft_tmp = s_eq_symbols + dft_size;
    float* s_eq_noise_sum = reinterpret_cast<float*>(s_fft_tmp + dft_size);
    unsigned int* s_eq_noise_count = reinterpret_cast<unsigned int*>(s_eq_noise_sum + blockDim.x);

    int symbol_idx = blockIdx.x;
    if (symbol_idx >= nof_symbols) return;

    int tid = threadIdx.x;
    int nof_threads = blockDim.x;

    /* Load noise variances to shared memory */
    __shared__ float s_noise_vars[8];
    if (tid < NOF_PORTS) {
        s_noise_vars[tid] = noise_vars[tid];
    }
    __syncthreads();

    /* Calculate RE range for this symbol */
    int re_per_symbol = dft_size;
    int re_start = symbol_idx * re_per_symbol;
    int re_end = min(re_start + re_per_symbol, nof_re);
    int nof_re_this_symbol = re_end - re_start;

    /* Phase 1: Equalize all REs for this symbol */
    float local_eq_noise_sum = 0.0f;
    unsigned int local_eq_noise_count = 0;
    for (int local_re = tid; local_re < nof_re_this_symbol; local_re += nof_threads) {
        int global_re_idx = re_start + local_re;
        int src_re = re_indices[global_re_idx];

        /* Decode symbol and subcarrier from src_re */
        int symbol = src_re / symbol_stride;
        int subcarrier = src_re % symbol_stride;

        /* Compute relative indices for estimate lookup */
        int symbol_rel = symbol - start_symbol;
        int re_in_symbol = subcarrier - start_subcarrier;

        /* Load channel data for all ports.
         * Grid is uploaded with ALL slot symbols (absolute indexing 0..13),
         * so use src_re directly for grid lookup.
         * Estimates are GPU-computed with relative indexing, so use symbol_rel/re_in_symbol. */
        cuFloatComplex y[8], h[8];

        #pragma unroll
        for (int p = 0; p < NOF_PORTS; p++) {
            int grid_idx = p * grid_stride + src_re;
            y[p] = cbf16_to_fp32(d_grid_cbf16[grid_idx]);

            /* Estimate layout: [symbol_rel, port, re_in_symbol] */
            int est_idx = symbol_rel * (NOF_PORTS * est_stride) + p * est_stride + re_in_symbol;
            h[p] = d_estimates[est_idx];
        }

        /* Equalization (ZF, MMSE, or MMSE-IRC) */
        cuFloatComplex eq;
        float eq_noise_var;
        equalize_symbol<NOF_PORTS, ALGORITHM>(y, h, s_noise_vars, tx_scaling, eq, eq_noise_var);
        if (sinr_noise_var_is_valid(eq_noise_var)) {
            local_eq_noise_sum += eq_noise_var;
            ++local_eq_noise_count;
        }

        /* Store equalized symbol to shared memory for IDFT. */
        s_eq_symbols[local_re] = eq;
    }

    /* Zero-pad if symbol has fewer REs */
    for (int local_re = nof_re_this_symbol + tid; local_re < dft_size; local_re += nof_threads) {
        s_eq_symbols[local_re] = make_cuFloatComplex(0.0f, 0.0f);
    }

    s_eq_noise_sum[tid] = local_eq_noise_sum;
    s_eq_noise_count[tid] = local_eq_noise_count;
    __syncthreads();

    for (int stride = nof_threads >> 1; stride > 0; stride >>= 1) {
        if (tid < stride) {
            s_eq_noise_sum[tid] += s_eq_noise_sum[tid + stride];
            s_eq_noise_count[tid] += s_eq_noise_count[tid + stride];
        }
        __syncthreads();
    }

    __shared__ float s_symbol_eq_noise_var;
    if (tid == 0) {
        float avg_eq_noise_var;
        if (s_eq_noise_count[0] > 0) {
            avg_eq_noise_var = s_eq_noise_sum[0] / static_cast<float>(s_eq_noise_count[0]);
            atomicAdd(d_eq_noise_var_sum, s_eq_noise_sum[0]);
            atomicAdd(d_eq_noise_var_count, s_eq_noise_count[0]);
        } else {
            avg_eq_noise_var = 0.0f;
            #pragma unroll
            for (int p = 0; p < NOF_PORTS; p++) {
                avg_eq_noise_var += s_noise_vars[p];
            }
            avg_eq_noise_var /= (NOF_PORTS * tx_scaling * tx_scaling);
        }
        s_symbol_eq_noise_var = fmaxf(avg_eq_noise_var, 1e-10f);
    }
    __syncthreads();

    /* Phase 2: Apply transform deprecoding with a shared-memory mixed-radix
     * inverse FFT. The recursive Cooley-Tukey layout is applied iteratively:
     *
     *   in[p, k1 + M*k2] -> out[p + P*n0, k1]
     *
     * where L = R*M is the remaining transform length and P is the product of
     * previous radices. This keeps output in natural time order and avoids a
     * separate permutation pass.
     */
    cuFloatComplex* stage_in = s_eq_symbols;
    cuFloatComplex* stage_out = s_fft_tmp;
    int prefix_count = 1;
    int remaining = dft_size;

    for (int stage = 0; stage < nof_fft_factors; ++stage) {
        int radix = static_cast<int>((packed_fft_factors >> (4 * stage)) & 0xf);
        int next_remaining = remaining / radix;

        for (int idx = tid; idx < dft_size; idx += nof_threads) {
            int k1 = idx % next_remaining;
            int new_prefix = idx / next_remaining;
            int n0 = new_prefix / prefix_count;
            int p = new_prefix - n0 * prefix_count;
            int in_base = p * remaining + k1;

            cuFloatComplex inputs[5];
            #pragma unroll
            for (int k2 = 0; k2 < 5; ++k2) {
                inputs[k2] = (k2 < radix) ? stage_in[in_base + next_remaining * k2] :
                                            make_cuFloatComplex(0.0f, 0.0f);
            }

            cuFloatComplex sum = mixed_radix_butterfly_idft_e2e(inputs, radix, n0);
            if ((n0 != 0) && (k1 != 0)) {
                cuFloatComplex tw = twiddle_idft_cycles_e2e(
                    static_cast<float>(n0 * k1) / static_cast<float>(remaining));
                sum = cmul_e2e(tw, sum);
            }
            stage_out[idx] = sum;
        }
        __syncthreads();

        cuFloatComplex* tmp = stage_in;
        stage_in = stage_out;
        stage_out = tmp;
        prefix_count *= radix;
        remaining = next_remaining;
    }

    for (int out_idx = tid; out_idx < dft_size; out_idx += nof_threads) {
        float scale = rsqrtf((float)dft_size);
        cuFloatComplex x = stage_in[out_idx];
        s_eq_symbols[out_idx] = make_cuFloatComplex(x.x * scale, x.y * scale);
    }
    __syncthreads();

    /* Phase 3: Soft demodulation + descrambling */
    /* Unitary transform deprecoding preserves the average equalized noise power. */
    float inv_noise = 1.0f / s_symbol_eq_noise_var;

    for (int local_re = tid; local_re < nof_re_this_symbol; local_re += nof_threads) {
        int global_re_idx = re_start + local_re;
        cuFloatComplex eq = s_eq_symbols[local_re];

        /* Soft demodulation */
        float llr_vals[8];

        if (mod_order == 2) {
            float scale = 2.0f * 1.41421356f * inv_noise;
            llr_vals[0] = eq.x * scale;
            llr_vals[1] = eq.y * scale;
        } else if (mod_order == 4) {
            float scale = inv_noise * 0.6324555f;
            float abs_re = fabsf(eq.x);
            float abs_im = fabsf(eq.y);
            llr_vals[0] = eq.x * scale * 2.0f;
            llr_vals[1] = eq.y * scale * 2.0f;
            llr_vals[2] = (0.6324555f * 2.0f - abs_re) * scale * 2.0f;
            llr_vals[3] = (0.6324555f * 2.0f - abs_im) * scale * 2.0f;
        } else if (mod_order == 6) {
            /* 64QAM max-log MAP soft demapping.
             * Uses closed-form distance to decision boundaries.
             * 1/sqrt(42) = 0.154303349962092
             */
            const float scale64 = 0.154303349962092f;
            float scale = inv_noise * scale64;
            float abs_re = fabsf(eq.x);
            float abs_im = fabsf(eq.y);
            /* Bit 0,1: linear in symbol */
            llr_vals[0] = eq.x * scale * 4.0f * 2.0f;
            llr_vals[1] = eq.y * scale * 4.0f * 2.0f;
            /* Bit 2,3: distance from 4M level */
            llr_vals[2] = (scale64 * 4.0f - abs_re) * scale * 2.0f * 2.0f;
            llr_vals[3] = (scale64 * 4.0f - abs_im) * scale * 2.0f * 2.0f;
            /* Bit 4,5: nested distance from 2M level */
            float level2 = scale64 * 2.0f;
            llr_vals[4] = (level2 - fabsf(abs_re - scale64 * 4.0f)) * scale * 2.0f * 2.0f;
            llr_vals[5] = (level2 - fabsf(abs_im - scale64 * 4.0f)) * scale * 2.0f * 2.0f;
        } else if (mod_order == 8) {
            float scale = inv_noise * 0.07669650f;
            float abs_re = fabsf(eq.x);
            float abs_im = fabsf(eq.y);
            llr_vals[0] = eq.x * scale * 2.0f;
            llr_vals[1] = eq.y * scale * 2.0f;
            llr_vals[2] = (abs_re - 0.07669650f * 8.0f) * scale * 2.0f;
            llr_vals[3] = (abs_im - 0.07669650f * 8.0f) * scale * 2.0f;
            float level2 = 0.07669650f * 4.0f;
            llr_vals[4] = (fabsf(abs_re - level2 * 2.0f) - level2) * scale * 2.0f;
            llr_vals[5] = (fabsf(abs_im - level2 * 2.0f) - level2) * scale * 2.0f;
            float level3 = 0.07669650f * 2.0f;
            float d_re = fabsf(abs_re - level2 * 2.0f);
            float d_im = fabsf(abs_im - level2 * 2.0f);
            llr_vals[6] = (fabsf(d_re - level3 * 2.0f) - level3) * scale * 2.0f;
            llr_vals[7] = (fabsf(d_im - level3 * 2.0f) - level3) * scale * 2.0f;
        }

        /* Descrambling - use on-the-fly LFSR (matches non-transform path) */
        int llr_base = global_re_idx * mod_order;

        /* Advance LFSRs to starting bit position for this RE.
         * NC_SKIP (1600) is already accounted for in the advance functions. */
        int bit_start = llr_base + NC_SKIP;
        uint32_t x1 = advance_x1_local(1, bit_start);
        uint32_t x2 = advance_x2_local(scramble_c_init, bit_start);

        for (int b = 0; b < mod_order; b++) {
            /* Generate scrambling bit on-the-fly */
            uint32_t scr_bit = (x1 ^ x2) & 1;

            float llr = scr_bit ? -llr_vals[b] : llr_vals[b];
            llr = fmaxf(-65504.0f, fminf(65504.0f, llr));
            llrs_half[llr_base + b] = __float2half(llr);

            /* Step LFSRs for next bit */
            x1 = step_x1_local(x1);
            x2 = step_x2_local(x2);
        }
    }
}

#ifdef OCUDU_PHY_CUDA_ENABLE_VKFFT
template <int NOF_PORTS, int ALGORITHM>
__global__ void kernel_pusch_e2e_deprecode_equalize_gather(
    const unsigned int* __restrict__ d_grid_cbf16,
    const cuFloatComplex* __restrict__ d_estimates,
    const int* __restrict__ re_indices,
    const float* __restrict__ noise_vars,
    cuFloatComplex* __restrict__ transform_symbols,
    float* __restrict__ symbol_noise_vars,
    float* __restrict__ d_eq_noise_var_sum,
    unsigned int* __restrict__ d_eq_noise_var_count,
    int nof_re,
    int grid_stride,
    int symbol_stride,
    int est_stride,
    int start_symbol,
    int start_subcarrier,
    float tx_scaling,
    int dft_size,
    int nof_symbols)
{
    extern __shared__ char shared_mem[];
    float* s_eq_noise_sum = reinterpret_cast<float*>(shared_mem);
    unsigned int* s_eq_noise_count = reinterpret_cast<unsigned int*>(s_eq_noise_sum + blockDim.x);

    int symbol_idx = blockIdx.x;
    if (symbol_idx >= nof_symbols) return;

    int tid = threadIdx.x;
    int nof_threads = blockDim.x;

    __shared__ float s_noise_vars[8];
    if (tid < NOF_PORTS) {
        s_noise_vars[tid] = noise_vars[tid];
    }
    __syncthreads();

    int re_start = symbol_idx * dft_size;
    int re_end = min(re_start + dft_size, nof_re);
    int nof_re_this_symbol = re_end - re_start;
    float local_eq_noise_sum = 0.0f;
    unsigned int local_eq_noise_count = 0;

    for (int local_re = tid; local_re < nof_re_this_symbol; local_re += nof_threads) {
        int global_re_idx = re_start + local_re;
        int src_re = re_indices[global_re_idx];
        int symbol = src_re / symbol_stride;
        int subcarrier = src_re % symbol_stride;
        int symbol_rel = symbol - start_symbol;
        int re_in_symbol = subcarrier - start_subcarrier;

        cuFloatComplex y[8], h[8];
        #pragma unroll
        for (int p = 0; p < NOF_PORTS; p++) {
            y[p] = cbf16_to_fp32(d_grid_cbf16[p * grid_stride + src_re]);
            h[p] = d_estimates[symbol_rel * (NOF_PORTS * est_stride) + p * est_stride + re_in_symbol];
        }

        cuFloatComplex eq;
        float eq_noise_var;
        equalize_symbol<NOF_PORTS, ALGORITHM>(y, h, s_noise_vars, tx_scaling, eq, eq_noise_var);
        if (sinr_noise_var_is_valid(eq_noise_var)) {
            local_eq_noise_sum += eq_noise_var;
            ++local_eq_noise_count;
        }

        transform_symbols[re_start + local_re] = eq;
    }

    for (int local_re = nof_re_this_symbol + tid; local_re < dft_size; local_re += nof_threads) {
        transform_symbols[re_start + local_re] = make_cuFloatComplex(0.0f, 0.0f);
    }

    s_eq_noise_sum[tid] = local_eq_noise_sum;
    s_eq_noise_count[tid] = local_eq_noise_count;
    __syncthreads();

    for (int stride = nof_threads >> 1; stride > 0; stride >>= 1) {
        if (tid < stride) {
            s_eq_noise_sum[tid] += s_eq_noise_sum[tid + stride];
            s_eq_noise_count[tid] += s_eq_noise_count[tid + stride];
        }
        __syncthreads();
    }

    if (tid == 0) {
        float avg_eq_noise_var;
        if (s_eq_noise_count[0] > 0) {
            avg_eq_noise_var = s_eq_noise_sum[0] / static_cast<float>(s_eq_noise_count[0]);
            atomicAdd(d_eq_noise_var_sum, s_eq_noise_sum[0]);
            atomicAdd(d_eq_noise_var_count, s_eq_noise_count[0]);
        } else {
            avg_eq_noise_var = 0.0f;
            #pragma unroll
            for (int p = 0; p < NOF_PORTS; p++) {
                avg_eq_noise_var += s_noise_vars[p];
            }
            avg_eq_noise_var /= (NOF_PORTS * tx_scaling * tx_scaling);
        }
        symbol_noise_vars[symbol_idx] = fmaxf(avg_eq_noise_var, 1e-10f);
    }
}

__global__ void kernel_pusch_e2e_deprecode_demod_descramble(
    const cuFloatComplex* __restrict__ transform_symbols,
    const float* __restrict__ symbol_noise_vars,
    __half* __restrict__ llrs_half,
    uint32_t scramble_c_init,
    int nof_re,
    int mod_order,
    int dft_size)
{
    int global_re_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (global_re_idx >= nof_re) return;

    int symbol_idx = global_re_idx / dft_size;
    cuFloatComplex eq = transform_symbols[global_re_idx];
    float scale = rsqrtf(static_cast<float>(dft_size));
    eq.x *= scale;
    eq.y *= scale;

    float inv_noise = 1.0f / symbol_noise_vars[symbol_idx];
    float llr_vals[8];

    if (mod_order == 2) {
        float llr_scale = 2.0f * 1.41421356f * inv_noise;
        llr_vals[0] = eq.x * llr_scale;
        llr_vals[1] = eq.y * llr_scale;
    } else if (mod_order == 4) {
        float llr_scale = inv_noise * 0.6324555f;
        float abs_re = fabsf(eq.x);
        float abs_im = fabsf(eq.y);
        llr_vals[0] = eq.x * llr_scale * 2.0f;
        llr_vals[1] = eq.y * llr_scale * 2.0f;
        llr_vals[2] = (0.6324555f * 2.0f - abs_re) * llr_scale * 2.0f;
        llr_vals[3] = (0.6324555f * 2.0f - abs_im) * llr_scale * 2.0f;
    } else if (mod_order == 6) {
        const float scale64 = 0.154303349962092f;
        float llr_scale = inv_noise * scale64;
        float abs_re = fabsf(eq.x);
        float abs_im = fabsf(eq.y);
        llr_vals[0] = eq.x * llr_scale * 4.0f * 2.0f;
        llr_vals[1] = eq.y * llr_scale * 4.0f * 2.0f;
        llr_vals[2] = (scale64 * 4.0f - abs_re) * llr_scale * 2.0f * 2.0f;
        llr_vals[3] = (scale64 * 4.0f - abs_im) * llr_scale * 2.0f * 2.0f;
        float level2 = scale64 * 2.0f;
        llr_vals[4] = (level2 - fabsf(abs_re - scale64 * 4.0f)) * llr_scale * 2.0f * 2.0f;
        llr_vals[5] = (level2 - fabsf(abs_im - scale64 * 4.0f)) * llr_scale * 2.0f * 2.0f;
    } else if (mod_order == 8) {
        float llr_scale = inv_noise * 0.07669650f;
        float abs_re = fabsf(eq.x);
        float abs_im = fabsf(eq.y);
        llr_vals[0] = eq.x * llr_scale * 2.0f;
        llr_vals[1] = eq.y * llr_scale * 2.0f;
        llr_vals[2] = (abs_re - 0.07669650f * 8.0f) * llr_scale * 2.0f;
        llr_vals[3] = (abs_im - 0.07669650f * 8.0f) * llr_scale * 2.0f;
        float level2 = 0.07669650f * 4.0f;
        llr_vals[4] = (fabsf(abs_re - level2 * 2.0f) - level2) * llr_scale * 2.0f;
        llr_vals[5] = (fabsf(abs_im - level2 * 2.0f) - level2) * llr_scale * 2.0f;
        float level3 = 0.07669650f * 2.0f;
        float d_re = fabsf(abs_re - level2 * 2.0f);
        float d_im = fabsf(abs_im - level2 * 2.0f);
        llr_vals[6] = (fabsf(d_re - level3 * 2.0f) - level3) * llr_scale * 2.0f;
        llr_vals[7] = (fabsf(d_im - level3 * 2.0f) - level3) * llr_scale * 2.0f;
    }

    int llr_base = global_re_idx * mod_order;
    int bit_start = llr_base + NC_SKIP;
    uint32_t x1 = advance_x1_local(1, bit_start);
    uint32_t x2 = advance_x2_local(scramble_c_init, bit_start);

    for (int b = 0; b < mod_order; b++) {
        uint32_t scr_bit = (x1 ^ x2) & 1;
        float llr = scr_bit ? -llr_vals[b] : llr_vals[b];
        llr = fmaxf(-65504.0f, fminf(65504.0f, llr));
        llrs_half[llr_base + b] = __float2half(llr);
        x1 = step_x1_local(x1);
        x2 = step_x2_local(x2);
    }
}

#endif


/* ============================================================================
 * GPU Warm-up for Kernel JIT Compilation
 * ============================================================================ */



/* ============================================================================
 * OPTIMIZED API - Uses fused FP16 kernels for maximum performance
 * ============================================================================ */




/* ============================================================================
 * KERNEL LAUNCH WRAPPERS (definitions) — see pusch_e2e_launch.cuh.
 * These exist so the host API TU can trigger kernel launches without the
 * kernel definitions/template instantiations leaving this TU.
 * ============================================================================ */

void launch_kernel_average_lse_estimates(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const cuFloatComplex* __restrict__ d_lse_all_symbols, cuFloatComplex* __restrict__ d_lse_averaged, int nof_dmrs_symbols, int nof_prb, int nof_ports, int total_dmrs_re_per_symbol, float beta) {
    kernel_average_lse_estimates<<<grid, block, shmem, stream>>>(d_lse_all_symbols, d_lse_averaged, nof_dmrs_symbols, nof_prb, nof_ports, total_dmrs_re_per_symbol, beta);
}

void launch_kernel_compute_mimo_csi_from_estimates(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const unsigned int* __restrict__ d_grid_cbf16, const __half2* __restrict__ d_estimates_fp16, const int* __restrict__ d_dmrs_symbol_indices, float* __restrict__ d_sinr_epre_result, float* __restrict__ d_accum, unsigned int* __restrict__ d_done_counter, int nof_prb, int nof_ports, int nof_layers, int nof_dmrs_symbols, int grid_stride, int symbol_stride, int start_prb, float pilot_spacing_hz, float delta_t_s, const float* __restrict__ d_symbol_start_times, float2* __restrict__ d_cfo_phasors, int start_symbol, float dmrs_scaling, int dmrs_type) {
    kernel_compute_mimo_csi_from_estimates<<<grid, block, shmem, stream>>>(d_grid_cbf16, d_estimates_fp16, d_dmrs_symbol_indices, d_sinr_epre_result, d_accum, d_done_counter, nof_prb, nof_ports, nof_layers, nof_dmrs_symbols, grid_stride, symbol_stride, start_prb, pilot_spacing_hz, delta_t_s, d_symbol_start_times, d_cfo_phasors, start_symbol, dmrs_scaling, dmrs_type);
}

void launch_kernel_compute_residual_sinr_epre_v3(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const unsigned int* __restrict__ d_grid_cbf16, const __half2* __restrict__ d_estimates_fp16, const uint32_t* __restrict__ d_dmrs_pilot_bits, const int* __restrict__ d_dmrs_symbol_indices, float* __restrict__ d_sinr_noise_accum, float* __restrict__ d_sinr_rsrp_accum, int nof_prb, int nof_ports, int nof_dmrs_symbols, int grid_stride, int symbol_stride, int start_prb, float dmrs_scaling, int words_per_dmrs_sym, int dmrs_type) {
    kernel_compute_residual_sinr_epre_v3<<<grid, block, shmem, stream>>>(d_grid_cbf16, d_estimates_fp16, d_dmrs_pilot_bits, d_dmrs_symbol_indices, d_sinr_noise_accum, d_sinr_rsrp_accum, nof_prb, nof_ports, nof_dmrs_symbols, grid_stride, symbol_stride, start_prb, dmrs_scaling, words_per_dmrs_sym, dmrs_type);
}

void launch_kernel_compute_ta_and_cfo(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const __half2* __restrict__ d_estimates_fp16, float* __restrict__ d_ta_result, float* __restrict__ d_cfo_result, float* __restrict__ d_accum, int nof_prb, int nof_ports, int nof_dmrs_symbols, float pilot_spacing_hz, float delta_t_s, const float* __restrict__ d_symbol_start_times, const int* __restrict__ d_dmrs_symbol_indices, float2* __restrict__ d_cfo_phasors, int start_symbol, int dmrs_type) {
    kernel_compute_ta_and_cfo<<<grid, block, shmem, stream>>>(d_estimates_fp16, d_ta_result, d_cfo_result, d_accum, nof_prb, nof_ports, nof_dmrs_symbols, pilot_spacing_hz, delta_t_s, d_symbol_start_times, d_dmrs_symbol_indices, d_cfo_phasors, start_symbol, dmrs_type);
}

void launch_kernel_cross_validation_noise_variance(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const __half2* __restrict__ d_estimates_fp16, float* __restrict__ d_noise_var_accum, const unsigned int* __restrict__ d_grid_cbf16, const uint32_t* __restrict__ d_dmrs_pilot_bits, const int* __restrict__ d_dmrs_symbol_indices, float* __restrict__ d_sinr_noise_accum, float* __restrict__ d_sinr_rsrp_accum, int nof_prb, int nof_ports, int nof_dmrs_symbols, int grid_stride, int symbol_stride, int start_prb, float dmrs_scaling, int words_per_dmrs_sym, const float* __restrict__ d_cfo_hz_ptr, const float* __restrict__ d_symbol_start_times, const float2* __restrict__ d_cfo_phasors, float* __restrict__ d_noise_vars_out, unsigned int* __restrict__ d_cv_done_counter, const float* __restrict__ d_epre_accum, float* __restrict__ d_sinr_epre_result, int noise_mode, int dmrs_type) {
    kernel_cross_validation_noise_variance<<<grid, block, shmem, stream>>>(d_estimates_fp16, d_noise_var_accum, d_grid_cbf16, d_dmrs_pilot_bits, d_dmrs_symbol_indices, d_sinr_noise_accum, d_sinr_rsrp_accum, nof_prb, nof_ports, nof_dmrs_symbols, grid_stride, symbol_stride, start_prb, dmrs_scaling, words_per_dmrs_sym, d_cfo_hz_ptr, d_symbol_start_times, d_cfo_phasors, d_noise_vars_out, d_cv_done_counter, d_epre_accum, d_sinr_epre_result, noise_mode, dmrs_type);
}

void launch_kernel_dmrs_lse_type1(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const unsigned int* __restrict__ d_grid_cbf16, const unsigned int* __restrict__ d_dmrs_seq, cuFloatComplex* __restrict__ d_lse_estimates, int nof_prb, int nof_ports, int grid_stride, int symbol_stride, int start_prb, int dmrs_symbol_idx, int start_symbol, float dmrs_scaling) {
    kernel_dmrs_lse_type1<<<grid, block, shmem, stream>>>(d_grid_cbf16, d_dmrs_seq, d_lse_estimates, nof_prb, nof_ports, grid_stride, symbol_stride, start_prb, dmrs_symbol_idx, start_symbol, dmrs_scaling);
}

void launch_kernel_dmrs_lse_type1_low_papr(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const unsigned int* __restrict__ d_grid_cbf16, const cuFloatComplex* __restrict__ d_pilots, cuFloatComplex* __restrict__ d_lse_estimates, int nof_prb, int nof_ports, int grid_stride, int symbol_stride, int start_prb, int dmrs_symbol_idx, int start_symbol, float dmrs_scaling) {
    kernel_dmrs_lse_type1_low_papr<<<grid, block, shmem, stream>>>(d_grid_cbf16, d_pilots, d_lse_estimates, nof_prb, nof_ports, grid_stride, symbol_stride, start_prb, dmrs_symbol_idx, start_symbol, dmrs_scaling);
}

void launch_kernel_fd_smoothing_fp16(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, __half2* __restrict__ d_estimates, int nof_prb, int nof_ports, int nof_dmrs_symbols, int dmrs_type) {
    kernel_fd_smoothing_fp16<<<grid, block, shmem, stream>>>(d_estimates, nof_prb, nof_ports, nof_dmrs_symbols, dmrs_type);
}

void launch_kernel_fd_smoothing_fp32(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, cuFloatComplex* __restrict__ d_estimates, int nof_prb, int nof_ports, int nof_dmrs_symbols) {
    kernel_fd_smoothing_fp32<<<grid, block, shmem, stream>>>(d_estimates, nof_prb, nof_ports, nof_dmrs_symbols);
}

void launch_kernel_finalize_noise_variance(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, float* __restrict__ d_noise_var_accum, const unsigned int* __restrict__ d_noise_count, int nof_ports, float dmrs_scaling, int nof_dmrs_symbols) {
    kernel_finalize_noise_variance<<<grid, block, shmem, stream>>>(d_noise_var_accum, d_noise_count, nof_ports, dmrs_scaling, nof_dmrs_symbols);
}

void launch_kernel_finalize_sinr_epre(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const float* __restrict__ d_sinr_noise_accum, const float* __restrict__ d_sinr_rsrp_accum, const float* __restrict__ d_epre_accum, float* __restrict__ d_noise_vars_out, float* __restrict__ d_sinr_epre_result, int nof_prb, int nof_ports, int nof_dmrs_symbols, float dmrs_scaling, int dmrs_type) {
    kernel_finalize_sinr_epre<<<grid, block, shmem, stream>>>(d_sinr_noise_accum, d_sinr_rsrp_accum, d_epre_accum, d_noise_vars_out, d_sinr_epre_result, nof_prb, nof_ports, nof_dmrs_symbols, dmrs_scaling, dmrs_type);
}

void launch_kernel_freq_interpolate_type1(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const cuFloatComplex* __restrict__ d_lse_estimates, cuFloatComplex* __restrict__ d_full_estimates, int nof_prb, int nof_ports) {
    kernel_freq_interpolate_type1<<<grid, block, shmem, stream>>>(d_lse_estimates, d_full_estimates, nof_prb, nof_ports);
}

void launch_kernel_fused_lse_freq_interp_noise_fp16(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const unsigned int* __restrict__ d_grid_cbf16, const uint32_t* __restrict__ d_dmrs_c_inits, const int* __restrict__ dmrs_symbol_indices, __half2* __restrict__ d_estimates_fp16, float* __restrict__ d_noise_var_accum, unsigned int* __restrict__ d_noise_count, float* __restrict__ d_rsrp_accum, float* __restrict__ d_epre_accum, int nof_prb, int nof_ports, int nof_dmrs_symbols, int grid_stride, int symbol_stride, int start_prb, float dmrs_scaling, int dmrs_type) {
    kernel_fused_lse_freq_interp_noise_fp16<<<grid, block, shmem, stream>>>(d_grid_cbf16, d_dmrs_c_inits, dmrs_symbol_indices, d_estimates_fp16, d_noise_var_accum, d_noise_count, d_rsrp_accum, d_epre_accum, nof_prb, nof_ports, nof_dmrs_symbols, grid_stride, symbol_stride, start_prb, dmrs_scaling, dmrs_type);
}

void launch_kernel_gather_dmrs_pilot_bits(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, uint32_t* __restrict__ d_dst, const uint32_t* __restrict__ d_src, const int* __restrict__ d_symbol_indices, int words_per_sym) {
    kernel_gather_dmrs_pilot_bits<<<grid, block, shmem, stream>>>(d_dst, d_src, d_symbol_indices, words_per_sym);
}

void launch_kernel_gather_dmrs_pilot_bits_reset(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, uint32_t* __restrict__ d_dst, const uint32_t* __restrict__ d_src, const int* __restrict__ d_symbol_indices, int words_per_sym, float* __restrict__ d_sinr_epre_result) {
    kernel_gather_dmrs_pilot_bits_reset<<<grid, block, shmem, stream>>>(d_dst, d_src, d_symbol_indices, words_per_sym, d_sinr_epre_result);
}

void launch_kernel_generate_dmrs_pilots(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, uint32_t* __restrict__ d_pilot_bits, uint32_t c_init, int nof_pilots) {
    kernel_generate_dmrs_pilots<<<grid, block, shmem, stream>>>(d_pilot_bits, c_init, nof_pilots);
}

void launch_kernel_generate_dmrs_pilots_batched(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, uint32_t* __restrict__ d_pilot_bits, int nof_pilots, int words_per_sym, int words_per_slot, int nof_slots, uint32_t scrambling_id, int n_scid) {
    kernel_generate_dmrs_pilots_batched<<<grid, block, shmem, stream>>>(d_pilot_bits, nof_pilots, words_per_sym, words_per_slot, nof_slots, scrambling_id, n_scid);
}

void launch_kernel_generate_low_papr_pilots(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, cuFloatComplex* __restrict__ d_pilots, int u, int M_zc) {
    kernel_generate_low_papr_pilots<<<grid, block, shmem, stream>>>(d_pilots, u, M_zc);
}

void launch_kernel_generate_scrambling_sequence(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, uint32_t* __restrict__ d_scrambling_seq, uint32_t c_init, int nof_bits) {
    kernel_generate_scrambling_sequence<<<grid, block, shmem, stream>>>(d_scrambling_seq, c_init, nof_bits);
}

void launch_kernel_mimo_dmrs_residual_noise_variance(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const unsigned int* __restrict__ d_grid_cbf16, const uint32_t* __restrict__ d_dmrs_pilot_bits, const int* __restrict__ d_dmrs_symbol_indices, const __half2* __restrict__ d_estimates_fp16, float* __restrict__ d_noise_var_accum, int nof_prb, int nof_ports, int nof_layers, int nof_dmrs_symbols, int grid_stride, int symbol_stride, int start_prb, float dmrs_scaling, int words_per_dmrs_sym, float* __restrict__ d_noise_vars_out, unsigned int* __restrict__ d_done_counter, const float* __restrict__ d_cfo_hz_ptr, const float* __restrict__ d_symbol_start_times, const float2* __restrict__ d_cfo_phasors, int dmrs_type) {
    kernel_mimo_dmrs_residual_noise_variance<<<grid, block, shmem, stream>>>(d_grid_cbf16, d_dmrs_pilot_bits, d_dmrs_symbol_indices, d_estimates_fp16, d_noise_var_accum, nof_prb, nof_ports, nof_layers, nof_dmrs_symbols, grid_stride, symbol_stride, start_prb, dmrs_scaling, words_per_dmrs_sym, d_noise_vars_out, d_done_counter, d_cfo_hz_ptr, d_symbol_start_times, d_cfo_phasors, dmrs_type);
}

void launch_kernel_mimo_dmrs_residual_noise_variance_wide(cudaStream_t stream, const unsigned int* __restrict__ d_grid_cbf16, const uint32_t* __restrict__ d_dmrs_pilot_bits, const int* __restrict__ d_dmrs_symbol_indices, const __half2* __restrict__ d_estimates_fp16, float* __restrict__ d_noise_var_accum, int nof_prb, int nof_ports, int nof_layers, int nof_dmrs_symbols, int grid_stride, int symbol_stride, int start_prb, float dmrs_scaling, int words_per_dmrs_sym, float* __restrict__ d_noise_vars_out, unsigned int* __restrict__ d_done_counter, const float* __restrict__ d_cfo_hz_ptr, const float* __restrict__ d_symbol_start_times, const float2* __restrict__ d_cfo_phasors, int dmrs_type) {
    int nof_cdm = (nof_layers + 1) / 2;
    int nof_pilots = pusch_dmrs_pilots_per_cdm(dmrs_type);
    dim3 grid(nof_prb);
    dim3 block(nof_pilots * nof_cdm, nof_ports);
    kernel_mimo_dmrs_residual_noise_variance_wide<<<grid, block, 0, stream>>>(d_grid_cbf16, d_dmrs_pilot_bits, d_dmrs_symbol_indices, d_estimates_fp16, d_noise_var_accum, nof_prb, nof_ports, nof_layers, nof_dmrs_symbols, grid_stride, symbol_stride, start_prb, dmrs_scaling, words_per_dmrs_sym, d_noise_vars_out, d_done_counter, d_cfo_hz_ptr, d_symbol_start_times, d_cfo_phasors, dmrs_type);
}

void launch_kernel_mimo_fd_smoothing_fp16(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, __half2* __restrict__ d_estimates, int nof_prb, int nof_ports, int nof_layers, int nof_dmrs_symbols, int dmrs_type) {
    kernel_mimo_fd_smoothing_fp16<<<grid, block, shmem, stream>>>(d_estimates, nof_prb, nof_ports, nof_layers, nof_dmrs_symbols, dmrs_type);
}

void launch_kernel_mimo_fd_smoothing_fp16_wide(cudaStream_t stream, __half2* __restrict__ d_estimates, int nof_prb, int nof_ports, int nof_layers, int nof_dmrs_symbols, int dmrs_type) {
    dim3 grid(nof_prb, nof_dmrs_symbols);
    dim3 block(12, nof_ports, nof_layers);
    size_t shmem = static_cast<size_t>(nof_layers) * nof_ports * 6 * sizeof(__half2);
    kernel_mimo_fd_smoothing_fp16_wide<<<grid, block, shmem, stream>>>(d_estimates, nof_prb, nof_ports, nof_layers, nof_dmrs_symbols, dmrs_type);
}

void launch_kernel_mimo_lse_2layer_fp16(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const unsigned int* __restrict__ d_grid_cbf16, const uint32_t* __restrict__ d_dmrs_seq, const int* __restrict__ dmrs_symbol_indices, __half2* __restrict__ d_estimates_fp16, int nof_prb, int nof_ports, int nof_dmrs_symbols, int grid_stride, int symbol_stride, int start_prb, float dmrs_scaling, int words_per_dmrs_sym_stride, int dmrs_type) {
    kernel_mimo_lse_2layer_fp16<<<grid, block, shmem, stream>>>(d_grid_cbf16, d_dmrs_seq, dmrs_symbol_indices, d_estimates_fp16, nof_prb, nof_ports, nof_dmrs_symbols, grid_stride, symbol_stride, start_prb, dmrs_scaling, words_per_dmrs_sym_stride, dmrs_type);
}

void launch_kernel_mimo_lse_3layer_fp16(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const unsigned int* __restrict__ d_grid_cbf16, const uint32_t* __restrict__ d_dmrs_seq, const int* __restrict__ dmrs_symbol_indices, __half2* __restrict__ d_estimates_fp16, int nof_prb, int nof_ports, int nof_dmrs_symbols, int grid_stride, int symbol_stride, int start_prb, float dmrs_scaling, int words_per_dmrs_sym_stride, int dmrs_type) {
    kernel_mimo_lse_3layer_fp16<<<grid, block, shmem, stream>>>(d_grid_cbf16, d_dmrs_seq, dmrs_symbol_indices, d_estimates_fp16, nof_prb, nof_ports, nof_dmrs_symbols, grid_stride, symbol_stride, start_prb, dmrs_scaling, words_per_dmrs_sym_stride, dmrs_type);
}

void launch_kernel_mimo_lse_4layer_fp16(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const unsigned int* __restrict__ d_grid_cbf16, const uint32_t* __restrict__ d_dmrs_seq, const int* __restrict__ dmrs_symbol_indices, __half2* __restrict__ d_estimates_fp16, int nof_prb, int nof_ports, int nof_dmrs_symbols, int grid_stride, int symbol_stride, int start_prb, float dmrs_scaling, int words_per_dmrs_sym_stride, int dmrs_type) {
    kernel_mimo_lse_4layer_fp16<<<grid, block, shmem, stream>>>(d_grid_cbf16, d_dmrs_seq, dmrs_symbol_indices, d_estimates_fp16, nof_prb, nof_ports, nof_dmrs_symbols, grid_stride, symbol_stride, start_prb, dmrs_scaling, words_per_dmrs_sym_stride, dmrs_type);
}

void launch_kernel_noise_variance_finalize(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, float* __restrict__ d_noise_vars, const float* __restrict__ d_partial_sums, int nof_ports, int nof_dmrs_symbols, int total_dmrs_re_per_symbol, float dmrs_scaling) {
    kernel_noise_variance_finalize<<<grid, block, shmem, stream>>>(d_noise_vars, d_partial_sums, nof_ports, nof_dmrs_symbols, total_dmrs_re_per_symbol, dmrs_scaling);
}

void launch_kernel_noise_variance_finalize_low_papr(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, float* __restrict__ d_noise_vars, const float* __restrict__ d_partial_sums, int nof_ports, int nof_dmrs_symbols, int total_dmrs_re_per_symbol) {
    kernel_noise_variance_finalize_low_papr<<<grid, block, shmem, stream>>>(d_noise_vars, d_partial_sums, nof_ports, nof_dmrs_symbols, total_dmrs_re_per_symbol);
}

void launch_kernel_noise_variance_from_dmrs(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const unsigned int* __restrict__ d_grid_cbf16, const unsigned int* __restrict__ d_dmrs_seq, const cuFloatComplex* __restrict__ d_lse_averaged, float* __restrict__ d_noise_var_partial, int nof_prb, int nof_ports, int grid_stride, int symbol_stride, int start_prb, int dmrs_symbol_idx, int start_symbol, float dmrs_scaling, int total_dmrs_re) {
    kernel_noise_variance_from_dmrs<<<grid, block, shmem, stream>>>(d_grid_cbf16, d_dmrs_seq, d_lse_averaged, d_noise_var_partial, nof_prb, nof_ports, grid_stride, symbol_stride, start_prb, dmrs_symbol_idx, start_symbol, dmrs_scaling, total_dmrs_re);
}

void launch_kernel_noise_variance_from_low_papr_dmrs(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const unsigned int* __restrict__ d_grid_cbf16, const cuFloatComplex* __restrict__ d_pilots, const cuFloatComplex* __restrict__ d_lse_averaged, float* __restrict__ d_noise_var_partial, int nof_prb, int nof_ports, int grid_stride, int symbol_stride, int start_prb, int dmrs_symbol_idx, float dmrs_scaling, int total_dmrs_re) {
    kernel_noise_variance_from_low_papr_dmrs<<<grid, block, shmem, stream>>>(d_grid_cbf16, d_pilots, d_lse_averaged, d_noise_var_partial, nof_prb, nof_ports, grid_stride, symbol_stride, start_prb, dmrs_symbol_idx, dmrs_scaling, total_dmrs_re);
}

void launch_kernel_pusch_e2e_deprecode_demod_descramble(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const cuFloatComplex* __restrict__ transform_symbols, const float* __restrict__ symbol_noise_vars, __half* __restrict__ llrs_half, uint32_t scramble_c_init, int nof_re, int mod_order, int dft_size) {
    kernel_pusch_e2e_deprecode_demod_descramble<<<grid, block, shmem, stream>>>(transform_symbols, symbol_noise_vars, llrs_half, scramble_c_init, nof_re, mod_order, dft_size);
}

void launch_kernel_reset_sinr_epre_result(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, float* __restrict__ d_sinr_epre_result) {
    kernel_reset_sinr_epre_result<<<grid, block, shmem, stream>>>(d_sinr_epre_result);
}

void launch_kernel_time_interpolate(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream, const cuFloatComplex* __restrict__ d_dmrs_estimates, cuFloatComplex* __restrict__ d_full_estimates, const int* __restrict__ dmrs_symbol_indices, int nof_dmrs_symbols, int nof_symbols, int nof_ports, int nof_re_per_symbol, int start_symbol, float output_scale) {
    kernel_time_interpolate<<<grid, block, shmem, stream>>>(d_dmrs_estimates, d_full_estimates, dmrs_symbol_indices, nof_dmrs_symbols, nof_symbols, nof_ports, nof_re_per_symbol, start_symbol, output_scale);
}

nr_ldpc_status_t launch_kernel_pusch_e2e_with_estimates(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream,
    int nof_rx_ports, int equalizer_algorithm, const unsigned int* __restrict__ d_grid_cbf16, const cuFloatComplex* __restrict__ d_estimates, const int* __restrict__ re_indices, const float* __restrict__ noise_vars, __half* __restrict__ llrs_half, const unsigned int* __restrict__ scramble_seq, float* __restrict__ d_eq_noise_var_sum, unsigned int* __restrict__ d_eq_noise_var_count, int nof_re, int mod_order, int grid_stride, int symbol_stride, int est_stride, int start_symbol, int start_subcarrier, float tx_scaling) {
    #define LX_kernel_pusch_e2e_with_estimates(PORTS, ALG) \
        kernel_pusch_e2e_with_estimates<PORTS, ALG><<<grid, block, shmem, stream>>>(d_grid_cbf16, d_estimates, re_indices, noise_vars, llrs_half, scramble_seq, d_eq_noise_var_sum, d_eq_noise_var_count, nof_re, mod_order, grid_stride, symbol_stride, est_stride, start_symbol, start_subcarrier, tx_scaling)
    #define DA_kernel_pusch_e2e_with_estimates(PORTS) \
        switch (equalizer_algorithm) { \
            case EQUALIZER_ZF: LX_kernel_pusch_e2e_with_estimates(PORTS, EQUALIZER_ZF); break; \
            case EQUALIZER_MMSE: LX_kernel_pusch_e2e_with_estimates(PORTS, EQUALIZER_MMSE); break; \
            case EQUALIZER_MMSE_IRC: default: LX_kernel_pusch_e2e_with_estimates(PORTS, EQUALIZER_MMSE_IRC); break; \
        }
    switch (nof_rx_ports) {
        case 1: DA_kernel_pusch_e2e_with_estimates(1); break;
        case 2: DA_kernel_pusch_e2e_with_estimates(2); break;
        case 4: DA_kernel_pusch_e2e_with_estimates(4); break;
        case 8: DA_kernel_pusch_e2e_with_estimates(8); break;
        default: return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    #undef LX_kernel_pusch_e2e_with_estimates
    #undef DA_kernel_pusch_e2e_with_estimates
    return NR_LDPC_SUCCESS;
}

nr_ldpc_status_t launch_kernel_pusch_e2e_deprecode_equalize_gather(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream,
    int nof_rx_ports, int equalizer_algorithm, const unsigned int* __restrict__ d_grid_cbf16, const cuFloatComplex* __restrict__ d_estimates, const int* __restrict__ re_indices, const float* __restrict__ noise_vars, cuFloatComplex* __restrict__ transform_symbols, float* __restrict__ symbol_noise_vars, float* __restrict__ d_eq_noise_var_sum, unsigned int* __restrict__ d_eq_noise_var_count, int nof_re, int grid_stride, int symbol_stride, int est_stride, int start_symbol, int start_subcarrier, float tx_scaling, int dft_size, int nof_symbols) {
    #define LX_kernel_pusch_e2e_deprecode_equalize_gather(PORTS, ALG) \
        kernel_pusch_e2e_deprecode_equalize_gather<PORTS, ALG><<<grid, block, shmem, stream>>>(d_grid_cbf16, d_estimates, re_indices, noise_vars, transform_symbols, symbol_noise_vars, d_eq_noise_var_sum, d_eq_noise_var_count, nof_re, grid_stride, symbol_stride, est_stride, start_symbol, start_subcarrier, tx_scaling, dft_size, nof_symbols)
    #define DA_kernel_pusch_e2e_deprecode_equalize_gather(PORTS) \
        switch (equalizer_algorithm) { \
            case EQUALIZER_ZF: LX_kernel_pusch_e2e_deprecode_equalize_gather(PORTS, EQUALIZER_ZF); break; \
            case EQUALIZER_MMSE: LX_kernel_pusch_e2e_deprecode_equalize_gather(PORTS, EQUALIZER_MMSE); break; \
            case EQUALIZER_MMSE_IRC: default: LX_kernel_pusch_e2e_deprecode_equalize_gather(PORTS, EQUALIZER_MMSE_IRC); break; \
        }
    switch (nof_rx_ports) {
        case 1: DA_kernel_pusch_e2e_deprecode_equalize_gather(1); break;
        case 2: DA_kernel_pusch_e2e_deprecode_equalize_gather(2); break;
        case 4: DA_kernel_pusch_e2e_deprecode_equalize_gather(4); break;
        case 8: DA_kernel_pusch_e2e_deprecode_equalize_gather(8); break;
        default: return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    #undef LX_kernel_pusch_e2e_deprecode_equalize_gather
    #undef DA_kernel_pusch_e2e_deprecode_equalize_gather
    return NR_LDPC_SUCCESS;
}

nr_ldpc_status_t launch_kernel_pusch_e2e_with_estimates_deprecode(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream,
    int nof_rx_ports, int equalizer_algorithm, const unsigned int* __restrict__ d_grid_cbf16, const cuFloatComplex* __restrict__ d_estimates, const int* __restrict__ re_indices, const float* __restrict__ noise_vars, __half* __restrict__ llrs_half, uint32_t scramble_c_init, float* __restrict__ d_eq_noise_var_sum, unsigned int* __restrict__ d_eq_noise_var_count, int nof_re, int mod_order, int grid_stride, int symbol_stride, int est_stride, int start_symbol, int start_subcarrier, float tx_scaling, int dft_size, uint64_t packed_fft_factors, int nof_fft_factors, int nof_symbols) {
    #define LX_kernel_pusch_e2e_with_estimates_deprecode(PORTS, ALG) \
        cudaFuncSetAttribute(kernel_pusch_e2e_with_estimates_deprecode<PORTS, ALG>, \
                             cudaFuncAttributeMaxDynamicSharedMemorySize, \
                             static_cast<int>(shmem)); \
        kernel_pusch_e2e_with_estimates_deprecode<PORTS, ALG><<<grid, block, shmem, stream>>>(d_grid_cbf16, d_estimates, re_indices, noise_vars, llrs_half, scramble_c_init, d_eq_noise_var_sum, d_eq_noise_var_count, nof_re, mod_order, grid_stride, symbol_stride, est_stride, start_symbol, start_subcarrier, tx_scaling, dft_size, packed_fft_factors, nof_fft_factors, nof_symbols)
    #define DA_kernel_pusch_e2e_with_estimates_deprecode(PORTS) \
        switch (equalizer_algorithm) { \
            case EQUALIZER_ZF: LX_kernel_pusch_e2e_with_estimates_deprecode(PORTS, EQUALIZER_ZF); break; \
            case EQUALIZER_MMSE: LX_kernel_pusch_e2e_with_estimates_deprecode(PORTS, EQUALIZER_MMSE); break; \
            case EQUALIZER_MMSE_IRC: default: LX_kernel_pusch_e2e_with_estimates_deprecode(PORTS, EQUALIZER_MMSE_IRC); break; \
        }
    switch (nof_rx_ports) {
        case 1: DA_kernel_pusch_e2e_with_estimates_deprecode(1); break;
        case 2: DA_kernel_pusch_e2e_with_estimates_deprecode(2); break;
        case 4: DA_kernel_pusch_e2e_with_estimates_deprecode(4); break;
        case 8: DA_kernel_pusch_e2e_with_estimates_deprecode(8); break;
        default: return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    #undef LX_kernel_pusch_e2e_with_estimates_deprecode
    #undef DA_kernel_pusch_e2e_with_estimates_deprecode
    return NR_LDPC_SUCCESS;
}

nr_ldpc_status_t launch_kernel_mimo_2layer_fp16(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream,
    int nof_rx_ports, int equalizer_algorithm, const unsigned int* __restrict__ d_grid_cbf16, const __half2* __restrict__ d_dmrs_estimates_fp16, const int* __restrict__ re_indices, const float* __restrict__ noise_vars, __half* __restrict__ llrs_half, const uint32_t* __restrict__ scramble_seq, float* __restrict__ d_eq_noise_var_sum, unsigned int* __restrict__ d_eq_noise_var_count, float* __restrict__ d_evm_error_sum, unsigned int* __restrict__ d_evm_symbol_count, int nof_re, int nof_dmrs_symbols, int nof_prb, int mod_order, int grid_stride, int symbol_stride, int start_symbol, int start_subcarrier, float tx_scaling, const int* __restrict__ d_dmrs_indices, const float* __restrict__ d_cfo_hz_ptr, const float* __restrict__ d_symbol_start_times, const float2* __restrict__ d_cfo_phasors, float2* __restrict__ d_eq_symbols_out, float* __restrict__ d_eq_noise_var_out) {
    #define LX_kernel_mimo_2layer_fp16(PORTS, ALG) \
        kernel_mimo_2layer_fp16<PORTS, ALG><<<grid, block, shmem, stream>>>(d_grid_cbf16, d_dmrs_estimates_fp16, re_indices, noise_vars, llrs_half, scramble_seq, d_eq_noise_var_sum, d_eq_noise_var_count, d_evm_error_sum, d_evm_symbol_count, nof_re, nof_dmrs_symbols, nof_prb, mod_order, grid_stride, symbol_stride, start_symbol, start_subcarrier, tx_scaling, d_dmrs_indices, d_cfo_hz_ptr, d_symbol_start_times, d_cfo_phasors, d_eq_symbols_out, d_eq_noise_var_out)
    #define DA_kernel_mimo_2layer_fp16(PORTS) \
        switch (equalizer_algorithm) { \
            case EQUALIZER_ZF: LX_kernel_mimo_2layer_fp16(PORTS, EQUALIZER_ZF); break; \
            case EQUALIZER_MMSE: LX_kernel_mimo_2layer_fp16(PORTS, EQUALIZER_MMSE); break; \
            case EQUALIZER_MMSE_IRC: default: LX_kernel_mimo_2layer_fp16(PORTS, EQUALIZER_MMSE_IRC); break; \
        }
    switch (nof_rx_ports) {
        case 2: DA_kernel_mimo_2layer_fp16(2); break;
        case 4: DA_kernel_mimo_2layer_fp16(4); break;
        case 8: DA_kernel_mimo_2layer_fp16(8); break;
        default: return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    #undef LX_kernel_mimo_2layer_fp16
    #undef DA_kernel_mimo_2layer_fp16
    return NR_LDPC_SUCCESS;
}

nr_ldpc_status_t launch_kernel_mimo_3layer_fp16(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream,
    int nof_rx_ports, int equalizer_algorithm, const unsigned int* __restrict__ d_grid_cbf16, const __half2* __restrict__ d_dmrs_estimates_fp16, const int* __restrict__ re_indices, const float* __restrict__ noise_vars, __half* __restrict__ llrs_half, const uint32_t* __restrict__ scramble_seq, float* __restrict__ d_eq_noise_var_sum, unsigned int* __restrict__ d_eq_noise_var_count, float* __restrict__ d_evm_error_sum, unsigned int* __restrict__ d_evm_symbol_count, int nof_re, int nof_dmrs_symbols, int nof_prb, int mod_order, int grid_stride, int symbol_stride, int start_symbol, int start_subcarrier, float tx_scaling, const int* __restrict__ d_dmrs_indices, const float* __restrict__ d_cfo_hz_ptr, const float* __restrict__ d_symbol_start_times, const float2* __restrict__ d_cfo_phasors, float2* __restrict__ d_eq_symbols_out, float* __restrict__ d_eq_noise_var_out) {
    #define LX_kernel_mimo_3layer_fp16(PORTS, ALG) \
        kernel_mimo_3layer_fp16<PORTS, ALG><<<grid, block, shmem, stream>>>(d_grid_cbf16, d_dmrs_estimates_fp16, re_indices, noise_vars, llrs_half, scramble_seq, d_eq_noise_var_sum, d_eq_noise_var_count, d_evm_error_sum, d_evm_symbol_count, nof_re, nof_dmrs_symbols, nof_prb, mod_order, grid_stride, symbol_stride, start_symbol, start_subcarrier, tx_scaling, d_dmrs_indices, d_cfo_hz_ptr, d_symbol_start_times, d_cfo_phasors, d_eq_symbols_out, d_eq_noise_var_out)
    #define DA_kernel_mimo_3layer_fp16(PORTS) \
        switch (equalizer_algorithm) { \
            case EQUALIZER_ZF: LX_kernel_mimo_3layer_fp16(PORTS, EQUALIZER_ZF); break; \
            case EQUALIZER_MMSE: LX_kernel_mimo_3layer_fp16(PORTS, EQUALIZER_MMSE); break; \
            case EQUALIZER_MMSE_IRC: default: LX_kernel_mimo_3layer_fp16(PORTS, EQUALIZER_MMSE_IRC); break; \
        }
    switch (nof_rx_ports) {
        case 4: DA_kernel_mimo_3layer_fp16(4); break;
        case 8: DA_kernel_mimo_3layer_fp16(8); break;
        default: return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    #undef LX_kernel_mimo_3layer_fp16
    #undef DA_kernel_mimo_3layer_fp16
    return NR_LDPC_SUCCESS;
}

nr_ldpc_status_t launch_kernel_mimo_4layer_fp16(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream,
    int nof_rx_ports, int equalizer_algorithm, const unsigned int* __restrict__ d_grid_cbf16, const __half2* __restrict__ d_dmrs_estimates_fp16, const int* __restrict__ re_indices, const float* __restrict__ noise_vars, __half* __restrict__ llrs_half, const uint32_t* __restrict__ scramble_seq, float* __restrict__ d_eq_noise_var_sum, unsigned int* __restrict__ d_eq_noise_var_count, float* __restrict__ d_evm_error_sum, unsigned int* __restrict__ d_evm_symbol_count, int nof_re, int nof_dmrs_symbols, int nof_prb, int mod_order, int grid_stride, int symbol_stride, int start_symbol, int start_subcarrier, float tx_scaling, const int* __restrict__ d_dmrs_indices, const float* __restrict__ d_cfo_hz_ptr, const float* __restrict__ d_symbol_start_times, const float2* __restrict__ d_cfo_phasors, float2* __restrict__ d_eq_symbols_out, float* __restrict__ d_eq_noise_var_out) {
    #define LX_kernel_mimo_4layer_fp16(PORTS, ALG) \
        kernel_mimo_4layer_fp16<PORTS, ALG><<<grid, block, shmem, stream>>>(d_grid_cbf16, d_dmrs_estimates_fp16, re_indices, noise_vars, llrs_half, scramble_seq, d_eq_noise_var_sum, d_eq_noise_var_count, d_evm_error_sum, d_evm_symbol_count, nof_re, nof_dmrs_symbols, nof_prb, mod_order, grid_stride, symbol_stride, start_symbol, start_subcarrier, tx_scaling, d_dmrs_indices, d_cfo_hz_ptr, d_symbol_start_times, d_cfo_phasors, d_eq_symbols_out, d_eq_noise_var_out)
    #define DA_kernel_mimo_4layer_fp16(PORTS) \
        switch (equalizer_algorithm) { \
            case EQUALIZER_ZF: LX_kernel_mimo_4layer_fp16(PORTS, EQUALIZER_ZF); break; \
            case EQUALIZER_MMSE: LX_kernel_mimo_4layer_fp16(PORTS, EQUALIZER_MMSE); break; \
            case EQUALIZER_MMSE_IRC: default: LX_kernel_mimo_4layer_fp16(PORTS, EQUALIZER_MMSE_IRC); break; \
        }
    switch (nof_rx_ports) {
        case 4: DA_kernel_mimo_4layer_fp16(4); break;
        case 8: DA_kernel_mimo_4layer_fp16(8); break;
        default: return NR_LDPC_ERROR_INVALID_CONFIG;
    }
    #undef LX_kernel_mimo_4layer_fp16
    #undef DA_kernel_mimo_4layer_fp16
    return NR_LDPC_SUCCESS;
}

nr_ldpc_status_t launch_kernel_fused_e2e_warp_cooperative_fp16(dim3 grid, dim3 block, size_t shmem, cudaStream_t stream,
    int nof_rx_ports, int equalizer_algorithm, int nof_dmrs_symbols, int mod_order, int time_interp_mode, const unsigned int* __restrict__ d_grid_cbf16, const __half2* __restrict__ d_dmrs_estimates_fp16, const int* __restrict__ re_indices, const float* __restrict__ noise_vars, __half* __restrict__ llrs_half, const uint32_t* __restrict__ scramble_seq, uint32_t scramble_c_init, float* __restrict__ d_eq_noise_var_sum, unsigned int* __restrict__ d_eq_noise_var_count, float* __restrict__ d_evm_error_sum, unsigned int* __restrict__ d_evm_symbol_count, int nof_re, int nof_prb, int grid_stride, int symbol_stride, int start_symbol, int start_subcarrier, float tx_scaling, const int* __restrict__ d_dmrs_indices, const float* __restrict__ d_cfo_hz_ptr, const float* __restrict__ d_symbol_start_times, const float2* __restrict__ d_cfo_phasors, float2* __restrict__ d_eq_symbols_out, float* __restrict__ d_eq_noise_var_out) {
    #define LWC(PORTS, ALG, DMRS, TAVG, MOD) \
        kernel_fused_e2e_warp_cooperative_fp16<PORTS, ALG, DMRS, TAVG, MOD><<<grid, block, shmem, stream>>>(d_grid_cbf16, d_dmrs_estimates_fp16, re_indices, noise_vars, llrs_half, scramble_seq, scramble_c_init, d_eq_noise_var_sum, d_eq_noise_var_count, d_evm_error_sum, d_evm_symbol_count, nof_re, nof_prb, grid_stride, symbol_stride, start_symbol, start_subcarrier, tx_scaling, d_dmrs_indices, d_cfo_hz_ptr, d_symbol_start_times, d_cfo_phasors, d_eq_symbols_out, d_eq_noise_var_out)
    #define D_MOD(PORTS, ALG, DMRS, TAVG) \
        switch (mod_order) { \
            case 2: LWC(PORTS, ALG, DMRS, TAVG, 2); break; \
            case 4: LWC(PORTS, ALG, DMRS, TAVG, 4); break; \
            case 6: LWC(PORTS, ALG, DMRS, TAVG, 6); break; \
            case 8: LWC(PORTS, ALG, DMRS, TAVG, 8); break; \
            default: return NR_LDPC_ERROR_INVALID_CONFIG; \
        }
    #define D_DMRS(PORTS, ALG, TAVG) \
        switch (nof_dmrs_symbols) { \
            case 1: D_MOD(PORTS, ALG, 1, TAVG); break; \
            case 2: D_MOD(PORTS, ALG, 2, TAVG); break; \
            case 3: D_MOD(PORTS, ALG, 3, TAVG); break; \
            case 4: default: D_MOD(PORTS, ALG, 4, TAVG); break; \
        }
    #define D_ALG(PORTS, TAVG) \
        switch (equalizer_algorithm) { \
            case EQUALIZER_ZF: D_DMRS(PORTS, EQUALIZER_ZF, TAVG); break; \
            case EQUALIZER_MMSE: D_DMRS(PORTS, EQUALIZER_MMSE, TAVG); break; \
            case EQUALIZER_MMSE_IRC: default: D_DMRS(PORTS, EQUALIZER_MMSE_IRC, TAVG); break; \
        }
    #define D_PORTS(TAVG) \
        switch (nof_rx_ports) { \
            case 1: D_ALG(1, TAVG); break; \
            case 2: D_ALG(2, TAVG); break; \
            case 4: D_ALG(4, TAVG); break; \
            case 8: D_ALG(8, TAVG); break; \
            default: return NR_LDPC_ERROR_INVALID_CONFIG; \
        }
    if (time_interp_mode == 1) {
        D_PORTS(false);
    } else {
        D_PORTS(true);
    }
    #undef LWC
    #undef D_MOD
    #undef D_DMRS
    #undef D_ALG
    #undef D_PORTS
    return NR_LDPC_SUCCESS;
}
