// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "pusch_demodulator_gpu_impl.h"
#include "../../dapp/native_cuda_control_context.h"
#include "../../channel_coding/ldpc/cuda/cuda_rt_utils.h"
#include "../../dapp/pusch_resident_dapp_cuda.h"
#include "../../phy_acceleration_runtime_options.h"
#include "cuda/pusch_sch_llr_compactor.h"
#include "pusch_acceleration_runtime_options.h"
#include "pusch_demodulation_metrics.h"
#include "pusch_inline_replacement.h"
#include "pusch_metric_readback_completion.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_codeword_buffer.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator_notifier.h"
#include "ocudu/phy/upper/dapp/pusch_dapp_demodulator.h"
#include "ocudu/phy/upper/dapp/pusch_resident_dapp.h"
#include "ocudu/ran/dmrs/dmrs.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/ran/sch/sch_dmrs_power.h"
#include "ocudu/ran/uci/uci_info.h"
#include "ocudu/support/ocudu_assert.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cuComplex.h>
#include <limits>
#include <modulation.h>
#include <mutex>
#include <pusch_e2e.h>
#include <sched.h>
#include <scrambling.h>
#include <unordered_map>

using namespace ocudu;

namespace {

/// Maximum SINR value to report when actual SINR cannot be computed.
/// This is above the highest MCS requirement (~22 dB for 256QAM) but finite to avoid scheduler issues.
constexpr float MAX_SINR_DB = 30.0f;

/// Sanitization bounds for stats reported out of the GPU path.
///
/// PUSCH CSI carries normalized dB values that are later calibrated to dBFS
/// for FAPI. In this path, positive EPRE/RSRP values are valid before the
/// FAPI calibration step, so do not clamp them to 0 dB here.
constexpr float RSRP_DB_MIN   = -140.0f;
constexpr float RSRP_DB_MAX   = 60.0f;
constexpr float EPRE_DB_MIN   = -140.0f;
constexpr float EPRE_DB_MAX   = 60.0f;
constexpr float SINR_DB_FLOOR = -10.0f;
constexpr float SINR_DB_CEIL  = 60.0f;
constexpr int   HOST_LLR_MAX  = log_likelihood_ratio::max().to_int();

inline int clamp_host_llr(int value)
{
  return std::max(-HOST_LLR_MAX, std::min(HOST_LLR_MAX, value));
}

std::atomic<bool> logged_pusch_direct_grid_path{false};
std::atomic<bool> logged_pusch_owned_grid_path{false};
std::atomic<bool> logged_pusch_host_grid_path{false};
std::atomic<bool> logged_pusch_noncompact_grid_path{false};
std::atomic<bool> logged_pusch_prepare_failed_grid_path{false};

bool pusch_grid_path_diagnostics_enabled()
{
  static const bool enabled = phy_acceleration_env_flag_enabled("OCUDU_PUSCH_GRID_PATH_DIAGNOSTICS", false);
  return enabled;
}

inline bool should_log_clamp(uint16_t rnti, const char* tag, float period_s = 10.0f)
{
  static std::mutex                             mtx;
  static std::unordered_map<uint64_t, uint64_t> last_log_ns;
  const uint64_t key    = (static_cast<uint64_t>(rnti) << 32) | static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tag));
  const uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
  const uint64_t period_ns = static_cast<uint64_t>(period_s * 1e9);

  std::lock_guard<std::mutex> lock(mtx);
  auto                        it = last_log_ns.find(key);
  if (it == last_log_ns.end() || (now_ns - it->second) >= period_ns) {
    last_log_ns[key] = now_ns;
    return true;
  }
  return false;
}

inline float sanitize_rsrp_db(float value, uint16_t rnti)
{
  if (!std::isfinite(value)) {
    return std::numeric_limits<float>::quiet_NaN();
  }
  if (value > RSRP_DB_MAX) {
    if (should_log_clamp(rnti, "rsrp_over")) {
      ocudulog::fetch_basic_logger("PHY").warning(
          "RSRP clamp: rnti=0x{:04x} reported {:+.1f} dB - clamped above sane range", rnti, value);
    }
    return RSRP_DB_MAX;
  }
  if (value < RSRP_DB_MIN) {
    if (should_log_clamp(rnti, "rsrp_under")) {
      ocudulog::fetch_basic_logger("PHY").warning(
          "RSRP clamp: rnti=0x{:04x} reported {:+.1f} dB below sane range", rnti, value);
    }
    return RSRP_DB_MIN;
  }
  return value;
}

inline float sanitize_epre_db(float value, uint16_t rnti)
{
  if (!std::isfinite(value)) {
    return std::numeric_limits<float>::quiet_NaN();
  }
  if (value > EPRE_DB_MAX) {
    if (should_log_clamp(rnti, "epre_over")) {
      ocudulog::fetch_basic_logger("PHY").warning(
          "EPRE clamp: rnti=0x{:04x} reported {:+.1f} dB - clamped above sane range", rnti, value);
    }
    return EPRE_DB_MAX;
  }
  if (value < EPRE_DB_MIN) {
    return EPRE_DB_MIN;
  }
  return value;
}

inline float sanitize_sinr_db(float value, uint16_t rnti)
{
  if (!std::isfinite(value)) {
    return std::numeric_limits<float>::quiet_NaN();
  }
  if (value < SINR_DB_FLOOR) {
    if (should_log_clamp(rnti, "sinr_floor")) {
      ocudulog::fetch_basic_logger("PHY").info("SINR floor: rnti=0x{:04x} reported {:+.1f} dB - floored to {:+.1f} so "
                                               "OLLA does not chase an unrealistic value",
                                               rnti,
                                               value,
                                               SINR_DB_FLOOR);
    }
    return SINR_DB_FLOOR;
  }
  if (value > SINR_DB_CEIL) {
    return SINR_DB_CEIL;
  }
  return value;
}

inline void sanitize_stats_in_place(pusch_demodulator_notifier::demodulation_stats& stats, uint16_t rnti)
{
  if (stats.channel_estimator_sinr_dB.has_value()) {
    float v = sanitize_sinr_db(*stats.channel_estimator_sinr_dB, rnti);
    if (std::isnan(v)) {
      stats.channel_estimator_sinr_dB.reset();
    } else {
      stats.channel_estimator_sinr_dB = v;
    }
  }
  if (stats.sinr_dB.has_value()) {
    float v = sanitize_sinr_db(stats.sinr_dB.value(), rnti);
    if (std::isnan(v)) {
      stats.sinr_dB.reset();
    } else {
      stats.sinr_dB = v;
    }
  }
  if (stats.rsrp_dB.has_value()) {
    float v = sanitize_rsrp_db(stats.rsrp_dB.value(), rnti);
    if (std::isnan(v)) {
      stats.rsrp_dB.reset();
    } else {
      stats.rsrp_dB = v;
    }
  }
  if (stats.epre_dB.has_value()) {
    float v = sanitize_epre_db(stats.epre_dB.value(), rnti);
    if (std::isnan(v)) {
      stats.epre_dB.reset();
    } else {
      stats.epre_dB = v;
    }
  }
}

/// \brief Post-EQ SINR from equalized noise variances (scheduler-facing CSI).
///
/// When a Class-A equalizer dApp runs inline, the host skips the conventional
/// E2E SINR D2H and the resident CE stub leaves CSI empty. Derive SINR from the
/// same post-EQ noise tensor used by soft demap: SINR_dB = -10 log10(mean σ²),
/// i.e. SINR under an assumed unit signal power. Reference Class-A estimators
/// publish physical post-equalization disturbance in that normalization, so no
/// calibration is required by default.
///
/// A positive default used by the older self-fit-residual estimator is unsafe
/// with the physical-noise contract: closed-loop PUSCH power control interprets
/// the inflated SINR as excess link margin and repeatedly commands the UE down.
/// Keep an explicit override for legacy/vendor modules with a documented noise
/// convention, but never apply an empirical offset to every Class-A module.
inline bool post_eq_sinr_db_from_mean_noise(float mean, float& sinr_db)
{
  static const float host_convention_cal_db = []() {
    if (const char* env = std::getenv("OCUDU_DAPP_UL_SINR_CAL_DB")) {
      char*       end   = nullptr;
      const float value = std::strtof(env, &end);
      if (end != env && std::isfinite(value) && value >= -20.0f && value <= 20.0f) {
        return value;
      }
    }
    return 0.0f;
  }();

  // Per-RE erasure filtering is performed on device before reduction. Do not
  // filter the rounded mean again: a mean near 1e5 may round to that boundary.
  if (!std::isfinite(mean) || mean <= 0.0f) {
    return false;
  }
  sinr_db          = -convert_power_to_dB(mean) + host_convention_cal_db;
  return std::isfinite(sinr_db);
}

static unsigned pusch_gpu_e2e_timing_warn_us()
{
  static const unsigned warn_us = phy_acceleration_env_unsigned("OCUDU_PUSCH_GPU_E2E_TIMING_WARN_US", 0);
  return warn_us;
}

constexpr unsigned MAX_GPU_UCI_POLAR_CODEBLOCKS = 2;

unsigned divide_ceil_u(unsigned value, unsigned divisor)
{
  return (value + divisor - 1U) / divisor;
}

bool launch_polar_uci_decode_half(const void*                d_llrs_half,
                                  unsigned                   nof_llrs,
                                  unsigned                   nof_payload_bits,
                                  polar_handle_t*            handles,
                                  polar_uci_decode_result_t* d_results,
                                  unsigned&                  nof_codeblocks,
                                  cudaStream_t               stream)
{
  nof_codeblocks = 0;
  if (!d_llrs_half || !handles || !d_results || nof_payload_bits <= 11 || nof_llrs == 0) {
    return false;
  }

  unsigned crc_size = get_uci_crc_size(nof_payload_bits);
  if (crc_size != 6 && crc_size != 11) {
    return false;
  }

  unsigned codeblocks = get_nof_uci_codeblocks(nof_payload_bits, nof_llrs);
  if (codeblocks == 0 || codeblocks > MAX_GPU_UCI_POLAR_CODEBLOCKS) {
    return false;
  }

  unsigned llr_offset = 0;
  for (unsigned cb = 0; cb != codeblocks; ++cb) {
    unsigned cb_payload_bits =
        (cb == 0) ? (nof_payload_bits / codeblocks) : divide_ceil_u(nof_payload_bits, codeblocks);
    unsigned cb_enc_bits     = (cb + 1 == codeblocks) ? (nof_llrs - llr_offset) : (nof_llrs / codeblocks);
    unsigned nof_filler_bits = (cb == 0) ? (nof_payload_bits % codeblocks) : 0;
    unsigned message_size    = cb_payload_bits + crc_size + nof_filler_bits;
    if (message_size == 0 || message_size > POLAR_MAX_N || cb_enc_bits == 0 || !handles[cb]) {
      return false;
    }

    polar_code_config_t cfg    = {};
    nr_ldpc_status_t    status = polar_code_configure(
        &cfg, static_cast<int>(message_size), static_cast<int>(cb_enc_bits), 10, POLAR_IBIL_PRESENT);
    if (status != NR_LDPC_SUCCESS) {
      return false;
    }

    status = polar_configure_async(handles[cb], &cfg, stream);
    if (status != NR_LDPC_SUCCESS) {
      return false;
    }

    const __half* cb_llrs = static_cast<const __half*>(d_llrs_half) + llr_offset;
    status                = polar_uci_rate_dematch_decode_crc_half(handles[cb],
                                                    d_results + cb,
                                                    cb_llrs,
                                                    static_cast<int>(cb_payload_bits),
                                                    static_cast<int>(nof_filler_bits),
                                                    static_cast<int>(crc_size),
                                                    stream);
    if (status != NR_LDPC_SUCCESS) {
      return false;
    }
    llr_offset += cb_enc_bits;
  }

  nof_codeblocks = codeblocks;
  return true;
}

/// \brief RT-friendly CUDA stream synchronization.
///
/// This function synchronizes on a CUDA stream while yielding the CPU to other threads.
/// When the calling thread has real-time (SCHED_FIFO/SCHED_RR) priority, a blocking
/// cudaStreamSynchronize() can cause priority inversion by preventing GPU driver threads
/// (which run at normal priority) from making progress.
///
/// This function uses cudaStreamQuery() in a loop with sched_yield() to allow other
/// threads to run while waiting for the GPU to complete.
///
/// \param stream The CUDA stream to synchronize on.
/// \return cudaSuccess on success, or an error code if the stream had an error.
/// Experiment knobs shared with cuda_rt_utils.h: OCUDU_CUDA_SYNC_BLOCKING=1 switches the
/// yielding query loops to plain blocking synchronization calls; OCUDU_CUDA_SYNC_SLEEP_US=N
/// throttles polling to one query every N microseconds.
inline bool cuda_sync_blocking_enabled()
{
  static const bool enabled = []() {
    const char* v = std::getenv("OCUDU_CUDA_SYNC_BLOCKING");
    return (v != nullptr) && (v[0] != '0');
  }();
  return enabled;
}

inline unsigned cuda_sync_sleep_us()
{
  static const unsigned sleep_us = []() -> unsigned {
    const char* v = std::getenv("OCUDU_CUDA_SYNC_SLEEP_US");
    if (v == nullptr || *v == '\0') {
      return 0;
    }
    return static_cast<unsigned>(std::strtoul(v, nullptr, 10));
  }();
  return sleep_us;
}

inline unsigned cuda_sync_spin_us()
{
  static const unsigned spin_us = []() -> unsigned {
    const char* v = std::getenv("OCUDU_CUDA_SYNC_SPIN_US");
    if (v == nullptr || *v == '\0') {
      return 0;
    }
    return static_cast<unsigned>(std::strtoul(v, nullptr, 10));
  }();
  return spin_us;
}

inline void cuda_sync_poll_pause()
{
  const unsigned spin_us = cuda_sync_spin_us();
  if (spin_us != 0) {
    timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const long budget_ns = static_cast<long>(spin_us) * 1000L;
    for (;;) {
      sched_yield();
      timespec t1;
      clock_gettime(CLOCK_MONOTONIC, &t1);
      long elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
      if (elapsed_ns >= budget_ns) {
        return;
      }
    }
  }
  const unsigned sleep_us = cuda_sync_sleep_us();
  if (sleep_us == 0) {
    sched_yield();
    return;
  }
  timespec ts = {0, static_cast<long>(sleep_us) * 1000L};
  nanosleep(&ts, nullptr);
}

inline cudaError_t cudaStreamSynchronizeYielding(cudaStream_t stream)
{
  if (cuda_sync_blocking_enabled()) {
    return cudaStreamSynchronize(stream);
  }
  cudaError_t err;
  while ((err = cudaStreamQuery(stream)) == cudaErrorNotReady) {
    cuda_sync_poll_pause();
  }
  return err;
}

/// \brief RT-friendly CUDA event synchronization.
///
/// Similar to cudaStreamSynchronizeYielding but for events.
/// Waits for an event to complete while yielding the CPU.
///
/// \param event The CUDA event to wait for.
/// \return cudaSuccess on success, or an error code if the event had an error.
inline cudaError_t cudaEventSynchronizeYielding(cudaEvent_t event)
{
  if (cuda_sync_blocking_enabled()) {
    return cudaEventSynchronize(event);
  }
  cudaError_t err;
  while ((err = cudaEventQuery(event)) == cudaErrorNotReady) {
    cuda_sync_poll_pause();
  }
  return err;
}

static unsigned get_gpu_ulsch_demultiplex_l1(const symbol_slot_mask& dmrs_symbol_mask)
{
  int first_symbol_dmrs = dmrs_symbol_mask.find_lowest(true);
  ocudu_assert(first_symbol_dmrs >= 0, "No DM-RS symbol found.");
  int first_symbol_without_dmrs =
      dmrs_symbol_mask.find_lowest(static_cast<size_t>(first_symbol_dmrs), dmrs_symbol_mask.size(), false);
  ocudu_assert(first_symbol_without_dmrs >= 0, "No DM-RS symbol found.");
  return static_cast<unsigned>(first_symbol_without_dmrs);
}

static unsigned get_gpu_ulsch_demultiplex_l1_csi(const symbol_slot_mask& dmrs_symbol_mask)
{
  int first_symbol_without_dmrs = dmrs_symbol_mask.find_lowest(false);
  ocudu_assert(first_symbol_without_dmrs >= 0, "No DM-RS symbol found.");
  return static_cast<unsigned>(first_symbol_without_dmrs);
}

static unsigned
get_gpu_ulsch_demultiplex_nof_re_prb_dmrs(dmrs_config_type dmrs, unsigned nof_cdm_groups_without_data, unsigned nof_prb)
{
  unsigned nof_re_dmrs_per_rb = nof_cdm_groups_without_data * get_nof_re_per_prb(dmrs);
  return (NOF_SUBCARRIERS_PER_RB - nof_re_dmrs_per_rb) * nof_prb;
}

static bounded_bitset<MAX_NOF_SUBCARRIERS>
gpu_re_set_select(const bounded_bitset<MAX_NOF_SUBCARRIERS>& re_set, unsigned d, unsigned m_re_count)
{
  bounded_bitset<MAX_NOF_SUBCARRIERS> result(re_set.size());

  for (unsigned count = 0, startpos = 0, d_count = 0; count != m_re_count;) {
    int found = re_set.find_lowest(startpos, re_set.size());
    ocudu_assert(found >= 0, "It must always find a true.");
    if (d_count % d == 0) {
      result.set(found);
      ++count;
    }
    ++d_count;
    startpos = found + 1;
  }

  return result;
}

static pusch_resident_sch_compaction get_resident_sch_compaction_config(const pusch_demodulator::configuration& config)
{
  return config.resident.sch_compaction;
}

static bool resident_host_uci_demux_enabled(const pusch_demodulator::configuration& config)
{
  return config.resident.host_uci_demux;
}

static bool resident_device_uci_demux_enabled(const pusch_demodulator::configuration& config)
{
  return config.resident.device_uci_demux;
}

static void build_resident_compaction_re_indices(std::vector<int>&                       sch_re_indices,
                                                 std::vector<int>&                       harq_ack_re_indices,
                                                 std::vector<int>&                       csi_part1_re_indices,
                                                 const pusch_demodulator::configuration& config,
                                                 unsigned                                nof_re_total)
{
  sch_re_indices.clear();
  harq_ack_re_indices.clear();
  csi_part1_re_indices.clear();

  const auto compaction = get_resident_sch_compaction_config(config);
  if (!compaction.enabled || compaction.nof_ul_sch_bits == 0) {
    return;
  }

  unsigned nof_bits_per_re = get_bits_per_symbol(config.modulation) * config.nof_tx_layers;
  ocudu_assert(nof_bits_per_re > 0, "Invalid PUSCH bits per RE.");
  ocudu_assert(compaction.nof_ul_sch_bits % nof_bits_per_re == 0,
               "The number of SCH LLRs must be a multiple of bits per RE.");

  unsigned expected_sch_re = compaction.nof_ul_sch_bits / nof_bits_per_re;
  sch_re_indices.reserve(expected_sch_re);
  harq_ack_re_indices.reserve(compaction.nof_enc_harq_ack_bits / nof_bits_per_re);
  csi_part1_re_indices.reserve(compaction.nof_enc_csi_part1_bits / nof_bits_per_re);

  unsigned l1      = get_gpu_ulsch_demultiplex_l1(config.dmrs_symb_pos);
  unsigned l1_csi  = get_gpu_ulsch_demultiplex_l1_csi(config.dmrs_symb_pos);
  unsigned nof_prb = static_cast<unsigned>(config.rb_mask.count());
  unsigned nof_re_dmrs =
      get_gpu_ulsch_demultiplex_nof_re_prb_dmrs(config.dmrs_type, config.nof_cdm_groups_without_data, nof_prb);

  unsigned m_rvd_count       = 0;
  unsigned m_harq_ack_count  = 0;
  unsigned m_csi_part1_count = 0;
  unsigned m_csi_part2_count = 0;
  unsigned src_re_base       = 0;

  for (unsigned ofdm_symbol_index = config.start_symbol_index,
                ofdm_symbol_end   = config.start_symbol_index + config.nof_symbols;
       ofdm_symbol_index != ofdm_symbol_end;
       ++ofdm_symbol_index) {
    bool     contain_dm_rs = config.dmrs_symb_pos.test(ofdm_symbol_index);
    unsigned m_ulsch       = contain_dm_rs ? nof_re_dmrs : nof_prb * NOF_SUBCARRIERS_PER_RB;
    if (m_ulsch == 0) {
      continue;
    }

    bounded_bitset<MAX_NOF_SUBCARRIERS> ulsch_re_set(m_ulsch);
    bounded_bitset<MAX_NOF_SUBCARRIERS> uci_re_set(m_ulsch);
    bounded_bitset<MAX_NOF_SUBCARRIERS> rvd_re_set(m_ulsch);
    bounded_bitset<MAX_NOF_SUBCARRIERS> harq_ack_re_set(m_ulsch);
    bounded_bitset<MAX_NOF_SUBCARRIERS> csi_part1_re_set(m_ulsch);
    bounded_bitset<MAX_NOF_SUBCARRIERS> csi_part2_re_set(m_ulsch);
    ulsch_re_set.fill(0, m_ulsch, true);
    uci_re_set.fill(0, m_ulsch, !contain_dm_rs);

    unsigned m_uci                  = uci_re_set.count();
    unsigned remainder_harq_ack_rvd = (compaction.nof_harq_ack_rvd - m_rvd_count) / nof_bits_per_re;
    if ((ofdm_symbol_index >= l1) && (m_uci > 0) && (remainder_harq_ack_rvd > 0)) {
      unsigned d          = 1;
      unsigned m_re_count = m_uci;
      if (remainder_harq_ack_rvd < m_uci) {
        d          = m_uci / remainder_harq_ack_rvd;
        m_re_count = remainder_harq_ack_rvd;
      }
      rvd_re_set = gpu_re_set_select(ulsch_re_set, d, m_re_count);
      m_rvd_count += m_re_count * nof_bits_per_re;
    }

    unsigned remainder_harq_ack = (compaction.nof_enc_harq_ack_bits - m_harq_ack_count) / nof_bits_per_re;
    if ((ofdm_symbol_index >= l1) && (m_uci > 0) && (compaction.nof_harq_ack_bits > 2) && (remainder_harq_ack > 0)) {
      unsigned d          = 1;
      unsigned m_re_count = m_uci;
      if (remainder_harq_ack < m_uci) {
        d          = m_uci / remainder_harq_ack;
        m_re_count = remainder_harq_ack;
      }
      harq_ack_re_set = gpu_re_set_select(uci_re_set, d, m_re_count);
      ulsch_re_set &= ~harq_ack_re_set;
      uci_re_set &= ~harq_ack_re_set;
      m_uci = uci_re_set.count();
      m_harq_ack_count += m_re_count * nof_bits_per_re;
    }

    unsigned remainder_csi_part1 = (compaction.nof_enc_csi_part1_bits - m_csi_part1_count) / nof_bits_per_re;
    unsigned m_rvd               = rvd_re_set.count();
    if ((ofdm_symbol_index >= l1_csi) && ((m_uci - m_rvd) > 0) && (remainder_csi_part1 > 0)) {
      unsigned d          = 1;
      unsigned m_re_count = m_uci - m_rvd;
      if (remainder_csi_part1 < (m_uci - m_rvd)) {
        d          = (m_uci - m_rvd) / remainder_csi_part1;
        m_re_count = remainder_csi_part1;
      }
      bounded_bitset<MAX_NOF_SUBCARRIERS> temp_re_set = ~rvd_re_set;
      temp_re_set &= uci_re_set;
      csi_part1_re_set = gpu_re_set_select(temp_re_set, d, m_re_count);
      ulsch_re_set &= ~csi_part1_re_set;
      uci_re_set &= ~csi_part1_re_set;
      m_uci = uci_re_set.count();
      m_csi_part1_count += m_re_count * nof_bits_per_re;
    }

    unsigned remainder_csi_part2 = (compaction.nof_enc_csi_part2_bits - m_csi_part2_count) / nof_bits_per_re;
    if ((ofdm_symbol_index >= l1_csi) && (m_uci > 0) && (remainder_csi_part2 > 0)) {
      unsigned d          = 1;
      unsigned m_re_count = m_uci;
      if (remainder_csi_part2 < m_uci) {
        d          = m_uci / remainder_csi_part2;
        m_re_count = remainder_csi_part2;
      }
      csi_part2_re_set = gpu_re_set_select(uci_re_set, d, m_re_count);
      ulsch_re_set &= ~csi_part2_re_set;
      uci_re_set &= ~csi_part2_re_set;
      m_csi_part2_count += m_re_count * nof_bits_per_re;
    }

    // HARQ-ACK with one or two information bits uses placeholders on reserved REs and does not remove SCH REs.
    if ((m_rvd > 0) && (compaction.nof_harq_ack_bits <= 2) && (remainder_harq_ack > 0)) {
      unsigned d          = 1;
      unsigned m_re_count = m_rvd;
      if (remainder_harq_ack < m_rvd) {
        d          = m_rvd / remainder_harq_ack;
        m_re_count = remainder_harq_ack;
      }
      harq_ack_re_set = gpu_re_set_select(rvd_re_set, d, m_re_count);
      m_harq_ack_count += m_re_count * nof_bits_per_re;
    }

    harq_ack_re_set.for_each(0, harq_ack_re_set.size(), [&](unsigned i_re) {
      harq_ack_re_indices.push_back(static_cast<int>(src_re_base + i_re));
    });
    csi_part1_re_set.for_each(0, csi_part1_re_set.size(), [&](unsigned i_re) {
      csi_part1_re_indices.push_back(static_cast<int>(src_re_base + i_re));
    });
    ulsch_re_set.for_each(
        0, ulsch_re_set.size(), [&](unsigned i_re) { sch_re_indices.push_back(static_cast<int>(src_re_base + i_re)); });
    src_re_base += m_ulsch;
  }

  ocudu_assert(src_re_base == nof_re_total, "SCH compaction RE count and demodulator RE count differ.");
  ocudu_assert(sch_re_indices.size() == expected_sch_re,
               "SCH compaction expected {} RE but selected {} RE.",
               expected_sch_re,
               sch_re_indices.size());
  ocudu_assert(harq_ack_re_indices.size() == (compaction.nof_enc_harq_ack_bits / nof_bits_per_re),
               "HARQ compaction expected {} RE but selected {} RE.",
               compaction.nof_enc_harq_ack_bits / nof_bits_per_re,
               harq_ack_re_indices.size());
  ocudu_assert(csi_part1_re_indices.size() == (compaction.nof_enc_csi_part1_bits / nof_bits_per_re),
               "CSI Part 1 compaction expected {} RE but selected {} RE.",
               compaction.nof_enc_csi_part1_bits / nof_bits_per_re,
               csi_part1_re_indices.size());
}

