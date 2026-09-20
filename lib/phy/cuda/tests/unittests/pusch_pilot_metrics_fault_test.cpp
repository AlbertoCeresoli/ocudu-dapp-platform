// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_e2e.h"
#include "pusch_pilot_mask.h"
#include "../../src/pusch_e2e_internal.cuh"
#include "../../src/pusch_e2e_preallocation.h"
#include <climits>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
unsigned resets = 0;
unsigned forbidden = 0;
unsigned cases = 0;
unsigned preallocation_cases = 0;
unsigned allocation_mode = 0;
unsigned allocation_calls = 0;
size_t allocated_bytes = 0;
unsigned fail_reset = 1;
void* reset_destinations[2]{};
size_t reset_lengths[2]{};
void require(bool value) { if (!value) throw std::runtime_error("pilot admission assertion"); }
template <typename T> T* pointer() { return reinterpret_cast<T*>(uintptr_t{0x10000}); }

pusch_e2e_ctx fixture()
{
    pusch_e2e_ctx h{};
    h.configured = true;
    auto& c = h.config;
    c.nof_prb = 51; c.nof_symbols = 14; c.nof_rx_ports = c.nof_tx_layers = 1;
    c.grid_nof_subcarriers = 273 * 12; c.grid_nof_symbols = 14;
    c.dmrs_type = DMRS_TYPE_1; c.dmrs_symbol_mask = (1 << 2) | (1 << 11);
    c.nof_cdm_groups_without_data = 2; c.scrambling_id = 2;
    c.dmrs_scaling = 1.41421356237F; c.mod_order = 2; c.scs_khz = 30;
    c.tx_scaling = 1; c.compensate_cfo = 1; c.noise_mode = 1;
    h.precomputed_words_per_sym = 103; h.precomputed_words_per_slot = 14 * 103;
    h.precomputed_nof_slots = 20; h.precomputed_pilots_per_symbol = 273 * 6;
    h.dmrs_pilots_precomputed = true; h.cached_dmrs_scrambling_id = 2;
    h.cached_dmrs_n_scid = 0; h.cached_dmrs_type = static_cast<int>(DMRS_TYPE_1);
    h.d_precomputed_dmrs_pilots = pointer<uint32_t>();
    h.precomputed_dmrs_pilots_capacity = 20 * 14 * 103 * sizeof(uint32_t);
    h.d_dmrs_pilot_bits = pointer<uint32_t>(); h.dmrs_pilot_bits_capacity = 2 * 103 * sizeof(uint32_t);
    h.d_estimates_fp16 = pointer<__half2>(); h.estimates_fp16_capacity = 2 * 273 * 12 * sizeof(__half2);
    h.d_noise_vars = pointer<float>(); h.noise_vars_capacity = sizeof(float);
    h.d_dmrs_indices = pointer<int>(); h.dmrs_indices_capacity = 2 * sizeof(int);
    h.d_dmrs_c_inits = pointer<uint32_t>(); h.d_accum_block = pointer<void>(); h.accum_block_size = 208;
    h.d_noise_count = pointer<unsigned>(); h.d_rsrp_accum = h.d_epre_accum = pointer<float>();
    h.d_sinr_noise_accum = h.d_sinr_rsrp_accum = h.d_sinr_epre_result = pointer<float>();
    h.d_ta_cfo_accum = h.d_cv_noise_accum = h.d_symbol_start_times = pointer<float>();
    h.d_cv_done_counter = pointer<unsigned>(); h.d_mimo_cfo_phasors = pointer<float2>();
    h.cached_dmrs_symbol_mask = -1;
    return h;
}

