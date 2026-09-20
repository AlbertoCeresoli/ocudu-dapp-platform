// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_e2e_launch.cuh"

namespace {
// Scalar kernel arguments are copied at submission, unlike a borrowed async
// H2D source on the caller's stack. No shape-specific graph or staging lease.
__global__ void prepare_pilot_metrics(int* indices, uint32_t* c_inits,
                                      int first_symbol, int second_symbol,
                                      uint32_t first_init, uint32_t second_init)
{
    indices[0] = first_symbol;
    indices[1] = second_symbol;
    c_inits[0] = first_init;
    c_inits[1] = second_init;
}
} // namespace

void launch_kernel_prepare_pilot_metrics(cudaStream_t stream, int* indices, uint32_t* c_inits,
                                         int first_symbol, int second_symbol,
                                         uint32_t first_init, uint32_t second_init)
{
    prepare_pilot_metrics<<<1, 1, 0, stream>>>(indices, c_inits, first_symbol, second_symbol,
                                             first_init, second_init);
}