/// Precomputed LUT mapping FP16 bit patterns to clamped INT8 (x4.0 scale).
/// 64KB table, initialized once on first use. Eliminates per-element FP16 bit
/// manipulation in the non-GPU-resident D2H LLR conversion path.
alignas(64) static int8_t fp16_to_int8_lut[65536];
static bool fp16_lut_initialized = false;

void init_fp16_to_int8_lut()
{
  for (unsigned bits = 0; bits < 65536; ++bits) {
    uint32_t sign = (bits >> 15) & 0x1;
    uint32_t exp  = (bits >> 10) & 0x1f;
    uint32_t mant = bits & 0x3ff;
    float    val;
    if (exp == 0) {
      // Subnormal or zero.
      val = (sign ? -1.0f : 1.0f) * (mant / 1024.0f) * (1.0f / 16384.0f);
    } else if (exp == 31) {
      // Device demodulator softbits are finite LLRs. Reserve +/-127 for fixed bits.
      val = sign ? -static_cast<float>(HOST_LLR_MAX) : static_cast<float>(HOST_LLR_MAX);
    } else {
      val = (sign ? -1.0f : 1.0f) * (1.0f + mant / 1024.0f) * std::pow(2.0f, static_cast<float>(exp) - 15.0f);
    }
    float scaled           = val * 4.0f;
    int   clamped          = static_cast<int>(std::round(scaled));
    clamped                = clamp_host_llr(clamped);
    fp16_to_int8_lut[bits] = static_cast<int8_t>(clamped);
  }
  fp16_lut_initialized = true;
}

} // namespace

/// Minimum symbols to use GPU (below this, CPU overhead wins).
static constexpr unsigned MIN_GPU_SYMBOLS = 256;

pusch_demodulator_gpu_impl::pusch_demodulator_gpu_impl(std::unique_ptr<channel_equalizer>       equalizer_,
                                                       std::unique_ptr<transform_precoder>      precoder_,
                                                       std::unique_ptr<demodulation_mapper>     demapper_fallback_,
                                                       std::unique_ptr<evm_calculator>          evm_calc_,
                                                       std::unique_ptr<pseudo_random_generator> descrambler_,
                                                       unsigned                                 max_nof_rb,
                                                       bool                                     compute_post_eq_sinr_,
                                                       bool                                     compensate_cfo,
                                                       channel_equalizer_algorithm_type         equalizer_algorithm,
                                                       int                                      nvcuda_device_id) :
  equalizer(std::move(equalizer_)),
  precoder(std::move(precoder_)),
  demapper_fallback(std::move(demapper_fallback_)),
  evm_calc(std::move(evm_calc_)),
  descrambler(std::move(descrambler_)),
  ch_re_copy(MAX_PORTS, max_nof_rb * NOF_SUBCARRIERS_PER_RB),
  temp_eq_re(max_nof_rb * NOF_SUBCARRIERS_PER_RB * pusch_constants::MAX_NOF_LAYERS),
  temp_eq_noise_vars(max_nof_rb * NOF_SUBCARRIERS_PER_RB * pusch_constants::MAX_NOF_LAYERS),
  ch_estimates_copy(max_nof_rb * NOF_SUBCARRIERS_PER_RB,
                    pusch_constants::MAX_NOF_RX_PORTS,
                    pusch_constants::MAX_NOF_LAYERS),
  compute_post_eq_sinr(compute_post_eq_sinr_),
  compensate_cfo_(compensate_cfo),
  equalizer_algorithm_(equalizer_algorithm),
  device_id_(nvcuda_device_id),
  max_nof_rb_(max_nof_rb)
{
  ocudu_assert(equalizer, "Invalid pointer to channel_equalizer object.");
  ocudu_assert(demapper_fallback, "Invalid pointer to demodulation_mapper object.");
  ocudu_assert(descrambler, "Invalid pointer to pseudo_random_generator object.");

  // Initialize FP16→INT8 conversion LUT once (64KB, thread-safe via static flag).
  if (!fp16_lut_initialized) {
    init_fp16_to_int8_lut();
  }

  // Enable optional path tracing for diagnostics and release qualification.
  const char* trace_env = std::getenv("OCUDU_PUSCH_ACCELERATION_TRACE");
  enable_path_tracing_  = trace_env && (std::string(trace_env) == "1" || std::string(trace_env) == "true");
  if (enable_path_tracing_) {
    fmt::print("[PUSCH GPU] Path tracing ENABLED via OCUDU_PUSCH_ACCELERATION_TRACE\n");
  }

  // Time interpolation mode: average (default) or linear.
  const char* ti_env = std::getenv("OCUDU_TIME_INTERP");
  if (ti_env && std::string(ti_env) == "linear") {
    time_interp_mode_ = 1;
    fmt::print("[PUSCH GPU] Time interpolation: linear (OCUDU_TIME_INTERP=linear)\n");
  }

  // Noise estimation mode: 1 = pilot-residual (default), 0 = cross-validation.
  // OCUDU_NOISE_MODE=cv restores the old cross-validation mode for A/B testing.
  const char* noise_env = std::getenv("OCUDU_NOISE_MODE");
  if (noise_env && std::string(noise_env) == "cv") {
    noise_mode_ = 0;
    fmt::print("[PUSCH GPU] Noise estimation: cross-validation (OCUDU_NOISE_MODE=cv)\n");
  }

  // Reserve space for accumulated symbols (max 14 symbols * 273 RB * 12 RE * 4 layers).
  size_t max_symbols = 14 * max_nof_rb * NOF_SUBCARRIERS_PER_RB * pusch_constants::MAX_NOF_LAYERS;
  accumulated_symbols_.reserve(max_symbols);
  accumulated_noise_vars_.reserve(max_symbols);

  // Initialize CUDA resources.
  (void)cudaGetLastError();
  // Explicitly bind to the configured device to ensure CUDA context stability across pinned threads.
  cudaError_t err = cudaSetDevice(device_id_);
  if (err != cudaSuccess) {
    (void)cudaGetLastError();
    int device_count = 0;
    err              = cudaGetDeviceCount(&device_count);
    if (err != cudaSuccess || device_count == 0 || cudaSetDevice(device_id_) != cudaSuccess) {
      (void)cudaGetLastError();
      gpu_available_ = false;
      return;
    }
  } else {
    (void)cudaGetLastError();
  }

  int device_count = 0;
  err              = cudaGetDeviceCount(&device_count);
  if (err == cudaSuccess && device_count == 0) {
    gpu_available_ = false;
    return;
  }
  (void)cudaGetLastError();

  // Configure CUDA to yield CPU when waiting for GPU operations.
  err = cudaSetDeviceFlags(cudaDeviceScheduleYield);
  if (err != cudaSuccess && err != cudaErrorSetOnActiveProcess) {
    // Non-fatal - continue.
  }

  // Create CUDA modulator handle.
  if (modulator_create(&mod_handle_) != 0) {
    gpu_available_ = false;
    return;
  }

  // Create CUDA scrambler handle.
  nr_ldpc_status_t status = scrambler_create(&scr_handle_);
  if (status != NR_LDPC_SUCCESS) {
    modulator_destroy(mod_handle_);
    mod_handle_    = nullptr;
    gpu_available_ = false;
    return;
  }

  err = ocudu::cudaStreamCreateUpperPhy(&stream_, "OCUDU_PUSCH_CUDA_STREAM_PRIORITY");
  if (err != cudaSuccess) {
    scrambler_destroy(scr_handle_);
    modulator_destroy(mod_handle_);
    scr_handle_    = nullptr;
    mod_handle_    = nullptr;
    gpu_available_ = false;
    return;
  }

  err = ocudu::cudaStreamCreateUpperPhy(&scr_stream_, "OCUDU_PUSCH_CUDA_STREAM_PRIORITY");
  if (err != cudaSuccess) {
    cudaStreamDestroy(stream_);
    scrambler_destroy(scr_handle_);
    modulator_destroy(mod_handle_);
    stream_        = nullptr;
    scr_handle_    = nullptr;
    mod_handle_    = nullptr;
    gpu_available_ = false;
    return;
  }

  // Create event for synchronizing scrambler completion.
  err = cudaEventCreateWithFlags(&scr_event_, cudaEventDisableTiming);
  if (err != cudaSuccess) {
    cudaStreamDestroy(scr_stream_);
    cudaStreamDestroy(stream_);
    scrambler_destroy(scr_handle_);
    modulator_destroy(mod_handle_);
    scr_stream_    = nullptr;
    stream_        = nullptr;
    scr_handle_    = nullptr;
    mod_handle_    = nullptr;
    gpu_available_ = false;
    return;
  }

  // Create completion event for async D2H tracking.
  err = cudaEventCreateWithFlags(&completion_event_, cudaEventDisableTiming);
  if (err != cudaSuccess) {
    cudaEventDestroy(scr_event_);
    cudaStreamDestroy(scr_stream_);
    cudaStreamDestroy(stream_);
    scrambler_destroy(scr_handle_);
    modulator_destroy(mod_handle_);
    completion_event_ = nullptr;
    scr_event_        = nullptr;
    scr_stream_       = nullptr;
    stream_           = nullptr;
    scr_handle_       = nullptr;
    mod_handle_       = nullptr;
    gpu_available_    = false;
    return;
  }

  // Create profiling events for latency breakdown (always enabled, minimal overhead)
  cudaEventCreate(&prof_h2d_start_);
  cudaEventCreate(&prof_h2d_end_);
  cudaEventCreate(&prof_kernel_start_);
  cudaEventCreate(&prof_kernel_end_);
  cudaEventCreate(&prof_d2h_start_);
  cudaEventCreate(&prof_d2h_end_);

  // Create CUDA E2E handle for GPU channel estimation.
  nr_ldpc_status_t e2e_status = pusch_e2e_create(&e2e_handle_);

  if (e2e_status == NR_LDPC_SUCCESS) {
    nr_ldpc_status_t preplan_status = pusch_e2e_preplan_transform_deprecoder(e2e_handle_, stream_);
    if (preplan_status != NR_LDPC_SUCCESS) {
      ocudulog::fetch_basic_logger("PHY").warning("PUSCH GPU: transform deprecoder preplan failed, status={}",
                                                  static_cast<int>(preplan_status));
    }
    use_gpu_chest_        = true;
    use_gpu_equalization_ = true; // E2E pipeline includes GPU equalization
    ocudulog::fetch_basic_logger("PHY").debug("PUSCH GPU: CUDA E2E handle created successfully");
  } else {
    e2e_handle_           = nullptr;
    use_gpu_chest_        = false;
    use_gpu_equalization_ = false;
    ocudulog::fetch_basic_logger("PHY").warning("PUSCH GPU: Failed to create CUDA E2E handle, status={}",
                                                static_cast<int>(e2e_status));
  }

  gpu_uci_polar_available_ = true;
  for (int buf = 0; buf != NUM_BUFFERS; ++buf) {
    for (unsigned cb = 0; cb != MAX_GPU_UCI_CODEBLOCKS; ++cb) {
      gpu_uci_polar_available_ =
          gpu_uci_polar_available_ && (polar_create(&harq_ack_polar_handles_[buf][cb]) == NR_LDPC_SUCCESS);
      gpu_uci_polar_available_ =
          gpu_uci_polar_available_ && (polar_create(&csi_part1_polar_handles_[buf][cb]) == NR_LDPC_SUCCESS);
    }
  }
  if (!gpu_uci_polar_available_) {
    ocudulog::fetch_basic_logger("PHY").warning(
        "PUSCH GPU: polar UCI decode handles unavailable; using host UCI fallback.");
  }

  gpu_available_ = true;

  // Pre-allocate pinned host buffers for D2H LLR transfers.
  constexpr size_t MAX_LLRS_PER_SLOT = 14 * 273 * 12 * pusch_constants::MAX_NOF_LAYERS * 8;
  err = cudaHostAlloc(&h_llrs_half_pinned_, MAX_LLRS_PER_SLOT * sizeof(uint16_t), cudaHostAllocDefault);
  if (err == cudaSuccess) {
    h_llrs_half_pinned_cap_ = MAX_LLRS_PER_SLOT;
  }
  // Create separate stream for async D2H copy.
  err = ocudu::cudaStreamCreateUpperPhy(&d2h_stream_, "OCUDU_PUSCH_CUDA_STREAM_PRIORITY");
  if (err != cudaSuccess) {
    cudaStreamCreateWithFlags(&d2h_stream_, cudaStreamNonBlocking);
  }

  // Create event for D2H completion tracking.
  err = cudaEventCreateWithFlags(&d2h_event_, cudaEventDisableTiming);
  if (err != cudaSuccess) {
    d2h_event_ = nullptr;
  }

  // Pre-allocate constellation diagnostic staging (pinned host + device tap + events).
  // Must not allocate on the first rate-limited capture during Msg3/RACH.
  ensure_eq_diag_resources();

  // Initialize CUDA memory pool for async allocations.
  cudaDeviceGetMemPool(&mem_pool_, device_id_);
  if (mem_pool_) {
    uint64_t threshold = UINT64_MAX;
    cudaMemPoolSetAttribute(mem_pool_, cudaMemPoolAttrReleaseThreshold, &threshold);
    use_mem_pool_ = true;
  }

  // Pre-allocate worst-case device buffers.
  constexpr size_t MAX_RE_PER_SLOT = 14 * 273 * 12 * 4;

  size_t symbols_bytes = MAX_RE_PER_SLOT * sizeof(cuFloatComplex);
  err                  = cudaMalloc(&d_symbols_, symbols_bytes);
  if (err == cudaSuccess) {
    d_symbols_capacity_ = symbols_bytes;
  }

  size_t noise_bytes = MAX_RE_PER_SLOT * sizeof(float);
  err                = cudaMalloc(&d_noise_vars_, noise_bytes);
  if (err == cudaSuccess) {
    d_noise_vars_capacity_ = noise_bytes;
  }

  // Allocate triple-buffered FP16 LLR outputs for GPU-resident pipeline.
  size_t llrs_half_bytes = MAX_LLRS_PER_SLOT * sizeof(uint16_t);
  for (int i = 0; i < NUM_BUFFERS; i++) {
    err = cudaMalloc(&d_llrs_half_[i], llrs_half_bytes);
    if (err == cudaSuccess) {
      d_llrs_half_capacity_[i] = llrs_half_bytes;
    }
  }

  // Pre-allocate unified input buffer for E2E path.
  constexpr size_t MAX_SUBCARRIERS = 273 * 12;
  constexpr size_t MAX_SYMBOLS     = 14;
  constexpr size_t MAX_PORTS_ALLOC = 8; // Support up to 8 RX ports for massive MIMO
  constexpr size_t ALIGN           = 256;

  size_t grid_stride         = MAX_SYMBOLS * MAX_SUBCARRIERS;
  size_t cbf16_bytes         = grid_stride * MAX_PORTS_ALLOC * sizeof(uint32_t);
  size_t indices_bytes       = MAX_RE_PER_SLOT * sizeof(int);
  size_t indices_offset_calc = ((cbf16_bytes + ALIGN - 1) / ALIGN) * ALIGN;
  size_t unified_total       = indices_offset_calc + ((indices_bytes + ALIGN - 1) / ALIGN) * ALIGN;

  // UCI compaction max: large multi-bit UCI with heavy rate matching. Overflow → CPU fallback.
  constexpr size_t MAX_UCI_LLRS       = 32768;
  constexpr size_t MAX_UCI_RE_INDICES = 16384;
  const size_t     uci_llrs_bytes     = MAX_UCI_LLRS * sizeof(uint16_t);
  const size_t     uci_indices_bytes  = MAX_UCI_RE_INDICES * sizeof(int);

  // DOUBLE-BUFFERING: Allocate two sets of buffers.
  for (int i = 0; i < NUM_BUFFERS; ++i) {
    err = cudaMalloc(&d_unified_input_[i], unified_total);
    if (err != cudaSuccess) {
      d_unified_input_[i] = nullptr;
    }
    err = cudaHostAlloc(&h_unified_staging_[i], unified_total, cudaHostAllocDefault);
    if (err != cudaSuccess) {
      h_unified_staging_[i] = nullptr;
    }
    err = cudaHostAlloc(reinterpret_cast<void**>(&h_harq_ack_decode_result_pinned_[i]),
                        sizeof(pusch_uci_short_decode_result) * MAX_GPU_UCI_CODEBLOCKS,
                        cudaHostAllocDefault);
    if (err != cudaSuccess) {
      h_harq_ack_decode_result_pinned_[i] = nullptr;
    }
    err = cudaHostAlloc(reinterpret_cast<void**>(&h_csi_part1_decode_result_pinned_[i]),
                        sizeof(pusch_uci_short_decode_result) * MAX_GPU_UCI_CODEBLOCKS,
                        cudaHostAllocDefault);
    if (err != cudaSuccess) {
      h_csi_part1_decode_result_pinned_[i] = nullptr;
    }
    err = cudaMalloc(&d_sch_llrs_half_[i], llrs_half_bytes);
    if (err == cudaSuccess) {
      d_sch_llrs_half_capacity_[i] = llrs_half_bytes;
    }
    err = cudaMalloc(&d_sch_re_indices_[i], indices_bytes);
    if (err == cudaSuccess) {
      d_sch_re_indices_capacity_[i] = MAX_RE_PER_SLOT;
    }
    // Preallocate UCI compaction buffers at max so first UCI-bearing PUSCH never
    // free+malloc under RT scheduling (same residency model as SCH LLRs/indices).
    err = cudaMalloc(&d_harq_ack_llrs_half_[i], uci_llrs_bytes);
    if (err == cudaSuccess) {
      d_harq_ack_llrs_half_capacity_[i] = uci_llrs_bytes;
    }
    err = cudaMalloc(&d_csi_part1_llrs_half_[i], uci_llrs_bytes);
    if (err == cudaSuccess) {
      d_csi_part1_llrs_half_capacity_[i] = uci_llrs_bytes;
    }
    err = cudaMalloc(&d_harq_ack_re_indices_[i], uci_indices_bytes);
    if (err == cudaSuccess) {
      d_harq_ack_re_indices_capacity_[i] = MAX_UCI_RE_INDICES;
    }
    err = cudaMalloc(&d_csi_part1_re_indices_[i], uci_indices_bytes);
    if (err == cudaSuccess) {
      d_csi_part1_re_indices_capacity_[i] = MAX_UCI_RE_INDICES;
    }
    err =
        cudaHostAlloc(reinterpret_cast<void**>(&h_harq_ack_llrs_half_pinned_[i]), uci_llrs_bytes, cudaHostAllocDefault);
    if (err == cudaSuccess) {
      h_harq_ack_llrs_half_pinned_cap_[i] = MAX_UCI_LLRS;
    }
    err = cudaHostAlloc(
        reinterpret_cast<void**>(&h_csi_part1_llrs_half_pinned_[i]), uci_llrs_bytes, cudaHostAllocDefault);
    if (err == cudaSuccess) {
      h_csi_part1_llrs_half_pinned_cap_[i] = MAX_UCI_LLRS;
    }
    err = cudaMalloc(&d_harq_ack_decode_result_[i], sizeof(pusch_uci_short_decode_result) * MAX_GPU_UCI_CODEBLOCKS);
    if (err != cudaSuccess) {
      d_harq_ack_decode_result_[i] = nullptr;
    }
    err = cudaMalloc(&d_csi_part1_decode_result_[i], sizeof(pusch_uci_short_decode_result) * MAX_GPU_UCI_CODEBLOCKS);
    if (err != cudaSuccess) {
      d_csi_part1_decode_result_[i] = nullptr;
    }
  }
  unified_buf_cap_ = unified_total;

  // Host compaction vectors: reserve max so .resize/.push_back never realloc on the hot path.
  h_re_indices_.reserve(MAX_RE_PER_SLOT);
  h_sch_re_indices_.reserve(MAX_RE_PER_SLOT);
  h_harq_ack_re_indices_.reserve(MAX_UCI_RE_INDICES);
  h_csi_part1_re_indices_.reserve(MAX_UCI_RE_INDICES);
  h_harq_ack_llrs_.reserve(MAX_UCI_LLRS);
  h_csi_part1_llrs_.reserve(MAX_UCI_LLRS);
  h_harq_ack_llrs_half_.reserve(MAX_UCI_LLRS);
  h_csi_part1_llrs_half_.reserve(MAX_UCI_LLRS);

  // Create dedicated stream for H2D transfers.
  err = ocudu::cudaStreamCreateUpperPhy(&h2d_stream_, "OCUDU_PUSCH_CUDA_STREAM_PRIORITY");
  if (err != cudaSuccess) {
    h2d_stream_ = nullptr;
  }

  // Create events to track H2D completion.
  for (int i = 0; i < NUM_BUFFERS; ++i) {
    err = cudaEventCreateWithFlags(&h2d_complete_[i], cudaEventDisableTiming);
    if (err != cudaSuccess) {
      h2d_complete_[i] = nullptr;
    }
  }

  // Create dedicated stream for D2H transfers (for triple-buffering overlap).
  err = ocudu::cudaStreamCreateUpperPhy(&d2h_stream_, "OCUDU_PUSCH_CUDA_STREAM_PRIORITY");
  if (err != cudaSuccess) {
    d2h_stream_ = nullptr;
  }

  // Create events to track D2H completion.
  for (int i = 0; i < NUM_BUFFERS; ++i) {
    err = cudaEventCreateWithFlags(&d2h_complete_[i], cudaEventDisableTiming);
    if (err != cudaSuccess) {
      d2h_complete_[i] = nullptr;
    }
  }

  // Create events to track kernel completion (enables async D2H start).
  for (int i = 0; i < NUM_BUFFERS; ++i) {
    err = cudaEventCreateWithFlags(&kernel_complete_[i], cudaEventDisableTiming);
    if (err != cudaSuccess) {
      kernel_complete_[i] = nullptr;
    }
  }

  gpu_e2e_timing_warn_us_ = pusch_gpu_e2e_timing_warn_us();
  if (gpu_e2e_timing_warn_us_ != 0) {
    bool timing_events_created = true;
    for (int i = 0; i < NUM_BUFFERS; ++i) {
      timing_events_created = timing_events_created && (cudaEventCreate(&gpu_e2e_timing_[i].start) == cudaSuccess) &&
                              (cudaEventCreate(&gpu_e2e_timing_[i].h2d_end) == cudaSuccess) &&
                              (cudaEventCreate(&gpu_e2e_timing_[i].kernel_end) == cudaSuccess) &&
                              (cudaEventCreate(&gpu_e2e_timing_[i].final_end) == cudaSuccess);
    }
    gpu_e2e_timing_enabled_ = timing_events_created;
    if (!gpu_e2e_timing_enabled_) {
      for (auto& timing : gpu_e2e_timing_) {
        if (timing.start) {
          cudaEventDestroy(timing.start);
          timing.start = nullptr;
        }
        if (timing.h2d_end) {
          cudaEventDestroy(timing.h2d_end);
          timing.h2d_end = nullptr;
        }
        if (timing.kernel_end) {
          cudaEventDestroy(timing.kernel_end);
          timing.kernel_end = nullptr;
        }
        if (timing.final_end) {
          cudaEventDestroy(timing.final_end);
          timing.final_end = nullptr;
        }
      }
      ocudulog::fetch_basic_logger("PHY").warning(
          "PUSCH GPU E2E timing disabled: failed to create CUDA timing events.");
    }
  }

  // Pre-configure the E2E handle for a max-size shape so configure-time caches
  // are allocated without launching dummy data-path kernels. The previous
  // process-based warmup could poison the CUDA context before the first real
  // PUSCH, making the real E2E configure fail with a stale illegal-access error.
  if (gpu_available_ && use_gpu_chest_ && e2e_handle_) {
    ocudulog::fetch_basic_logger("PHY").debug("Pre-configuring E2E GPU caches for max size (273 PRB, 256QAM)...");

    pusch_e2e_config_t max_cfg          = {};
    max_cfg.nof_prb                     = 273; // Max bandwidth (100 MHz)
    max_cfg.start_prb                   = 0;
    max_cfg.nof_symbols                 = 14;
    max_cfg.start_symbol                = 0;
    max_cfg.dmrs_symbol_mask            = 0x0804; // Symbols 2 and 11
    max_cfg.dmrs_type                   = DMRS_TYPE_1;
    max_cfg.nof_cdm_groups_without_data = 2;
    max_cfg.mod_order                   = 8; // 256QAM (maximum)
    max_cfg.nof_rx_ports                = 4; // Standard max: 4 RX antennas
    max_cfg.nof_tx_layers               = 1; // Single layer (SISO)
    max_cfg.n_id                        = 0;
    max_cfg.n_scid                      = 0;
    max_cfg.slot_idx                    = 0;
    max_cfg.dmrs_scaling                = 1.0f;
    max_cfg.tx_scaling                  = 1.0f;
    max_cfg.grid_nof_subcarriers        = 273 * 12;
    max_cfg.grid_nof_symbols            = 14;
    max_cfg.equalizer_algorithm =
        (equalizer_algorithm_ == channel_equalizer_algorithm_type::mmse) ? EQUALIZER_MMSE : EQUALIZER_ZF;
    max_cfg.scrambling_id     = 0;
    max_cfg.rnti              = 0xffff;
    max_cfg.scs_khz           = 30; // Default 30 kHz SCS for warmup
    max_cfg.use_low_papr_dmrs = 0;  // Normal DMRS (no transform precoding)
    max_cfg.n_rs_id           = 0;
    max_cfg.enable_evm_metric = 0; // Keep latency warmup on the default metrics path.

    nr_ldpc_status_t cfg_status = pusch_e2e_configure(e2e_handle_, &max_cfg);
    if (cfg_status != NR_LDPC_SUCCESS) {
      ocudulog::fetch_basic_logger("PHY").warning("E2E pre-alloc: configuration failed with status {}",
                                                  static_cast<int>(cfg_status));
    } else {
      ocudulog::fetch_basic_logger("PHY").debug("E2E GPU cache pre-configuration complete");
    }

    // Warm the transform-precoded Msg3 path as well. Msg3 uses low-PAPR DMRS
    // and the custom DFT-s-OFDM deprecoder, whose first kernel setup must not
    // occur inside the OTA RAR/Msg3 timing window.
    unsigned         msg3_prb = std::max(1U, std::min(max_nof_rb_, 6U));
    nr_ldpc_status_t warm_status =
        pusch_e2e_warmup_transform_deprecoding(e2e_handle_, stream_, static_cast<int>(msg3_prb), 1);
    if (warm_status != NR_LDPC_SUCCESS) {
      ocudulog::fetch_basic_logger("PHY").warning("E2E Msg3 transform warmup failed with status {}",
                                                  static_cast<int>(warm_status));
    } else {
      ocudulog::fetch_basic_logger("PHY").debug("E2E Msg3 transform warmup complete");
    }
    // Resolve the dApp metric kernels at setup, using an empty input. Never read
    // uninitialized data and never defer first-launch setup to a live grant.
    if (pusch_e2e_dapp_noise_async(e2e_handle_, nullptr, 0, stream_) != NR_LDPC_SUCCESS) {
      ocudulog::fetch_basic_logger("PHY").warning("PUSCH dApp noise reduction warmup failed");
    }
    cudaStreamSynchronizeYielding(stream_);
  } else if (gpu_available_) {
    cudaStreamSynchronizeYielding(stream_);
  }
}

