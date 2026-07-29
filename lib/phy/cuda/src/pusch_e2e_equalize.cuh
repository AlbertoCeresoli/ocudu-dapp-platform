// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/**
 * @file pusch_e2e_equalize.cuh
 * @brief Private implementation header for the PUSCH E2E. Not public API
 * 
 * __constant__ tables are marked `static` so each including TU gets its own 
 * module-local copy (CUDA constant memory cannot be linked across TUs)
 */

#pragma once
#include <cuComplex.h>
#include "pusch_e2e.h"                   // EQUALIZER_ZF/MMSE/MMSE_IRC
#include "ocudu_phy_cuda_mimo_math.cuh"  // mimo_invert_2x2_hermitian

/* Deep-fade sentinel: eq_noise_var set to this when channel is dead.
 * Makes inv_noise ≈ 0 → LLR ≈ 0 (erasure), matching CPU behavior.
 * SINR accumulation excludes this exact value as the GPU infinity-equivalent. */
static constexpr float EQ_NOISE_VAR_DEEP_FADE = 1e6f;

/**
 * Equalize a single symbol using the specified algorithm.
 *
 * @tparam NOF_PORTS Number of receive ports
 * @tparam ALGORITHM Equalization algorithm (EQUALIZER_ZF, EQUALIZER_MMSE, EQUALIZER_MMSE_IRC)
 * @param y Received symbols per port
 * @param h Channel estimates per port
 * @param noise_vars Per-port noise variances
 * @param tx_scaling TX amplitude scaling
 * @param eq_symbol Output equalized symbol
 * @param eq_noise_var Output post-equalization noise variance
 */
