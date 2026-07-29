// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/**
 * @file pusch_e2e_qam_demod.cuh
 * @brief Private implementation header for the PUSCH E2E translation units. Not public API.
 * 
 * __constant__ tables are marked `static` so each including TU gets its own
 * module-local copy (CUDA constant memory cannot be linked across TUs)
 */

#pragma once
#include <cuComplex.h>
#include <cmath>

/* ============================================================================
 * 64QAM Piecewise-Linear Demodulation Lookup Tables
 * Per TS 38.211: Optimal LLR approximation using interval functions
 * M = 1/sqrt(42) = 0.154303349962092
 * Pre-computed slope and intercept values for constant memory
 * ============================================================================ */

/* Bits 0,1: 8 intervals, width = 2M */
static __device__ const float SLOPE_01[8] = {
    2.46885359939347f,  1.85164019954510f,  1.23442679969673f,  0.61721339984837f,
    0.61721339984837f,  1.23442679969673f,  1.85164019954510f,  2.46885359939347f
};
static __device__ const float INTERCEPT_01[8] = {
    1.14285714285714f,  0.57142857142857f,  0.19047619047619f,  0.0f,
    0.0f,              -0.19047619047619f, -0.57142857142857f, -1.14285714285714f
};

/* Bits 2,3: 8 intervals, width = 2M */
static __device__ const float SLOPE_23[8] = {
    1.23442679969673f,   0.61721339984837f,  0.61721339984837f,  1.23442679969673f,
   -1.23442679969673f,  -0.61721339984837f, -0.61721339984837f, -1.23442679969673f
};
static __device__ const float INTERCEPT_23[8] = {
    0.95238095238095f,  0.38095238095238f,  0.38095238095238f,  0.57142857142857f,
    0.57142857142857f,  0.38095238095238f,  0.38095238095238f,  0.95238095238095f
};

/* Bits 4,5: 4 intervals, width = 4M */
static __device__ const float SLOPE_45[4] = {
    0.61721339984837f, -0.61721339984837f,
    0.61721339984837f, -0.61721339984837f
};
static __device__ const float INTERCEPT_45[4] = {
    0.57142857142857f, -0.19047619047619f,
   -0.19047619047619f,  0.57142857142857f
};

/* ============================================================================
 * 256QAM Piecewise-Linear Demodulation Lookup Tables
 * Keep these in constant memory. Declaring them inside soft_demod_256qam_piecewise
 * creates a 448-byte per-thread stack frame in every inlined 256QAM MIMO kernel.
 * ============================================================================ */

static __device__ const float QAM256_SLOPE_01[16] = {
    32.0f * 0.07669649888473704f, 28.0f * 0.07669649888473704f,
    24.0f * 0.07669649888473704f, 20.0f * 0.07669649888473704f,
    16.0f * 0.07669649888473704f, 12.0f * 0.07669649888473704f,
     8.0f * 0.07669649888473704f,  4.0f * 0.07669649888473704f,
     4.0f * 0.07669649888473704f,  8.0f * 0.07669649888473704f,
    12.0f * 0.07669649888473704f, 16.0f * 0.07669649888473704f,
    20.0f * 0.07669649888473704f, 24.0f * 0.07669649888473704f,
    28.0f * 0.07669649888473704f, 32.0f * 0.07669649888473704f
};
static __device__ const float QAM256_INTERCEPT_01[16] = {
    112.0f/85, 84.0f/85, 60.0f/85, 40.0f/85, 24.0f/85, 12.0f/85, 4.0f/85, 0.0f,
      0.0f,   -4.0f/85, -12.0f/85, -24.0f/85, -40.0f/85, -60.0f/85, -84.0f/85, -112.0f/85
};

static __device__ const float QAM256_SLOPE_23[16] = {
    16.0f * 0.07669649888473704f,  12.0f * 0.07669649888473704f,
     8.0f * 0.07669649888473704f,   4.0f * 0.07669649888473704f,
     4.0f * 0.07669649888473704f,   8.0f * 0.07669649888473704f,
    12.0f * 0.07669649888473704f,  16.0f * 0.07669649888473704f,
   -16.0f * 0.07669649888473704f, -12.0f * 0.07669649888473704f,
    -8.0f * 0.07669649888473704f,  -4.0f * 0.07669649888473704f,
    -4.0f * 0.07669649888473704f,  -8.0f * 0.07669649888473704f,
   -12.0f * 0.07669649888473704f, -16.0f * 0.07669649888473704f
};
static __device__ const float QAM256_INTERCEPT_23[16] = {
    88.0f/85, 60.0f/85, 36.0f/85, 16.0f/85, 16.0f/85, 28.0f/85, 36.0f/85, 40.0f/85,
    40.0f/85, 36.0f/85, 28.0f/85, 16.0f/85, 16.0f/85, 36.0f/85, 60.0f/85, 88.0f/85
};