void pusch_demodulator_gpu_impl::ensure_eq_diag_resources()
{
  // Host pinned + completion event are required for every capture path (NeuralEq D2H and E2E tap).
  // Device tap buffers are only needed for conventional E2E set_eq_output — allocate best-effort.
  const std::size_t capacity = MAX_CAPTURE_EQ_SYMBOLS;
  const std::size_t eq_bytes = capacity * sizeof(std::complex<float>);
  const std::size_t nv_bytes = capacity * sizeof(float);

  if (h_eq_diag_pinned_ == nullptr) {
    if (cudaMallocHost(reinterpret_cast<void**>(&h_eq_diag_pinned_), eq_bytes) != cudaSuccess) {
      h_eq_diag_pinned_ = nullptr;
      static_cast<void>(cudaGetLastError());
      eq_diag_capacity_ = 0;
      return;
    }
  }
  if (eq_diag_ready_event_ == nullptr) {
    if (cudaEventCreateWithFlags(&eq_diag_ready_event_, cudaEventDisableTiming) != cudaSuccess) {
      eq_diag_ready_event_ = nullptr;
      static_cast<void>(cudaGetLastError());
    }
  }
  if (eq_diag_event_ == nullptr) {
    if (cudaEventCreateWithFlags(&eq_diag_event_, cudaEventDisableTiming) != cudaSuccess) {
      eq_diag_event_ = nullptr;
      static_cast<void>(cudaGetLastError());
    }
  }
  // E2E conventional tap buffers (optional for NeuralEq path).
  if (d_eq_diag_ == nullptr) {
    if (cudaMalloc(&d_eq_diag_, eq_bytes) != cudaSuccess) {
      d_eq_diag_ = nullptr;
      static_cast<void>(cudaGetLastError());
    }
  }
  if (d_eq_noise_diag_ == nullptr) {
    if (cudaMalloc(&d_eq_noise_diag_, nv_bytes) != cudaSuccess) {
      d_eq_noise_diag_ = nullptr;
      static_cast<void>(cudaGetLastError());
    }
  }
  // Capacity is usable as soon as host staging exists.
  if (h_eq_diag_pinned_ != nullptr) {
    eq_diag_capacity_ = capacity;
  }
}

void pusch_demodulator_gpu_impl::configure_e2e_eq_tap(bool enable)
{
  if (e2e_handle_ == nullptr) {
    return;
  }
  if (enable) {
    ensure_eq_diag_resources();
    if (d_eq_diag_ == nullptr || d_eq_noise_diag_ == nullptr) {
      return;
    }
    pusch_e2e_set_eq_output(e2e_handle_, static_cast<float2*>(d_eq_diag_), static_cast<float*>(d_eq_noise_diag_));
    eq_diag_tap_on_ = true;
  } else if (eq_diag_tap_on_) {
    pusch_e2e_set_eq_output(e2e_handle_, nullptr, nullptr);
    eq_diag_tap_on_ = false;
  }
}

void pusch_demodulator_gpu_impl::enable_symbol_capture()
{
  // Request capture for the next demodulation only. The previous grant's diagnostic storage remains owned
  // until begin_grant() succeeds in demodulate(), where all per-grant capture state is reset together.
  capture_symbols_enabled_ = true;
  capture_requested_slot_  = true;
  ocudulog::fetch_basic_logger("PHY").debug(
      "constellation capture enable pinned={} cap={}", h_eq_diag_pinned_ != nullptr, eq_diag_capacity_);
}

void pusch_demodulator_gpu_impl::disable_symbol_capture()
{
  capture_symbols_enabled_ = false;
}

bool pusch_demodulator_gpu_impl::force_eq_diag_host_copy()
{
  // Only actual symbol producers may advertise a source. last_nof_eq_re_ is
  // also the noise-reduction extent: a full receiver can set it without
  // supplying symbols, so it is NOT permission to read d_symbols_.
  const void* src = last_capture_src_;
  std::size_t n   = last_capture_n_;
  if (src == nullptr || n == 0) {
    if (last_e2e_eq_count_ > 0 && d_eq_diag_ != nullptr) {
      src = d_eq_diag_;
      n   = last_e2e_eq_count_;
    }
  }
  if (src == nullptr || n == 0) {
    return false;
  }
  ensure_eq_diag_resources();
  n = std::min(n, eq_diag_capacity_ != 0 ? eq_diag_capacity_ : n);
  if (n == 0) {
    return false;
  }
  captured_eq_host_.resize(n);
  // Blocking D2H — only called from decoder pre-join after LDPC has already waited on stream_.
  if (cudaMemcpy(captured_eq_host_.data(), src, n * sizeof(std::complex<float>), cudaMemcpyDeviceToHost) !=
      cudaSuccess) {
    static_cast<void>(cudaGetLastError());
    captured_eq_host_.clear();
    eq_diag_host_valid_ = false;
    eq_diag_count_      = 0;
    return false;
  }
  eq_diag_count_      = n;
  eq_diag_host_valid_ = true;
  eq_diag_queued_     = true;
  // Also mirror into pinned buffer for any legacy readers.
  if (h_eq_diag_pinned_ != nullptr && eq_diag_capacity_ >= n) {
    std::memcpy(h_eq_diag_pinned_, captured_eq_host_.data(), n * sizeof(std::complex<float>));
  }
  return true;
}

void pusch_demodulator_gpu_impl::queue_eq_symbol_d2h(const void* src,
                                                     std::size_t nof_eq_symbols,
                                                     cudaEvent_t ready_event)
{
  // Fully off the slot-critical CPU path: only enqueue GPU work.
  // Sibling stream waits for source-ready, then D2H in parallel with soft-demod/LDPC.
  if ((!capture_symbols_enabled_ && !capture_requested_slot_) || src == nullptr || nof_eq_symbols == 0) {
    static std::atomic<unsigned> skip_logs{0};
    if (skip_logs.fetch_add(1, std::memory_order_relaxed) < 4) {
      ocudulog::fetch_basic_logger("PHY").debug("constellation queue SKIP enabled={} req={} src={} n={} this={}",
                                                capture_symbols_enabled_,
                                                capture_requested_slot_,
                                                src != nullptr,
                                                nof_eq_symbols,
                                                static_cast<const void*>(this));
    }
    return;
  }
  ensure_eq_diag_resources();
  // Always record the device source so pre-join can force a blocking D2H even if async fails.
  const std::size_t n_cap = eq_diag_capacity_ != 0 ? eq_diag_capacity_ : MAX_CAPTURE_EQ_SYMBOLS;
  const std::size_t n     = std::min(nof_eq_symbols, n_cap);
  last_capture_src_       = src;
  last_capture_n_         = n;
  static std::atomic<unsigned> queue_logs{0};
  if (queue_logs.fetch_add(1, std::memory_order_relaxed) < 4) {
    ocudulog::fetch_basic_logger("PHY").debug("constellation queue OK n={} src={} pinned={} this={}",
                                              n,
                                              src != nullptr,
                                              h_eq_diag_pinned_ != nullptr,
                                              static_cast<const void*>(this));
  }
  if (h_eq_diag_pinned_ == nullptr || eq_diag_capacity_ == 0) {
    return;
  }
  const std::size_t bytes    = n * sizeof(std::complex<float>);
  cudaStream_t      download = d2h_stream_ != nullptr ? d2h_stream_ : stream_;
  if (d2h_stream_ != nullptr) {
    if (ready_event != nullptr) {
      cudaStreamWaitEvent(download, ready_event, 0);
    } else if (eq_diag_ready_event_ != nullptr) {
      cudaEventRecord(eq_diag_ready_event_, stream_);
      cudaStreamWaitEvent(download, eq_diag_ready_event_, 0);
    }
  }
  if (cudaMemcpyAsync(h_eq_diag_pinned_, src, bytes, cudaMemcpyDeviceToHost, download) == cudaSuccess) {
    if (eq_diag_event_ != nullptr) {
      cudaEventRecord(eq_diag_event_, download);
    }
    eq_diag_count_      = n;
    eq_diag_queued_     = true;
    eq_diag_host_valid_ = false; // valid only after event/stream sync or force copy
  } else {
    static_cast<void>(cudaGetLastError());
    eq_diag_queued_     = false;
    eq_diag_count_      = 0;
    eq_diag_host_valid_ = false;
  }
}

span<const std::complex<float>> pusch_demodulator_gpu_impl::get_captured_symbols() const
{
  static_assert(sizeof(cf_t) == sizeof(std::complex<float>), "cf_t must be compatible with std::complex<float>.");
  // Preferred: stable host snapshot filled by wait_captured_symbols().
  if (!captured_eq_host_.empty()) {
    return span<const std::complex<float>>(captured_eq_host_.data(), captured_eq_host_.size());
  }
  // Host already validated (after wait_captured_symbols or sync fallback).
  if (eq_diag_host_valid_ && h_eq_diag_pinned_ && eq_diag_count_ != 0) {
    return span<const std::complex<float>>(h_eq_diag_pinned_, eq_diag_count_);
  }
  // Async path: only return when D2H has finished — never stall the main demod stream.
  if (eq_diag_queued_ && eq_diag_event_ && h_eq_diag_pinned_ && eq_diag_count_ != 0) {
    if (cudaEventQuery(eq_diag_event_) == cudaSuccess) {
      return span<const std::complex<float>>(h_eq_diag_pinned_, eq_diag_count_);
    }
    // Not ready yet (caller on slot-critical path) — empty is OK; pre-join will wait+retry.
    return {};
  }
  // CPU-equalization path already has host-side accumulated symbols (no D2H).
  if (accumulated_count_ != 0 && !accumulated_symbols_.empty()) {
    return span<const std::complex<float>>(reinterpret_cast<const std::complex<float>*>(accumulated_symbols_.data()),
                                           accumulated_count_);
  }
  return {};
}

void pusch_demodulator_gpu_impl::wait_captured_symbols()
{
  // Off slot-critical path (decoder pre-join). Prefer async D2H completion, then force a
  // blocking copy from the device EQ buffer for this slot (NeuralEq d_symbols_ or E2E tap).
  if (!capture_requested_slot_ && !eq_diag_queued_ && last_capture_src_ == nullptr && last_nof_eq_re_ == 0 &&
      last_e2e_eq_count_ == 0 && captured_eq_host_.empty()) {
    return;
  }

  // 1) Drain async D2H if it was queued.
  if (eq_diag_queued_ && eq_diag_event_ != nullptr) {
    if (cudaEventSynchronize(eq_diag_event_) == cudaSuccess && h_eq_diag_pinned_ != nullptr && eq_diag_count_ != 0) {
      captured_eq_host_.assign(h_eq_diag_pinned_, h_eq_diag_pinned_ + eq_diag_count_);
      eq_diag_host_valid_     = true;
      capture_requested_slot_ = false;
      return;
    }
    static_cast<void>(cudaGetLastError());
  }
  if (eq_diag_queued_ && d2h_stream_ != nullptr && h_eq_diag_pinned_ != nullptr && eq_diag_count_ != 0) {
    cudaStreamSynchronize(d2h_stream_);
    captured_eq_host_.assign(h_eq_diag_pinned_, h_eq_diag_pinned_ + eq_diag_count_);
    eq_diag_host_valid_     = true;
    capture_requested_slot_ = false;
    return;
  }

  // 2) Force blocking D2H from device EQ (always available after LDPC has waited on stream_).
  if (force_eq_diag_host_copy()) {
    capture_requested_slot_ = false;
    return;
  }

  // 3) CPU EQ path.
  if (accumulated_count_ != 0 && !accumulated_symbols_.empty()) {
    captured_eq_host_.resize(accumulated_count_);
    std::memcpy(
        captured_eq_host_.data(), accumulated_symbols_.data(), accumulated_count_ * sizeof(std::complex<float>));
    eq_diag_count_          = accumulated_count_;
    eq_diag_host_valid_     = true;
    capture_requested_slot_ = false;
    return;
  }

  capture_requested_slot_ = false;
  // Rate-limited so we can see empty captures without spamming every slot.
  ocudulog::fetch_basic_logger("PHY").debug(
      "constellation capture empty after wait: queued={} count={} last_nof_eq={} last_e2e={} dapp_inline={}",
      eq_diag_queued_,
      eq_diag_count_,
      last_nof_eq_re_,
      last_e2e_eq_count_,
      last_dapp_inline_);
}

pusch_demodulator_gpu_impl::~pusch_demodulator_gpu_impl()
{
  if (stream_ != nullptr) {
    cudaStreamSynchronizeYielding(stream_);
  }
  if (resident_dapp_) {
    // The hook slot can retain this adapter beyond the demodulator lifetime. Do not leave it
    // holding a completion token for an event owned and destroyed below.
    resident_dapp_->on_completion(nullptr);
  }
  resident_dapp_.reset();

  // Free device memory.
  if (d_symbols_)
    cudaFree(d_symbols_);
  if (d_noise_vars_)
    cudaFree(d_noise_vars_);
  for (int i = 0; i < NUM_BUFFERS; ++i) {
    if (d_llrs_half_[i])
      cudaFree(d_llrs_half_[i]);
    if (d_sch_llrs_half_[i])
      cudaFree(d_sch_llrs_half_[i]);
    if (d_sch_re_indices_[i])
      cudaFree(d_sch_re_indices_[i]);
    if (d_harq_ack_llrs_half_[i])
      cudaFree(d_harq_ack_llrs_half_[i]);
    if (d_harq_ack_re_indices_[i])
      cudaFree(d_harq_ack_re_indices_[i]);
    if (d_csi_part1_llrs_half_[i])
      cudaFree(d_csi_part1_llrs_half_[i]);
    if (d_csi_part1_re_indices_[i])
      cudaFree(d_csi_part1_re_indices_[i]);
    if (d_harq_ack_decode_result_[i])
      cudaFree(d_harq_ack_decode_result_[i]);
    if (d_csi_part1_decode_result_[i])
      cudaFree(d_csi_part1_decode_result_[i]);
    for (unsigned cb = 0; cb != MAX_GPU_UCI_CODEBLOCKS; ++cb) {
      polar_destroy(harq_ack_polar_handles_[i][cb]);
      polar_destroy(csi_part1_polar_handles_[i][cb]);
    }
  }
  if (d_full_grid_)
    cudaFree(d_full_grid_);
  if (d_full_estimates_)
    cudaFree(d_full_estimates_);

  for (int i = 0; i < NUM_BUFFERS; ++i) {
    if (d_unified_input_[i])
      cudaFree(d_unified_input_[i]);
    if (h_unified_staging_[i])
      cudaFreeHost(h_unified_staging_[i]);
    if (h_harq_ack_llrs_half_pinned_[i])
      cudaFreeHost(h_harq_ack_llrs_half_pinned_[i]);
    if (h_csi_part1_llrs_half_pinned_[i])
      cudaFreeHost(h_csi_part1_llrs_half_pinned_[i]);
    if (h_harq_ack_decode_result_pinned_[i])
      cudaFreeHost(h_harq_ack_decode_result_pinned_[i]);
    if (h_csi_part1_decode_result_pinned_[i])
      cudaFreeHost(h_csi_part1_decode_result_pinned_[i]);
    if (h2d_complete_[i])
      cudaEventDestroy(h2d_complete_[i]);
    if (d2h_complete_[i])
      cudaEventDestroy(d2h_complete_[i]);
    if (kernel_complete_[i])
      cudaEventDestroy(kernel_complete_[i]);
    if (gpu_e2e_timing_[i].start)
      cudaEventDestroy(gpu_e2e_timing_[i].start);
    if (gpu_e2e_timing_[i].h2d_end)
      cudaEventDestroy(gpu_e2e_timing_[i].h2d_end);
    if (gpu_e2e_timing_[i].kernel_end)
      cudaEventDestroy(gpu_e2e_timing_[i].kernel_end);
    if (gpu_e2e_timing_[i].final_end)
      cudaEventDestroy(gpu_e2e_timing_[i].final_end);
  }

  if (h_llrs_half_pinned_)
    cudaFreeHost(h_llrs_half_pinned_);
  if (h_eq_diag_pinned_) {
    cudaFreeHost(h_eq_diag_pinned_);
    h_eq_diag_pinned_ = nullptr;
  }
  if (d_eq_diag_) {
    cudaFree(d_eq_diag_);
    d_eq_diag_ = nullptr;
  }
  if (d_eq_noise_diag_) {
    cudaFree(d_eq_noise_diag_);
    d_eq_noise_diag_ = nullptr;
  }
  if (eq_diag_ready_event_) {
    cudaEventDestroy(eq_diag_ready_event_);
    eq_diag_ready_event_ = nullptr;
  }
  if (eq_diag_event_) {
    cudaEventDestroy(eq_diag_event_);
    eq_diag_event_ = nullptr;
  }
  if (d2h_stream_)
    cudaStreamDestroy(d2h_stream_);
  if (d2h_event_)
    cudaEventDestroy(d2h_event_);
  if (h2d_stream_)
    cudaStreamDestroy(h2d_stream_);
  if (completion_event_)
    cudaEventDestroy(completion_event_);
  if (scr_event_)
    cudaEventDestroy(scr_event_);
  if (scr_stream_)
    cudaStreamDestroy(scr_stream_);
  if (stream_)
    cudaStreamDestroy(stream_);

  // Free profiling events
  if (prof_h2d_start_)
    cudaEventDestroy(prof_h2d_start_);
  if (prof_h2d_end_)
    cudaEventDestroy(prof_h2d_end_);
  if (prof_kernel_start_)
    cudaEventDestroy(prof_kernel_start_);
  if (prof_kernel_end_)
    cudaEventDestroy(prof_kernel_end_);
  if (prof_d2h_start_)
    cudaEventDestroy(prof_d2h_start_);
  if (prof_d2h_end_)
    cudaEventDestroy(prof_d2h_end_);

  // Destroy CUDA handles.
  if (e2e_handle_)
    pusch_e2e_destroy(e2e_handle_);
  if (scr_handle_)
    scrambler_destroy(scr_handle_);
  if (mod_handle_)
    modulator_destroy(mod_handle_);
}

const re_buffer_reader<cbf16_t>&
pusch_demodulator_gpu_impl::get_ch_data_re(const resource_grid_reader&              grid,
                                           unsigned                                 i_symbol,
                                           const re_symbol_mask_type&               re_mask,
                                           const static_vector<uint8_t, MAX_PORTS>& rx_ports)
{
  unsigned nof_re = re_mask.count();
  int      begin  = re_mask.find_lowest();
  int      end    = re_mask.find_highest();
  ocudu_assert(begin <= end, "Invalid mask.");

  if (nof_re == static_cast<unsigned>(end + 1 - begin)) {
    ch_re_view.resize(rx_ports.size(), nof_re);

    for (unsigned i_port = 0, i_port_end = rx_ports.size(); i_port != i_port_end; ++i_port) {
      span<const cbf16_t> ch_data_re = grid.get_view(i_port, i_symbol);
      ch_re_view.set_slice(i_port, ch_data_re.subspan(begin, nof_re));
    }
    return ch_re_view;
  }

  ch_re_copy.resize(rx_ports.size(), nof_re);

  for (unsigned i_port = 0, i_port_end = rx_ports.size(); i_port != i_port_end; ++i_port) {
    span<cbf16_t> re_port_buffer = ch_re_copy.get_slice(i_port);

    re_port_buffer = grid.get(re_port_buffer, rx_ports[i_port], i_symbol, 0, re_mask);

    ocudu_assert(
        re_port_buffer.empty(), "Invalid number of RE read from the grid. {} RE are missing.", re_port_buffer.size());
  }

  return ch_re_copy;
}

const channel_equalizer::ch_est_list&
pusch_demodulator_gpu_impl::get_ch_data_estimates(const dmrs_pusch_estimator_results&      est_results,
                                                  unsigned                                 i_symbol,
                                                  unsigned                                 nof_tx_layers,
                                                  const re_symbol_mask_type&               re_mask,
                                                  std::optional<unsigned>                  dc_position,
                                                  const static_vector<uint8_t, MAX_PORTS>& rx_ports)
{
  unsigned nof_rx_ports = rx_ports.size();
  unsigned nof_re       = re_mask.count();
  int      begin        = re_mask.find_lowest();
  int      end          = re_mask.find_highest();
  ocudu_assert((begin >= 0) && (end >= 0), "Invalid mask.");

  ch_estimates_copy.resize(nof_re, nof_rx_ports, nof_tx_layers);

  for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
    // Extract noise variance for this port.
    noise_var_estimates[i_port] = est_results.get_noise_variance(i_port);

    for (unsigned i_layer = 0; i_layer != nof_tx_layers; ++i_layer) {
      span<cbf16_t> ch_port_buffer = ch_estimates_copy.get_channel(i_port, i_layer);

      est_results.get_symbol_ch_estimate(ch_port_buffer, i_symbol, i_port, i_layer, re_mask);

      if (dc_position.has_value() && re_mask.test(*dc_position)) {
        re_symbol_mask_type local_mask        = re_mask.slice(begin, *dc_position);
        unsigned            relative_position = local_mask.count();
        ch_port_buffer[relative_position]     = 0;
      }
    }
  }

  return ch_estimates_copy;
}