template <int NOF_PORTS, int ALGORITHM>
__device__ __forceinline__ void equalize_symbol(
    const cuFloatComplex* y,
    const cuFloatComplex* h,
    const float* noise_vars,
    float tx_scaling,
    cuFloatComplex& eq_symbol,
    float& eq_noise_var)
{
    float inv_scaling = 1.0f / tx_scaling;

    if (ALGORITHM == EQUALIZER_ZF) {
        /* Zero Forcing: w = h^H / |h|^2 */
        float H_sq = 0.0f;
        cuFloatComplex hH_y = make_cuFloatComplex(0.0f, 0.0f);

        #pragma unroll
        for (int p = 0; p < NOF_PORTS; p++) {
            H_sq += h[p].x * h[p].x + h[p].y * h[p].y;
            /* h^H * y = conj(h) * y */
            hH_y.x += h[p].x * y[p].x + h[p].y * y[p].y;
            hH_y.y += h[p].x * y[p].y - h[p].y * y[p].x;
        }

        float inv_H_sq = (H_sq > 1e-10f) ? (1.0f / H_sq) : 0.0f;
        eq_symbol.x = hH_y.x * inv_H_sq * inv_scaling;
        eq_symbol.y = hH_y.y * inv_H_sq * inv_scaling;

        /* ZF noise enhancement: sigma^2 / |h|^2 */
        float avg_noise = 0.0f;
        for (int p = 0; p < NOF_PORTS; p++) {
            avg_noise += noise_vars[p];
        }
        avg_noise /= NOF_PORTS;
        eq_noise_var = (H_sq > 1e-10f)
            ? fmaxf(avg_noise * inv_H_sq / (tx_scaling * tx_scaling), 1e-10f)
            : EQ_NOISE_VAR_DEEP_FADE;

    } else if (ALGORITHM == EQUALIZER_MMSE) {
        /* MMSE: w = h^H / (|h|^2 + sigma^2) with gain normalization
         *
         * CPU formula (equalize_mmse_mxn_simd.h, single-layer):
         *   correction = H_sq / (H_sq + σ²)    where σ² = avg noise across ports
         *   gain_norm  = 1 / correction
         *   eq_symbol  = (h^H y) / (H_sq + σ²) * gain_norm / tx_scaling
         *             = (h^H y) / H_sq / tx_scaling
         *   noise_var  = gain_norm - 1 = σ² / H_sq   (scaled by 1/tx_scaling²)
         */
        float H_sq = 0.0f;
        float avg_noise = 0.0f;
        cuFloatComplex hH_y = make_cuFloatComplex(0.0f, 0.0f);

        #pragma unroll
        for (int p = 0; p < NOF_PORTS; p++) {
            float h_norm_sq = h[p].x * h[p].x + h[p].y * h[p].y;
            H_sq += h_norm_sq;
            avg_noise += noise_vars[p];
            hH_y.x += h[p].x * y[p].x + h[p].y * y[p].y;
            hH_y.y += h[p].x * y[p].y - h[p].y * y[p].x;
        }
        avg_noise /= NOF_PORTS;

        float denom = H_sq + avg_noise;
        float inv_denom = (denom > 1e-10f) ? (1.0f / denom) : 0.0f;

        /* CPU-matching gain normalization: correction = H_sq / (H_sq + σ²) */
        float correction = (H_sq > 1e-10f) ? (H_sq * inv_denom) : 0.0f;
        float gain_norm = (correction > 1e-10f) ? (1.0f / correction) : 1.0f;

        eq_symbol.x = hH_y.x * inv_denom * inv_scaling * gain_norm;
        eq_symbol.y = hH_y.y * inv_denom * inv_scaling * gain_norm;

        /* CPU-matching noise: gain_norm - 1 = σ²/H_sq, scaled by 1/tx_scaling² */
        eq_noise_var = (correction > 1e-10f)
            ? fmaxf((gain_norm - 1.0f) / (tx_scaling * tx_scaling), 1e-10f)
            : EQ_NOISE_VAR_DEEP_FADE;

    } else {
        /* MMSE-IRC: w = (H^H R_n^{-1} H + I)^{-1} H^H R_n^{-1} y
         *
         * Uses per-port noise weighting but CPU-matching gain normalization:
         *   correction = Σ Re(w_p * h_p)     (effective combining gain)
         *   gain_norm  = 1 / correction
         *   noise_var  = gain_norm - 1
         */
        float HRnH = 1.0f;
        cuFloatComplex Rn_inv_H[8];

        #pragma unroll
        for (int p = 0; p < NOF_PORTS; p++) {
            float inv_sigma = (noise_vars[p] > 1e-10f) ? (1.0f / noise_vars[p]) : 0.0f;
            Rn_inv_H[p] = make_cuFloatComplex(inv_sigma * h[p].x, inv_sigma * h[p].y);
            HRnH += h[p].x * Rn_inv_H[p].x + h[p].y * Rn_inv_H[p].y;
        }

        float inv_HRnH = 1.0f / HRnH;  /* Always > 0 due to +1 regularization */

        cuFloatComplex eq = make_cuFloatComplex(0.0f, 0.0f);
        float correction = 0.0f;
        #pragma unroll
        for (int p = 0; p < NOF_PORTS; p++) {
            cuFloatComplex w = make_cuFloatComplex(Rn_inv_H[p].x * inv_HRnH, Rn_inv_H[p].y * inv_HRnH);
            eq.x += w.x * y[p].x + w.y * y[p].y;
            eq.y += w.x * y[p].y - w.y * y[p].x;
            /* correction = Σ Re(w_p * h_p) = Σ Re(h_p^H R_n^{-1} h_p) / (H^H R_n^{-1} H + 1) */
            correction += w.x * h[p].x + w.y * h[p].y;
        }

        float gain_norm = (correction > 1e-10f) ? (1.0f / correction) : 1.0f;

        eq_symbol.x = eq.x * inv_scaling * gain_norm;
        eq_symbol.y = eq.y * inv_scaling * gain_norm;
        /* CPU-matching noise: gain_norm - 1, scaled by 1/tx_scaling² */
        eq_noise_var = (correction > 1e-10f)
            ? fmaxf((gain_norm - 1.0f) / (tx_scaling * tx_scaling), 1e-10f)
            : EQ_NOISE_VAR_DEEP_FADE;
    }
}