static __device__ const float QAM256_SLOPE_45[16] = {
     8.0f * 0.07669649888473704f,   4.0f * 0.07669649888473704f,
     4.0f * 0.07669649888473704f,   8.0f * 0.07669649888473704f,
    -8.0f * 0.07669649888473704f,  -4.0f * 0.07669649888473704f,
    -4.0f * 0.07669649888473704f,  -8.0f * 0.07669649888473704f,
     8.0f * 0.07669649888473704f,   4.0f * 0.07669649888473704f,
     4.0f * 0.07669649888473704f,   8.0f * 0.07669649888473704f,
    -8.0f * 0.07669649888473704f,  -4.0f * 0.07669649888473704f,
    -4.0f * 0.07669649888473704f,  -8.0f * 0.07669649888473704f
};
static __device__ const float QAM256_INTERCEPT_45[16] = {
    52.0f/85, 24.0f/85, 24.0f/85, 44.0f/85, -20.0f/85, -8.0f/85, -8.0f/85, -12.0f/85,
   -12.0f/85, -8.0f/85, -8.0f/85, -20.0f/85, 44.0f/85, 24.0f/85, 24.0f/85, 52.0f/85
};

static __device__ const float QAM256_SLOPE_67[8] = {
     4.0f * 0.07669649888473704f, -4.0f * 0.07669649888473704f,
     4.0f * 0.07669649888473704f, -4.0f * 0.07669649888473704f,
     4.0f * 0.07669649888473704f, -4.0f * 0.07669649888473704f,
     4.0f * 0.07669649888473704f, -4.0f * 0.07669649888473704f
};
static __device__ const float QAM256_INTERCEPT_67[8] = {
    28.0f/85, -20.0f/85, 12.0f/85, -4.0f/85, -4.0f/85, 12.0f/85, -20.0f/85, 28.0f/85
};

/**
 * Soft demodulation for 64QAM using piecewise-linear approximation.
 * Matches the CPU implementation for accurate LLR computation at all SINR levels.
 */
__device__ __forceinline__ void soft_demod_64qam_piecewise(
    cuFloatComplex eq, float inv_noise, float llr_vals[6])
{
    /* M = 1/sqrt(42) = 0.154303349962092 */
    const float INTERVAL_WIDTH_01 = 0.308606699924184f;  // Bits 0,1: 2M intervals
    const float INTERVAL_WIDTH_23 = 0.308606699924184f;  // Bits 2,3: 2M intervals
    const float INTERVAL_WIDTH_45 = 0.617213399848368f;  // Bits 4,5: 4M intervals

    /* Bits 0,1: Sign bits (8 intervals) - CRITICAL: offset must be OUTSIDE floor() */
    int idx_x_01 = max(0, min(7, (int)floorf(eq.x / INTERVAL_WIDTH_01) + 4));
    int idx_y_01 = max(0, min(7, (int)floorf(eq.y / INTERVAL_WIDTH_01) + 4));
    llr_vals[0] = (SLOPE_01[idx_x_01] * eq.x + INTERCEPT_01[idx_x_01]) * inv_noise;
    llr_vals[1] = (SLOPE_01[idx_y_01] * eq.y + INTERCEPT_01[idx_y_01]) * inv_noise;

    /* Bits 2,3: Inner/outer group (8 intervals) */
    int idx_x_23 = max(0, min(7, (int)floorf(eq.x / INTERVAL_WIDTH_23) + 4));
    int idx_y_23 = max(0, min(7, (int)floorf(eq.y / INTERVAL_WIDTH_23) + 4));
    llr_vals[2] = (SLOPE_23[idx_x_23] * eq.x + INTERCEPT_23[idx_x_23]) * inv_noise;
    llr_vals[3] = (SLOPE_23[idx_y_23] * eq.y + INTERCEPT_23[idx_y_23]) * inv_noise;

    /* Bits 4,5: Nested magnitude (4 intervals) */
    int idx_x_45 = max(0, min(3, (int)floorf(eq.x / INTERVAL_WIDTH_45) + 2));
    int idx_y_45 = max(0, min(3, (int)floorf(eq.y / INTERVAL_WIDTH_45) + 2));
    llr_vals[4] = (SLOPE_45[idx_x_45] * eq.x + INTERCEPT_45[idx_x_45]) * inv_noise;
    llr_vals[5] = (SLOPE_45[idx_y_45] * eq.y + INTERCEPT_45[idx_y_45]) * inv_noise;
}