void check(pusch_e2e_ctx h, bool admitted, const void* grid = pointer<uint32_t>(), int dc = INT_MIN)
{
    resets = forbidden = 0;
    unsigned char before[sizeof(h)];
    std::memcpy(before, &h, sizeof(h));
    require(pusch_e2e_validate_pilot_metrics(&h, grid, dc == INT_MIN ? -1 : dc) ==
            (admitted ? NR_LDPC_SUCCESS : NR_LDPC_ERROR_INVALID_CONFIG));
    require(resets == 0 && forbidden == 0 && std::memcmp(before, &h, sizeof(h)) == 0);
    const auto result = dc == INT_MIN ? pusch_e2e_process_pilot_metrics(&h, grid, nullptr)
                                     : pusch_e2e_process_pilot_metrics_with_dc(&h, grid, dc, nullptr);
    require(result == (admitted ? NR_LDPC_ERROR_CUDA_FAILED : NR_LDPC_ERROR_INVALID_CONFIG));
    require(resets == unsigned(admitted) && forbidden == 0);
    require(std::memcmp(before, &h, sizeof(h)) == 0);
    ++cases;
}

pusch_e2e_ctx refresh_fixture()
{
    auto h = fixture();
    auto* base = static_cast<char*>(h.d_accum_block);
    h.d_eq_noise_var_sum = reinterpret_cast<float*>(base + 132);
    h.d_eq_noise_var_count = reinterpret_cast<unsigned*>(base + 136);
    h.d_evm_error_sum = reinterpret_cast<float*>(base + 140);
    h.d_evm_symbol_count = reinterpret_cast<unsigned*>(base + 144);
    h.d_ta_cfo_accum = reinterpret_cast<float*>(base + 148);
    return h;
}

void check_refresh(pusch_e2e_ctx h, bool admitted, unsigned failing_reset = 1)
{
    resets = forbidden = 0; fail_reset = failing_reset;
    unsigned char before[sizeof(h)]; std::memcpy(before, &h, sizeof(h));
    const auto result = pusch_e2e_refresh_pilot_metrics_with_dc(&h, pointer<uint32_t>(), 0, nullptr);
    require(result == (admitted ? NR_LDPC_ERROR_CUDA_FAILED : NR_LDPC_ERROR_INVALID_CONFIG));
    require(resets == (admitted ? failing_reset : 0U) && forbidden == 0);
    require(std::memcmp(before, &h, sizeof(h)) == 0);
    if (admitted) {
        require(reset_destinations[0] == h.d_accum_block && reset_lengths[0] == 132);
        if (failing_reset == 2) {
            require(reset_destinations[1] == static_cast<char*>(h.d_accum_block) + 148 && reset_lengths[1] == 60);
        }
    }
    fail_reset = 1; ++cases;
}