void pusch_demodulator_gpu_impl::demodulate(pusch_codeword_buffer&              codeword_buffer,
                                            pusch_demodulator_notifier&         notifier,
                                            const resource_grid_reader&         grid,
                                            const dmrs_pusch_estimator_results& est_results,
                                            const configuration&                config)
{
  // Admission precedes even host-vector resets: an earlier receiver or diagnostic stream may still own them.
  // The dependency loan normally prevents reentry. Fail closed here as well; never overwrite pending tensors
  // or carry a completed neural acceptance descriptor into a later baseline/CE/EQ grant.
  if (resident_dapp_ && !resident_dapp_->begin_grant()) {
    gpu_llrs_valid_         = false;
    host_codeword_written_  = false;
    inline_input_discarded_ = true;
    return;
  }

  unsigned nof_rx_ports = static_cast<unsigned>(config.rx_ports.size());

  // Reset GPU LLR state from previous demodulation.
  gpu_llrs_valid_                    = false;
  last_device_grid_used_             = false;
  last_owned_device_grid_used_       = false;
  last_dapp_inline_                  = false;
  last_dapp_post_eq_noise_valid_     = false;
  last_dapp_evm_valid_               = false;
  last_common_pilot_csi_             = false;
  last_nof_eq_re_                    = 0;
  last_capture_src_                  = nullptr;
  last_capture_n_                    = 0;
  last_e2e_eq_count_                  = 0;
  eq_diag_queued_                    = false;
  eq_diag_host_valid_                = false;
  eq_diag_count_                     = 0;
  captured_eq_host_.clear();
  gpu_uci_llrs_valid_                = false;
  gpu_uci_decoded_valid_             = false;
  gpu_harq_ack_decoded_              = false;
  gpu_csi_part1_decoded_             = false;
  gpu_uci_demux_pending_             = false;
  gpu_uci_device_decode_requested_   = false;
  host_codeword_written_             = false;
  inline_input_discarded_            = false;
  last_resident_llrs_                = nullptr;
  h_harq_ack_payload_size_           = 0;
  h_csi_part1_payload_size_          = 0;
  h_harq_ack_status_                 = uci_status::unknown;
  h_csi_part1_status_                = uci_status::unknown;
  h_harq_ack_decode_result_          = {};
  h_csi_part1_decode_result_         = {};
  h_harq_ack_decode_nof_codeblocks_  = 0;
  h_csi_part1_decode_nof_codeblocks_ = 0;
  h_harq_ack_expected_payload_bits_  = 0;
  h_csi_part1_expected_payload_bits_ = 0;
  h_harq_ack_llrs_.clear();
  h_csi_part1_llrs_.clear();

  // Clear accumulated buffers.
  accumulated_symbols_.clear();
  accumulated_noise_vars_.clear();
  accumulated_count_ = 0;

  // Clear raw data buffers for GPU equalization path.
  raw_ch_symbols_.clear();
  raw_ch_estimates_.clear();
  raw_accumulated_re_  = 0;
  cached_nof_rx_ports_ = nof_rx_ports;

  // Calculate the number of bits per RE.
  unsigned nof_bits_per_re = config.nof_tx_layers * get_bits_per_symbol(config.modulation);
  ocudu_assert(nof_bits_per_re > 0, "Invalid bits per RE.");

  bool supported_gpu_mimo = (config.nof_tx_layers == 1) || (config.nof_tx_layers == 2) || (config.nof_tx_layers == 3) ||
                            (config.nof_tx_layers == 4);
  bool can_use_gpu_eq = use_gpu_equalization_ && supported_gpu_mimo &&
                        (nof_rx_ports == 1 || nof_rx_ports == 2 || nof_rx_ports == 4 || nof_rx_ports == 8) &&
                        (config.nof_tx_layers <= nof_rx_ports); // Layers <= ports required

  // Stats accumulators.
  unsigned total_sinr_softbit_count   = 0;
  float    total_noise_var_accumulate = 0.0;
  unsigned total_evm_symbol_count     = 0;
  float    total_evm_accumulate       = 0.0F;

  if (config.nof_tx_layers != 1) {
    unsigned c_init = static_cast<unsigned>(config.rnti) * pow2(15) + config.n_id;
    descrambler->init(c_init);
  }

  // Get grid dimensions for GPU E2E path.
  unsigned nof_subcarriers = grid.get_nof_subc();

  // Pre-calculate exact RE count and resize vector for GPU E2E path.
  // When config matches previous call, reuse cached RE indices AND skip the expensive
  // kronecker_product mask computation (saves ~7-15μs total per slot).
  size_t gpu_write_idx     = 0;
  bool   re_indices_cached = false;
  // Declare RE masks — only computed when needed (cache miss or CPU path).
  re_symbol_mask_type re_mask;
  re_symbol_mask_type re_mask_dmrs;

  if (can_use_gpu_eq) {
    int dmrs_type_val = (config.dmrs_type == dmrs_config_type::type1) ? 1 : 2;
    if (re_indices_cache_.matches(config.rb_mask,
                                  config.dmrs_symb_pos.to_uint64(),
                                  config.start_symbol_index,
                                  config.nof_symbols,
                                  dmrs_type_val,
                                  config.nof_cdm_groups_without_data,
                                  nof_subcarriers)) {
      // Cache hit — reuse h_re_indices_ and RE count from previous call.
      // Skip kronecker_product mask computation (not needed when indices are cached).
      raw_accumulated_re_ = re_indices_cache_.cached_total_re;
      re_indices_cached   = true;
    } else {
      // Cache miss — compute RE masks and RE count.
      re_prb_mask active_re_per_prb      = ~re_prb_mask();
      re_prb_mask active_re_per_prb_dmrs = ~get_dmrs_prb_mask(config.dmrs_type, config.nof_cdm_groups_without_data);
      re_mask                            = config.rb_mask.kronecker_product<NOF_SUBCARRIERS_PER_RB>(active_re_per_prb);
      re_mask_dmrs          = config.rb_mask.kronecker_product<NOF_SUBCARRIERS_PER_RB>(active_re_per_prb_dmrs);
      size_t total_re_count = 0;
      for (unsigned i_symbol = config.start_symbol_index, i_symbol_end = config.start_symbol_index + config.nof_symbols;
           i_symbol != i_symbol_end;
           ++i_symbol) {
        const re_symbol_mask_type& symbol_re_mask = config.dmrs_symb_pos.test(i_symbol) ? re_mask_dmrs : re_mask;
        total_re_count += symbol_re_mask.count();
      }
      h_re_indices_.resize(total_re_count);
    }
  }

  // Process each OFDM symbol (skip entirely when GPU indices are cached).
  if (!re_indices_cached) {
    // Compute RE masks for the symbol loop (CPU path or GPU cache miss).
    // Moved here from top of function to skip on GPU cache hit (~2-5μs saved).
    if (re_mask.size() == 0) {
      re_prb_mask active_re_per_prb      = ~re_prb_mask();
      re_prb_mask active_re_per_prb_dmrs = ~get_dmrs_prb_mask(config.dmrs_type, config.nof_cdm_groups_without_data);
      re_mask                            = config.rb_mask.kronecker_product<NOF_SUBCARRIERS_PER_RB>(active_re_per_prb);
      re_mask_dmrs = config.rb_mask.kronecker_product<NOF_SUBCARRIERS_PER_RB>(active_re_per_prb_dmrs);
    }
    for (unsigned i_symbol = config.start_symbol_index, i_symbol_end = config.start_symbol_index + config.nof_symbols;
         i_symbol != i_symbol_end;
         ++i_symbol) {
      re_symbol_mask_type& symbol_re_mask = config.dmrs_symb_pos.test(i_symbol) ? re_mask_dmrs : re_mask;
      unsigned             nof_re_symbol  = symbol_re_mask.count();

      if (nof_re_symbol == 0) {
        continue;
      }

      if (can_use_gpu_eq) {
        // Build linear RE indices for GPU extraction.
        unsigned base_idx = i_symbol * nof_subcarriers;
        symbol_re_mask.for_each(0, symbol_re_mask.size(), [&](unsigned k) {
          h_re_indices_[gpu_write_idx++] = static_cast<int>(base_idx + k);
        });
        raw_accumulated_re_ += nof_re_symbol;
      } else {
        // Extract data symbols and channel estimates for CPU path.
        std::optional<unsigned> dc_position = config.enable_transform_precoding ? std::nullopt : config.dc_position;

        // Calculate local RE interval.
        interval<unsigned>      re_interval(config.rb_mask.find_lowest() * NOF_SUBCARRIERS_PER_RB,
                                       (config.rb_mask.find_highest() + 1) * NOF_SUBCARRIERS_PER_RB);
        re_symbol_mask_type     symbol_re_mask_local = symbol_re_mask.slice(re_interval.start(), re_interval.stop());
        std::optional<unsigned> dc_position_local    = std::nullopt;
        if (dc_position.has_value() && re_interval.contains(*dc_position)) {
          dc_position_local = *dc_position - re_interval.start();
        }

        const re_buffer_reader<cbf16_t>&      ch_re = get_ch_data_re(grid, i_symbol, symbol_re_mask, config.rx_ports);
        const channel_equalizer::ch_est_list& ch_estimates = get_ch_data_estimates(
            est_results, i_symbol, config.nof_tx_layers, symbol_re_mask_local, dc_position_local, config.rx_ports);

        // CPU equalization path.
        span<cf_t>  eq_re         = span<cf_t>(temp_eq_re).first(nof_re_symbol * config.nof_tx_layers);
        span<float> eq_noise_vars = span<float>(temp_eq_noise_vars).first(nof_re_symbol * config.nof_tx_layers);

        equalizer->equalize(
            eq_re, eq_noise_vars, ch_re, ch_estimates, span<float>(noise_var_estimates).first(nof_rx_ports), 1.0F);

        // Revert transform precoding if needed.
        if (config.enable_transform_precoding) {
          ocudu_assert(config.nof_tx_layers == 1, "Transform precoding requires 1 layer.");
          precoder->deprecode_ofdm_symbol(eq_re, eq_re);
          precoder->deprecode_ofdm_symbol_noise(eq_noise_vars, eq_noise_vars);
        }

        if (config.nof_tx_layers == 1) {
          // Accumulate for batch GPU processing.
          accumulated_symbols_.insert(accumulated_symbols_.end(), eq_re.begin(), eq_re.end());
          accumulated_noise_vars_.insert(accumulated_noise_vars_.end(), eq_noise_vars.begin(), eq_noise_vars.end());
          accumulated_count_ += nof_re_symbol * config.nof_tx_layers;

          // Accumulate SINR stats.
          if (compute_post_eq_sinr) {
            for (float var : eq_noise_vars) {
              if (!std::isinf(var)) {
                total_noise_var_accumulate += var;
                ++total_sinr_softbit_count;
              }
            }
          }
        } else {
          unsigned count_re_symbol = 0;
          while (count_re_symbol != nof_re_symbol) {
            unsigned remain_nof_subc = nof_re_symbol - count_re_symbol;

            span<log_likelihood_ratio> codeword =
                codeword_buffer.get_next_block_view(remain_nof_subc * nof_bits_per_re);

            ocudu_assert(codeword.size() % nof_bits_per_re == 0,
                         "The codeword block size (i.e., {}) must be multiple of the number of bits per RE (i.e., {}).",
                         codeword.size(),
                         nof_bits_per_re);

            unsigned          nof_block_softbits    = codeword.size();
            unsigned          codeword_block_offset = count_re_symbol * config.nof_tx_layers;
            unsigned          codeword_block_size   = nof_block_softbits / get_bits_per_symbol(config.modulation);
            span<const cf_t>  eq_re_block           = eq_re.subspan(codeword_block_offset, codeword_block_size);
            span<const float> eq_noise_vars_block   = eq_noise_vars.subspan(codeword_block_offset, codeword_block_size);

            demapper_fallback->demodulate_soft(codeword, eq_re_block, eq_noise_vars_block, config.modulation);

            unsigned symbol_evm_symbol_count = 0;
            float    symbol_evm_accumulate   = 0.0F;
            if (evm_calc) {
              symbol_evm_symbol_count = codeword_block_size;
              symbol_evm_accumulate   = static_cast<float>(codeword_block_size) *
                                      evm_calc->calculate(codeword, eq_re_block, config.modulation);
            }

            static_bit_buffer<pusch_constants::MAX_NOF_BITS_PER_OFDM_SYMBOL> scrambling_seq(nof_block_softbits);
            descrambler->generate(scrambling_seq);
            for (unsigned i = 0; i != nof_block_softbits; ++i) {
              if (scrambling_seq.extract(i, 1) == 1) {
                codeword[i] = -codeword[i];
              }
            }

            count_re_symbol += nof_block_softbits / nof_bits_per_re;

            if (compute_post_eq_sinr) {
              for (float var : eq_noise_vars_block) {
                if (!std::isinf(var)) {
                  total_noise_var_accumulate += var;
                  ++total_sinr_softbit_count;
                }
              }
            }

            if (count_re_symbol == nof_re_symbol) {
              pusch_demodulator_notifier::demodulation_stats stats;
              if (compute_post_eq_sinr && (total_sinr_softbit_count != 0) && (total_noise_var_accumulate > 0.0)) {
                stats.sinr_dB.emplace(
                    -convert_power_to_dB(total_noise_var_accumulate / static_cast<float>(total_sinr_softbit_count)));
              } else {
                stats.sinr_dB.emplace(std::numeric_limits<float>::infinity());
              }
              if (symbol_evm_symbol_count != 0) {
                stats.evm.emplace(symbol_evm_accumulate / static_cast<float>(symbol_evm_symbol_count));
              }
              notifier.on_provisional_stats(i_symbol, stats);

              total_evm_symbol_count += symbol_evm_symbol_count;
              total_evm_accumulate += symbol_evm_accumulate;
            }

            codeword_buffer.on_new_block(codeword, scrambling_seq);
          }
        }
      }
    }
  } // end if (!re_indices_cached)

  // Update RE indices cache after building new indices (on cache miss).
  if (can_use_gpu_eq && !re_indices_cached) {
    int dmrs_type_val = (config.dmrs_type == dmrs_config_type::type1) ? 1 : 2;
    re_indices_cache_.update(config.rb_mask,
                             config.dmrs_symb_pos.to_uint64(),
                             config.start_symbol_index,
                             config.nof_symbols,
                             dmrs_type_val,
                             config.nof_cdm_groups_without_data,
                             nof_subcarriers,
                             raw_accumulated_re_);
  }

  // Now process all accumulated data.
  if (can_use_gpu_eq && raw_accumulated_re_ > 0) {
    // End-to-end GPU path with GPU equalization.
    if (enable_path_tracing_) {
      ocudulog::fetch_basic_logger("PHY").warning(
          "PUSCH GPU: Taking GPU E2E path. accumulated_re={} can_use_gpu_eq={} layers={} ports={}",
          raw_accumulated_re_,
          can_use_gpu_eq,
          config.nof_tx_layers,
          nof_rx_ports);
    }
    std::chrono::steady_clock::time_point gpu_e2e_start;
    if (enable_path_tracing_) {
      gpu_e2e_start = std::chrono::steady_clock::now();
    }
    process_gpu_e2e(codeword_buffer, notifier, grid, est_results, config);
    if (enable_path_tracing_) {
      auto gpu_e2e_end = std::chrono::steady_clock::now();
      auto gpu_e2e_us  = std::chrono::duration_cast<std::chrono::microseconds>(gpu_e2e_end - gpu_e2e_start);
      ocudulog::fetch_basic_logger("PHY").info("PUSCH GPU: E2E path time: {} us", gpu_e2e_us.count());
    }
  } else if (!can_use_gpu_eq && (config.nof_tx_layers == 1) && accumulated_count_ >= MIN_GPU_SYMBOLS &&
             gpu_available_) {
    // GPU soft demod with CPU equalization.
    if (enable_path_tracing_) {
      ocudulog::fetch_basic_logger("PHY").warning(
          "PUSCH GPU: Taking GPU BATCH path. accumulated_count={} min_threshold={} "
          "use_gpu_eq={} layers={} ports={}",
          accumulated_count_,
          MIN_GPU_SYMBOLS,
          use_gpu_equalization_,
          config.nof_tx_layers,
          nof_rx_ports);
    }
    std::chrono::steady_clock::time_point gpu_batch_start;
    if (enable_path_tracing_) {
      gpu_batch_start = std::chrono::steady_clock::now();
    }
    process_gpu_batch(codeword_buffer, notifier, config);
    if (enable_path_tracing_) {
      auto gpu_batch_end = std::chrono::steady_clock::now();
      auto gpu_batch_us  = std::chrono::duration_cast<std::chrono::microseconds>(gpu_batch_end - gpu_batch_start);
      ocudulog::fetch_basic_logger("PHY").info("PUSCH GPU: BATCH path time: {} us", gpu_batch_us.count());
    }
  } else if (!can_use_gpu_eq && accumulated_count_ > 0) {
    // Full CPU fallback.
    if (enable_path_tracing_) {
      ocudulog::fetch_basic_logger("PHY").warning(
          "PUSCH GPU: Taking CPU FALLBACK path. gpu_available={} accumulated_count={} "
          "min_threshold={} can_use_gpu_eq={} layers={} ports={}",
          gpu_available_,
          accumulated_count_,
          MIN_GPU_SYMBOLS,
          can_use_gpu_eq,
          config.nof_tx_layers,
          nof_rx_ports);
    }
    std::chrono::steady_clock::time_point cpu_fallback_start;
    if (enable_path_tracing_) {
      cpu_fallback_start = std::chrono::steady_clock::now();
    }
    process_cpu_fallback(codeword_buffer, notifier, config);
    if (enable_path_tracing_) {
      auto cpu_fallback_end = std::chrono::steady_clock::now();
      auto cpu_fallback_us =
          std::chrono::duration_cast<std::chrono::microseconds>(cpu_fallback_end - cpu_fallback_start);
      ocudulog::fetch_basic_logger("PHY").info("PUSCH GPU: CPU FALLBACK path time: {} us", cpu_fallback_us.count());
    }
  } else if (enable_path_tracing_ && accumulated_count_ == 0 && raw_accumulated_re_ == 0) {
    ocudulog::fetch_basic_logger("PHY").warning(
        "PUSCH GPU: No data to process. accumulated_count={} raw_accumulated_re={} can_use_gpu_eq={}",
        accumulated_count_,
        raw_accumulated_re_,
        can_use_gpu_eq);
  }

  if (inline_input_discarded_) {
    // No host softbits/end marker or synthetic "clean" stats may escape a failed inline submission.
    return;
  }

  // Report final stats (only for CPU equalization paths).
  if (!(can_use_gpu_eq && raw_accumulated_re_ > 0)) {
    pusch_demodulator_notifier::demodulation_stats stats;
    if ((total_sinr_softbit_count != 0) && (total_noise_var_accumulate > 0.0)) {
      float mean_noise_var = total_noise_var_accumulate / static_cast<float>(total_sinr_softbit_count);
      float sinr_db        = -convert_power_to_dB(mean_noise_var);
      stats.sinr_dB.emplace(sinr_db);
    } else {
      stats.sinr_dB.emplace(MAX_SINR_DB);
    }
    if (total_evm_symbol_count != 0) {
      stats.evm.emplace(total_evm_accumulate / static_cast<float>(total_evm_symbol_count));
    } else {
      stats.evm.emplace(0.0f);
    }
    sanitize_stats_in_place(stats, static_cast<uint16_t>(config.rnti));
    notifier.on_end_stats(stats);
  }

  // In full GPU-resident mode, SCH does not flow through the host codeword buffer. In hybrid UCI mode, the host demux
  // still needs an end marker for HARQ/CSI while SCH is decoded from the compact device buffer.
  if (!gpu_resident_mode_ || resident_host_uci_demux_enabled(config) || host_codeword_written_) {
    codeword_buffer.on_end_codeword();
  }
}

void pusch_demodulator_gpu_impl::process_gpu_batch(pusch_codeword_buffer&      codeword_buffer,
                                                   pusch_demodulator_notifier& notifier,
                                                   const configuration&        config)
{
  size_t num_symbols = accumulated_count_;
  int    mod_order   = get_bits_per_symbol(config.modulation);
  size_t num_llrs    = num_symbols * mod_order;

  // Buffers are pre-allocated at MAX capacity in constructor - no reallocation needed.
  // Validate that workload fits within pre-allocated capacity.
  size_t symbols_bytes   = num_symbols * sizeof(cuFloatComplex);
  size_t noise_bytes     = num_symbols * sizeof(float);
  size_t llrs_half_bytes = num_llrs * sizeof(uint16_t);

  if (symbols_bytes > d_symbols_capacity_ || noise_bytes > d_noise_vars_capacity_ ||
      llrs_half_bytes > d_llrs_half_capacity_[0]) {
    ocudulog::fetch_basic_logger("PHY").error(
        "PUSCH GPU: Workload exceeds pre-allocated capacity (symbols={}/{} bytes, noise={}/{} bytes, llrs={}/{} bytes)",
        symbols_bytes,
        d_symbols_capacity_,
        noise_bytes,
        d_noise_vars_capacity_,
        llrs_half_bytes,
        d_llrs_half_capacity_[0]);
    process_cpu_fallback(codeword_buffer, notifier, config);
    return;
  }

  // Copy symbols to GPU.
  cudaMemcpyAsync(d_symbols_, accumulated_symbols_.data(), symbols_bytes, cudaMemcpyHostToDevice, stream_);

  // Copy noise variances to GPU.
  cudaMemcpyAsync(d_noise_vars_, accumulated_noise_vars_.data(), noise_bytes, cudaMemcpyHostToDevice, stream_);

  // Configure and generate scrambling sequence.
  nr_scrambling_config_t scr_cfg = {};
  scr_cfg.n_RNTI                 = static_cast<uint16_t>(config.rnti);
  scr_cfg.n_ID                   = config.n_id;
  scr_cfg.q                      = 0;
  scr_cfg.n_s                    = static_cast<uint8_t>(config.slot.slot_index());

  nr_ldpc_status_t status = scrambler_configure(scr_handle_, &scr_cfg);
  if (status != NR_LDPC_SUCCESS) {
    cudaStreamSynchronizeYielding(stream_);
    process_cpu_fallback(codeword_buffer, notifier, config);
    return;
  }

  status = scrambler_generate_sequence(scr_handle_, static_cast<int>(num_llrs), stream_);
  if (status != NR_LDPC_SUCCESS) {
    cudaStreamSynchronizeYielding(stream_);
    process_cpu_fallback(codeword_buffer, notifier, config);
    return;
  }

  const uint32_t* d_scramble_seq = scrambler_get_sequence_ptr(scr_handle_);

  // Fused GPU soft demodulation + descrambling with FP16 output.
  // FP16 path uses better-optimized downstream rate dematching and LDPC decode kernels.
  // Batch path uses buffer 0 (no triple buffering for non-E2E).
  int result = modulator_soft_demod_descramble_half(mod_handle_,
                                                    static_cast<cuFloatComplex*>(d_symbols_),
                                                    static_cast<float*>(d_noise_vars_),
                                                    d_scramble_seq,
                                                    d_llrs_half_[0],
                                                    static_cast<int>(num_symbols),
                                                    mod_order,
                                                    stream_);

  if (result != 0) {
    cudaStreamSynchronizeYielding(stream_);
    process_cpu_fallback(codeword_buffer, notifier, config);
    return;
  }

  last_num_llrs_  = num_llrs;
  last_n_id_      = config.n_id;
  last_rnti_      = static_cast<uint16_t>(config.rnti);
  gpu_llrs_valid_ = true;

  // Report SINR and EVM before early return in GPU-resident mode.
  // For batch path, compute SINR from accumulated noise variance.
  {
    pusch_demodulator_notifier::demodulation_stats stats;
    if (num_symbols > 0 && !accumulated_noise_vars_.empty()) {
      float total_noise_var = 0.0f;
      for (float nv : accumulated_noise_vars_) {
        total_noise_var += nv;
      }
      float mean_noise_var = total_noise_var / static_cast<float>(accumulated_noise_vars_.size());
      if (mean_noise_var > 0.0f) {
        float sinr_db = -convert_power_to_dB(mean_noise_var);
        stats.sinr_dB.emplace(sinr_db);
        // Approximate EVM from SINR: EVM_rms = 10^(-SINR_dB/20) for normalized constellations.
        stats.evm.emplace(std::pow(10.0f, -sinr_db / 20.0f));
      } else {
        stats.sinr_dB.emplace(MAX_SINR_DB);
        stats.evm.emplace(0.0f);
      }
    } else {
      stats.sinr_dB.emplace(MAX_SINR_DB);
      stats.evm.emplace(0.0f);
    }
    sanitize_stats_in_place(stats, static_cast<uint16_t>(config.rnti));
    notifier.on_end_stats(stats);
  }

  if (gpu_resident_mode_) {
    return;
  }

  // Copy FP16 LLRs to host (batch path uses buffer 0).
  uint16_t*             h_llrs_half = h_llrs_half_pinned_;
  std::vector<uint16_t> h_llrs_half_fallback;
  if (!h_llrs_half || num_llrs > h_llrs_half_pinned_cap_) {
    h_llrs_half_fallback.resize(num_llrs);
    h_llrs_half = h_llrs_half_fallback.data();
  }
  cudaMemcpyAsync(h_llrs_half, d_llrs_half_[0], num_llrs * sizeof(uint16_t), cudaMemcpyDeviceToHost, stream_);
  cudaStreamSynchronizeYielding(stream_);

  // Convert FP16 LLRs to INT8 and write to codeword buffer.
  constexpr float FP16_TO_INT8_SCALE = 4.0f;
  size_t          offset             = 0;
  while (offset < num_llrs) {
    size_t remain = std::min(num_llrs - offset, size_t(pusch_constants::MAX_NOF_BITS_PER_OFDM_SYMBOL));
    span<log_likelihood_ratio> block      = codeword_buffer.get_next_block_view(remain);
    size_t                     block_size = block.size();
    if (block_size == 0) {
      ocudulog::fetch_basic_logger("PHY").error(
          "PUSCH GPU host LLR conversion made no progress (offset={} num_llrs={}).", offset, num_llrs);
      break;
    }

    // Convert FP16 to INT8 with scaling and clamping.
    for (size_t i = 0; i < block_size; ++i) {
      uint16_t bits = h_llrs_half[offset + i];
      float    fp16_val;
      uint32_t sign = (bits >> 15) & 0x1;
      uint32_t exp  = (bits >> 10) & 0x1f;
      uint32_t mant = bits & 0x3ff;
      if (exp == 0) {
        fp16_val = (sign ? -1.0f : 1.0f) * (mant / 1024.0f) * (1.0f / 16384.0f);
      } else if (exp == 31) {
        fp16_val = sign ? -static_cast<float>(HOST_LLR_MAX) : static_cast<float>(HOST_LLR_MAX);
      } else {
        fp16_val = (sign ? -1.0f : 1.0f) * (1.0f + mant / 1024.0f) * std::pow(2.0f, static_cast<float>(exp) - 15.0f);
      }
      float scaled  = fp16_val * FP16_TO_INT8_SCALE;
      int   clamped = static_cast<int>(std::round(scaled));
      clamped       = clamp_host_llr(clamped);
      block[i]      = log_likelihood_ratio(static_cast<int8_t>(clamped));
    }

    static_bit_buffer<pusch_constants::MAX_NOF_BITS_PER_OFDM_SYMBOL> dummy_seq(block_size);
    codeword_buffer.on_new_block(block, dummy_seq);
    offset += block_size;
  }
  host_codeword_written_ = true;
}