/**
 * Soft demodulation for 256QAM using piecewise-linear approximation.
 * Matches the CPU implementation for accurate LLR computation at all SINR levels.
 */
__device__ __forceinline__ void soft_demod_256qam_piecewise(
    cuFloatComplex eq, float inv_noise, float llr_vals[8])
{
    /* M = 1/sqrt(170) = 0.07669649888473704 */
    const float M = 0.07669649888473704f;
    const float INTERVAL_WIDTH_01 = 2.0f * M;  // Bits 0,1: 2M intervals (16 intervals)
    const float INTERVAL_WIDTH_23 = 2.0f * M;  // Bits 2,3: 2M intervals (16 intervals)
    const float INTERVAL_WIDTH_45 = 2.0f * M;  // Bits 4,5: 2M intervals (16 intervals)
    const float INTERVAL_WIDTH_67 = 4.0f * M;  // Bits 6,7: 4M intervals (8 intervals)

    /* Compute LLRs - CRITICAL: offset must be OUTSIDE floor() */

    /* Bits 0,1: Sign bits (16 intervals, offset by 8) */
    int idx_x_01 = max(0, min(15, (int)floorf(eq.x / INTERVAL_WIDTH_01) + 8));
    int idx_y_01 = max(0, min(15, (int)floorf(eq.y / INTERVAL_WIDTH_01) + 8));
    llr_vals[0] = (QAM256_SLOPE_01[idx_x_01] * eq.x + QAM256_INTERCEPT_01[idx_x_01]) * inv_noise;
    llr_vals[1] = (QAM256_SLOPE_01[idx_y_01] * eq.y + QAM256_INTERCEPT_01[idx_y_01]) * inv_noise;

    /* Bits 2,3: Inner/outer group (16 intervals, offset by 8) */
    int idx_x_23 = max(0, min(15, (int)floorf(eq.x / INTERVAL_WIDTH_23) + 8));
    int idx_y_23 = max(0, min(15, (int)floorf(eq.y / INTERVAL_WIDTH_23) + 8));
    llr_vals[2] = (QAM256_SLOPE_23[idx_x_23] * eq.x + QAM256_INTERCEPT_23[idx_x_23]) * inv_noise;
    llr_vals[3] = (QAM256_SLOPE_23[idx_y_23] * eq.y + QAM256_INTERCEPT_23[idx_y_23]) * inv_noise;

    /* Bits 4,5: Nested magnitude (16 intervals, offset by 8) */
    int idx_x_45 = max(0, min(15, (int)floorf(eq.x / INTERVAL_WIDTH_45) + 8));
    int idx_y_45 = max(0, min(15, (int)floorf(eq.y / INTERVAL_WIDTH_45) + 8));
    llr_vals[4] = (QAM256_SLOPE_45[idx_x_45] * eq.x + QAM256_INTERCEPT_45[idx_x_45]) * inv_noise;
    llr_vals[5] = (QAM256_SLOPE_45[idx_y_45] * eq.y + QAM256_INTERCEPT_45[idx_y_45]) * inv_noise;

    /* Bits 6,7: Finest level (8 intervals, offset by 4) */
    int idx_x_67 = max(0, min(7, (int)floorf(eq.x / INTERVAL_WIDTH_67) + 4));
    int idx_y_67 = max(0, min(7, (int)floorf(eq.y / INTERVAL_WIDTH_67) + 4));
    llr_vals[6] = (QAM256_SLOPE_67[idx_x_67] * eq.x + QAM256_INTERCEPT_67[idx_x_67]) * inv_noise;
    llr_vals[7] = (QAM256_SLOPE_67[idx_y_67] * eq.y + QAM256_INTERCEPT_67[idx_y_67]) * inv_noise;
}