/* ============================================================================
 * MIMO EQUALIZATION HELPERS (2-layer and 4-layer support)
 * ============================================================================ */

/**
 * Equalize a single RE for 2-layer MIMO using ZF or MMSE.
 *
 * For 2-layer MIMO, we compute:
 *   G = H^H * H (2x2 Gram matrix)
 *   eq = G^{-1} * H^H * y
 *
 * @tparam NOF_PORTS Number of receive ports (must be >= 2)
 * @tparam ALGORITHM Equalization algorithm (ZF or MMSE)
 * @param y Received symbols per port [NOF_PORTS]
 * @param H Channel matrix H[port][layer] with shape [NOF_PORTS][2]
 * @param noise_vars Per-port noise variances [NOF_PORTS]
 * @param tx_scaling TX amplitude scaling
 * @param eq_symbols Output equalized symbols [2]
 * @param eq_noise_vars Output per-layer noise variances [2]
 */
template <int NOF_PORTS, int ALGORITHM>
__device__ __forceinline__ void equalize_symbol_2layer(
    const cuFloatComplex* y,
    const cuFloatComplex H[NOF_PORTS][2],
    const float* noise_vars,
    float tx_scaling,
    cuFloatComplex eq_symbols[2],
    float eq_noise_vars[2])
{
    // CPU multi-layer equalizer uses the most pessimistic per-port estimate.
    float noise_var_est = 0.0f;
    #pragma unroll
    for (int p = 0; p < NOF_PORTS; p++) {
        noise_var_est = fmaxf(noise_var_est, noise_vars[p]);
    }

    // Compute Gram matrix G = H^H * H [2x2 Hermitian]
    cuFloatComplex G00 = make_cuFloatComplex(0.0f, 0.0f);
    cuFloatComplex G01 = make_cuFloatComplex(0.0f, 0.0f);
    cuFloatComplex G11 = make_cuFloatComplex(0.0f, 0.0f);

    #pragma unroll
    for (int p = 0; p < NOF_PORTS; p++) {
        cuFloatComplex h0 = H[p][0];
        cuFloatComplex h1 = H[p][1];

        // G00 = sum |h0|² (real)
        G00.x += h0.x * h0.x + h0.y * h0.y;

        // G01 = sum conj(h0) * h1
        G01 = cuCaddf(G01, cuCmulf(cuConjf(h0), h1));

        // G11 = sum |h1|² (real)
        G11.x += h1.x * h1.x + h1.y * h1.y;
    }

    float tx_scaling_sq = tx_scaling * tx_scaling;
    float noise_reg = noise_var_est / tx_scaling_sq;

    // CPU scales H by tx_scaling before MMSE regularization. Since this GPU
    // path keeps H unscaled, use the equivalent noise / tx_scaling^2 term.
    if constexpr (ALGORITHM == EQUALIZER_MMSE || ALGORITHM == EQUALIZER_MMSE_IRC) {
        G00.x += noise_reg;
        G11.x += noise_reg;
    }

    // Invert Gram matrix
    cuFloatComplex G_inv[4];
    mimo_invert_2x2_hermitian(G00, G01, G11, G_inv, 1e-6f);

    // Compute matched filter: mf = H^H * y [2x1]
    cuFloatComplex mf[2] = {make_cuFloatComplex(0.0f, 0.0f), make_cuFloatComplex(0.0f, 0.0f)};

    #pragma unroll
    for (int p = 0; p < NOF_PORTS; p++) {
        // mf[layer] = sum conj(H[p][layer]) * y[p]
        mf[0] = cuCaddf(mf[0], cuCmulf(cuConjf(H[p][0]), y[p]));
        mf[1] = cuCaddf(mf[1], cuCmulf(cuConjf(H[p][1]), y[p]));
    }

    // Equalization: eq = G^{-1} * mf
    float inv_scaling = 1.0f / tx_scaling;

    cuFloatComplex eq0 = cuCaddf(cuCmulf(G_inv[0], mf[0]), cuCmulf(G_inv[1], mf[1]));
    cuFloatComplex eq1 = cuCaddf(cuCmulf(G_inv[2], mf[0]), cuCmulf(G_inv[3], mf[1]));

    if constexpr (ALGORITHM == EQUALIZER_MMSE || ALGORITHM == EQUALIZER_MMSE_IRC) {
        float c0 = 1.0f - noise_reg * G_inv[0].x;
        float c1 = 1.0f - noise_reg * G_inv[3].x;
        float corr0 = (c0 > 1e-10f) ? (1.0f / c0) : 0.0f;
        float corr1 = (c1 > 1e-10f) ? (1.0f / c1) : 0.0f;
        eq_symbols[0] = make_cuFloatComplex(eq0.x * inv_scaling * corr0, eq0.y * inv_scaling * corr0);
        eq_symbols[1] = make_cuFloatComplex(eq1.x * inv_scaling * corr1, eq1.y * inv_scaling * corr1);
        eq_noise_vars[0] = (corr0 > 1.0f) ? (corr0 - 1.0f) : EQ_NOISE_VAR_DEEP_FADE;
        eq_noise_vars[1] = (corr1 > 1.0f) ? (corr1 - 1.0f) : EQ_NOISE_VAR_DEEP_FADE;
    } else {
        eq_symbols[0] = make_cuFloatComplex(eq0.x * inv_scaling, eq0.y * inv_scaling);
        eq_symbols[1] = make_cuFloatComplex(eq1.x * inv_scaling, eq1.y * inv_scaling);

        float noise_scale = noise_var_est / tx_scaling_sq;
        eq_noise_vars[0] = (noise_scale * G_inv[0].x > 1e-10f)
            ? noise_scale * G_inv[0].x : EQ_NOISE_VAR_DEEP_FADE;
        eq_noise_vars[1] = (noise_scale * G_inv[3].x > 1e-10f)
            ? noise_scale * G_inv[3].x : EQ_NOISE_VAR_DEEP_FADE;
    }
}