void pusch_demodulator_gpu_impl::process_gpu_e2e(pusch_codeword_buffer&              codeword_buffer,
                                                 pusch_demodulator_notifier&         notifier,
                                                 const resource_grid_reader&         grid,
                                                 const dmrs_pusch_estimator_results& est_results,
                                                 const configuration&                config)
{
  if (enable_path_tracing_) {
    ocudulog::fetch_basic_logger("PHY").info("[PUSCH GPU E2E] Entering process_gpu_e2e");
  }
  // Host-side demod sub-phases (steady_clock). Complements the aggregate "demod=" wall time.
  const auto t_demod0 = std::chrono::steady_clock::now();
  auto       us_since = [](std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - t0).count();
  };
  float us_prep  = 0.0f;
  float us_cfg   = 0.0f;
  float us_h2d   = 0.0f;
  float us_eq    = 0.0f;
  float us_post  = 0.0f;
  int   cfg_kind = 0; // 0=hit, 1=full configure, 2=data-params only

  const auto resident_compaction = get_resident_sch_compaction_config(config);
  size_t     nof_re              = raw_accumulated_re_;
  unsigned   nof_rx_ports        = cached_nof_rx_ports_;
  unsigned   nof_tx_layers       = config.nof_tx_layers;
  int        mod_order           = get_bits_per_symbol(config.modulation);
  size_t     num_llrs            = nof_re * nof_tx_layers * mod_order; // MIMO: layers * mod_order LLRs per RE

  // Grid dimensions.
  unsigned nof_subcarriers = grid.get_nof_subc();
  unsigned nof_symbols     = grid.get_nof_symbols();
  size_t   grid_stride     = static_cast<size_t>(nof_symbols) * nof_subcarriers;
  size_t   full_grid_size  = grid_stride * nof_rx_ports;

  // Clear any stale CUDA errors from previous async operations.
  cudaGetLastError();

  // Calculate sizes for unified buffer.
  // Note: GPU E2E path computes channel estimates internally, so we don't need to upload them.
  // This reduces H2D transfer size by ~50%.
  size_t cbf16_bytes   = full_grid_size * sizeof(uint32_t);
  size_t indices_bytes = h_re_indices_.size() * sizeof(int);
  int    dmrs_type_int = (config.dmrs_type == dmrs_config_type::type1) ? 1 : 2;

  // Compute offsets with 256-byte alignment.
  // Layout: [grid_cbf16 | RE_indices] (channel and noise estimates are computed on GPU)
  constexpr size_t ALIGN     = 256;
  unified_grid_offset_       = 0;
  unified_indices_offset_    = ((cbf16_bytes + ALIGN - 1) / ALIGN) * ALIGN;
  size_t unified_total_bytes = unified_indices_offset_ + ((indices_bytes + ALIGN - 1) / ALIGN) * ALIGN;

  // TRIPLE-BUFFERING: Rotate through buffers 0→1→2→0 to allow:
  // - CPU prep of next slot's H2D (buffer N+1)
  // - GPU processing current slot (buffer N)
  // - D2H transfer of previous slot (buffer N-1)
  // All three operations can overlap for maximum throughput.
  int buf_idx                = current_buf_;
  current_buf_               = (current_buf_ + 1) % NUM_BUFFERS; // Rotate to next buffer
  cudaStream_t upload_stream = h2d_stream_ ? h2d_stream_ : stream_;

  if (gpu_e2e_timing_enabled_) {
    auto& timing = gpu_e2e_timing_[buf_idx];
    if (timing.pending) {
      cudaError_t query_status = cudaEventQuery(timing.final_end);
      if (query_status == cudaSuccess) {
        float h2d_ms    = 0.0f;
        float kernel_ms = 0.0f;
        float tail_ms   = 0.0f;
        float total_ms  = 0.0f;
        if (cudaEventElapsedTime(&h2d_ms, timing.start, timing.h2d_end) == cudaSuccess &&
            cudaEventElapsedTime(&kernel_ms, timing.h2d_end, timing.kernel_end) == cudaSuccess &&
            cudaEventElapsedTime(&tail_ms, timing.kernel_end, timing.final_end) == cudaSuccess &&
            cudaEventElapsedTime(&total_ms, timing.start, timing.final_end) == cudaSuccess) {
          const float total_us = total_ms * 1000.0f;
          if (total_us >= static_cast<float>(gpu_e2e_timing_warn_us_)) {
            ocudulog::fetch_basic_logger("PHY").warning(
                "PUSCH GPU E2E TIMING: rnti=0x{:04x} total_us={:.0f} h2d_us={:.0f} gpu_us={:.0f} tail_d2h_us={:.0f} "
                "prb={} re={} mod={} h2d_bytes={} direct_grid={} re_idx_cached={} resident={} compact_sch={} "
                "configured={} llr_d2h={}",
                timing.rnti,
                total_us,
                h2d_ms * 1000.0f,
                kernel_ms * 1000.0f,
                tail_ms * 1000.0f,
                timing.nof_prb,
                timing.nof_re,
                timing.mod_order,
                timing.h2d_bytes,
                timing.direct_grid,
                timing.re_indices_cached,
                timing.resident,
                timing.compact_sch,
                timing.configured,
                timing.includes_llr_d2h);
          }
        }
      } else if (query_status != cudaErrorNotReady) {
        (void)cudaGetLastError();
      }
      timing.pending = false;
    }
  }

  // PHASE 2.1: Replace blocking sync with event-based wait for THIS buffer only.
  // Only wait if this buffer was used before (d2h_complete_ will be recorded).
  // If D2H completed, then H2D and kernel must have completed too (dependency chain).
  // This allows slot N+1's H2D to overlap with slot N's kernel/D2H.
  if (d2h_complete_[buf_idx]) {
    cudaStreamWaitEvent(stream_, d2h_complete_[buf_idx], 0);
    if (upload_stream != stream_) {
      // The upload stream writes into the same triple-buffer slot. Order it after the previous
      // user of this slot too, otherwise H2D can race ahead of the main stream wait on discrete GPUs.
      cudaStreamWaitEvent(upload_stream, d2h_complete_[buf_idx], 0);
    }
  }

  // Verify buffers are large enough (they were pre-allocated in constructor).
  if (unified_total_bytes > unified_buf_cap_ || !d_unified_input_[0] || !h_unified_staging_[0] ||
      !d_unified_input_[1] || !h_unified_staging_[1] || !d_unified_input_[2] || !h_unified_staging_[2]) {
    ocudulog::fetch_basic_logger("PHY").warning(
        "process_gpu_e2e: BUFFER CHECK FAILED - total={} cap={}", unified_total_bytes, unified_buf_cap_);
    process_cpu_grid_fallback(codeword_buffer, notifier, grid, est_results, config);
    return;
  }

  // Wait for any previous H2D transfer on this buffer to complete.
  if (h2d_complete_[buf_idx] && h2d_stream_) {
    cudaStreamWaitEvent(stream_, h2d_complete_[buf_idx], 0);
  }

  // Compute device pointers from current unified buffer.
  d_grid_cbf16_ = static_cast<char*>(d_unified_input_[buf_idx]) + unified_grid_offset_;
  d_re_indices_ = reinterpret_cast<int*>(static_cast<char*>(d_unified_input_[buf_idx]) + unified_indices_offset_);

  bool rx_ports_are_compact = grid.get_nof_ports() == nof_rx_ports;
  for (unsigned port = 0; port != nof_rx_ports && rx_ports_are_compact; ++port) {
    rx_ports_are_compact = config.rx_ports[port] == port;
  }
  struct device_grid_hold {
    enum class kind { none, live_grid, owned_snapshot };
    const resource_grid_reader* g{nullptr};
    void*                       stream{nullptr};
    kind                        armed_kind{kind::none};
    void                        arm_live(const resource_grid_reader& grid, void* s)
    {
      g          = &grid;
      stream     = s;
      armed_kind = kind::live_grid;
    }
    void arm_owned(const resource_grid_reader& grid, void* s)
    {
      g          = &grid;
      stream     = s;
      armed_kind = kind::owned_snapshot;
    }
    void commit()
    {
      if (armed_kind != kind::none && g != nullptr) {
        const bool recorded = armed_kind == kind::owned_snapshot ? g->on_owned_device_snapshot_reading_enqueued(stream)
                                                                 : g->on_device_grid_reading_enqueued(stream);
        if (!recorded) {
          // No fence event could be recorded, so the enqueued grid read is
          // invisible to pull_pages_home; drain it before a host access can
          // migrate the pages out from under it (Xid 31 on GB10).
          cudaStreamSynchronizeYielding(static_cast<cudaStream_t>(stream));
        }
        armed_kind = kind::none;
      }
    }
    void drop()
    {
      if (armed_kind != kind::none && g != nullptr) {
        if (armed_kind == kind::owned_snapshot) {
          g->release_owned_device_snapshot_reading();
        } else {
          g->release_device_grid_reading();
        }
        armed_kind = kind::none;
      }
    }
    ~device_grid_hold() { drop(); }
  } grid_hold;

  bool        use_device_grid_reader       = false;
  bool        use_owned_device_grid_reader = false;
  const void* owned_grid                   = grid.get_owned_device_snapshot_cbf16();
  if (rx_ports_are_compact && owned_grid != nullptr && grid.owned_device_snapshot_is_pusch_compatible() &&
      grid.prepare_owned_device_snapshot_reading(stream_)) {
    d_grid_cbf16_                = const_cast<void*>(owned_grid);
    use_device_grid_reader       = true;
    use_owned_device_grid_reader = true;
    grid_hold.arm_owned(grid, stream_);
  } else if (rx_ports_are_compact && grid.supports_device_grid_reading() && (grid.get_device_grid_cbf16() != nullptr)) {
    use_device_grid_reader = grid.prepare_device_grid_reading(stream_);
    if (use_device_grid_reader) {
      d_grid_cbf16_ = const_cast<void*>(grid.get_device_grid_cbf16());
      grid_hold.arm_live(grid, stream_);
    }
  }
  if (use_owned_device_grid_reader) {
    auto& path_logger = ocudulog::fetch_basic_logger("PHY");
    if (!logged_pusch_owned_grid_path.exchange(true)) {
      if (pusch_grid_path_diagnostics_enabled()) {
        path_logger.warning("PUSCH GPU grid path selected: owned CUDA snapshot (live managed grid untouched).");
      } else {
        path_logger.info("PUSCH GPU grid path selected: owned CUDA snapshot (live managed grid untouched).");
      }
    }
  } else if (use_device_grid_reader) {
    auto& path_logger = ocudulog::fetch_basic_logger("PHY");
    if (!logged_pusch_direct_grid_path.exchange(true)) {
      path_logger.info("PUSCH GPU grid path selected: direct CUDA-visible resource-grid reader.");
    }
  } else if (grid.supports_device_grid_reading() && !rx_ports_are_compact) {
    auto& path_logger = ocudulog::fetch_basic_logger("PHY");
    if (!logged_pusch_noncompact_grid_path.exchange(true)) {
      path_logger.warning("PUSCH GPU grid path selected: host grid copy; CUDA-visible reader rejected due to "
                          "noncompact RX ports.");
    }
  } else if (grid.supports_device_grid_reading()) {
    auto& path_logger = ocudulog::fetch_basic_logger("PHY");
    if (!logged_pusch_prepare_failed_grid_path.exchange(true)) {
      path_logger.warning("PUSCH GPU grid path selected: host grid copy; CUDA-visible reader prepare failed.");
    }
  } else {
    auto& path_logger = ocudulog::fetch_basic_logger("PHY");
    if (!logged_pusch_host_grid_path.exchange(true)) {
      path_logger.warning("PUSCH GPU grid path selected: host grid copy.");
    }
  }
  last_device_grid_used_       = use_device_grid_reader;
  last_owned_device_grid_used_ = use_owned_device_grid_reader;

  // Validate FP16 LLR buffer capacity (pre-allocated at MAX in constructor).
  size_t llrs_half_bytes = num_llrs * sizeof(uint16_t);
  if (llrs_half_bytes > d_llrs_half_capacity_[0]) {
    ocudulog::fetch_basic_logger("PHY").error(
        "PUSCH GPU E2E: LLR workload exceeds pre-allocated capacity ({}/{} bytes)",
        llrs_half_bytes,
        d_llrs_half_capacity_[0]);
    grid_hold.drop();
    process_cpu_grid_fallback(codeword_buffer, notifier, grid, est_results, config);
    return;
  }

  bool compact_sch_llrs           = gpu_resident_mode_ && resident_compaction.enabled;
  bool compact_uci_on_device      = compact_sch_llrs && resident_device_uci_demux_enabled(config);
  bool decode_short_uci_on_device = false;
  if (compact_sch_llrs) {
    unsigned mod_order_compact = static_cast<unsigned>(mod_order);
    bool     compaction_cache_hit =
        compaction_re_indices_cache_.matches(true,
                                             static_cast<unsigned>(config.rb_mask.count()),
                                             static_cast<unsigned>(config.rb_mask.find_lowest()),
                                             static_cast<unsigned>(config.rb_mask.find_highest()),
                                             config.dmrs_symb_pos.to_uint64(),
                                             config.start_symbol_index,
                                             config.nof_symbols,
                                             dmrs_type_int,
                                             config.nof_cdm_groups_without_data,
                                             mod_order_compact,
                                             config.nof_tx_layers,
                                             resident_compaction.nof_ul_sch_bits,
                                             resident_compaction.nof_harq_ack_rvd,
                                             resident_compaction.nof_enc_harq_ack_bits,
                                             resident_compaction.nof_harq_ack_bits,
                                             resident_compaction.nof_enc_csi_part1_bits,
                                             resident_compaction.nof_csi_part1_bits,
                                             resident_compaction.nof_enc_csi_part2_bits);
    if (!compaction_cache_hit) {
      build_resident_compaction_re_indices(
          h_sch_re_indices_, h_harq_ack_re_indices_, h_csi_part1_re_indices_, config, static_cast<unsigned>(nof_re));
      compaction_re_indices_cache_.update(true,
                                          static_cast<unsigned>(config.rb_mask.count()),
                                          static_cast<unsigned>(config.rb_mask.find_lowest()),
                                          static_cast<unsigned>(config.rb_mask.find_highest()),
                                          config.dmrs_symb_pos.to_uint64(),
                                          config.start_symbol_index,
                                          config.nof_symbols,
                                          dmrs_type_int,
                                          config.nof_cdm_groups_without_data,
                                          mod_order_compact,
                                          config.nof_tx_layers,
                                          resident_compaction.nof_ul_sch_bits,
                                          resident_compaction.nof_harq_ack_rvd,
                                          resident_compaction.nof_enc_harq_ack_bits,
                                          resident_compaction.nof_harq_ack_bits,
                                          resident_compaction.nof_enc_csi_part1_bits,
                                          resident_compaction.nof_csi_part1_bits,
                                          resident_compaction.nof_enc_csi_part2_bits);
    }
    decode_short_uci_on_device =
        compact_uci_on_device && phy_acceleration_env_flag_enabled("OCUDU_PUSCH_ACCELERATION_UCI_DEVICE_DECODE");
    bool decode_polar_uci_on_device =
        compact_uci_on_device && gpu_uci_polar_available_ &&
        ((!h_harq_ack_re_indices_.empty() && (resident_compaction.nof_harq_ack_bits > 11)) ||
         (!h_csi_part1_re_indices_.empty() && (resident_compaction.nof_csi_part1_bits > 11)));
    gpu_uci_device_decode_requested_ = decode_short_uci_on_device || decode_polar_uci_on_device;
    size_t sch_llrs_half_bytes       = resident_compaction.nof_ul_sch_bits * sizeof(uint16_t);
    // SCH buffers are pre-allocated at MAX in the constructor — never free/malloc here.
    if (h_sch_re_indices_.empty() || sch_llrs_half_bytes > d_sch_llrs_half_capacity_[buf_idx] ||
        h_sch_re_indices_.size() > d_sch_re_indices_capacity_[buf_idx]) {
      ocudulog::fetch_basic_logger("PHY").error(
          "PUSCH GPU E2E: SCH compaction workload exceeds pre-allocated capacity (sch_llrs={} cap={} sch_re={} cap={})",
          sch_llrs_half_bytes,
          d_sch_llrs_half_capacity_[buf_idx],
          h_sch_re_indices_.size(),
          d_sch_re_indices_capacity_[buf_idx]);
      grid_hold.drop();
      process_cpu_grid_fallback(codeword_buffer, notifier, grid, est_results, config);
      return;
    }

    if (compact_uci_on_device) {
      unsigned nof_bits_per_re_for_uci = config.nof_tx_layers * get_bits_per_symbol(config.modulation);
      size_t   harq_ack_llrs           = h_harq_ack_re_indices_.size() * nof_bits_per_re_for_uci;
      size_t   csi_part1_llrs          = h_csi_part1_re_indices_.size() * nof_bits_per_re_for_uci;
      size_t   harq_ack_bytes          = harq_ack_llrs * sizeof(uint16_t);
      size_t   csi_part1_bytes         = csi_part1_llrs * sizeof(uint16_t);

      // UCI buffers are pre-allocated at MAX in the constructor — capacity check only.
      if (harq_ack_bytes > d_harq_ack_llrs_half_capacity_[buf_idx] ||
          csi_part1_bytes > d_csi_part1_llrs_half_capacity_[buf_idx] ||
          h_harq_ack_re_indices_.size() > d_harq_ack_re_indices_capacity_[buf_idx] ||
          h_csi_part1_re_indices_.size() > d_csi_part1_re_indices_capacity_[buf_idx] ||
          !d_harq_ack_decode_result_[buf_idx] || !d_csi_part1_decode_result_[buf_idx]) {
        ocudulog::fetch_basic_logger("PHY").error(
            "PUSCH GPU E2E: UCI compaction workload exceeds pre-allocated capacity "
            "(HARQ {} RE / {} LLR, CSI1 {} RE / {} LLR).",
            h_harq_ack_re_indices_.size(),
            harq_ack_llrs,
            h_csi_part1_re_indices_.size(),
            csi_part1_llrs);
        grid_hold.drop();
        process_cpu_grid_fallback(codeword_buffer, notifier, grid, est_results, config);
        return;
      }

      h_harq_ack_llrs_.resize(harq_ack_llrs);
      h_csi_part1_llrs_.resize(csi_part1_llrs);
      h_harq_ack_llrs_half_size_[buf_idx]  = harq_ack_llrs;
      h_csi_part1_llrs_half_size_[buf_idx] = csi_part1_llrs;

      // Prefer pinned host staging (preallocated); fall back to pageable vectors if pin failed at ctor.
      if (h_harq_ack_llrs_half_pinned_[buf_idx] == nullptr ||
          harq_ack_llrs > h_harq_ack_llrs_half_pinned_cap_[buf_idx]) {
        h_harq_ack_llrs_half_.resize(harq_ack_llrs);
      }
      if (h_csi_part1_llrs_half_pinned_[buf_idx] == nullptr ||
          csi_part1_llrs > h_csi_part1_llrs_half_pinned_cap_[buf_idx]) {
        h_csi_part1_llrs_half_.resize(csi_part1_llrs);
      }
    } else {
      h_harq_ack_llrs_.clear();
      h_csi_part1_llrs_.clear();
      h_harq_ack_llrs_half_.clear();
      h_csi_part1_llrs_half_.clear();
      h_harq_ack_llrs_half_size_[buf_idx]  = 0;
      h_csi_part1_llrs_half_size_[buf_idx] = 0;
    }
  }

  // Get host staging pointer for geometry metadata when the RE-index cache misses.
  char* h_staging            = static_cast<char*>(h_unified_staging_[buf_idx]);
  int*  h_indices_staging    = reinterpret_cast<int*>(h_staging + unified_indices_offset_);
  bool  re_indices_on_device = d_re_indices_offsets_[buf_idx] == unified_indices_offset_ &&
                              d_re_indices_cache_[buf_idx].matches(config.rb_mask,
                                                                   config.dmrs_symb_pos.to_uint64(),
                                                                   config.start_symbol_index,
                                                                   config.nof_symbols,
                                                                   dmrs_type_int,
                                                                   config.nof_cdm_groups_without_data,
                                                                   nof_subcarriers);

  // Check grid data contiguity per port for direct DMA.
  std::chrono::steady_clock::time_point grid_staging_start;
  if (enable_path_tracing_) {
    grid_staging_start = std::chrono::steady_clock::now();
  }

  // Store per-port grid source pointers and contiguity flags for H2D DMA below.
  // When the grid is backed by pinned memory (cudaHostRegister), the DMA proceeds
  // at full NVLink bandwidth without internal staging. For unpinned grids, CUDA
  // handles the copy transparently (slightly slower but correct).
  struct port_grid_info {
    const void* src;
    size_t      bytes;
  };
  std::array<port_grid_info, 8> port_grids;
  bool                          all_contiguous = true;

  if (!use_device_grid_reader) {
    for (unsigned port = 0; port < nof_rx_ports; ++port) {
      span<const cbf16_t> first_view    = grid.get_view(config.rx_ports[port], 0);
      bool                is_contiguous = true;
      if (nof_symbols > 1) {
        span<const cbf16_t> second_view = grid.get_view(config.rx_ports[port], 1);
        is_contiguous                   = (second_view.data() == first_view.data() + nof_subcarriers);
      }

      if (is_contiguous && first_view.size() >= nof_subcarriers) {
        port_grids[port] = {first_view.data(), nof_symbols * nof_subcarriers * sizeof(cbf16_t)};
      } else {
        // Non-contiguous grid: fall back to staging memcpy for this port.
        all_contiguous = false;
        break;
      }
    }
  }

  // Copy RE indices only when this triple-buffer slot does not already hold
  // the cached allocation/DMRS geometry.
  if (!re_indices_on_device) {
    std::memcpy(h_indices_staging, h_re_indices_.data(), indices_bytes);
  }
  if (enable_path_tracing_) {
    auto grid_staging_end = std::chrono::steady_clock::now();
    last_grid_staging_us_ = std::chrono::duration<float, std::micro>(grid_staging_end - grid_staging_start).count();
  }

  us_prep           = us_since(t_demod0);
  const auto t_cfg0 = std::chrono::steady_clock::now();

  // --- Configure E2E handle with caching to avoid cudaDeviceSynchronize ---
  // Use max_prb=273 for 100MHz to avoid reconfig on dynamic PRB allocation changes.
  static constexpr int MAX_PRB_100MHZ = 273;
  int                  config_prb     = (static_cast<int>(config.rb_mask.count()) <= MAX_PRB_100MHZ)
                                            ? MAX_PRB_100MHZ
                                            : static_cast<int>(config.rb_mask.count());

  // DMRS scrambling parameters from demodulator configuration.
  uint32_t dmrs_scrambling_id = static_cast<uint32_t>(config.dmrs_scrambling_id);
  int      n_scid             = config.n_scid ? 1 : 0;
  int      scs_khz            = 15 * (1 << config.slot.numerology());
  int      use_low_papr_dmrs  = config.enable_transform_precoding ? 1 : 0;
  int      n_rs_id =
      (config.enable_transform_precoding && config.n_rs_id.has_value()) ? static_cast<int>(config.n_rs_id.value()) : 0;
  int compensate_cfo = compensate_cfo_ ? 1 : 0;

  // Structural match avoids full pusch_e2e_configure (DMRS precompute + stream sync).
  // RNTI / n_id / mod_order are data-scramble params only — update via lightweight path.
  // Per-slot fields (slot_idx, nof_prb, start_prb, dmrs mask) use update_slot_config.
  const bool structural_matched =
      e2e_config_cache_.matches_structural(config_prb,
                                           static_cast<int>(config.nof_symbols),
                                           static_cast<int>(nof_rx_ports),
                                           static_cast<int>(config.nof_tx_layers),
                                           static_cast<int>(nof_subcarriers),
                                           static_cast<int>(nof_symbols),
                                           dmrs_type_int,
                                           static_cast<int>(config.dmrs_symb_pos.to_uint64()),
                                           static_cast<int>(config.nof_cdm_groups_without_data),
                                           static_cast<int>(dmrs_scrambling_id),
                                           n_scid,
                                           scs_khz,
                                           use_low_papr_dmrs,
                                           n_rs_id,
                                           compensate_cfo,
                                           time_interp_mode_,
                                           noise_mode_);
  const bool data_params_matched =
      e2e_config_cache_.data_params_match(static_cast<uint16_t>(config.rnti), config.n_id, mod_order);

  nr_ldpc_status_t status               = NR_LDPC_SUCCESS;
  bool             configured_this_call = false;
  if (!structural_matched && e2e_handle_) {
    // Structural config changed - must full reconfigure (may sync for DMRS/scramble precompute).
    pusch_e2e_config_t e2e_cfg   = {};
    e2e_cfg.nof_prb              = config_prb;
    e2e_cfg.nof_symbols          = static_cast<int>(config.nof_symbols);
    e2e_cfg.nof_rx_ports         = static_cast<int>(nof_rx_ports);
    e2e_cfg.nof_tx_layers        = static_cast<int>(config.nof_tx_layers); // MIMO: 1, 2, 3, or 4 layers supported
    e2e_cfg.grid_nof_subcarriers = static_cast<int>(nof_subcarriers);
    e2e_cfg.grid_nof_symbols     = static_cast<int>(nof_symbols);
    e2e_cfg.dmrs_type            = (dmrs_type_int == 1) ? DMRS_TYPE_1 : DMRS_TYPE_2;
    e2e_cfg.dmrs_symbol_mask     = static_cast<int>(config.dmrs_symb_pos.to_uint64());
    e2e_cfg.nof_cdm_groups_without_data = static_cast<int>(config.nof_cdm_groups_without_data);
    e2e_cfg.scrambling_id               = dmrs_scrambling_id;
    e2e_cfg.n_scid                      = n_scid;
    e2e_cfg.slot_idx                    = static_cast<int>(config.slot.slot_index());

    // DMRS scaling is the power boost factor relative to data symbols.
    // Per TS38.214, this is 10^(-beta/20) where beta = get_sch_to_dmrs_ratio_dB().
    // For nof_cdm_groups_without_data=2, beta=-3dB, so dmrs_scaling=10^(3/20)≈1.41.
    float beta_dmrs_db   = get_sch_to_dmrs_ratio_dB(config.nof_cdm_groups_without_data);
    e2e_cfg.dmrs_scaling = std::pow(10.0f, -beta_dmrs_db / 20.0f);
    e2e_cfg.mod_order    = mod_order;
    e2e_cfg.rnti         = static_cast<uint16_t>(config.rnti);
    e2e_cfg.n_id         = static_cast<uint16_t>(config.n_id);
    e2e_cfg.start_prb    = static_cast<int>(config.rb_mask.find_lowest());
    e2e_cfg.start_symbol = static_cast<int>(config.start_symbol_index);
    e2e_cfg.tx_scaling   = 1.0f;
    e2e_cfg.equalizer_algorithm =
        (equalizer_algorithm_ == channel_equalizer_algorithm_type::mmse) ? EQUALIZER_MMSE : EQUALIZER_ZF;
    e2e_cfg.scs_khz = scs_khz;

    // Low-PAPR DMRS configuration for transform precoding (MSG3).
    e2e_cfg.use_low_papr_dmrs = use_low_papr_dmrs;
    e2e_cfg.n_rs_id           = n_rs_id;

    // CFO compensation: apply estimated CFO to channel estimates during equalization.
    e2e_cfg.compensate_cfo = compensate_cfo;

    // Time interpolation: 0 = average (default, matches CPU), 1 = linear.
    e2e_cfg.time_interp_mode = time_interp_mode_;

    // Noise estimation: 1 = pilot-residual (matches CPU estimate_noise() method).
    // Cross-validation (mode 0) uses empirical 2.5x calibration that breaks under fading.
    e2e_cfg.noise_mode = noise_mode_;

    // EVM accumulation is only needed when EVM is explicitly requested or when
    // post-equalization SINR is selected. Avoid the extra per-RE work on the
    // default channel-estimator metrics path.
    e2e_cfg.enable_evm_metric = (evm_calc || compute_post_eq_sinr) ? 1 : 0;

    status = pusch_e2e_configure(e2e_handle_, &e2e_cfg);
    if (status != NR_LDPC_SUCCESS) {
      ocudulog::fetch_basic_logger("PHY").warning("PUSCH GPU E2E configure failed with status {}; falling back to CPU.",
                                                  static_cast<int>(status));
      grid_hold.drop();
      process_cpu_grid_fallback(codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    e2e_config_cache_.update(static_cast<uint16_t>(config.rnti),
                             config.n_id,
                             config_prb,
                             static_cast<int>(config.nof_symbols),
                             static_cast<int>(nof_rx_ports),
                             static_cast<int>(config.nof_tx_layers),
                             static_cast<int>(nof_subcarriers),
                             static_cast<int>(nof_symbols),
                             mod_order,
                             dmrs_type_int,
                             static_cast<int>(config.dmrs_symb_pos.to_uint64()),
                             static_cast<int>(config.nof_cdm_groups_without_data),
                             dmrs_scrambling_id,
                             n_scid,
                             scs_khz,
                             use_low_papr_dmrs,
                             n_rs_id,
                             compensate_cfo,
                             time_interp_mode_,
                             noise_mode_);
    configured_this_call = true;
    cfg_kind             = 1;
    // Clear any CUDA errors from configure's internal kernel launches.
    cudaGetLastError();
  } else if (structural_matched && !data_params_matched && e2e_handle_) {
    // MCS / RNTI-only change: update data scramble without DMRS re-precompute.
    status = pusch_e2e_update_data_params(e2e_handle_, static_cast<uint16_t>(config.rnti), config.n_id, mod_order);
    if (status != NR_LDPC_SUCCESS) {
      ocudulog::fetch_basic_logger("PHY").warning(
          "PUSCH GPU E2E data-params update failed with status {}; falling back to CPU.", static_cast<int>(status));
      grid_hold.drop();
      process_cpu_grid_fallback(codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    e2e_config_cache_.update(static_cast<uint16_t>(config.rnti),
                             config.n_id,
                             config_prb,
                             static_cast<int>(config.nof_symbols),
                             static_cast<int>(nof_rx_ports),
                             static_cast<int>(config.nof_tx_layers),
                             static_cast<int>(nof_subcarriers),
                             static_cast<int>(nof_symbols),
                             mod_order,
                             dmrs_type_int,
                             static_cast<int>(config.dmrs_symb_pos.to_uint64()),
                             static_cast<int>(config.nof_cdm_groups_without_data),
                             dmrs_scrambling_id,
                             n_scid,
                             scs_khz,
                             use_low_papr_dmrs,
                             n_rs_id,
                             compensate_cfo,
                             time_interp_mode_,
                             noise_mode_);
    cfg_kind = 2;
    cudaGetLastError();
  }
  us_cfg            = us_since(t_cfg0);
  const auto t_h2d0 = std::chrono::steady_clock::now();

  // Update per-slot config values that change between PUSCH transmissions.
  // This is a lightweight call that doesn't trigger cudaDeviceSynchronize.
  int actual_nof_prb   = static_cast<int>(config.rb_mask.count());
  int actual_start_prb = static_cast<int>(config.rb_mask.find_lowest());
  int actual_slot_idx  = static_cast<int>(config.slot.slot_index());
  int actual_dmrs_mask = static_cast<int>(config.dmrs_symb_pos.to_uint64());
  if (e2e_handle_ &&
      (configured_this_call ||
       !e2e_slot_update_cache_.matches(actual_nof_prb, actual_start_prb, actual_slot_idx, actual_dmrs_mask))) {
    pusch_e2e_update_slot_config(e2e_handle_, actual_nof_prb, actual_start_prb, actual_slot_idx, actual_dmrs_mask);
    e2e_slot_update_cache_.update(actual_nof_prb, actual_start_prb, actual_slot_idx, actual_dmrs_mask);
  }

  // --- H2D transfer on dedicated stream (overlaps with previous slot's GPU work) ---
  const size_t timed_h2d_bytes =
      (use_device_grid_reader ? 0 : cbf16_bytes) + (re_indices_on_device ? 0 : indices_bytes);

  if (gpu_e2e_timing_enabled_) {
    auto& timing             = gpu_e2e_timing_[buf_idx];
    timing.rnti              = static_cast<uint16_t>(config.rnti);
    timing.nof_prb           = static_cast<unsigned>(config.rb_mask.count());
    timing.nof_re            = static_cast<unsigned>(std::min<std::size_t>(nof_re, UINT32_MAX));
    timing.mod_order         = static_cast<unsigned>(mod_order);
    timing.h2d_bytes         = timed_h2d_bytes;
    timing.direct_grid       = use_device_grid_reader;
    timing.re_indices_cached = re_indices_on_device;
    timing.resident          = gpu_resident_mode_;
    timing.compact_sch       = compact_sch_llrs;
    timing.configured        = configured_this_call;
    timing.includes_llr_d2h  = false;
    cudaEventRecord(timing.start, upload_stream);
  }

  // Record profiling: H2D start
  if (enable_path_tracing_ && prof_h2d_start_) {
    cudaEventRecord(prof_h2d_start_, upload_stream);
  }

  if (use_device_grid_reader) {
    // Device-visible grid path: the producer has already made the BF16 grid visible
    // and prepare_device_grid_reading() ordered this stream after its ready event.
  } else if (all_contiguous) {
    // FAST PATH: DMA grid data directly from grid views → device (skip staging memcpy).
    // When grid is backed by pinned memory (cudaHostRegister), this is a direct NVLink DMA.
    // For unpinned grids, CUDA handles the copy transparently via internal staging.
    char* d_grid_base = static_cast<char*>(d_unified_input_[buf_idx]) + unified_grid_offset_;

    // Check if all ports are contiguous in memory (tensor layout: [subc, symbol, port]).
    // When ports are sequential, the tensor is cross-port contiguous and we can coalesce
    // all per-port DMAs into a single cudaMemcpyAsync call, saving ~2μs per extra port.
    bool all_ports_contiguous = (nof_rx_ports > 0);
    for (unsigned port = 1; port < nof_rx_ports && all_ports_contiguous; ++port) {
      const char* expected = static_cast<const char*>(port_grids[port - 1].src) + port_grids[port - 1].bytes;
      all_ports_contiguous = (port_grids[port].src == expected);
    }

    if (all_ports_contiguous && nof_rx_ports > 0) {
      // Single coalesced DMA for all ports (common case with sequential port indices).
      size_t total_grid_bytes = 0;
      for (unsigned port = 0; port < nof_rx_ports; ++port) {
        total_grid_bytes += port_grids[port].bytes;
      }
      cudaMemcpyAsync(d_grid_base, port_grids[0].src, total_grid_bytes, cudaMemcpyHostToDevice, upload_stream);
    } else {
      // Per-port DMA (non-sequential port indices or non-contiguous tensor).
      for (unsigned port = 0; port < nof_rx_ports; ++port) {
        size_t port_byte_offset = port * grid_stride * sizeof(uint32_t);
        cudaMemcpyAsync(d_grid_base + port_byte_offset,
                        port_grids[port].src,
                        port_grids[port].bytes,
                        cudaMemcpyHostToDevice,
                        upload_stream);
      }
    }
  } else {
    // SLOW PATH: Non-contiguous grid — fall back to staging buffer for grid data.
    uint32_t* h_grid_staging = reinterpret_cast<uint32_t*>(h_staging + unified_grid_offset_);
    for (unsigned port = 0; port < nof_rx_ports; ++port) {
      size_t port_offset = port * grid_stride;
      for (unsigned sym = 0; sym < nof_symbols; ++sym) {
        size_t              sym_offset = port_offset + sym * nof_subcarriers;
        span<const cbf16_t> grid_view  = grid.get_view(config.rx_ports[port], sym);
        size_t              copy_size  = std::min(static_cast<size_t>(nof_subcarriers), grid_view.size());
        std::memcpy(&h_grid_staging[sym_offset], grid_view.data(), copy_size * sizeof(cbf16_t));
      }
    }
    cudaMemcpyAsync(static_cast<char*>(d_unified_input_[buf_idx]) + unified_grid_offset_,
                    h_staging + unified_grid_offset_,
                    cbf16_bytes,
                    cudaMemcpyHostToDevice,
                    upload_stream);
  }

  // DMA metadata. RE indices are geometry-only and cached per device buffer.
  if (!re_indices_on_device) {
    cudaMemcpyAsync(d_re_indices_, h_indices_staging, indices_bytes, cudaMemcpyHostToDevice, upload_stream);
    d_re_indices_cache_[buf_idx].update(config.rb_mask,
                                        config.dmrs_symb_pos.to_uint64(),
                                        config.start_symbol_index,
                                        config.nof_symbols,
                                        dmrs_type_int,
                                        config.nof_cdm_groups_without_data,
                                        nof_subcarriers,
                                        raw_accumulated_re_);
    d_re_indices_offsets_[buf_idx] = unified_indices_offset_;
  }

  // Record profiling: H2D end
  if (enable_path_tracing_ && prof_h2d_end_) {
    cudaEventRecord(prof_h2d_end_, upload_stream);
  }
  if (gpu_e2e_timing_enabled_) {
    cudaEventRecord(gpu_e2e_timing_[buf_idx].h2d_end, upload_stream);
  }

  // Record event when H2D completes (so next iteration can wait if needed).
  if (h2d_complete_[buf_idx] && h2d_stream_) {
    cudaEventRecord(h2d_complete_[buf_idx], upload_stream);
  }

  // Main stream waits for H2D to complete before processing.
  if (h2d_stream_) {
    cudaStreamWaitEvent(stream_, h2d_complete_[buf_idx], 0);
  }
  us_h2d           = us_since(t_h2d0);
  const auto t_eq0 = std::chrono::steady_clock::now();

  // Record profiling: Kernel start
  if (enable_path_tracing_ && prof_kernel_start_) {
    cudaEventRecord(prof_kernel_start_, stream_);
  }

  // Give a resident full-receiver dApp first authority over the prepared grid, RE indices,
  // inherited stream and caller-owned FP16 output. Overwrite permission is independent of
  // successful output: a partial/uncertain submission can fail AND forbid conventional recovery.
  pusch_inline_replacement replacement;
  bool dapp_inline_used         = false;
  bool dapp_post_eq_noise_valid = false;
  bool dapp_eq_symbols_valid    = false;
  auto demodulate_equalized     = [&]() {
    // PUSCH data scrambling (TS 38.211 §6.3.1.1): c_init = n_RNTI·2^15 + q·2^14 + n_ID.
    // Slot index is NOT part of data scrambling (only DMRS). Passing n_s into
    // scrambler_configure() injects (n_s&0x1F)<<4 into c_init and yields ~100% UL CRC
    // even when the Class-A equalizer symbols are correct — which is exactly the OTA
    // "DAPP-EQ ok but all NOK" failure mode. Match the working GPU E2E path (n_s=0).
    nr_scrambling_config_t scr_cfg{};
    scr_cfg.n_RNTI = static_cast<uint16_t>(config.rnti);
    scr_cfg.n_ID   = config.n_id;
    scr_cfg.q      = 0;
    scr_cfg.n_s    = 0;
    status         = scrambler_configure(scr_handle_, &scr_cfg);
    if (status == NR_LDPC_SUCCESS) {
      status = scrambler_generate_sequence(scr_handle_, static_cast<int>(num_llrs), stream_);
    }
    if (status != NR_LDPC_SUCCESS) {
      return false;
    }
    const int demod_status = modulator_soft_demod_descramble_half(mod_handle_,
                                                                  static_cast<cuFloatComplex*>(d_symbols_),
                                                                  static_cast<float*>(d_noise_vars_),
                                                                  scrambler_get_sequence_ptr(scr_handle_),
                                                                  d_llrs_half_[buf_idx],
                                                                  static_cast<int>(nof_re * nof_tx_layers),
                                                                  mod_order,
                                                                  stream_);
    status                 = demod_status == 0 ? NR_LDPC_SUCCESS : NR_LDPC_ERROR_CUDA_FAILED;
    return demod_status == 0;
  };
  // Opt-in common feedback for controlled baseline/full-receiver comparisons.
  // Select channel_estimator SINR explicitly in the sector configuration; never
  // relabel pilot SINR as post-EQ. Unsupported receiver grants remain conventional.
  static const bool common_pilot_csi =
      phy_acceleration_env_flag_enabled("OCUDU_PUSCH_COMMON_PILOT_CSI", false);
  const int pilot_dc = config.dc_position.has_value() &&
                               *config.dc_position <= static_cast<unsigned>(std::numeric_limits<int>::max())
                           ? static_cast<int>(*config.dc_position)
                           : config.dc_position.has_value() ? -2 : -1;
  const bool common_pilot_admitted = common_pilot_csi &&
      pusch_e2e_validate_pilot_metrics(e2e_handle_, d_grid_cbf16_, pilot_dc) == NR_LDPC_SUCCESS;
  if (resident_dapp_) {
    // Per-UE routing: one route per grant, before the mode is chosen.
    resident_dapp_->select_route(resident_dapp_->resolve_route(config.cell_id, to_value(config.rnti)));
  }
  const pusch_resident_dapp_mode resident_mode =
      resident_dapp_ ? resident_dapp_->mode_with_pilot_admission(common_pilot_csi, common_pilot_admitted)
                     : pusch_resident_dapp_mode::inactive;
  if (resident_mode != pusch_resident_dapp_mode::inactive) {
    const pusch_resident_dapp_buffers buffers{.grid_cbf16           = d_grid_cbf16_,
                                              .data_re_indices      = reinterpret_cast<const uint32_t*>(d_re_indices_),
                                              .equalized_cf32       = resident_mode != pusch_resident_dapp_mode::receiver ||
                                                                              capture_symbols_enabled_ || capture_requested_slot_
                                                                          ? d_symbols_
                                                                          : nullptr,
                                              .post_eq_noise_f32    = d_noise_vars_,
                                              .llrs_f16             = d_llrs_half_[buf_idx],
                                              .nof_grid_ports       = nof_rx_ports,
                                              .nof_grid_symbols     = nof_symbols,
                                              .nof_grid_subcarriers = nof_subcarriers,
                                              .nof_data_re          = static_cast<uint32_t>(nof_re),
                                              .device_ordinal       = 0,
                                              .completion_slot      = static_cast<uint32_t>(buf_idx),
                                              .execution_stream     = reinterpret_cast<uintptr_t>(stream_),
                                              .codeword = {.data = d_llrs_half_[buf_idx],
                                                           .nof_softbits = num_llrs,
                                                           .execution_context = static_cast<void*>(stream_),
                                                           .completion_token = static_cast<void*>(kernel_complete_[buf_idx]),
                                                           .valid = gpu_resident_mode_ && !compact_sch_llrs,
                                                           .n_id = config.n_id,
                                                           .rnti = static_cast<uint16_t>(config.rnti),
                                                           .buffer_index = buf_idx},
                                              .consumer_streams = {static_cast<void*>(stream_),
                                                                   static_cast<void*>(h2d_stream_),
                                                                   static_cast<void*>(d2h_stream_)}};
    if (resident_mode == pusch_resident_dapp_mode::channel_estimator) {
      const auto invoked = resident_dapp_->invoke_channel_estimator(buffers, config);
      replacement.invoked(invoked);
      if (invoked.status == OCUDU_DAPP_OK_V1 && !invoked.invoke_fallback) {
        if (config.nof_tx_layers <= 2) {
          const bool eq_ok =
              launch_pusch_resident_ce_equalizer(static_cast<const uint32_t*>(d_grid_cbf16_),
                                                 static_cast<const float2*>(resident_dapp_->channel_estimates_cf32()),
                                                 resident_dapp_->channel_noise_f32(),
                                                 reinterpret_cast<const uint32_t*>(d_re_indices_),
                                                 static_cast<float2*>(d_symbols_),
                                                 static_cast<float*>(d_noise_vars_),
                                                 nof_rx_ports,
                                                 nof_tx_layers,
                                                 nof_symbols,
                                                 nof_subcarriers,
                                                 static_cast<uint32_t>(nof_re),
                                                 stream_);
          if (eq_ok) {
            // Stash EQ availability even if soft-demod later fails — pre-join force-D2H needs this.
            last_nof_eq_re_   = static_cast<std::size_t>(nof_re) * static_cast<std::size_t>(nof_tx_layers);
            last_capture_src_ = d_symbols_;
            last_capture_n_   = last_nof_eq_re_;
            // Async constellation D2H in parallel with soft-demod (sibling stream).
            queue_eq_symbol_d2h(d_symbols_, last_nof_eq_re_);
          }
          dapp_inline_used = eq_ok && demodulate_equalized();
          // The first equalization's demapper variance has already been consumed
          // by soft demodulation on this stream. If the CE supplied a distinct
          // scheduler disturbance, recompute only the post-EQ reporting buffer
          // after soft demodulation. This keeps MMSE/LLRs calibrated while making
          // scheduler SINR include channel-estimation error, without a host copy.
          if (dapp_inline_used) {
            if (const float* scheduler_noise = resident_dapp_->channel_scheduler_noise_f32();
                scheduler_noise != nullptr) {
              dapp_inline_used = launch_pusch_resident_ce_scheduler_noise(
                  static_cast<const float2*>(resident_dapp_->channel_estimates_cf32()),
                  scheduler_noise,
                  reinterpret_cast<const uint32_t*>(d_re_indices_),
                  static_cast<float*>(d_noise_vars_),
                  nof_rx_ports,
                  nof_tx_layers,
                  nof_symbols,
                  nof_subcarriers,
                  static_cast<uint32_t>(nof_re),
                  stream_);
            }
          }
          status                   = dapp_inline_used ? NR_LDPC_SUCCESS : NR_LDPC_ERROR_CUDA_FAILED;
          dapp_post_eq_noise_valid = dapp_inline_used;
          dapp_eq_symbols_valid    = dapp_inline_used;
        }
      }
    } else if (resident_mode == pusch_resident_dapp_mode::equalizer) {
      const auto invoked = resident_dapp_->invoke_equalizer(buffers, config);
      replacement.invoked(invoked);
      if (invoked.status == OCUDU_DAPP_OK_V1 && !invoked.invoke_fallback) {
        // Stash EQ availability even if soft-demod later fails — pre-join force-D2H needs this.
        last_nof_eq_re_   = static_cast<std::size_t>(nof_re) * static_cast<std::size_t>(nof_tx_layers);
        last_capture_src_ = d_symbols_;
        last_capture_n_   = last_nof_eq_re_;
        // NeuralEq path: EQ already enqueued on stream_; async D2H overlaps soft-demod/LDPC.
        queue_eq_symbol_d2h(d_symbols_, last_nof_eq_re_);
        dapp_inline_used         = demodulate_equalized();
        dapp_post_eq_noise_valid = dapp_inline_used;
        dapp_eq_symbols_valid    = dapp_inline_used;
      }
    } else if (resident_mode == pusch_resident_dapp_mode::receiver) {
      const auto invoked = resident_dapp_->invoke_receiver(buffers, config);
      replacement.invoked(invoked);
      if (invoked.status == OCUDU_DAPP_OK_V1 && !invoked.invoke_fallback) {
        // Same PUSCH data c_init as demodulate_equalized / GPU E2E (n_s must be 0).
        nr_scrambling_config_t scr_cfg{};
        scr_cfg.n_RNTI = static_cast<uint16_t>(config.rnti);
        scr_cfg.n_ID   = config.n_id;
        scr_cfg.q      = 0;
        scr_cfg.n_s    = 0;
        status         = scrambler_configure(scr_handle_, &scr_cfg);
        if (status == NR_LDPC_SUCCESS) {
          status = scrambler_descramble_llr_half_inplace(
              scr_handle_, d_llrs_half_[buf_idx], static_cast<int>(num_llrs), stream_);
        }
        dapp_inline_used         = status == NR_LDPC_SUCCESS;
        dapp_post_eq_noise_valid = dapp_inline_used && resident_dapp_->receiver_post_eq_noise_valid();
        dapp_eq_symbols_valid    = dapp_inline_used && resident_dapp_->receiver_equalized_symbols_valid();
        if (dapp_eq_symbols_valid) {
          last_nof_eq_re_   = static_cast<std::size_t>(nof_re) * static_cast<std::size_t>(nof_tx_layers);
          last_capture_src_ = d_symbols_;
          last_capture_n_   = last_nof_eq_re_;
          // Existing bounded optional diagnostic tap: stream-ordered, caller-owned
          // symbols, no synchronous telemetry work or stale baseline constellation.
          queue_eq_symbol_d2h(d_symbols_, last_nof_eq_re_);
        }
      }
    }
  }

  if (!dapp_inline_used) {
    replacement.continuation_failed();
  }
  if (resident_dapp_ && !resident_dapp_->on_inline_output_enqueued(static_cast<uint32_t>(buf_idx), dapp_inline_used)) {
    replacement.continuation_failed();
    dapp_inline_used = false;
    // Any accepted constellation must follow the receiver that supplied the
    // final LLRs. Invalidate the failed tap; selected replacement failures are drained/discarded below.
    last_capture_src_ = nullptr;
    last_capture_n_ = 0;
    eq_diag_queued_ = false;
    eq_diag_host_valid_ = false;
    eq_diag_count_ = 0;
  }

  if (replacement.must_discard()) {
    // Exceptional path only. Module work, staging and optional constellation copies must all finish
    // before returning the direct-grid hold or allowing this demodulator's storage to be reused.
    const bool drained = drain_inline_streams(std::array<cudaStream_t, 3>{stream_, h2d_stream_, d2h_stream_},
                                               [](cudaStream_t stream) {
                                                 return cudaStreamSynchronizeYielding(stream) == cudaSuccess;
                                               });
    if (!drained) {
      // No production quarantine/resource-retirement owner is installed yet. Do not unwind/recycle
      // uncertain storage or manufacture a completion event. Completion-required modules remain disabled.
      report_fatal_error("PUSCH failed inline receiver could not be fenced; refusing unsafe buffer reuse.");
    }
    resident_dapp_->on_completion(nullptr);
    resident_dapp_->receiver_consumers_returned();
    grid_hold.drop();
    gpu_llrs_valid_ = false;
    host_codeword_written_ = false;
    last_metrics_submission_ok_ = false;
    last_dapp_inline_ = false;
    last_dapp_post_eq_noise_valid_ = false;
    last_dapp_evm_valid_ = false;
    last_resident_llrs_ = nullptr;
    last_num_llrs_ = 0;
    last_capture_src_ = nullptr;
    last_capture_n_ = 0;
    eq_diag_queued_ = false;
    eq_diag_host_valid_ = false;
    eq_diag_count_ = 0;
    captured_eq_host_.clear();
    inline_input_discarded_ = true;
    ocudulog::fetch_basic_logger("PHY").warning(
        "PUSCH inline receiver input discarded for rnti=0x{:04x}; no conventional overwrite or decode.",
        static_cast<unsigned>(config.rnti));
    return;
  }

  // Conventional GPU E2E: optionally mirror equalized symbols for constellation (ai-ran set_eq_output tap).
  // Zero cost when capture is off (NULL pointers). NeuralEq / dApp-inline path already queued D2H from d_symbols_.
  const bool want_e2e_eq_tap = !dapp_inline_used && capture_symbols_enabled_;
  if (want_e2e_eq_tap) {
    configure_e2e_eq_tap(true);
  } else {
    configure_e2e_eq_tap(false);
  }

  // Run the conventional E2E kernel unless the dApp produced the complete pre-descrambling
  // result and OCUDU successfully appended scrambling reversal.
  if (!dapp_inline_used && config.enable_transform_precoding) {
    int dft_size         = static_cast<int>(config.rb_mask.count()) * NOF_SUBCARRIERS_PER_RB;
    int nof_data_symbols = (dft_size > 0) ? static_cast<int>(nof_re / static_cast<unsigned>(dft_size)) : 0;

    status = pusch_e2e_process_full_gpu_with_deprecoding(e2e_handle_,
                                                         d_grid_cbf16_,
                                                         d_llrs_half_[buf_idx],
                                                         d_re_indices_,
                                                         static_cast<int>(nof_re),
                                                         dft_size,
                                                         nof_data_symbols,
                                                         stream_);
  } else if (!dapp_inline_used && config.nof_tx_layers == 1) {
    status = pusch_e2e_process_full_gpu_optimized(
        e2e_handle_, d_grid_cbf16_, d_llrs_half_[buf_idx], d_re_indices_, static_cast<int>(nof_re), stream_);
  } else if (!dapp_inline_used) {
    status = pusch_e2e_process_full_gpu_optimized_mimo_half(
        e2e_handle_, d_grid_cbf16_, d_llrs_half_[buf_idx], d_re_indices_, static_cast<int>(nof_re), stream_);
  }

  // Record profiling: Kernel end
  if (enable_path_tracing_ && prof_kernel_end_) {
    cudaEventRecord(prof_kernel_end_, stream_);
  }
  if (gpu_e2e_timing_enabled_) {
    cudaEventRecord(gpu_e2e_timing_[buf_idx].kernel_end, stream_);
  }
  us_eq              = us_since(t_eq0);
  const auto t_post0 = std::chrono::steady_clock::now();

  if (!dapp_inline_used && status != NR_LDPC_SUCCESS) {
    cudaStreamSynchronizeYielding(stream_);
    if (resident_dapp_) {
      resident_dapp_->on_completion(nullptr);
    }
    cudaError_t cuda_status = cudaGetLastError();
    ocudulog::fetch_basic_logger("PHY").warning(
        "PUSCH GPU E2E kernel failed with status {} (CUDA: {}); falling back to CPU.",
        static_cast<int>(status),
        cudaGetErrorString(cuda_status));
    grid_hold.drop();
    process_cpu_grid_fallback(codeword_buffer, notifier, grid, est_results, config);
    return;
  }

  bool common_pilot_failed = false;
  if (common_pilot_admitted) {
    // This must precede grid_hold.commit(): its completion event is the grid's
    // lifetime boundary. Refreshing after it would read an already-reusable grid.
    const auto pilot_status = dapp_inline_used
        ? pusch_e2e_process_pilot_metrics_with_dc(e2e_handle_, d_grid_cbf16_, pilot_dc, stream_)
        : pusch_e2e_refresh_pilot_metrics_with_dc(e2e_handle_, d_grid_cbf16_, pilot_dc, stream_);
    last_common_pilot_csi_ = pilot_status == NR_LDPC_SUCCESS;
    common_pilot_failed = !last_common_pilot_csi_;
    // No CPU rerun, invented SINR, or overwriting already-produced LLRs on a
    // measurement failure. Suppress all CSI and retain the existing completion fence.
  }

  if (use_device_grid_reader) {
    grid_hold.commit();
  }

  if (compact_sch_llrs) {
    unsigned nof_bits_per_re_compact = static_cast<unsigned>(config.nof_tx_layers * mod_order);
    unsigned mod_order_compact       = static_cast<unsigned>(mod_order);
    bool     compaction_indices_on_device =
        d_compaction_re_indices_cache_[buf_idx].matches(true,
                                                        static_cast<unsigned>(config.rb_mask.count()),
                                                        static_cast<unsigned>(config.rb_mask.find_lowest()),
                                                        static_cast<unsigned>(config.rb_mask.find_highest()),
                                                        config.dmrs_symb_pos.to_uint64(),
                                                        config.start_symbol_index,
                                                        config.nof_symbols,
                                                        dmrs_type_int,
                                                        config.nof_cdm_groups_without_data,
                                                        mod_order_compact,
                                                        config.nof_tx_layers,
                                                        resident_compaction.nof_ul_sch_bits,
                                                        resident_compaction.nof_harq_ack_rvd,
                                                        resident_compaction.nof_enc_harq_ack_bits,
                                                        resident_compaction.nof_harq_ack_bits,
                                                        resident_compaction.nof_enc_csi_part1_bits,
                                                        resident_compaction.nof_csi_part1_bits,
                                                        resident_compaction.nof_enc_csi_part2_bits);
    if (!compaction_indices_on_device) {
      d_compaction_re_indices_cache_[buf_idx].update(true,
                                                     static_cast<unsigned>(config.rb_mask.count()),
                                                     static_cast<unsigned>(config.rb_mask.find_lowest()),
                                                     static_cast<unsigned>(config.rb_mask.find_highest()),
                                                     config.dmrs_symb_pos.to_uint64(),
                                                     config.start_symbol_index,
                                                     config.nof_symbols,
                                                     dmrs_type_int,
                                                     config.nof_cdm_groups_without_data,
                                                     mod_order_compact,
                                                     config.nof_tx_layers,
                                                     resident_compaction.nof_ul_sch_bits,
                                                     resident_compaction.nof_harq_ack_rvd,
                                                     resident_compaction.nof_enc_harq_ack_bits,
                                                     resident_compaction.nof_harq_ack_bits,
                                                     resident_compaction.nof_enc_csi_part1_bits,
                                                     resident_compaction.nof_csi_part1_bits,
                                                     resident_compaction.nof_enc_csi_part2_bits);
      d_sch_re_indices_uploaded_[buf_idx]       = false;
      d_harq_ack_re_indices_uploaded_[buf_idx]  = false;
      d_csi_part1_re_indices_uploaded_[buf_idx] = false;
    }

    auto ensure_sch_indices_on_device = [&]() {
      if (d_sch_re_indices_uploaded_[buf_idx]) {
        return;
      }
      cudaMemcpyAsync(d_sch_re_indices_[buf_idx],
                      h_sch_re_indices_.data(),
                      h_sch_re_indices_.size() * sizeof(int),
                      cudaMemcpyHostToDevice,
                      stream_);
      d_sch_re_indices_uploaded_[buf_idx] = true;
    };
    auto ensure_harq_indices_on_device = [&]() {
      if (h_harq_ack_re_indices_.empty() || d_harq_ack_re_indices_uploaded_[buf_idx]) {
        return;
      }
      cudaMemcpyAsync(d_harq_ack_re_indices_[buf_idx],
                      h_harq_ack_re_indices_.data(),
                      h_harq_ack_re_indices_.size() * sizeof(int),
                      cudaMemcpyHostToDevice,
                      stream_);
      d_harq_ack_re_indices_uploaded_[buf_idx] = true;
    };
    auto ensure_csi_part1_indices_on_device = [&]() {
      if (h_csi_part1_re_indices_.empty() || d_csi_part1_re_indices_uploaded_[buf_idx]) {
        return;
      }
      cudaMemcpyAsync(d_csi_part1_re_indices_[buf_idx],
                      h_csi_part1_re_indices_.data(),
                      h_csi_part1_re_indices_.size() * sizeof(int),
                      cudaMemcpyHostToDevice,
                      stream_);
      d_csi_part1_re_indices_uploaded_[buf_idx] = true;
    };

    ensure_sch_indices_on_device();

    h_harq_ack_expected_payload_bits_  = resident_compaction.nof_harq_ack_bits;
    h_csi_part1_expected_payload_bits_ = resident_compaction.nof_csi_part1_bits;

    bool can_decode_harq_short =
        decode_short_uci_on_device && !h_harq_ack_re_indices_.empty() && (resident_compaction.nof_harq_ack_bits <= 11);
    bool can_decode_csi1_short = decode_short_uci_on_device && !h_csi_part1_re_indices_.empty() &&
                                 (resident_compaction.nof_csi_part1_bits <= 11);
    bool can_decode_harq_polar = compact_uci_on_device && gpu_uci_polar_available_ && !h_harq_ack_re_indices_.empty() &&
                                 (resident_compaction.nof_harq_ack_bits > 11);
    bool can_decode_csi1_polar = compact_uci_on_device && gpu_uci_polar_available_ &&
                                 !h_csi_part1_re_indices_.empty() && (resident_compaction.nof_csi_part1_bits > 11);
    bool erase_harq_placeholders = compact_uci_on_device && !h_harq_ack_re_indices_.empty() &&
                                   (resident_compaction.nof_harq_ack_bits > 0) &&
                                   (resident_compaction.nof_harq_ack_bits <= 2);
    bool harq_device_decode_launched = false;
    bool csi1_device_decode_launched = false;

    if (can_decode_harq_short || can_decode_csi1_short) {
      if (can_decode_harq_short || erase_harq_placeholders) {
        ensure_harq_indices_on_device();
      }
      if (can_decode_csi1_short) {
        ensure_csi_part1_indices_on_device();
      }
      pusch_compact_sch_and_decode_uci_short_blocks_half(
          d_llrs_half_[buf_idx],
          d_sch_llrs_half_[buf_idx],
          d_sch_re_indices_[buf_idx],
          static_cast<unsigned>(h_sch_re_indices_.size()),
          can_decode_harq_short ? d_harq_ack_re_indices_[buf_idx] : nullptr,
          can_decode_harq_short ? static_cast<unsigned>(h_harq_ack_re_indices_.size()) : 0,
          can_decode_harq_short ? resident_compaction.nof_harq_ack_bits : 0,
          can_decode_harq_short ? d_harq_ack_decode_result_[buf_idx] : nullptr,
          can_decode_csi1_short ? d_csi_part1_re_indices_[buf_idx] : nullptr,
          can_decode_csi1_short ? static_cast<unsigned>(h_csi_part1_re_indices_.size()) : 0,
          can_decode_csi1_short ? resident_compaction.nof_csi_part1_bits : 0,
          can_decode_csi1_short ? d_csi_part1_decode_result_[buf_idx] : nullptr,
          nof_bits_per_re_compact,
          mod_order,
          stream_,
          erase_harq_placeholders ? d_harq_ack_re_indices_[buf_idx] : nullptr,
          erase_harq_placeholders ? static_cast<unsigned>(h_harq_ack_re_indices_.size()) : 0);
      if (can_decode_harq_short) {
        harq_device_decode_launched       = true;
        h_harq_ack_decode_nof_codeblocks_ = 1;
      }
      if (can_decode_csi1_short) {
        csi1_device_decode_launched        = true;
        h_csi_part1_decode_nof_codeblocks_ = 1;
      }
    } else {
      if (erase_harq_placeholders) {
        ensure_harq_indices_on_device();
      }
      pusch_compact_sch_llrs_half(d_llrs_half_[buf_idx],
                                  d_sch_llrs_half_[buf_idx],
                                  d_sch_re_indices_[buf_idx],
                                  static_cast<unsigned>(h_sch_re_indices_.size()),
                                  nof_bits_per_re_compact,
                                  stream_,
                                  erase_harq_placeholders ? d_harq_ack_re_indices_[buf_idx] : nullptr,
                                  erase_harq_placeholders ? static_cast<unsigned>(h_harq_ack_re_indices_.size()) : 0);
    }

    if (compact_uci_on_device) {
      if (!h_harq_ack_re_indices_.empty() && !harq_device_decode_launched) {
        ensure_harq_indices_on_device();
        pusch_compact_sch_llrs_half(d_llrs_half_[buf_idx],
                                    d_harq_ack_llrs_half_[buf_idx],
                                    d_harq_ack_re_indices_[buf_idx],
                                    static_cast<unsigned>(h_harq_ack_re_indices_.size()),
                                    nof_bits_per_re_compact,
                                    stream_);
        if (can_decode_harq_polar) {
          auto* d_results = static_cast<polar_uci_decode_result_t*>(d_harq_ack_decode_result_[buf_idx]);
          harq_device_decode_launched =
              launch_polar_uci_decode_half(d_harq_ack_llrs_half_[buf_idx],
                                           static_cast<unsigned>(h_harq_ack_llrs_half_.size()),
                                           resident_compaction.nof_harq_ack_bits,
                                           harq_ack_polar_handles_[buf_idx],
                                           d_results,
                                           h_harq_ack_decode_nof_codeblocks_,
                                           stream_);
        }
        // ALWAYS queue the LLR D2H, even when device polar decode is also
        // running. If device decode later fails its CRC (publish_device_uci
        // returns false), finalize_resident_uci falls through to the host
        // LLR demap — and that path reads h_harq_ack_llrs_half_ unconditionally.
        // Before this fix, when device decode launched the D2H was skipped
        // and the fallback read uninitialized memory → scheduler got garbage
        // LLRs → HARQ-ACK decode failed silently → looked like UE went silent.
        // The D2H is a few hundred bytes per slot; cost is negligible.
        cudaMemcpyAsync(h_harq_ack_llrs_half_pinned_[buf_idx] ? h_harq_ack_llrs_half_pinned_[buf_idx]
                                                              : h_harq_ack_llrs_half_.data(),
                        d_harq_ack_llrs_half_[buf_idx],
                        h_harq_ack_llrs_half_size_[buf_idx] * sizeof(uint16_t),
                        cudaMemcpyDeviceToHost,
                        stream_);
      }
      if (!h_csi_part1_re_indices_.empty() && !csi1_device_decode_launched) {
        ensure_csi_part1_indices_on_device();
        pusch_compact_sch_llrs_half(d_llrs_half_[buf_idx],
                                    d_csi_part1_llrs_half_[buf_idx],
                                    d_csi_part1_re_indices_[buf_idx],
                                    static_cast<unsigned>(h_csi_part1_re_indices_.size()),
                                    nof_bits_per_re_compact,
                                    stream_);
        if (can_decode_csi1_polar) {
          auto* d_results = static_cast<polar_uci_decode_result_t*>(d_csi_part1_decode_result_[buf_idx]);
          csi1_device_decode_launched =
              launch_polar_uci_decode_half(d_csi_part1_llrs_half_[buf_idx],
                                           static_cast<unsigned>(h_csi_part1_llrs_half_.size()),
                                           resident_compaction.nof_csi_part1_bits,
                                           csi_part1_polar_handles_[buf_idx],
                                           d_results,
                                           h_csi_part1_decode_nof_codeblocks_,
                                           stream_);
        }
        // ALWAYS queue the LLR D2H (see HARQ-ACK block above for rationale).
        cudaMemcpyAsync(h_csi_part1_llrs_half_pinned_[buf_idx] ? h_csi_part1_llrs_half_pinned_[buf_idx]
                                                               : h_csi_part1_llrs_half_.data(),
                        d_csi_part1_llrs_half_[buf_idx],
                        h_csi_part1_llrs_half_size_[buf_idx] * sizeof(uint16_t),
                        cudaMemcpyDeviceToHost,
                        stream_);
      }
      if (harq_device_decode_launched) {
        cudaMemcpyAsync(h_harq_ack_decode_result_pinned_[buf_idx] ? h_harq_ack_decode_result_pinned_[buf_idx]
                                                                  : h_harq_ack_decode_result_.data(),
                        d_harq_ack_decode_result_[buf_idx],
                        sizeof(pusch_uci_short_decode_result) * h_harq_ack_decode_nof_codeblocks_,
                        cudaMemcpyDeviceToHost,
                        stream_);
      }
      if (csi1_device_decode_launched) {
        cudaMemcpyAsync(h_csi_part1_decode_result_pinned_[buf_idx] ? h_csi_part1_decode_result_pinned_[buf_idx]
                                                                   : h_csi_part1_decode_result_.data(),
                        d_csi_part1_decode_result_[buf_idx],
                        sizeof(pusch_uci_short_decode_result) * h_csi_part1_decode_nof_codeblocks_,
                        cudaMemcpyDeviceToHost,
                        stream_);
      }
    }
  }

  // Launch async SINR D2H after all device-side resident-output work. The completion event below covers both the
  // compact SCH LLR stream and the tiny SINR D2H.
  //
  // Resident no-UCI path (default on; OCUDU_PUSCH_SINR_SIDE_STREAM=0 disables): route the tiny SINR
  // readback copies to d2h_stream_ so the compute stream goes straight from the demod
  // kernels to the decoder's kernels. Skip when Class-A dApp produced LLRs inline.
  static const bool sinr_side_stream = []() {
    const char* v = std::getenv("OCUDU_PUSCH_SINR_SIDE_STREAM");
    return (v == nullptr) || (v[0] != '0');
  }();
  const bool resident_skip_sync =
      gpu_resident_mode_ && !resident_host_uci_demux_enabled(config) && !compact_uci_on_device;
  last_sinr_on_side_stream_ = false;
  last_metrics_submission_ok_  = false;
  last_metrics_fence_recorded_ = false;
  bool producer_event_recorded = false;
  bool noise_metrics_failed = false;
  bool dapp_evm_valid = false;
  if (dapp_inline_used && (dapp_post_eq_noise_valid || dapp_eq_symbols_valid)) {
    // Same compute stream as the producer: the existing completion fence covers
    // reduction and readback for both immediate and deferred stats. The dApp's
    // equalized symbols add decision-directed EVM with the baseline's QAM
    // convention in the same pass. No reporting-time allocation, full tensor D2H
    // or additional synchronization.
    const unsigned count = nof_re * nof_tx_layers;
    if (pusch_e2e_dapp_metrics_async(e2e_handle_,
                                     dapp_post_eq_noise_valid ? static_cast<const float*>(d_noise_vars_) : nullptr,
                                     dapp_eq_symbols_valid ? static_cast<const float2*>(d_symbols_) : nullptr,
                                     count,
                                     static_cast<unsigned>(mod_order),
                                     stream_) != NR_LDPC_SUCCESS) {
      dapp_post_eq_noise_valid = false;
      noise_metrics_failed = true;
      static_cast<void>(cudaGetLastError());
    } else {
      dapp_evm_valid = dapp_eq_symbols_valid;
    }
    last_metrics_submission_ok_ = dapp_post_eq_noise_valid || dapp_evm_valid;
  }
  if (!dapp_inline_used || last_common_pilot_csi_) {
    last_metrics_submission_ok_ = false;
    if (resident_skip_sync && sinr_side_stream && (d2h_stream_ != nullptr) && kernel_complete_[buf_idx] &&
        d2h_complete_[buf_idx]) {
      producer_event_recorded = cudaEventRecord(kernel_complete_[buf_idx], stream_) == cudaSuccess;
      if (producer_event_recorded && cudaStreamWaitEvent(d2h_stream_, kernel_complete_[buf_idx], 0) == cudaSuccess) {
        // A failed second copy can leave the first pending. Keep the side-stream
        // completion path even on submission failure, but never report its values.
        last_sinr_on_side_stream_ = true;
        last_metrics_submission_ok_ =
            pusch_e2e_sinr_async_launch_checked(e2e_handle_, d2h_stream_) == NR_LDPC_SUCCESS;
      }
    } else {
      last_metrics_submission_ok_ =
          pusch_e2e_sinr_async_launch_checked(e2e_handle_, stream_) == NR_LDPC_SUCCESS;
    }
  }
  if (common_pilot_failed || noise_metrics_failed) {
    last_metrics_submission_ok_ = false;
  }

  // PHASE 2.2: Record kernel completion event for async D2H / dApp coordination.
  // Side-stream SINR path already recorded kernel_complete_ above.
  if (kernel_complete_[buf_idx]) {
    bool event_ok = producer_event_recorded;
    if (!event_ok) {
      if (cudaEventRecord(kernel_complete_[buf_idx], stream_) != cudaSuccess) {
        last_metrics_submission_ok_ = false;
        static_cast<void>(cudaGetLastError());
        cudaStreamSynchronizeYielding(stream_);
        event_ok = false;
      } else {
        event_ok = true;
      }
    }
    if (!last_sinr_on_side_stream_) {
      last_metrics_fence_recorded_ = event_ok;
    }
    if (resident_dapp_) {
      resident_dapp_->on_completion(event_ok ? static_cast<void*>(kernel_complete_[buf_idx]) : nullptr);
    }
  } else if (resident_dapp_) {
    cudaStreamSynchronizeYielding(stream_);
    resident_dapp_->on_completion(nullptr);
  }

  // Conventional E2E constellation D2H: wait on kernel_complete, copy d_eq_diag_ on sibling stream
  // (overlaps LDPC on main stream). Skip if NeuralEq already queued D2H from d_symbols_.
  if (want_e2e_eq_tap && d_eq_diag_ != nullptr) {
    const std::size_t nof_eq = static_cast<std::size_t>(nof_re) * static_cast<std::size_t>(std::max(1U, nof_tx_layers));
    last_e2e_eq_count_       = nof_eq;
    if (!eq_diag_queued_) {
      queue_eq_symbol_d2h(d_eq_diag_, nof_eq, kernel_complete_[buf_idx]);
    }
  }

  // Store which buffer was just written for decoder to read from.
  last_buf_                      = buf_idx;
  last_resident_llrs_            = compact_sch_llrs ? d_sch_llrs_half_[buf_idx] : d_llrs_half_[buf_idx];
  last_num_llrs_                 = compact_sch_llrs ? resident_compaction.nof_ul_sch_bits : num_llrs;
  last_n_id_                     = config.n_id;
  last_rnti_                     = static_cast<uint16_t>(config.rnti);
  gpu_llrs_valid_                = true;
  last_dapp_inline_              = dapp_inline_used;
  last_dapp_post_eq_noise_valid_ = dapp_inline_used && dapp_post_eq_noise_valid;
  last_dapp_evm_valid_           = dapp_inline_used && dapp_evm_valid;
  // Preserve last_nof_eq_re_ if EQ was already stashed above (NeuralEq / CE-EQ). Only overwrite
  // when dApp-inline succeeded so deferred SINR still has a size; do NOT zero it when soft-demod
  // falls back — constellation force-D2H still needs the EQ buffer size.
  if (dapp_inline_used) {
    last_nof_eq_re_ = static_cast<std::size_t>(nof_re) * static_cast<std::size_t>(nof_tx_layers);
  }

  // GPU-resident mode without scheduler-visible UCI: SKIP sync and SINR readback here. The decoder will sync with the
  // GPU via cudaStreamSynchronize, after which SINR can be read from pinned memory via report_deferred_sinr().
  // UCI-bearing PUSCH keeps the early sync so HARQ/CSI can be notified before SCH completion.
  if (gpu_resident_mode_ && !resident_host_uci_demux_enabled(config) && !compact_uci_on_device) {
    last_demod_sync_us_    = 0; // No sync in demodulator
    gpu_uci_demux_pending_ = compact_uci_on_device;
    if (gpu_e2e_timing_enabled_) {
      cudaEventRecord(gpu_e2e_timing_[buf_idx].final_end, stream_);
      gpu_e2e_timing_[buf_idx].pending = true;
    }
    if (d2h_complete_[buf_idx]) {
      // When the SINR readback was routed to the side stream, the completion event must
      // cover those copies (it transitively covers the demod kernels via kernel_complete_).
      last_metrics_fence_recorded_ =
          cudaEventRecord(d2h_complete_[buf_idx], last_sinr_on_side_stream_ ? d2h_stream_ : stream_) == cudaSuccess;
      if (!last_metrics_fence_recorded_) {
        last_metrics_submission_ok_ = false;
        // Exceptional path only: an old event must not permit reuse while a
        // partial readback still writes the single host result buffer.
        static_cast<void>(cudaStreamSynchronizeYielding(last_sinr_on_side_stream_ ? d2h_stream_ : stream_));
      }
    }
    us_post = us_since(t_post0);
    // Host wall breakdown for the resident async path (kernels still in flight).
    ocudulog::fetch_basic_logger("PHY").info(
        "PUSCH demod phase rnti=0x{:x} prep={:.1f}us cfg={:.1f}us(kind={}) h2d={:.1f}us eq={:.1f}us "
        "post={:.1f}us total={:.1f}us dapp={} direct_grid={} re_idx_cached={} compact={}",
        static_cast<unsigned>(config.rnti),
        us_prep,
        us_cfg,
        cfg_kind,
        us_h2d,
        us_eq,
        us_post,
        us_since(t_demod0),
        dapp_inline_used,
        use_device_grid_reader,
        re_indices_on_device,
        compact_sch_llrs);
    return;
  }

  // Non-GPU-resident mode: sync and report SINR immediately.
  {
    auto sync_start = std::chrono::steady_clock::now();
    cudaError_t metrics_completion;
    if (kernel_complete_[buf_idx]) {
      metrics_completion = last_metrics_fence_recorded_ ? cudaEventSynchronizeYielding(kernel_complete_[buf_idx])
                                                       : cudaStreamSynchronizeYielding(stream_);
    } else {
      metrics_completion = cudaStreamSynchronizeYielding(stream_);
    }
    last_metrics_submission_ok_ = last_metrics_submission_ok_ && metrics_completion == cudaSuccess;
    auto sync_end       = std::chrono::steady_clock::now();
    last_demod_sync_us_ = std::chrono::duration<float, std::micro>(sync_end - sync_start).count();

    if (enable_path_tracing_) {
      pusch_e2e_print_diagnostics(e2e_handle_, stream_);
    }

    // Read SINR/EPRE/RSRP/TA from pinned memory (async D2H already complete after event sync).
    // Class-A dApp inline: derive post-EQ SINR from d_noise_vars_ (E2E SINR path skipped).
    if (dapp_inline_used) {
      notifier.on_end_stats(read_completed_inline_stats());
    } else if (last_metrics_submission_ok_) {
      float gpu_sinr_db    = pusch_e2e_post_eq_sinr_get_result(e2e_handle_);
      float gpu_ch_sinr_db = pusch_e2e_channel_estimator_sinr_get_result(e2e_handle_);
      float gpu_epre_db    = pusch_e2e_epre_get_result(e2e_handle_);
      float gpu_rsrp_db    = pusch_e2e_rsrp_get_result(e2e_handle_);
      float gpu_ta_s       = pusch_e2e_ta_get_result(e2e_handle_);
      float gpu_cfo_hz     = pusch_e2e_cfo_get_result(e2e_handle_);
      float gpu_evm        = pusch_e2e_evm_get_result(e2e_handle_);

      pusch_demodulator_notifier::demodulation_stats stats =
          make_pusch_e2e_demodulation_stats(
              gpu_sinr_db, gpu_ch_sinr_db, gpu_epre_db, gpu_rsrp_db, gpu_ta_s, gpu_cfo_hz, gpu_evm);
      sanitize_stats_in_place(stats, static_cast<uint16_t>(config.rnti));
      notifier.on_end_stats(stats);
    } else {
      notifier.on_end_stats({});
    }
  }

  if (compact_uci_on_device) {
    gpu_uci_demux_pending_ = true;
    finalize_resident_uci();
    if (gpu_e2e_timing_enabled_) {
      cudaEventRecord(gpu_e2e_timing_[buf_idx].final_end, stream_);
      gpu_e2e_timing_[buf_idx].pending = true;
    }
    if (d2h_complete_[buf_idx]) {
      cudaEventRecord(d2h_complete_[buf_idx], stream_);
    }
    return;
  }

  // Non-GPU-resident mode: Copy LLRs back to host.
  uint16_t*             h_llrs_half = h_llrs_half_pinned_;
  std::vector<uint16_t> h_llrs_half_fallback;
  if (!h_llrs_half || num_llrs > h_llrs_half_pinned_cap_) {
    h_llrs_half_fallback.resize(num_llrs);
    h_llrs_half = h_llrs_half_fallback.data();
  }

  // PHASE 2.2: Start D2H on d2h_stream_ for async overlap.
  // Use d2h_stream_ if available, otherwise fall back to main stream.
  cudaStream_t download_stream = d2h_stream_ ? d2h_stream_ : stream_;

  // Wait for kernel to complete before starting D2H.
  if (kernel_complete_[buf_idx] && d2h_stream_) {
    cudaStreamWaitEvent(download_stream, kernel_complete_[buf_idx], 0);
  }

  // Record profiling: D2H start
  if (enable_path_tracing_ && prof_d2h_start_) {
    cudaEventRecord(prof_d2h_start_, download_stream);
  }

  // Copy from the buffer we just wrote to (buf_idx).
  cudaMemcpyAsync(
      h_llrs_half, d_llrs_half_[buf_idx], num_llrs * sizeof(uint16_t), cudaMemcpyDeviceToHost, download_stream);

  // Record profiling: D2H end
  if (enable_path_tracing_ && prof_d2h_end_) {
    cudaEventRecord(prof_d2h_end_, download_stream);
  }
  if (gpu_e2e_timing_enabled_) {
    gpu_e2e_timing_[buf_idx].includes_llr_d2h = true;
    cudaEventRecord(gpu_e2e_timing_[buf_idx].final_end, download_stream);
    gpu_e2e_timing_[buf_idx].pending = true;
  }

  // Record D2H completion for buffer reuse tracking.
  if (d2h_complete_[buf_idx]) {
    cudaEventRecord(d2h_complete_[buf_idx], download_stream);
  }

  // PHASE 2.3: Sync on D2H completion event (more precise than stream sync).
  // This is necessary - CPU needs LLR data immediately to continue processing.
  // Using event sync allows overlap with other streams (H2D for next slot).
  if (d2h_complete_[buf_idx]) {
    cudaEventSynchronizeYielding(d2h_complete_[buf_idx]);
  } else {
    // Fallback if event not available.
    cudaStreamSynchronizeYielding(download_stream);
  }

  // Convert/copy LLRs to the codeword buffer.
  size_t offset = 0;
  while (offset < num_llrs) {
    size_t remain = std::min(num_llrs - offset, size_t(pusch_constants::MAX_NOF_BITS_PER_OFDM_SYMBOL));
    span<log_likelihood_ratio> block      = codeword_buffer.get_next_block_view(remain);
    size_t                     block_size = block.size();
    if (block_size == 0) {
      ocudulog::fetch_basic_logger("PHY").error(
          "PUSCH GPU host LLR conversion made no progress (offset={} num_llrs={}).", offset, num_llrs);
      break;
    }

    for (size_t i = 0; i < block_size; ++i) {
      block[i] = log_likelihood_ratio(fp16_to_int8_lut[h_llrs_half[offset + i]]);
    }

    static_bit_buffer<pusch_constants::MAX_NOF_BITS_PER_OFDM_SYMBOL> dummy_seq(block_size);
    codeword_buffer.on_new_block(block, dummy_seq);
    offset += block_size;
  }
  host_codeword_written_ = true;
}

void pusch_demodulator_gpu_impl::process_cpu_grid_fallback(pusch_codeword_buffer&              codeword_buffer,
                                                           pusch_demodulator_notifier&         notifier,
                                                           const resource_grid_reader&         grid,
                                                           const dmrs_pusch_estimator_results& est_results,
                                                           const configuration&                config)
{
  accumulated_symbols_.clear();
  accumulated_noise_vars_.clear();
  accumulated_count_ = 0;

  unsigned nof_rx_ports = static_cast<unsigned>(config.rx_ports.size());

  re_prb_mask         active_re_per_prb      = ~re_prb_mask();
  re_prb_mask         active_re_per_prb_dmrs = ~get_dmrs_prb_mask(config.dmrs_type, config.nof_cdm_groups_without_data);
  re_symbol_mask_type re_mask      = config.rb_mask.kronecker_product<NOF_SUBCARRIERS_PER_RB>(active_re_per_prb);
  re_symbol_mask_type re_mask_dmrs = config.rb_mask.kronecker_product<NOF_SUBCARRIERS_PER_RB>(active_re_per_prb_dmrs);

  unsigned total_sinr_softbit_count   = 0;
  float    total_noise_var_accumulate = 0.0F;

  for (unsigned i_symbol = config.start_symbol_index, i_symbol_end = config.start_symbol_index + config.nof_symbols;
       i_symbol != i_symbol_end;
       ++i_symbol) {
    const re_symbol_mask_type& symbol_re_mask = config.dmrs_symb_pos.test(i_symbol) ? re_mask_dmrs : re_mask;
    unsigned                   nof_re_symbol  = symbol_re_mask.count();

    if (nof_re_symbol == 0) {
      continue;
    }

    std::optional<unsigned> dc_position = config.enable_transform_precoding ? std::nullopt : config.dc_position;

    interval<unsigned>      re_interval(config.rb_mask.find_lowest() * NOF_SUBCARRIERS_PER_RB,
                                   (config.rb_mask.find_highest() + 1) * NOF_SUBCARRIERS_PER_RB);
    re_symbol_mask_type     symbol_re_mask_local = symbol_re_mask.slice(re_interval.start(), re_interval.stop());
    std::optional<unsigned> dc_position_local    = std::nullopt;
    if (dc_position.has_value() && re_interval.contains(*dc_position)) {
      dc_position_local = *dc_position - re_interval.start();
    }

    const re_buffer_reader<cbf16_t>&      ch_re = get_ch_data_re(grid, i_symbol, symbol_re_mask, config.rx_ports);
    const channel_equalizer::ch_est_list& ch_estimates = get_ch_data_estimates(
        est_results, i_symbol, config.nof_tx_layers, symbol_re_mask_local, dc_position_local, config.rx_ports);

    span<cf_t>  eq_re         = span<cf_t>(temp_eq_re).first(nof_re_symbol * config.nof_tx_layers);
    span<float> eq_noise_vars = span<float>(temp_eq_noise_vars).first(nof_re_symbol * config.nof_tx_layers);

    equalizer->equalize(
        eq_re, eq_noise_vars, ch_re, ch_estimates, span<float>(noise_var_estimates).first(nof_rx_ports), 1.0F);

    if (config.enable_transform_precoding) {
      ocudu_assert(config.nof_tx_layers == 1, "Transform precoding requires 1 layer.");
      precoder->deprecode_ofdm_symbol(eq_re, eq_re);
      precoder->deprecode_ofdm_symbol_noise(eq_noise_vars, eq_noise_vars);
    }

    accumulated_symbols_.insert(accumulated_symbols_.end(), eq_re.begin(), eq_re.end());
    accumulated_noise_vars_.insert(accumulated_noise_vars_.end(), eq_noise_vars.begin(), eq_noise_vars.end());
    accumulated_count_ += eq_re.size();

    if (compute_post_eq_sinr) {
      for (float var : eq_noise_vars) {
        if (!std::isinf(var)) {
          total_noise_var_accumulate += var;
          ++total_sinr_softbit_count;
        }
      }
    }
  }

  process_cpu_fallback(codeword_buffer, notifier, config);

  pusch_demodulator_notifier::demodulation_stats stats;
  if (compute_post_eq_sinr && (total_sinr_softbit_count != 0) && (total_noise_var_accumulate > 0.0F)) {
    stats.sinr_dB.emplace(
        -convert_power_to_dB(total_noise_var_accumulate / static_cast<float>(total_sinr_softbit_count)));
  } else {
    stats.sinr_dB.emplace(MAX_SINR_DB);
  }
  sanitize_stats_in_place(stats, static_cast<uint16_t>(config.rnti));
  notifier.on_end_stats(stats);
}

void pusch_demodulator_gpu_impl::process_cpu_fallback(pusch_codeword_buffer&      codeword_buffer,
                                                      pusch_demodulator_notifier& notifier,
                                                      const configuration&        config)
{
  // Initialize descrambler.
  unsigned c_init = static_cast<unsigned>(config.rnti) * pow2(15) + config.n_id;
  descrambler->init(c_init);

  size_t num_symbols = accumulated_count_;
  int    mod_order   = get_bits_per_symbol(config.modulation);

  // Process accumulated symbols.
  std::vector<log_likelihood_ratio> temp_llrs(num_symbols * mod_order);

  // Soft demodulation.
  demapper_fallback->demodulate_soft(
      temp_llrs, span<const cf_t>(accumulated_symbols_), span<const float>(accumulated_noise_vars_), config.modulation);

  // Feed to codeword buffer with descrambling.
  size_t offset = 0;
  while (offset < temp_llrs.size()) {
    size_t remain = std::min(temp_llrs.size() - offset, size_t(pusch_constants::MAX_NOF_BITS_PER_OFDM_SYMBOL));
    span<log_likelihood_ratio> codeword   = codeword_buffer.get_next_block_view(remain);
    size_t                     block_size = codeword.size();

    // Copy LLRs.
    std::copy(temp_llrs.begin() + offset, temp_llrs.begin() + offset + block_size, codeword.begin());

    // Generate and apply scrambling sequence.
    static_bit_buffer<pusch_constants::MAX_NOF_BITS_PER_OFDM_SYMBOL> scrambling_seq(block_size);
    descrambler->generate(scrambling_seq);

    // Descramble.
    for (size_t i = 0; i < block_size; ++i) {
      if (scrambling_seq.extract(i, 1) == 1) {
        codeword[i] = -codeword[i];
      }
    }

    codeword_buffer.on_new_block(codeword, scrambling_seq);
    offset += block_size;
  }
  host_codeword_written_ = true;
}

void pusch_demodulator_gpu_impl::finalize_resident_uci()
{
  if (!gpu_uci_demux_pending_ && (gpu_uci_llrs_valid_ || gpu_uci_decoded_valid_)) {
    return;
  }

  // In the normal GPU-resident path, this is called from the decoder pre-join
  // callback after the decoder has synchronized the same stream. If resident
  // decode is skipped, make this method safe by waiting only when the recorded
  // demodulator completion event is still pending.
  if (gpu_uci_demux_pending_) {
    const int buf_idx = last_buf_;
    if ((buf_idx >= 0) && kernel_complete_[buf_idx]) {
      cudaError_t query_status = cudaEventQuery(kernel_complete_[buf_idx]);
      if (query_status == cudaErrorNotReady) {
        cudaEventSynchronizeYielding(kernel_complete_[buf_idx]);
      }
    } else {
      cudaStreamSynchronizeYielding(stream_);
    }
  }

  const int                            buf_idx = last_buf_;
  const pusch_uci_short_decode_result* harq_decode_results =
      ((buf_idx >= 0) && h_harq_ack_decode_result_pinned_[buf_idx]) ? h_harq_ack_decode_result_pinned_[buf_idx]
                                                                    : h_harq_ack_decode_result_.data();
  const pusch_uci_short_decode_result* csi_decode_results =
      ((buf_idx >= 0) && h_csi_part1_decode_result_pinned_[buf_idx]) ? h_csi_part1_decode_result_pinned_[buf_idx]
                                                                     : h_csi_part1_decode_result_.data();
  const uint16_t* harq_llrs_half = ((buf_idx >= 0) && h_harq_ack_llrs_half_pinned_[buf_idx])
                                       ? h_harq_ack_llrs_half_pinned_[buf_idx]
                                       : h_harq_ack_llrs_half_.data();
  const uint16_t* csi_llrs_half  = ((buf_idx >= 0) && h_csi_part1_llrs_half_pinned_[buf_idx])
                                       ? h_csi_part1_llrs_half_pinned_[buf_idx]
                                       : h_csi_part1_llrs_half_.data();
  size_t harq_llrs_half_size     = (buf_idx >= 0) ? h_harq_ack_llrs_half_size_[buf_idx] : h_harq_ack_llrs_half_.size();
  size_t csi_llrs_half_size = (buf_idx >= 0) ? h_csi_part1_llrs_half_size_[buf_idx] : h_csi_part1_llrs_half_.size();

  auto publish_device_uci = [](span<const pusch_uci_short_decode_result>                 results,
                               unsigned                                                  nof_codeblocks,
                               unsigned                                                  expected_payload_bits,
                               std::array<uint8_t, uci_constants::MAX_NOF_PAYLOAD_BITS>& payload,
                               size_t&                                                   payload_size,
                               uci_status&                                               status) {
    if (nof_codeblocks == 0 || nof_codeblocks > results.size() || results[0].decoded == 0) {
      return false;
    }

    size_t offset = 0;
    bool   crc_ok = true;
    for (unsigned cb = 0; cb != nof_codeblocks; ++cb) {
      if (results[cb].decoded == 0) {
        return false;
      }
      crc_ok          = crc_ok && (results[cb].status != 0);
      size_t nof_bits = std::min<size_t>(results[cb].nof_bits, payload.size() - offset);
      for (size_t i = 0; i != nof_bits; ++i) {
        payload[offset + i] = results[cb].payload[i];
      }
      offset += nof_bits;
    }

    payload_size = std::min<size_t>(offset, expected_payload_bits);
    status       = (crc_ok && (offset == expected_payload_bits)) ? uci_status::valid : uci_status::invalid;
    return true;
  };

  bool need_host_uci_llrs = false;
  bool harq_ack_published =
      !h_harq_ack_re_indices_.empty() && gpu_uci_device_decode_requested_ &&
      publish_device_uci(span<const pusch_uci_short_decode_result>(harq_decode_results, MAX_GPU_UCI_CODEBLOCKS),
                         h_harq_ack_decode_nof_codeblocks_,
                         h_harq_ack_expected_payload_bits_,
                         h_harq_ack_payload_,
                         h_harq_ack_payload_size_,
                         h_harq_ack_status_);
  if (harq_ack_published) {
    gpu_harq_ack_decoded_ = true;
  } else {
    for (size_t i = 0; i < harq_llrs_half_size; ++i) {
      h_harq_ack_llrs_[i] = log_likelihood_ratio(fp16_to_int8_lut[harq_llrs_half[i]]);
    }
    need_host_uci_llrs = need_host_uci_llrs || !h_harq_ack_llrs_.empty();
  }

  bool csi_part1_published =
      !h_csi_part1_re_indices_.empty() && gpu_uci_device_decode_requested_ &&
      publish_device_uci(span<const pusch_uci_short_decode_result>(csi_decode_results, MAX_GPU_UCI_CODEBLOCKS),
                         h_csi_part1_decode_nof_codeblocks_,
                         h_csi_part1_expected_payload_bits_,
                         h_csi_part1_payload_,
                         h_csi_part1_payload_size_,
                         h_csi_part1_status_);
  if (csi_part1_published) {
    gpu_csi_part1_decoded_ = true;
  } else {
    for (size_t i = 0; i < csi_llrs_half_size; ++i) {
      h_csi_part1_llrs_[i] = log_likelihood_ratio(fp16_to_int8_lut[csi_llrs_half[i]]);
    }
    need_host_uci_llrs = need_host_uci_llrs || !h_csi_part1_llrs_.empty();
  }

  gpu_uci_llrs_valid_    = need_host_uci_llrs;
  gpu_uci_decoded_valid_ = gpu_harq_ack_decoded_ || gpu_csi_part1_decoded_;
  gpu_uci_demux_pending_ = false;

  // Diagnostic: when neither valid path produced output, the caller
  // (pusch_processor_impl.cpp:602-605) just logs a generic warning about
  // "UCI may have used fallback demux". That's too coarse — under repeat
  // failure there's no way to tell which subpath dropped. This log reports
  // the exact state so we can point the fix at the right condition.
  //
  // Rate-limited via a thread-local counter: first incident per thread fires
  // (catches the problem), then one every 512 slots (catches a running trend)
  // without flooding the log.
  if (!gpu_uci_llrs_valid_ && !gpu_uci_decoded_valid_) {
    thread_local unsigned diag_counter = 0;
    if ((diag_counter++ & 0x1ff) == 0) {
      ocudulog::fetch_basic_logger("PHY").info(
          "GPU UCI demux produced no output: harq_re={} csi_re={} device_decode_req={} "
          "harq_pub={} csi_pub={} harq_llrs_half={} csi_llrs_half={}",
          h_harq_ack_re_indices_.size(),
          h_csi_part1_re_indices_.size(),
          gpu_uci_device_decode_requested_,
          harq_ack_published,
          csi_part1_published,
          harq_llrs_half_size,
          csi_llrs_half_size);
    }
  }
}

bool pusch_demodulator_gpu_impl::install_resident_dapp(std::shared_ptr<pusch_resident_dapp> dapp)
{
  if (!dapp || resident_dapp_ || !stream_) return false;
  auto context = capture_native_cuda_control_context(reinterpret_cast<uintptr_t>(stream_));
  if (!context || !dapp->bind_control_context(std::move(context))) return false;
  resident_dapp_ = std::move(dapp);
  return true;
}

float pusch_demodulator_gpu_impl::report_deferred_sinr(pusch_demodulator_notifier& notifier)
{
  // Test the current fence even after a failed submission: the first of two
  // readbacks may still be queued. Never inspect stale host results on failure.
  const bool valid_buffer = last_buf_ >= 0 && last_buf_ < NUM_BUFFERS;
  cudaEvent_t completion_event = valid_buffer ? d2h_complete_[last_buf_] : nullptr;
  if (completion_event == nullptr && valid_buffer && !last_sinr_on_side_stream_) {
    completion_event = kernel_complete_[last_buf_];
  }
  last_metrics_submission_ok_ =
      pusch_metric_readback_completed(last_metrics_submission_ok_ && valid_buffer,
                                     last_metrics_fence_recorded_,
                                     last_sinr_on_side_stream_,
                                     completion_event,
                                     last_sinr_on_side_stream_ ? d2h_stream_ : stream_);
  if (!last_metrics_submission_ok_) {
    notifier.on_end_stats({});
    return 0.0F;
  }
  // Class-A equalizer dApp inline: E2E SINR D2H was skipped; after decoder
  // stream sync, derive CSI from post-EQ noise for CRC / scheduler feedback.
  if (last_dapp_inline_) {
    notifier.on_end_stats(read_completed_inline_stats());
    return 0.0F;
  }
  // Read SINR from pinned memory. The async D2H was launched before kernel_complete_
  // event was recorded. After the decoder's cudaStreamSynchronize, the E2E kernel
  // (and its SINR D2H) are guaranteed complete.
  //
  // If the SINR readback was routed to the side D2H stream (OCUDU_PUSCH_SINR_SIDE_STREAM),
  // the decoder's compute-stream sync does not order it; wait on the recorded completion
  // event (the tiny copies finish during decode, so this is effectively a no-op query).
  float gpu_sinr_db    = pusch_e2e_post_eq_sinr_get_result(e2e_handle_);
  float gpu_ch_sinr_db = pusch_e2e_channel_estimator_sinr_get_result(e2e_handle_);
  float gpu_epre_db    = pusch_e2e_epre_get_result(e2e_handle_);
  float gpu_rsrp_db    = pusch_e2e_rsrp_get_result(e2e_handle_);
  float gpu_ta_s       = pusch_e2e_ta_get_result(e2e_handle_);
  float gpu_cfo_hz     = pusch_e2e_cfo_get_result(e2e_handle_);
  float gpu_evm        = pusch_e2e_evm_get_result(e2e_handle_);

  auto stats = make_pusch_e2e_demodulation_stats(
      gpu_sinr_db, gpu_ch_sinr_db, gpu_epre_db, gpu_rsrp_db, gpu_ta_s, gpu_cfo_hz, gpu_evm);
  sanitize_stats_in_place(stats, last_rnti_);
  notifier.on_end_stats(stats);

  return 0.0F;
}

pusch_demodulator_notifier::demodulation_stats pusch_demodulator_gpu_impl::read_completed_inline_stats() const
{
  if (!last_metrics_submission_ok_) {
    return {};
  }
  // No getters before the immediate/deferred caller has checked completion.
  float post_eq_sinr = NAN;
  if (last_dapp_post_eq_noise_valid_) {
    static_cast<void>(post_eq_sinr_db_from_mean_noise(pusch_e2e_dapp_noise_get_mean(e2e_handle_), post_eq_sinr));
  }
  auto stats = make_pusch_e2e_demodulation_stats(
      post_eq_sinr,
      last_common_pilot_csi_ ? pusch_e2e_channel_estimator_sinr_get_result(e2e_handle_) : NAN,
      last_common_pilot_csi_ ? pusch_e2e_epre_get_result(e2e_handle_) : NAN,
      last_common_pilot_csi_ ? pusch_e2e_rsrp_get_result(e2e_handle_) : NAN,
      last_common_pilot_csi_ ? pusch_e2e_ta_get_result(e2e_handle_) : NAN,
      last_common_pilot_csi_ ? pusch_e2e_cfo_get_result(e2e_handle_) : NAN,
      last_dapp_evm_valid_ ? pusch_e2e_evm_get_result(e2e_handle_) : NAN);
  stats = select_pusch_inline_demodulation_stats(
      stats, true, last_common_pilot_csi_, last_dapp_post_eq_noise_valid_, last_dapp_evm_valid_);
  sanitize_stats_in_place(stats, last_rnti_);
  return stats;
}

void pusch_demodulator_gpu_impl::report_deferred_dapp_completion()
{
  if (resident_dapp_ != nullptr && last_buf_ >= 0) {
    resident_dapp_->observe_inline_completion(static_cast<uint32_t>(last_buf_));
  }
}