void check_initial_pilot_storage()
{
    auto allocate = [](pusch_e2e_ctx& h, unsigned mode) {
        allocation_calls = 0; allocated_bytes = 0; allocation_mode = mode;
        const auto status = pusch_preallocate_fp16_estimates(h);
        allocation_mode = 0;
        ++preallocation_cases;
        return status;
    };
    pusch_e2e_ctx fresh{};
    require(allocate(fresh, 2) == NR_LDPC_SUCCESS && allocation_calls == 1);
    require(allocated_bytes == size_t{4} * 273 * 12 * 4 * 4 * sizeof(__half2));
    require(fresh.d_estimates_fp16 == pointer<__half2>() && fresh.estimates_fp16_capacity == allocated_bytes);
    auto allocate_seeds = [](pusch_e2e_ctx& h, unsigned mode) {
        allocation_calls = 0; allocated_bytes = 0; allocation_mode = mode;
        const auto status = pusch_preallocate_dmrs_c_inits(h);
        allocation_mode = 0;
        ++preallocation_cases;
        return status;
    };
    require(allocate_seeds(fresh, 2) == NR_LDPC_SUCCESS && allocation_calls == 1);
    require(allocated_bytes == 4 * sizeof(uint32_t) && fresh.d_dmrs_c_inits == pointer<uint32_t>());
    // Model the first grant's admission: no baseline process/allocator is run.
    for (int width : {1, 4, 12, 51, 273}) {
        for (int qm : {2, 4, 6, 8}) {
            auto h = fixture(); h.config.nof_prb = width; h.config.mod_order = qm;
            h.d_estimates_fp16 = fresh.d_estimates_fp16;
            h.estimates_fp16_capacity = fresh.estimates_fp16_capacity;
            h.d_dmrs_c_inits = fresh.d_dmrs_c_inits;
            require(pusch_e2e_validate_pilot_metrics(&h, pointer<uint32_t>(), -1) == NR_LDPC_SUCCESS);
        }
    }
    require(allocate(fresh, 2) == NR_LDPC_ERROR_INVALID_CONFIG && allocation_calls == 0);
    for (unsigned bad = 0; bad != 3; ++bad) {
        pusch_e2e_ctx h{};
        if (bad == 0) h.configured = true;
        if (bad == 1) h.d_estimates_fp16 = pointer<__half2>();
        if (bad == 2) h.estimates_fp16_capacity = 1;
        unsigned char before[sizeof(h)]; std::memcpy(before, &h, sizeof(h));
        require(allocate(h, 2) == NR_LDPC_ERROR_INVALID_CONFIG && allocation_calls == 0);
        require(std::memcmp(before, &h, sizeof(h)) == 0);
    }
    for (unsigned mode : {1U, 3U}) {
        pusch_e2e_ctx h{};
        unsigned char before[sizeof(h)]; std::memcpy(before, &h, sizeof(h));
        require(allocate(h, mode) == NR_LDPC_ERROR_ALLOC_FAILED && allocation_calls == 1);
        require(std::memcmp(before, &h, sizeof(h)) == 0);
    }
    require(allocate_seeds(fresh, 2) == NR_LDPC_ERROR_INVALID_CONFIG && allocation_calls == 0);
    {
        pusch_e2e_ctx h{}; h.configured = true;
        require(allocate_seeds(h, 2) == NR_LDPC_ERROR_INVALID_CONFIG && allocation_calls == 0);
        require(h.configured && h.d_dmrs_c_inits == nullptr);
    }
    for (unsigned mode : {1U, 3U}) {
        pusch_e2e_ctx h{};
        unsigned char before[sizeof(h)]; std::memcpy(before, &h, sizeof(h));
        require(allocate_seeds(h, mode) == NR_LDPC_ERROR_ALLOC_FAILED && allocation_calls == 1);
        require(std::memcmp(before, &h, sizeof(h)) == 0);
    }
    require(forbidden == 0 && resets == 0);
}
} // namespace

// A valid admission transaction is stopped at its FIRST CUDA submission.
// Allocation is permitted only in the explicit startup-helper fake above;
// admission/processing must not allocate, upload stack data or synchronize.
// No path in this executable creates a GPU context.
extern "C" cudaError_t __wrap_cudaMemsetAsync(void* destination, int, size_t length, cudaStream_t)
{
    if (resets < 2) { reset_destinations[resets] = destination; reset_lengths[resets] = length; }
    ++resets; return resets < fail_reset ? cudaSuccess : cudaErrorInvalidValue;
}
extern "C" cudaError_t __wrap_cudaMalloc(void** destination, size_t bytes)
{
    if (allocation_mode) {
        ++allocation_calls; allocated_bytes = bytes;
        *destination = allocation_mode == 2 ? pointer<void>() : nullptr;
        return allocation_mode == 1 ? cudaErrorMemoryAllocation : cudaSuccess;
    }
    ++forbidden; return cudaErrorInvalidValue;
}
extern "C" cudaError_t __wrap_cudaFree(void*)
{ ++forbidden; return cudaErrorInvalidValue; }
extern "C" cudaError_t __wrap_cudaMemcpyAsync(void*, const void*, size_t, cudaMemcpyKind, cudaStream_t)
{ ++forbidden; return cudaErrorInvalidValue; }
extern "C" cudaError_t __wrap_cudaStreamSynchronize(cudaStream_t)
{ ++forbidden; return cudaErrorInvalidValue; }