__device__ __forceinline__ cuFloatComplex cuCnegf_local(cuFloatComplex a)
{
    return make_cuFloatComplex(-a.x, -a.y);
}

__device__ __forceinline__ cuFloatComplex cuCdivf_regularized(cuFloatComplex a, cuFloatComplex b, float reg)
{
    float denom = b.x * b.x + b.y * b.y + reg;
    return make_cuFloatComplex((a.x * b.x + a.y * b.y) / denom,
                               (a.y * b.x - a.x * b.y) / denom);
}

__device__ __forceinline__ bool mimo_invert_4x4_gauss_jordan_inplace(
    cuFloatComplex* in,
    cuFloatComplex* G_inv)
{
    #pragma unroll
    for (int i = 0; i < 16; i++) {
        G_inv[i] = make_cuFloatComplex(0.0f, 0.0f);
    }

    #pragma unroll
    for (int i = 0; i < 4; i++) {
        G_inv[i * 4 + i] = make_cuFloatComplex(1.0f, 0.0f);
    }

    bool valid = true;

    #pragma unroll
    for (int i = 0; i < 4; i++) {
        cuFloatComplex pivot = in[i * 4 + i];
        float pivot_norm = pivot.x * pivot.x + pivot.y * pivot.y;
        valid = valid && (pivot_norm > 0.0f) && isfinite(pivot_norm);

        float inv_pivot_norm = (pivot_norm > 0.0f) ? (1.0f / pivot_norm) : 0.0f;
        cuFloatComplex pivot_inv = make_cuFloatComplex(pivot.x * inv_pivot_norm, -pivot.y * inv_pivot_norm);

        #pragma unroll
        for (int j = i + 1; j < 4; j++) {
            in[i * 4 + j] = cuCmulf(in[i * 4 + j], pivot_inv);
        }

        #pragma unroll
        for (int j = 0; j <= i; j++) {
            G_inv[i * 4 + j] = cuCmulf(G_inv[i * 4 + j], pivot_inv);
        }

        #pragma unroll
        for (int k = 0; k < 4; k++) {
            if (k != i) {
                cuFloatComplex factor = in[k * 4 + i];

                #pragma unroll
                for (int j = i; j < 4; j++) {
                    in[k * 4 + j] = cuCsubf(in[k * 4 + j], cuCmulf(factor, in[i * 4 + j]));
                }

                #pragma unroll
                for (int j = 0; j < i; j++) {
                    G_inv[k * 4 + j] = cuCsubf(G_inv[k * 4 + j], cuCmulf(factor, G_inv[i * 4 + j]));
                }

                G_inv[k * 4 + i] = cuCnegf_local(cuCmulf(factor, G_inv[i * 4 + i]));
            }
        }
    }

    return valid;
}

