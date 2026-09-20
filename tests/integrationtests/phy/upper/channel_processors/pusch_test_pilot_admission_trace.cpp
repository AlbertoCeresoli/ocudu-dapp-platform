// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// Test-executable linker interposition only. No production L1 logging or ABI change.
#include "lib/phy/cuda/src/pusch_e2e_internal.cuh"
#include "ocudu/ran/resource_block.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static_assert(ocudu::MAX_NOF_SUBCARRIERS <= 275 * 12, "Update the pilot-input grid bound for a larger host grid");

extern "C" nr_ldpc_status_t __real_pusch_e2e_validate_pilot_metrics(pusch_e2e_handle_t, const void*, int);

extern "C" nr_ldpc_status_t __wrap_pusch_e2e_validate_pilot_metrics(pusch_e2e_handle_t handle, const void* grid, int dc)
{
  const auto        result = __real_pusch_e2e_validate_pilot_metrics(handle, grid, dc);
  static const bool trace  = [] {
    const char* value = std::getenv("OCUDU_TEST_PILOT_ADMISSION_TRACE");
    return value && std::strcmp(value, "1") == 0;
  }();
  static std::atomic<unsigned> traced{0};
  if (!trace || traced.fetch_add(1, std::memory_order_relaxed) >= 8 || !handle) {
    return result;
  }
  const auto& c = handle->config;
  std::fprintf(stderr,
               "PILOT_ADMISSION status=%d configured=%d grid_present=%d grid_alignment=%zu dc=%d "
               "prb=%d start_prb=%d ports=%d layers=%d low_papr=%d dmrs_type=%d mask=%d scs=%d "
               "grid_symbols=%d grid_subcarriers=%d start_symbol=%d symbols=%d slot=%d scaling=%g "
               "scrambling_id=%u n_scid=%d noise_mode=%d compensate_cfo=%d\n",
               int(result),
               int(handle->configured),
               int(grid != nullptr),
               size_t(reinterpret_cast<uintptr_t>(grid) % alignof(uint32_t)),
               dc,
               c.nof_prb,
               c.start_prb,
               c.nof_rx_ports,
               c.nof_tx_layers,
               c.use_low_papr_dmrs,
               int(c.dmrs_type),
               c.dmrs_symbol_mask,
               c.scs_khz,
               c.grid_nof_symbols,
               c.grid_nof_subcarriers,
               c.start_symbol,
               c.nof_symbols,
               c.slot_idx,
               double(c.dmrs_scaling),
               c.scrambling_id,
               c.n_scid,
               c.noise_mode,
               c.compensate_cfo);
  std::fprintf(
      stderr,
      "PILOT_CACHE ready=%d words=%d words_slot=%d slots=%d pilots_symbol=%d id=%u scid=%d type=%d "
      "precomputed_bytes=%zu gathered_bytes=%zu fp16_bytes=%zu noise_bytes=%zu indices_bytes=%zu accum_bytes=%zu\n",
      int(handle->dmrs_pilots_precomputed),
      handle->precomputed_words_per_sym,
      handle->precomputed_words_per_slot,
      handle->precomputed_nof_slots,
      handle->precomputed_pilots_per_symbol,
      handle->cached_dmrs_scrambling_id,
      handle->cached_dmrs_n_scid,
      handle->cached_dmrs_type,
      handle->precomputed_dmrs_pilots_capacity,
      handle->dmrs_pilot_bits_capacity,
      handle->estimates_fp16_capacity,
      handle->noise_vars_capacity,
      handle->dmrs_indices_capacity,
      handle->accum_block_size);
  unsigned    bit        = 0;
  uint32_t    present    = 0;
  const void* pointers[] = {handle->d_precomputed_dmrs_pilots,
                            handle->d_dmrs_pilot_bits,
                            handle->d_estimates_fp16,
                            handle->d_noise_vars,
                            handle->d_dmrs_indices,
                            handle->d_dmrs_c_inits,
                            handle->d_accum_block,
                            handle->d_noise_count,
                            handle->d_rsrp_accum,
                            handle->d_epre_accum,
                            handle->d_sinr_noise_accum,
                            handle->d_sinr_rsrp_accum,
                            handle->d_sinr_epre_result,
                            handle->d_ta_cfo_accum,
                            handle->d_cv_noise_accum,
                            handle->d_cv_done_counter,
                            handle->d_symbol_start_times,
                            handle->d_mimo_cfo_phasors};
  for (const void* pointer : pointers) {
    present |= uint32_t(pointer != nullptr) << bit++;
  }
  std::fprintf(stderr, "PILOT_STORAGE present=%08x expected=%08x\n", present, (uint32_t{1} << bit) - 1);
  std::fflush(stderr);
  return result;
}