int main()
{
    try {
        check_initial_pilot_storage();
        require(pusch_e2e_process_pilot_metrics(nullptr, nullptr, nullptr) == NR_LDPC_ERROR_INVALID_CONFIG);
        require(pusch_e2e_process_pilot_metrics_with_dc(nullptr, nullptr, 0, nullptr) == NR_LDPC_ERROR_INVALID_CONFIG);
        for (int prb = 1; prb <= 273; ++prb) {
            for (int qm : {2, 4, 6, 8}) {
                auto h = fixture(); h.config.nof_prb = prb; h.config.mod_order = qm;
                h.config.start_prb = 273 - prb; h.config.slot_idx = prb % 20;
                check(h, true);
                const int first = h.config.start_prb * 12;
                for (int dc : {-1, first, first + 1, first + prb * 6, first + prb * 12 - 1})
                    check(h, true, pointer<uint32_t>(), dc);
            }
        }
        check(fixture(), false, pointer<uint32_t>(), -2);
        check(fixture(), false, pointer<uint32_t>(), INT_MAX);
        check(fixture(), false, pointer<uint32_t>(), 273 * 12);
        check(fixture(), true, pointer<uint32_t>(), 273 * 12 - 1); // Outside grant, inside grid.
        // Actual host grids may be wider than the CUDA grant/pilot capacity.
        // This reproduces the full-host 3300-subcarrier first-grant boundary.
        for (int grid_prb : {274, 275}) {
            for (int width : {1, 4, 51, 273}) {
                for (int qm : {2, 4, 6, 8}) {
                    auto h = fixture(); h.config.grid_nof_subcarriers = grid_prb * 12;
                    h.config.nof_prb = width; h.config.mod_order = qm;
                    check(h, true);
                }
            }
        }
        {
            auto h = fixture(); h.config.grid_nof_subcarriers = 275 * 12;
            h.config.start_prb = 273; h.config.nof_prb = 1;
            check(h, false); // Inside the grid but beyond the prepared pilot cache.
        }
        {
            auto h = fixture(); h.config.grid_nof_subcarriers = 276 * 12;
            check(h, false);
        }
        {
            auto h = fixture(); h.config.grid_nof_subcarriers = 275 * 12;
            check(h, true, pointer<uint32_t>(), 275 * 12 - 1);
            check(h, false, pointer<uint32_t>(), 275 * 12);
        }
        for (int prb = 1; prb <= 273; ++prb) {
            const int width = prb * 12, first = (273 - prb) * 12;
            for (int dc = -1; dc <= 273 * 12; ++dc) {
                const int erased = ocudu::cuda::pilot_mask::erased_type1_pilot(dc, first, width);
                const bool removed = dc >= first && dc < first + width && (dc - first) % 2 == 0;
                require(erased == (removed ? dc - first : -1));
                require(ocudu::cuda::pilot_mask::observed_type1_pilots(width, erased) == width / 2 - int(removed));
                if (removed) require(!ocudu::cuda::pilot_mask::observed(dc - first, erased));
            }
        }
        check(fixture(), false, nullptr);
        check(fixture(), false, reinterpret_cast<const void*>(uintptr_t{0x10001}));
#define BAD(field, value) { auto h = fixture(); h.field = value; check(h, false); }
        BAD(configured, false);
        BAD(config.nof_rx_ports, 2); BAD(config.nof_tx_layers, 2);
        BAD(config.use_low_papr_dmrs, 1); BAD(config.dmrs_type, DMRS_TYPE_2);
        BAD(config.scs_khz, 15); BAD(config.dmrs_symbol_mask, 0);
        BAD(config.dmrs_symbol_mask, 1 << 2); BAD(config.dmrs_symbol_mask, 0x4004);
        BAD(config.dmrs_symbol_mask, 0x0805); BAD(config.dmrs_symbol_mask, -1);
        BAD(config.grid_nof_symbols, 13); BAD(config.grid_nof_subcarriers, 11);
        BAD(config.grid_nof_subcarriers, 3277); BAD(config.grid_nof_subcarriers, INT_MAX);
        BAD(config.nof_prb, 0); BAD(config.nof_prb, -1); BAD(config.nof_prb, INT_MAX);
        BAD(config.start_prb, -1); BAD(config.start_prb, 223); BAD(config.start_prb, INT_MAX);
        BAD(config.start_symbol, -1); BAD(config.start_symbol, INT_MAX);
        BAD(config.start_symbol, 3); BAD(config.nof_symbols, 0); BAD(config.nof_symbols, INT_MAX);
        BAD(config.nof_symbols, 11); BAD(config.slot_idx, -1);
        BAD(config.dmrs_scaling, 0); BAD(config.dmrs_scaling, -1);
        BAD(config.dmrs_scaling, NAN); BAD(config.dmrs_scaling, INFINITY);
        BAD(config.scrambling_id, 65536); BAD(config.n_scid, 2);
        BAD(config.noise_mode, -1); BAD(config.compensate_cfo, 2);
        BAD(dmrs_pilots_precomputed, false); BAD(precomputed_words_per_sym, 0);
        BAD(precomputed_words_per_sym, INT_MAX); BAD(precomputed_words_per_slot, INT_MAX);
        BAD(precomputed_nof_slots, 0); BAD(precomputed_nof_slots, INT_MAX);
        BAD(precomputed_pilots_per_symbol, 305); BAD(precomputed_pilots_per_symbol, INT_MAX);
        BAD(cached_dmrs_scrambling_id, 3); BAD(cached_dmrs_n_scid, 1); BAD(cached_dmrs_type, -1);
        BAD(precomputed_dmrs_pilots_capacity, 0); BAD(dmrs_pilot_bits_capacity, 823);
        BAD(estimates_fp16_capacity, 2 * 51 * 12 * sizeof(__half2) - 1);
        BAD(noise_vars_capacity, 3); BAD(dmrs_indices_capacity, 7); BAD(accum_block_size, 207);
        BAD(d_precomputed_dmrs_pilots, nullptr); BAD(d_dmrs_pilot_bits, nullptr);
        BAD(d_estimates_fp16, nullptr); BAD(d_noise_vars, nullptr); BAD(d_dmrs_indices, nullptr);
        BAD(d_dmrs_c_inits, nullptr); BAD(d_accum_block, nullptr); BAD(d_noise_count, nullptr);
        BAD(d_rsrp_accum, nullptr); BAD(d_epre_accum, nullptr); BAD(d_sinr_noise_accum, nullptr);
        BAD(d_sinr_rsrp_accum, nullptr); BAD(d_sinr_epre_result, nullptr); BAD(d_ta_cfo_accum, nullptr);
        BAD(d_cv_noise_accum, nullptr); BAD(d_cv_done_counter, nullptr);
        BAD(d_symbol_start_times, nullptr); BAD(d_mimo_cfo_phasors, nullptr);
#undef BAD
        check_refresh(refresh_fixture(), true);
        check_refresh(refresh_fixture(), true, 2);
#define BAD_REFRESH(field, value) { auto h = refresh_fixture(); h.field = value; check_refresh(h, false); }
        BAD_REFRESH(configured, false);
        BAD_REFRESH(accum_block_size, 207); BAD_REFRESH(accum_block_size, 209);
        BAD_REFRESH(d_eq_noise_var_sum, nullptr); BAD_REFRESH(d_eq_noise_var_count, pointer<unsigned>());
        BAD_REFRESH(d_evm_error_sum, pointer<float>()); BAD_REFRESH(d_evm_symbol_count, pointer<unsigned>());
        BAD_REFRESH(d_ta_cfo_accum, pointer<float>());
        BAD_REFRESH(d_accum_block, reinterpret_cast<void*>(UINTPTR_MAX - 16));
#undef BAD_REFRESH
        std::printf("{\"status\":\"PILOT_METRIC_ADMISSION_OK\",\"cases\":%u,\"preallocation_cases\":%u}\n",
                    cases, preallocation_cases);
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s case=%u\n", e.what(), cases); return 1; }
}