__device__ __forceinline__ void mimo_invert_3x3(
    const cuFloatComplex* G,
    cuFloatComplex* G_inv,
    float reg = 1e-6f)
{
    cuFloatComplex a = G[0], b = G[1], c = G[2];
    cuFloatComplex d = G[3], e = G[4], f = G[5];
    cuFloatComplex g = G[6], h = G[7], i = G[8];

    cuFloatComplex ei_fh = cuCsubf(cuCmulf(e, i), cuCmulf(f, h));
    cuFloatComplex di_fg = cuCsubf(cuCmulf(d, i), cuCmulf(f, g));
    cuFloatComplex dh_eg = cuCsubf(cuCmulf(d, h), cuCmulf(e, g));
    cuFloatComplex det = cuCaddf(cuCsubf(cuCmulf(a, ei_fh), cuCmulf(b, di_fg)), cuCmulf(c, dh_eg));

    cuFloatComplex cof0 = ei_fh;
    cuFloatComplex cof1 = cuCnegf_local(di_fg);
    cuFloatComplex cof2 = dh_eg;
    cuFloatComplex cof3 = cuCnegf_local(cuCsubf(cuCmulf(b, i), cuCmulf(c, h)));
    cuFloatComplex cof4 = cuCsubf(cuCmulf(a, i), cuCmulf(c, g));
    cuFloatComplex cof5 = cuCnegf_local(cuCsubf(cuCmulf(a, h), cuCmulf(b, g)));
    cuFloatComplex cof6 = cuCsubf(cuCmulf(b, f), cuCmulf(c, e));
    cuFloatComplex cof7 = cuCnegf_local(cuCsubf(cuCmulf(a, f), cuCmulf(c, d)));
    cuFloatComplex cof8 = cuCsubf(cuCmulf(a, e), cuCmulf(b, d));

    G_inv[0] = cuCdivf_regularized(cof0, det, reg);
    G_inv[1] = cuCdivf_regularized(cof3, det, reg);
    G_inv[2] = cuCdivf_regularized(cof6, det, reg);
    G_inv[3] = cuCdivf_regularized(cof1, det, reg);
    G_inv[4] = cuCdivf_regularized(cof4, det, reg);
    G_inv[5] = cuCdivf_regularized(cof7, det, reg);
    G_inv[6] = cuCdivf_regularized(cof2, det, reg);
    G_inv[7] = cuCdivf_regularized(cof5, det, reg);
    G_inv[8] = cuCdivf_regularized(cof8, det, reg);
}

template <int NOF_PORTS, int ALGORITHM>
__device__ __forceinline__ void equalize_symbol_3layer(
    const cuFloatComplex* y,
    const cuFloatComplex H[NOF_PORTS][3],
    const float* noise_vars,
    float tx_scaling,
    cuFloatComplex eq_symbols[3],
    float eq_noise_vars[3])
{
    float noise_var_est = 0.0f;
    #pragma unroll
    for (int p = 0; p < NOF_PORTS; p++) {
        noise_var_est = fmaxf(noise_var_est, noise_vars[p]);
    }

    float tx_scaling_sq = tx_scaling * tx_scaling;
    float noise_reg = noise_var_est / tx_scaling_sq;

    cuFloatComplex G[9];
    #pragma unroll
    for (int i = 0; i < 3; i++) {
        #pragma unroll
        for (int j = i; j < 3; j++) {
            cuFloatComplex sum = make_cuFloatComplex(0.0f, 0.0f);
            #pragma unroll
            for (int p = 0; p < NOF_PORTS; p++) {
                sum = cuCaddf(sum, cuCmulf(cuConjf(H[p][i]), H[p][j]));
            }
            if constexpr (ALGORITHM == EQUALIZER_MMSE || ALGORITHM == EQUALIZER_MMSE_IRC) {
                if (i == j) {
                    sum.x += noise_reg;
                }
            }
            G[i * 3 + j] = sum;
            if (i != j) {
                G[j * 3 + i] = cuConjf(sum);
            }
        }
    }

    cuFloatComplex G_inv[9];
    mimo_invert_3x3(G, G_inv, 1e-6f);

    cuFloatComplex mf[3];
    #pragma unroll
    for (int l = 0; l < 3; l++) {
        mf[l] = make_cuFloatComplex(0.0f, 0.0f);
    }

    #pragma unroll
    for (int p = 0; p < NOF_PORTS; p++) {
        #pragma unroll
        for (int l = 0; l < 3; l++) {
            mf[l] = cuCaddf(mf[l], cuCmulf(cuConjf(H[p][l]), y[p]));
        }
    }

    float inv_scaling = 1.0f / tx_scaling;

    #pragma unroll
    for (int l = 0; l < 3; l++) {
        cuFloatComplex eq = make_cuFloatComplex(0.0f, 0.0f);
        #pragma unroll
        for (int k = 0; k < 3; k++) {
            eq = cuCaddf(eq, cuCmulf(G_inv[l * 3 + k], mf[k]));
        }
        if constexpr (ALGORITHM == EQUALIZER_MMSE || ALGORITHM == EQUALIZER_MMSE_IRC) {
            float c = 1.0f - noise_reg * G_inv[l * 3 + l].x;
            float corr = (c > 1e-10f) ? (1.0f / c) : 0.0f;
            eq_symbols[l] = make_cuFloatComplex(eq.x * inv_scaling * corr, eq.y * inv_scaling * corr);
            eq_noise_vars[l] = (corr > 1.0f) ? (corr - 1.0f) : EQ_NOISE_VAR_DEEP_FADE;
        } else {
            float noise_scale = noise_var_est / tx_scaling_sq;
            eq_symbols[l] = make_cuFloatComplex(eq.x * inv_scaling, eq.y * inv_scaling);
            eq_noise_vars[l] = (noise_scale * G_inv[l * 3 + l].x > 1e-10f)
                ? noise_scale * G_inv[l * 3 + l].x : EQ_NOISE_VAR_DEEP_FADE;
        }
    }
}

/**
 * Equalize a single RE for 4-layer MIMO using ZF or MMSE.
 *
 * @tparam NOF_PORTS Number of receive ports (must be >= 4)
 * @tparam ALGORITHM Equalization algorithm (ZF or MMSE)
 * @param y Received symbols per port [NOF_PORTS]
 * @param H Channel matrix H[port][layer] with shape [NOF_PORTS][4]
 * @param noise_vars Per-port noise variances [NOF_PORTS]
 * @param tx_scaling TX amplitude scaling
 * @param eq_symbols Output equalized symbols [4]
 * @param eq_noise_vars Output per-layer noise variances [4]
 */
template <int NOF_PORTS, int ALGORITHM>
__device__ __forceinline__ void equalize_symbol_4layer(
    const cuFloatComplex* y,
    const cuFloatComplex H[NOF_PORTS][4],
    const float* noise_vars,
    float tx_scaling,
    cuFloatComplex eq_symbols[4],
    float eq_noise_vars[4])
{
    // CPU multi-layer equalizer uses the most pessimistic per-port estimate.
    float noise_var_est = 0.0f;
    #pragma unroll
    for (int p = 0; p < NOF_PORTS; p++) {
        noise_var_est = fmaxf(noise_var_est, noise_vars[p]);
    }

    float tx_scaling_sq = tx_scaling * tx_scaling;
    float noise_reg = noise_var_est / tx_scaling_sq;

    // Compute Gram matrix G = H^H * H [4x4 Hermitian]
    cuFloatComplex G[16];
    #pragma unroll
    for (int i = 0; i < 4; i++) {
        #pragma unroll
        for (int j = i; j < 4; j++) {
            cuFloatComplex sum = make_cuFloatComplex(0.0f, 0.0f);
            #pragma unroll
            for (int p = 0; p < NOF_PORTS; p++) {
                sum = cuCaddf(sum, cuCmulf(cuConjf(H[p][i]), H[p][j]));
            }
            // For MMSE, add noise to diagonal
            if constexpr (ALGORITHM == EQUALIZER_MMSE || ALGORITHM == EQUALIZER_MMSE_IRC) {
                if (i == j) {
                    sum.x += noise_reg;
                }
            }
            G[i * 4 + j] = sum;
            if (i != j) {
                G[j * 4 + i] = cuConjf(sum);  // Lower triangle
            }
        }
    }

    // Invert Gram matrix with Gauss-Jordan elimination. This avoids the
    // determinant-damped block inverse losing gain near ill-conditioned REs.
    cuFloatComplex G_inv[16];
    bool valid_inverse = mimo_invert_4x4_gauss_jordan_inplace(G, G_inv);

    // Compute matched filter: mf = H^H * y [4x1]
    cuFloatComplex mf[4];
    #pragma unroll
    for (int l = 0; l < 4; l++) {
        mf[l] = make_cuFloatComplex(0.0f, 0.0f);
    }

    #pragma unroll
    for (int p = 0; p < NOF_PORTS; p++) {
        #pragma unroll
        for (int l = 0; l < 4; l++) {
            mf[l] = cuCaddf(mf[l], cuCmulf(cuConjf(H[p][l]), y[p]));
        }
    }

    // Equalization: eq = G^{-1} * mf
    float inv_scaling = 1.0f / tx_scaling;

    #pragma unroll
    for (int l = 0; l < 4; l++) {
        cuFloatComplex eq = make_cuFloatComplex(0.0f, 0.0f);
        #pragma unroll
        for (int k = 0; k < 4; k++) {
            eq = cuCaddf(eq, cuCmulf(G_inv[l * 4 + k], mf[k]));
        }
        if (!valid_inverse) {
            eq_symbols[l] = make_cuFloatComplex(0.0f, 0.0f);
            eq_noise_vars[l] = EQ_NOISE_VAR_DEEP_FADE;
            continue;
        }
        if constexpr (ALGORITHM == EQUALIZER_MMSE || ALGORITHM == EQUALIZER_MMSE_IRC) {
            float c = 1.0f - noise_reg * G_inv[l * 4 + l].x;
            float corr = (c > 1e-10f) ? (1.0f / c) : 0.0f;
            eq_symbols[l] = make_cuFloatComplex(eq.x * inv_scaling * corr, eq.y * inv_scaling * corr);
            eq_noise_vars[l] = (corr > 1.0f) ? (corr - 1.0f) : EQ_NOISE_VAR_DEEP_FADE;
        } else {
            eq_symbols[l] = make_cuFloatComplex(eq.x * inv_scaling, eq.y * inv_scaling);

            float noise_scale = noise_var_est / tx_scaling_sq;
            eq_noise_vars[l] = (noise_scale * G_inv[l * 4 + l].x > 1e-10f)
                ? noise_scale * G_inv[l * 4 + l].x : EQ_NOISE_VAR_DEEP_FADE;
        }
    }
}
