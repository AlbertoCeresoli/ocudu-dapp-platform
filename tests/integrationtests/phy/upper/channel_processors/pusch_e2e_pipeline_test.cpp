// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief E2E GPU Pipeline Test: Feeds identical RX grid to CPU and full GPU E2E pipelines,
/// compares TB output byte-for-byte, and reports detailed latency breakdown.
///
/// The GPU E2E path auto-routes inside pusch_processor_impl::process() when both
/// the GPU demodulator and GPU batch decoder are available (pxsch_type = "gpu").

#include "pxsch_bler_test_channel_emulator.h"
#include "pxsch_bler_test_factories.h"
#include "pusch_test_cold_trace.h"
#include "pusch_test_payload_buffer.h"
#include "pusch_test_failure_trace.h"
#include "pusch_test_pooled_processor.h"
#ifdef OCUDU_PUSCH_TEST_DAPP
#include "pusch_test_dapp_fixture.h"
#include "pusch_test_receiver_retirement_trace.h"
#include "pusch_test_receiver_capture.h"
#include "ocudu/dapp/runtime/artifact_verifier.h"
#endif
#include "ocudu/ocuduvec/bit.h"
#include "ocudu/ocuduvec/sc_prod.h"
#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_modulation/channel_modulation_factories.h"
#include "ocudu/phy/upper/channel_processors/pdsch/pdsch_encoder.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_decoder_result.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_processor_result_notifier.h"
#include "ocudu/phy/upper/rx_buffer_pool.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/trx_buffer_identifier.h"
#include "ocudu/phy/upper/unique_rx_buffer.h"
#include "ocudu/ran/precoding/precoding_codebooks.h"
#include "ocudu/ran/pusch/pusch_mcs.h"
#include "ocudu/ran/resource_allocation/rb_interval.h"
#include "ocudu/ran/sch/sch_dmrs_power.h"
#include "ocudu/ran/sch/sch_mcs.h"
#include "ocudu/ran/sch/sch_segmentation.h"
#include "ocudu/ran/sch/tbs_calculator.h"
#include "ocudu/ran/transform_precoding/transform_precoding_helpers.h"
#include "ocudu/support/executors/task_worker_pool.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef ENABLE_CUDA
#include "pusch_test_cuda_context.h"
#include "pusch_transform_deprecoder_selection.h"
#include "cuda/cuda_rt_utils.h"
#include "cuda/pusch_device_grid_reader_cuda.h"
#include "lib/phy/upper/resource_grid_cuda_visible_impl.h"
#include <cuda_runtime_api.h>
#endif

using namespace ocudu;

static symbol_slot_mask make_dmrs_symbol_mask(const std::vector<unsigned>& symbols)
{
  symbol_slot_mask mask(MAX_NSYMB_PER_SLOT);
  for (unsigned symbol : symbols) {
    if (symbol >= MAX_NSYMB_PER_SLOT) {
      throw std::runtime_error(fmt::format("Invalid DMRS symbol index {}.", symbol));
    }
    mask.set(symbol);
  }
  if (mask.none() || mask.count() > 4) {
    throw std::runtime_error(fmt::format("Expected 1-4 DMRS symbols, got {}.", mask.count()));
  }
  return mask;
}

static symbol_slot_mask parse_dmrs_symbols(const std::string& value)
{
  std::vector<unsigned> symbols;
  std::stringstream     ss(value);
  std::string           token;
  while (std::getline(ss, token, ',')) {
    if (!token.empty()) {
      symbols.push_back(static_cast<unsigned>(std::stoul(token)));
    }
  }
  return make_dmrs_symbol_mask(symbols);
}

static std::string dmrs_symbols_to_string(const symbol_slot_mask& mask)
{
  std::string result;
  for (unsigned i = 0; i != mask.size(); ++i) {
    if (!mask.test(i)) {
      continue;
    }
    if (!result.empty()) {
      result += ",";
    }
    result += std::to_string(i);
  }
  return result;
}

static dmrs_config_type parse_dmrs_type(const std::string& value)
{
  if (value == "type1" || value == "TYPE1" || value == "1") {
    return dmrs_config_type::type1;
  }
  if (value == "type2" || value == "TYPE2" || value == "2") {
    return dmrs_config_type::type2;
  }
  throw std::runtime_error(fmt::format("Invalid DMRS type '{}'. Expected type1 or type2.", value));
}

// Test parameters (compile-time defaults).
static constexpr subcarrier_spacing scs                         = subcarrier_spacing::kHz30;
static uint16_t                     rnti                        = 0x1234;
static constexpr unsigned           bwp_start_rb                = 0;
static constexpr unsigned           nof_ofdm_symbols            = 14;
static symbol_slot_mask             dmrs_symbols_mask           = make_dmrs_symbol_mask({2, 11});
static constexpr unsigned           nof_ldpc_iterations         = 6;
static dmrs_config_type             dmrs                        = dmrs_config_type::type1;
static constexpr unsigned           nof_cdm_groups_without_data = 2;
static constexpr cyclic_prefix      cy_prefix                   = cyclic_prefix::NORMAL;
static constexpr unsigned           rv                          = 0;
static unsigned                     n_id                        = 0;
static unsigned                     scrambling_id               = 0;
static bool                         n_scid                      = false;
static constexpr bool               use_early_stop              = true;
static pusch_mcs_table              mcs_table                   = pusch_mcs_table::qam64;
static bool                         enable_transform_precoding  = false;
static std::string                  transform_deprecoder_backend;
static unsigned                     n_rs_id = 0;
static std::optional<uint32_t>      payload_seed;

static uint32_t parse_u32(const char* text, uint32_t maximum)
{
  uint32_t   value{};
  const auto end    = text + std::strlen(text);
  const auto parsed = std::from_chars(text, end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end || value > maximum) {
    throw std::runtime_error(std::string("Invalid unsigned value: ") + text);
  }
  return value;
}

static double epoch_seconds()
{
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// CLI-configurable antenna/layer parameters.
static unsigned nof_layers   = 1;
static unsigned nof_rx_ports = 1;

// CLI-configurable parameters.
static unsigned nof_iterations       = 100;
static unsigned nof_warmup           = 5;
static unsigned cpu_every            = 1;
static float    max_ta_offset_us     = 0.0f;
static float    cfo_std_hz           = 0.0f;
static bool     quiet_mode           = false;
static bool     debug_logs           = false;
static bool     require_gpu_resident = false;
// Default to platform auto policy: managed CUDA-visible RX grids on integrated
// GPUs (e.g. GB10), pinned/host fallback on discrete. Matches production
// resource_grid_cuda_visible_factory auto selection. Override with --rx-device-grid.
#ifdef ENABLE_CUDA
static std::string rx_device_grid_mode = "auto";
#else
static std::string rx_device_grid_mode;
#endif
static std::string               gpu_output_buffer_mode = "vector";
static std::string               dapp_manifest;
static std::string               sample_jsonl_path;
static std::string               cold_jsonl_path;
static std::string               receiver_capture_prefix;
static unsigned                  benchmark_workers = 0;
static std::chrono::milliseconds iteration_pause{0};
static std::chrono::milliseconds post_teardown_hold{0};
static std::chrono::milliseconds phase_observation{0};
static bool                      require_all_tb = false;
static bool                      decoded_sensitivity = false;
static std::string               channel_profile = "single-tap";
static std::string               fading_distribution = "uniform-phase";
static bool                      receiver_completion = false;
static bool                      cuda_owned_context = false;
static bool                      cuda_init_only = false;
static bool                      trace_terminate = false;
static bool                      require_pilot_csi = false;
static channel_state_information::sinr_type sinr_method = channel_state_information::sinr_type::post_equalization;

namespace {

#ifdef ENABLE_CUDA
class cuda_stream_guard
{
public:
  cuda_stream_guard()
  {
    cudaError_t status = ocudu::cudaStreamCreateUpperPhy(&stream, "OCUDU_PUSCH_CUDA_STREAM_PRIORITY");
    if (status != cudaSuccess) {
      throw std::runtime_error(
          fmt::format("Failed to create CUDA grid-producer stream: {}", cudaGetErrorString(status)));
    }
  }

  ~cuda_stream_guard()
  {
    if (stream != nullptr) {
      (void)cudaStreamSynchronize(stream);
      (void)cudaStreamDestroy(stream);
    }
  }

  cudaStream_t get() const { return stream; }

private:
  cudaStream_t stream = nullptr;
};
#endif

class pusch_processor_notifier_adaptor : public pusch_processor_result_notifier
{
public:
  void on_uci(const pusch_processor_result_control& uci_) override {}

  void on_sch(const pusch_processor_result_data& sch_) override
  {
    std::unique_lock<std::mutex> lock(mutex);
    completed = true;
    sch       = sch_;
    cvar.notify_all();
  }

  const pusch_processor_result_data& wait_for_completion()
  {
    std::unique_lock<std::mutex> lock(mutex);
    while (!completed) {
      cvar.wait(lock);
    }
    return sch;
  }

  void reset()
  {
    std::unique_lock<std::mutex> lock(mutex);
    completed = false;
  }

  bool                        completed = false;
  pusch_processor_result_data sch;
  std::mutex                  mutex;
  std::condition_variable     cvar;
};

class pdsch_processor_notifier_adaptor : public pdsch_processor_notifier
{
public:
  void on_finish_processing() override { completed = true; }

  void wait_for_completion()
  {
    while (!completed.load()) {
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  }

  void reset() { completed = false; }

private:
  std::atomic<bool> completed = {false};
};

class pusch_transform_precoding_tx_generator
{
public:
  pusch_transform_precoding_tx_generator()
  {
    auto                                   ldpc_encoder_factory = create_ldpc_encoder_factory_sw("auto");
    auto                                   rate_matcher_factory = create_ldpc_rate_matcher_factory_sw();
    auto                                   crc_factory          = create_crc_calculator_factory_sw("auto");
    auto                                   segmenter_factory    = create_ldpc_segmenter_tx_factory_sw(crc_factory);
    pdsch_encoder_factory_sw_configuration enc_cfg;
    enc_cfg.encoder_factory      = ldpc_encoder_factory;
    enc_cfg.rate_matcher_factory = rate_matcher_factory;
    enc_cfg.segmenter_factory    = segmenter_factory;

    auto pdsch_encoder_factory = create_pdsch_encoder_factory_sw(enc_cfg);
    encoder                    = pdsch_encoder_factory->create();
    modulator                  = create_modulation_mapper_factory()->create();
    scrambler                  = create_pseudo_random_generator_sw_factory()->create();
    low_papr_generator         = create_low_papr_sequence_generator_sw_factory()->create();
    dft_factory                = create_dft_processor_factory_generic();
  }

  void process(resource_grid&                      grid,
               span<const uint8_t>                 transport_block,
               const pdsch_encoder::configuration& encoder_config,
               const prb_interval&                 freq_allocation,
               const symbol_slot_mask&             dmrs_symbols,
               uint16_t                            rnti,
               unsigned                            n_id,
               unsigned                            n_rs_id,
               modulation_scheme                   modulation)
  {
    unsigned nof_prb  = freq_allocation.length();
    unsigned dft_size = nof_prb * NOF_SUBCARRIERS_PER_RB;
    unsigned nof_bits = encoder_config.nof_ch_symbols * get_bits_per_symbol(modulation);

    codeword.resize(nof_bits);
    scrambled_bits.resize(nof_bits);
    packed_bits.resize(nof_bits);
    data_symbols.resize(encoder_config.nof_ch_symbols);
    dft_out.resize(dft_size);
    dmrs_sequence.resize(nof_prb * get_nof_re_per_prb(dmrs));

    encoder->encode(codeword, transport_block, encoder_config);

    scrambler->init(rnti * pow2(15) + n_id);
    scrambler->apply_xor(scrambled_bits, codeword);
    ocuduvec::bit_pack(packed_bits, scrambled_bits);
    modulator->modulate(data_symbols, packed_bits, modulation);

    dft_processor::configuration dft_cfg;
    dft_cfg.dir  = dft_processor::direction::DIRECT;
    dft_cfg.size = dft_size;
    auto  dft    = dft_factory->create(dft_cfg);
    float scale  = 1.0F / std::sqrt(static_cast<float>(dft_size));

    grid.set_all_zero();
    resource_grid_writer&               writer      = grid.get_writer();
    unsigned                            k_init      = freq_allocation.start() * NOF_SUBCARRIERS_PER_RB;
    unsigned                            data_offset = 0;
    bounded_bitset<MAX_NOF_SUBCARRIERS> dmrs_re_mask(dft_size);
    for (unsigned i_prb = 0; i_prb != nof_prb; ++i_prb) {
      for (unsigned k = 0; k != NOF_SUBCARRIERS_PER_RB; k += 2) {
        dmrs_re_mask.set(i_prb * NOF_SUBCARRIERS_PER_RB + k);
      }
    }

    for (unsigned symbol = 0; symbol != nof_ofdm_symbols; ++symbol) {
      if (dmrs_symbols.test(symbol)) {
        low_papr_generator->generate(dmrs_sequence, n_rs_id % 30, 0, 0, 1);
        writer.put(0, symbol, k_init, dmrs_re_mask, dmrs_sequence);
        continue;
      }

      span<const cf_t> qam_symbol = span<const cf_t>(data_symbols).subspan(data_offset, dft_size);
      std::copy(qam_symbol.begin(), qam_symbol.end(), dft->get_input().begin());
      ocuduvec::sc_prod(dft_out, dft->run(), scale);
      writer.put(0, symbol, k_init, dft_out);
      data_offset += dft_size;
    }

    ocudu_assert(data_offset == data_symbols.size(),
                 "Expected to map {} transform-precoded symbols, mapped {}.",
                 data_symbols.size(),
                 data_offset);
  }

private:
  std::unique_ptr<pdsch_encoder>               encoder;
  std::unique_ptr<modulation_mapper>           modulator;
  std::unique_ptr<pseudo_random_generator>     scrambler;
  std::unique_ptr<low_papr_sequence_generator> low_papr_generator;
  std::shared_ptr<dft_processor_factory>       dft_factory;
  std::vector<uint8_t>                         codeword;
  std::vector<uint8_t>                         scrambled_bits;
  dynamic_bit_buffer                           packed_bits;
  std::vector<cf_t>                            data_symbols;
  std::vector<cf_t>                            dft_out;
  std::vector<cf_t>                            dmrs_sequence;
};

struct latency_stats {
  double   min_us = 1e9;
  double   max_us = 0;
  double   sum_us = 0;
  unsigned count  = 0;

  void add(double us)
  {
    min_us = std::min(min_us, us);
    max_us = std::max(max_us, us);
    sum_us += us;
    count++;
  }

  double mean() const { return count > 0 ? sum_us / count : 0; }
};

struct csi_metric_stats {
  double   sum       = 0.0;
  unsigned count     = 0;
  unsigned populated = 0;

  void add(std::optional<float> val)
  {
    ++count;
    if (val.has_value()) {
      sum += static_cast<double>(val.value());
      ++populated;
    }
  }

  bool   has_data() const { return populated > 0; }
  double mean() const { return populated > 0 ? sum / populated : 0.0; }
  double population_rate() const { return count > 0 ? 100.0 * populated / count : 0.0; }
};

struct test_results {
  unsigned cpu_pass              = 0;
  unsigned cpu_fail              = 0;
  unsigned gpu_pass              = 0;
  unsigned gpu_fail              = 0;
  unsigned disagreements         = 0;
  unsigned byte_mismatches       = 0;
  unsigned output_guard_failures = 0;
  unsigned gpu_truth_mismatches  = 0;
  unsigned gpu_e2e_count         = 0; ///< Number of iterations with resident GPU pipeline timing.
  unsigned gpu_device_grid_count = 0; ///< Number of iterations that consumed the producer-owned device grid.
  unsigned gpu_owned_grid_count  = 0; ///< Number of iterations that consumed the immutable owned snapshot.

  latency_stats cpu_latency;
  latency_stats gpu_latency;
  double        cpu_iters_sum   = 0;
  double        gpu_iters_sum   = 0;
  unsigned      iters_count     = 0;
  unsigned      cpu_iters_count = 0;

  // CSI metric parity tracking.
  csi_metric_stats cpu_sinr, gpu_sinr;
  csi_metric_stats cpu_evm, gpu_evm;
  csi_metric_stats cpu_epre, gpu_epre;
  csi_metric_stats cpu_ta, gpu_ta;
  csi_metric_stats cpu_rsrp, gpu_rsrp;
  csi_metric_stats cpu_cfo, gpu_cfo;
  csi_metric_stats cpu_ta_error, gpu_ta_error;
  csi_metric_stats cpu_cfo_error, gpu_cfo_error;

  // GPU pipeline timing breakdown.
  latency_stats gpu_grid_staging;
  latency_stats gpu_demod_sync;
  latency_stats gpu_deinterleave;
  latency_stats gpu_rate_dematch;
  latency_stats gpu_ldpc_decode;
  latency_stats gpu_crc_check;
  latency_stats gpu_d2h_transfer;
  latency_stats gpu_extract_bits;
  latency_stats gpu_ch_estimate;
  latency_stats gpu_process_data_setup;
  latency_stats gpu_demod_call;
  latency_stats gpu_decode_call;
  latency_stats gpu_join_notify;
  latency_stats gpu_sinr_readback;
  latency_stats gpu_decoder_cfg;
  latency_stats gpu_rm_cfg;
  latency_stats gpu_completion_wait;
  latency_stats gpu_tb_output_copy;
  latency_stats gpu_avg_iters_query;
};

void run_e2e_pipeline_test(unsigned nof_prb, float sinr_dB, sch_mcs_index mcs_idx,
                           test_support::pusch_cold_trace& cold_trace)
{
  fmt::print("\n=== E2E GPU Pipeline Test ===\n");
  fmt::print(
      "Config: {} PRB, {:.1f} dB SINR, MCS {} (table={}), {} layer(s), {} RX port(s), DMRS {} [{}], "
      "transform_precoding={}, {} iterations, {} warmup, rx_device_grid={}, gpu_output={}{}{}\n\n",
      nof_prb,
      sinr_dB,
      mcs_idx,
      pusch_mcs_table_to_string(mcs_table),
      nof_layers,
      nof_rx_ports,
      to_string(dmrs),
      dmrs_symbols_to_string(dmrs_symbols_mask),
      enable_transform_precoding
          ? fmt::format("on ({})", transform_deprecoder_backend.empty() ? "default" : transform_deprecoder_backend)
          : "off",
      nof_iterations,
      nof_warmup,
      rx_device_grid_mode.empty() ? "off" : rx_device_grid_mode,
      gpu_output_buffer_mode,
      max_ta_offset_us > 0.0f ? fmt::format(", TA offset: +/-{:.1f}us", max_ta_offset_us) : "",
      cfo_std_hz > 0.0f ? fmt::format(", CFO std: {:.1f}Hz", cfo_std_hz) : "");

  test_results                                   results;
  const uint32_t                                 selected_seed = payload_seed ? *payload_seed : std::random_device{}();
  const auto                                     close_samples = [](FILE* file) { std::fclose(file); };
  std::unique_ptr<FILE, decltype(close_samples)> samples(nullptr, close_samples);
  if (!sample_jsonl_path.empty()) {
    samples.reset(std::fopen(sample_jsonl_path.c_str(), "wx"));
    if (!samples)
      throw std::runtime_error("A new writable sample JSONL path is required.");
    fmt::print(
        samples.get(),
        "{{\"kind\":\"start\",\"prbs\":{},\"mcs\":{},\"iterations\":{},\"warmups\":{},\"dapp\":{},\"pace_ms\":{},"
        "\"epoch_s\":{},\"seed\":{},\"sinr_dB\":{},\"mcs_table\":\"{}\",\"workers\":{},\"ports\":{},\"layers\":{},"
        "\"receiver_completion\":{},\"require_pilot_csi\":{},\"common_pilot_csi_requested\":{},\"sinr_method\":\"{}\","
        "\"decoded_sensitivity\":{},\"require_all_tb\":{},\"channel_profile\":\"{}\","
        "\"fading_distribution\":\"{}\",\"post_teardown_hold_ms\":{},\"phase_observation_ms\":{},\"diagnostic_capture\":{}}}\n",
        nof_prb,
        mcs_idx,
        nof_iterations,
        nof_warmup,
        !dapp_manifest.empty(),
        iteration_pause.count(),
        epoch_seconds(),
        selected_seed,
        sinr_dB,
        pusch_mcs_table_to_string(mcs_table),
        benchmark_workers,
        nof_rx_ports,
        nof_layers,
        receiver_completion,
        require_pilot_csi,
        require_pilot_csi,
        sinr_method == channel_state_information::sinr_type::channel_estimator ? "channel_estimator" : "post_equalization",
        decoded_sensitivity,
        require_all_tb,
        channel_profile,
        fading_distribution,
        post_teardown_hold.count(),
        phase_observation.count(),
        !receiver_capture_prefix.empty());
    std::fflush(samples.get());
  }

  // Setup thread pool.
  unsigned max_nof_threads = benchmark_workers ? benchmark_workers : std::min(8U, std::thread::hardware_concurrency());
  auto worker_pool = cold_trace.measure("executor_create", [&] {
    return std::make_unique<task_worker_pool<concurrent_queue_policy::locking_mpmc>>("thread", max_nof_threads, 1024);
  });
  auto executor = std::make_unique<task_worker_pool_executor<concurrent_queue_policy::locking_mpmc>>(*worker_pool);

  ocudulog::fetch_basic_logger("ALL").set_level(debug_logs ? ocudulog::basic_levels::debug
                                                           : ocudulog::basic_levels::warning);

  if (enable_transform_precoding) {
    if (nof_layers != 1) {
      throw std::runtime_error("PUSCH transform precoding is only valid with one UL layer.");
    }
    if (dmrs != dmrs_config_type::type1) {
      throw std::runtime_error("This transform-precoded PUSCH test uses low-PAPR DMRS and requires --dmrs-type type1.");
    }
    if (!transform_precoding::is_nof_prbs_valid(nof_prb)) {
      throw std::runtime_error(fmt::format("{} PRB is not a valid transform-precoding DFT size.", nof_prb));
    }
  }

  sch_mcs_description mcs_descr       = pusch_mcs_get_config(mcs_table, mcs_idx, enable_transform_precoding, false);
  prb_interval        freq_allocation = {bwp_start_rb, bwp_start_rb + nof_prb};

  tbs_calculator_configuration tbs_config = {};
  tbs_config.mcs_descr                    = mcs_descr;
  tbs_config.n_prb                        = freq_allocation.length();
  tbs_config.nof_layers                   = nof_layers;
  tbs_config.nof_symb_sh                  = nof_ofdm_symbols;
  tbs_config.nof_dmrs_prb                 = enable_transform_precoding
                                                ? NOF_SUBCARRIERS_PER_RB * dmrs_symbols_mask.count()
                                                : get_nof_re_per_prb(dmrs) * dmrs_symbols_mask.count() * nof_cdm_groups_without_data;
  unsigned tbs                            = tbs_calculator_calculate(tbs_config).to_bits().value();

  ldpc_base_graph_type ldpc_base_graph =
      get_ldpc_base_graph(mcs_descr.get_normalised_target_code_rate(), units::bits(tbs));

  rb_allocation freq_alloc = rb_allocation::make_type1(freq_allocation.start(), freq_allocation.length(), std::nullopt);

  // Create factories.
#ifdef OCUDU_PUSCH_TEST_DAPP
  // Declared before all processors/factories so their worker references retire
  // before the isolated service is destroyed, including exception unwinding.
  std::unique_ptr<test_support::pusch_test_dapp_fixture> dapp_fixture;
#endif
  pxsch_test_dapp_config dapp_hooks;
#ifdef OCUDU_PUSCH_TEST_DAPP
  if (!dapp_manifest.empty()) {
    dapp_fixture = cold_trace.measure("dapp_service_create", [&] {
      return std::make_unique<test_support::pusch_test_dapp_fixture>(
          dapp_manifest, max_nof_threads + 1, nof_prb, nof_rx_ports, nof_layers, receiver_completion);
    });
    dapp_hooks = dapp_fixture->factory_config();
  }
#endif
  cold_trace.record("cpu_factories_create", "begin");
  std::shared_ptr<resource_grid_factory> grid_factory = create_resource_grid_factory();

  std::shared_ptr<pdsch_processor_factory> pdsch_factory =
      create_sw_pdsch_processor_factory(*executor, max_nof_threads + 1, "", "auto");

  std::shared_ptr<pusch_processor_factory> cpu_factory =
      create_sw_pusch_processor_factory(*executor,
                                        max_nof_threads + 1,
                                        nof_ldpc_iterations,
                                        use_early_stop,
                                        "auto",
                                        port_channel_estimator_td_interpolation_strategy::average,
                                        channel_equalizer_algorithm_type::mmse,
                                        {},
                                        nof_prb,
                                        nof_layers,
                                        sinr_method,
                                        nof_rx_ports,
                                        &cold_trace);
  cold_trace.record("cpu_factories_create", "end");

  std::shared_ptr<pusch_processor_factory> gpu_factory = cold_trace.measure("gpu_factory_create", [&] {
    return create_sw_pusch_processor_factory(*executor,
                                             max_nof_threads + 1,
                                             nof_ldpc_iterations,
                                             use_early_stop,
                                             "gpu",
                                             port_channel_estimator_td_interpolation_strategy::average,
                                             channel_equalizer_algorithm_type::mmse,
                                             dapp_hooks,
                                             nof_prb,
                                             nof_layers,
                                             sinr_method,
                                             nof_rx_ports,
                                             &cold_trace);
  });

  if (!gpu_factory) {
    worker_pool->stop();
    throw std::runtime_error("GPU PUSCH factory not available.");
  }

  // Create processors.
  cold_trace.record("cpu_processors_create", "begin");
  auto pdsch_proc = pdsch_factory->create();
  auto cpu_proc   = cpu_factory->create();
  cold_trace.record("cpu_processors_create", "end");
  auto gpu_proc = cold_trace.measure("gpu_processor_create", [&] {
    return std::make_unique<pusch_test_pooled_processor>(gpu_factory->create());
  });
#ifdef OCUDU_PUSCH_TEST_DAPP
  if (dapp_fixture)
    cold_trace.measure("dapp_activate_and_bind", [&] { dapp_fixture->activate(); });
#endif
  auto transform_tx_generator =
      enable_transform_precoding ? std::make_unique<pusch_transform_precoding_tx_generator>() : nullptr;

  // Create grids. Paired markers survive a killed process without changing the sample schema.
  cold_trace.record("grid_create", "begin");
  auto                           tx_grid = grid_factory->create(nof_layers, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
  std::unique_ptr<resource_grid> rx_grid;
#ifdef ENABLE_CUDA
  std::unique_ptr<resource_grid>     direct_managed_rx_grid;
  std::unique_ptr<cuda_stream_guard> direct_managed_producer_stream;
  if (rx_device_grid_mode == "direct-managed") {
    auto managed_grid =
        std::make_unique<cuda_visible_resource_grid>(nof_rx_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
    if (!managed_grid->is_valid()) {
      throw std::runtime_error("Failed to allocate direct managed PUSCH RX resource grid.");
    }
    // Keep the CPU reference on an independent host grid. A CPU decode of the
    // same managed allocation would migrate it home before the GPU decode and
    // invalidate the producer->consumer residency contract we intend to test.
    rx_grid                        = grid_factory->create(nof_rx_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
    direct_managed_rx_grid         = std::move(managed_grid);
    direct_managed_producer_stream = std::make_unique<cuda_stream_guard>();
  } else if (rx_device_grid_mode == "auto") {
    resource_grid_cuda_visible_factory cuda_grid_factory(resource_grid_cuda_visible_factory::direction::uplink);
    rx_grid = cuda_grid_factory.create(nof_rx_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
  } else
#endif
  {
    rx_grid = grid_factory->create(nof_rx_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
  }

#ifdef ENABLE_CUDA
  std::unique_ptr<pusch_device_grid_reader_cuda> gpu_device_grid_reader;
  if (!rx_device_grid_mode.empty() && (rx_device_grid_mode != "direct-managed") && (rx_device_grid_mode != "auto")) {
    auto allocation_mode   = (rx_device_grid_mode == "managed")
                                 ? pusch_device_grid_reader_cuda::grid_allocation_mode::managed
                                 : pusch_device_grid_reader_cuda::grid_allocation_mode::device;
    gpu_device_grid_reader = std::make_unique<pusch_device_grid_reader_cuda>(rx_grid->get_reader(), allocation_mode);
    if (!gpu_device_grid_reader->supports_device_grid_reading()) {
      throw std::runtime_error("Failed to allocate PUSCH RX CUDA-visible grid.");
    }
  }
#endif

  cold_trace.record("grid_create", "end");

  unsigned nof_codeblocks = compute_nof_codeblocks(units::bits(tbs), ldpc_base_graph);

  rx_buffer_pool_config pool_config;
  pool_config.max_codeblock_size   = ldpc::MAX_CODEBLOCK_SIZE;
  pool_config.nof_buffers          = 2;
  pool_config.nof_codeblocks       = nof_codeblocks;
  pool_config.expire_timeout_slots = 10;
  pool_config.external_soft_bits   = false;

  auto cpu_pool = create_rx_buffer_pool(pool_config);
  auto gpu_pool = create_rx_buffer_pool(pool_config);

  // Channel emulator.
  channel_emulator emulator(channel_profile,
                            fading_distribution,
                            sinr_dB,
                            0.0f,
                            0,
                            nof_layers,
                            nof_rx_ports,
                            nof_prb * NOF_SUBCARRIERS_PER_RB,
                            nof_ofdm_symbols,
                            max_nof_threads,
                            scs,
                            *executor);

  std::mt19937                          rgen(selected_seed);
  std::vector<cf_t>                     ta_symbol_buffer(nof_prb * NOF_SUBCARRIERS_PER_RB);
  std::uniform_real_distribution<float> ta_dist(-max_ta_offset_us, max_ta_offset_us);
  std::normal_distribution<float>       cfo_dist(0.0f, std::max(cfo_std_hz, 1e-9f));
  float                                 scs_hz = 15000.0f * static_cast<float>(1 << to_numerology_value(scs));

  // Precompute OFDM symbol start times for CFO injection.
  std::array<double, MAX_NSYMB_PER_SLOT> symbol_start_times_s{};
  {
    double symbol_duration_s = 1.0 / static_cast<double>(scs_hz);
    symbol_start_times_s[0]  = cy_prefix.get_length(0, scs).to_seconds();
    for (unsigned i = 1; i < MAX_NSYMB_PER_SLOT; ++i) {
      symbol_start_times_s[i] =
          symbol_start_times_s[i - 1] + cy_prefix.get_length(i, scs).to_seconds() + symbol_duration_s;
    }
  }

  fmt::print("TBS: {} bits ({} bytes), Base Graph: {}, Codeblocks: {}, Modulation: {}\n\n",
             tbs,
             tbs / 8,
             ldpc_base_graph == ldpc_base_graph_type::BG1 ? "BG1" : "BG2",
             nof_codeblocks,
             to_string(mcs_descr.modulation));

  pdsch_encoder::configuration transform_encoder_config;
  if (enable_transform_precoding) {
    transform_encoder_config.base_graph = ldpc_base_graph;
    transform_encoder_config.rv         = rv;
    transform_encoder_config.mod        = mcs_descr.modulation;
    transform_encoder_config.Nref       = 0;
    transform_encoder_config.nof_layers = 1;
    transform_encoder_config.nof_ch_symbols =
        (nof_ofdm_symbols - dmrs_symbols_mask.count()) * nof_prb * NOF_SUBCARRIERS_PER_RB;
  }

  // === Main test loop ===
  // Warmup iterations run first (nof_warmup), then measured iterations (nof_iterations).
  // GPU E2E validation is checked after first warmup completes.
  unsigned total_iterations  = nof_warmup + nof_iterations;
  bool     gpu_e2e_validated = false;

  // Allocate once, outside the frame loop. The completion notifier is awaited
  // before the next reset, so changing output contents cannot race GPU writes.
  // The old per-frame managed/pinned allocation churn was outside gpu_us but
  // could still perturb the colocated cell and subsequent measurements.
  auto gpu_rx_data = cold_trace.measure("output_allocate", [&] {
    return std::make_unique<test_support::guarded_payload_buffer>(tbs / 8, gpu_output_buffer_mode);
  });
#ifdef OCUDU_PUSCH_TEST_DAPP
  std::unique_ptr<test_support::pusch_test_receiver_capture> receiver_capture;
  if (!receiver_capture_prefix.empty()) {
    receiver_capture = cold_trace.measure("receiver_capture_allocate", [&] {
      return std::make_unique<test_support::pusch_test_receiver_capture>(
          receiver_capture_prefix, get_bits_per_symbol(mcs_descr.modulation), mcs_idx.value(), !dapp_manifest.empty());
    });
  }
#endif
  if (phase_observation.count()) {
    cold_trace.measure("observe_prepared", [] { std::this_thread::sleep_for(phase_observation); });
  }
  cold_trace.record("frame_loop", "begin");

  for (unsigned iter = 0; iter < total_iterations; ++iter) {
    bool is_warmup = (iter < nof_warmup);
    if (iteration_pause.count())
      std::this_thread::sleep_for(iteration_pause);

    // Generate random TX data.
    std::vector<uint8_t> tx_data(tbs / 8);
    for (auto& byte : tx_data) {
      byte = static_cast<uint8_t>(rgen() & 0xff);
    }

    std::vector<uint8_t>   cpu_rx_data(tbs / 8);

    if (enable_transform_precoding) {
      transform_tx_generator->process(*tx_grid,
                                      tx_data,
                                      transform_encoder_config,
                                      freq_allocation,
                                      dmrs_symbols_mask,
                                      rnti,
                                      n_id,
                                      n_rs_id,
                                      mcs_descr.modulation);
    } else {
      // PDSCH PDU.
      pdsch_processor::pdu_t pdsch_pdu;
      pdsch_pdu.context                     = std::nullopt;
      pdsch_pdu.slot                        = slot_point(to_numerology_value(scs), iter + 1);
      pdsch_pdu.rnti                        = to_rnti(rnti);
      pdsch_pdu.bwp_size_rb                 = nof_prb;
      pdsch_pdu.bwp_start_rb                = bwp_start_rb;
      pdsch_pdu.cp                          = cy_prefix;
      pdsch_pdu.n_id                        = n_id;
      pdsch_pdu.ref_point                   = pdsch_processor::pdu_t::PRB0;
      pdsch_pdu.dmrs_symbol_mask            = dmrs_symbols_mask;
      pdsch_pdu.dmrs                        = dmrs;
      pdsch_pdu.scrambling_id               = scrambling_id;
      pdsch_pdu.n_scid                      = n_scid;
      pdsch_pdu.nof_cdm_groups_without_data = nof_cdm_groups_without_data;
      pdsch_pdu.freq_alloc                  = freq_alloc;
      pdsch_pdu.start_symbol_index          = 0;
      pdsch_pdu.nof_symbols                 = nof_ofdm_symbols;
      pdsch_pdu.tbs_lbrm                    = tbs_lbrm_default;
      pdsch_pdu.reserved                    = {};
      pdsch_pdu.ratio_pdsch_data_to_sss_dB  = 0.0F;
      pdsch_pdu.ratio_pdsch_dmrs_to_sss_dB  = get_sch_to_dmrs_ratio_dB(nof_cdm_groups_without_data);
      pdsch_pdu.precoding                   = precoding_configuration::make_wideband(make_identity(nof_layers));
      pdsch_pdu.codewords.emplace_back(
          pdsch_processor::codeword_description{mcs_descr.modulation, rv, ldpc_base_graph});

      // Transmit.
      pdsch_processor_notifier_adaptor tx_notifier;
      pdsch_proc->process(tx_grid->get_writer(), tx_notifier, {shared_transport_block(tx_data)}, pdsch_pdu);
      tx_notifier.wait_for_completion();
    }

    // Apply channel.
    emulator.run(rx_grid->get_writer(), tx_grid->get_reader());

    // Apply random timing offset as frequency-domain phase slope.
    float injected_ta_us = 0.0f;
    if (max_ta_offset_us > 0.0f) {
      injected_ta_us        = ta_dist(rgen);
      float    ta_s         = injected_ta_us * 1e-6f;
      float    phase_per_sc = -2.0f * static_cast<float>(M_PI) * scs_hz * ta_s;
      unsigned nof_subc     = nof_prb * NOF_SUBCARRIERS_PER_RB;
      for (unsigned port = 0; port < nof_rx_ports; ++port) {
        for (unsigned sym = 0; sym < nof_ofdm_symbols; ++sym) {
          rx_grid->get_reader().get(ta_symbol_buffer, port, sym, 0);
          for (unsigned k = 0; k < nof_subc; ++k) {
            float phase = phase_per_sc * static_cast<float>(k);
            ta_symbol_buffer[k] *= cf_t(std::cos(phase), std::sin(phase));
          }
          rx_grid->get_writer().put(port, sym, 0, ta_symbol_buffer);
        }
      }
    }

    // Apply random CFO as per-symbol uniform phase rotation.
    float injected_cfo_hz = 0.0f;
    if (cfo_std_hz > 0.0f) {
      injected_cfo_hz   = cfo_dist(rgen);
      unsigned nof_subc = nof_prb * NOF_SUBCARRIERS_PER_RB;
      for (unsigned port = 0; port < nof_rx_ports; ++port) {
        for (unsigned sym = 0; sym < nof_ofdm_symbols; ++sym) {
          float phase = static_cast<float>(2.0 * M_PI * symbol_start_times_s[sym] * injected_cfo_hz);
          cf_t  coeff(std::cos(phase), std::sin(phase));
          rx_grid->get_reader().get(ta_symbol_buffer, port, sym, 0);
          for (unsigned k = 0; k < nof_subc; ++k) {
            ta_symbol_buffer[k] *= coeff;
          }
          rx_grid->get_writer().put(port, sym, 0, ta_symbol_buffer);
        }
      }
    }

#ifdef ENABLE_CUDA
    if (gpu_device_grid_reader && !gpu_device_grid_reader->stage_host_grid_async()) {
      throw std::runtime_error("Failed to stage PUSCH RX grid into CUDA-visible memory.");
    }
#endif

    // PUSCH PDU.
    static_vector<uint8_t, MAX_PORTS> rx_ports(nof_rx_ports);
    std::iota(rx_ports.begin(), rx_ports.end(), 0U);

    pusch_processor::pdu_t pusch_pdu;
    pusch_pdu.slot             = slot_point(to_numerology_value(scs), iter + 1);
    pusch_pdu.rnti             = to_rnti(rnti);
    pusch_pdu.bwp_size_rb      = nof_prb;
    pusch_pdu.bwp_start_rb     = bwp_start_rb;
    pusch_pdu.cp               = cy_prefix;
    pusch_pdu.mcs_descr        = mcs_descr;
    pusch_pdu.codeword         = {rv, ldpc_base_graph, true};
    pusch_pdu.uci              = {};
    pusch_pdu.n_id             = n_id;
    pusch_pdu.nof_tx_layers    = nof_layers;
    pusch_pdu.rx_ports         = rx_ports;
    pusch_pdu.dmrs_symbol_mask = dmrs_symbols_mask;
    if (enable_transform_precoding) {
      pusch_pdu.dmrs = pusch_processor::dmrs_transform_precoding_configuration{.n_rs_id = n_rs_id};
    } else {
      pusch_pdu.dmrs = pusch_processor::dmrs_configuration{.dmrs                        = dmrs,
                                                           .scrambling_id               = scrambling_id,
                                                           .n_scid                      = n_scid,
                                                           .nof_cdm_groups_without_data = nof_cdm_groups_without_data};
    }
    pusch_pdu.tbs_lbrm           = tbs_lbrm_default;
    pusch_pdu.freq_alloc         = freq_alloc;
    pusch_pdu.start_symbol_index = 0;
    pusch_pdu.nof_symbols        = nof_ofdm_symbols;
    pusch_pdu.dc_position        = std::nullopt;

    // CPU decode. With --cpu-every N > 1 the CPU reference runs only every Nth measured
    // iteration: the ~10+ ms CPU decode between GPU slots lets the GPU idle/downclock,
    // which distorts GPU latency relative to a real gNB slot cadence.
    const bool run_cpu = is_warmup || (cpu_every <= 1) || (((iter - nof_warmup) % cpu_every) == 0);

    pusch_processor_notifier_adaptor   cpu_notifier;
    const pusch_processor_result_data* cpu_result_ptr = nullptr;
    double                             cpu_us         = 0.0;
    if (run_cpu) {
      unique_rx_buffer cpu_buffer =
          cpu_pool->get_pool().reserve(pusch_pdu.slot, trx_buffer_identifier(to_rnti(rnti), 0), nof_codeblocks, true);

      auto cpu_start = std::chrono::steady_clock::now();
      cpu_proc->process(cpu_rx_data, std::move(cpu_buffer), cpu_notifier, rx_grid->get_reader(), pusch_pdu);
      cpu_result_ptr = &cpu_notifier.wait_for_completion();
      auto cpu_end   = std::chrono::steady_clock::now();
      cpu_us         = std::chrono::duration<double, std::micro>(cpu_end - cpu_start).count();
    }

    // GPU decode. For direct-managed mode, publish the asynchronous producer
    // immediately before consumption. There is deliberately no host sync or
    // CPU decode in between: correctness depends on the grid-ready event.
#ifdef ENABLE_CUDA
    if (direct_managed_rx_grid) {
      span<const cbf16_t> source      = rx_grid->get_reader().get_contiguous_view();
      const size_t        expected_re = static_cast<size_t>(nof_rx_ports) * MAX_NSYMB_PER_SLOT * MAX_NOF_SUBCARRIERS;
      if (source.size() != expected_re) {
        throw std::runtime_error(
            fmt::format("Host reference grid is not contiguous ({} RE, expected {}).", source.size(), expected_re));
      }

      resource_grid_writer& gpu_writer = direct_managed_rx_grid->get_writer();
      cudaStream_t          producer   = direct_managed_producer_stream->get();
      if (!gpu_writer.prepare_device_grid_mapping(producer)) {
        throw std::runtime_error("CUDA-visible grid rejected the GPU producer mapping.");
      }
      cudaError_t copy_status = cudaMemcpyAsync(gpu_writer.get_device_grid_bf16(),
                                                source.data(),
                                                source.size() * sizeof(cbf16_t),
                                                cudaMemcpyHostToDevice,
                                                producer);
      if (copy_status != cudaSuccess) {
        (void)gpu_writer.cancel_device_grid_mapping();
        throw std::runtime_error(
            fmt::format("Failed to enqueue CUDA-visible grid producer: {}", cudaGetErrorString(copy_status)));
      }
      if (!gpu_writer.supports_owned_device_grid_snapshot() ||
          !gpu_writer.publish_owned_device_grid_snapshot(producer)) {
        (void)gpu_writer.cancel_device_grid_mapping();
        throw std::runtime_error("CUDA-visible grid failed to publish the immutable producer-owned snapshot.");
      }
      if (!gpu_writer.on_device_grid_mapping_enqueued(producer)) {
        (void)gpu_writer.cancel_device_grid_mapping();
        throw std::runtime_error("CUDA-visible grid failed to publish the GPU producer completion event.");
      }

      // Model the live GB10 ordering hazard: a CPU PUCCH-style read migrates
      // the managed allocation home after lower-PHY publication but before the
      // asynchronous PUSCH task claims its input. PUSCH must still consume the
      // immutable device-owned snapshot, not stage the live grid from host.
      const resource_grid_reader& direct_reader = direct_managed_rx_grid->get_reader();
      if (!direct_reader.stage_owned_snapshot_from_host() ||
          !direct_reader.owned_device_snapshot_is_pusch_compatible()) {
        throw std::runtime_error("Class-C host staging replaced or invalidated the immutable PUSCH producer snapshot.");
      }
      span<const cbf16_t> host_visible = direct_reader.get_view(0, 0);
      if (host_visible.empty()) {
        throw std::runtime_error("Host consumer could not read the published managed grid.");
      }
    }
#endif

    unique_rx_buffer gpu_buffer =
        gpu_pool->get_pool().reserve(pusch_pdu.slot, trx_buffer_identifier(to_rnti(rnti), 1), nof_codeblocks, true);

    pusch_processor_notifier_adaptor gpu_notifier;
#ifdef ENABLE_CUDA
    const resource_grid_reader& gpu_grid_reader =
        direct_managed_rx_grid
            ? direct_managed_rx_grid->get_reader()
            : (gpu_device_grid_reader ? static_cast<const resource_grid_reader&>(*gpu_device_grid_reader)
                                      : rx_grid->get_reader());
#else
    const resource_grid_reader& gpu_grid_reader = rx_grid->get_reader();
#endif
#ifdef OCUDU_PUSCH_TEST_DAPP
    uint64_t invoke_before = 0, fallback_before = 0, deadline_before = 0;
    if (dapp_fixture) {
      invoke_before   = dapp_fixture->counter(OCUDU_DAPP_METRIC_INVOKE_COUNT_V1);
      fallback_before = dapp_fixture->fallback_count();
      deadline_before = dapp_fixture->counter(OCUDU_DAPP_METRIC_CLASS_A_COMPLETION_DEADLINE_MISSES_V1);
    }
#endif
#ifdef OCUDU_PUSCH_TEST_DAPP
    if (receiver_capture) receiver_capture->begin(iter);
#endif
    auto gpu_start = std::chrono::steady_clock::now();
#ifdef OCUDU_PUSCH_TEST_DAPP
    test_support::enable_receiver_retirement_trace(trace_terminate);
#endif
    gpu_rx_data->reset();
    gpu_proc->process(gpu_rx_data->as_span(), std::move(gpu_buffer), gpu_notifier, gpu_grid_reader, pusch_pdu);
    const auto& gpu_result     = gpu_notifier.wait_for_completion();
    // Both baseline and receiver must return the real processing lease before
    // this iteration's payload, input grid and notifier can be reused/destroyed.
    gpu_proc->require_idle();
    auto        gpu_end        = std::chrono::steady_clock::now();
    double      gpu_us         = std::chrono::duration<double, std::micro>(gpu_end - gpu_start).count();
    auto        gpu_rx_payload = gpu_rx_data->payload_vector();
#ifdef OCUDU_PUSCH_TEST_DAPP
    if (receiver_capture) {
      receiver_capture->finish(tx_data, tx_grid->get_reader().get_contiguous_view(),
                               rx_grid->get_reader().get_contiguous_view());
    }
#endif
    uint64_t    invoke_delta = 0, fallback_delta = 0, deadline_delta = 0;
    std::string fallback_detail;
#ifdef OCUDU_PUSCH_TEST_DAPP
    if (dapp_fixture) {
      invoke_delta   = dapp_fixture->counter(OCUDU_DAPP_METRIC_INVOKE_COUNT_V1) - invoke_before;
      fallback_delta = dapp_fixture->fallback_count() - fallback_before;
      deadline_delta = dapp_fixture->counter(OCUDU_DAPP_METRIC_CLASS_A_COMPLETION_DEADLINE_MISSES_V1) - deadline_before;
      if (fallback_delta != 0) {
        fallback_detail = dapp_fixture->fallback_details();
      }
    }
#endif
    const bool matches_tx = gpu_rx_payload == tx_data;
    if (gpu_result.data.tb_crc_ok && !matches_tx)
      ++results.gpu_truth_mismatches;
    if (samples) {
      auto number = [](std::optional<float> value) {
        return value && std::isfinite(*value) ? fmt::format("{}", *value) : std::string("null");
      };
      const auto& timing = gpu_result.data.acceleration_pipeline_timing;
      std::string input_hash, payload_hash;
#ifdef OCUDU_PUSCH_TEST_DAPP
      // Evidence only, outside timing: hash the original host-side received
      // grant and transmitted payload. The direct-managed test uses a separate
      // immutable device snapshot; hashing never reads that live allocation.
      std::vector<cbf16_t> observed_input;
      for (unsigned port = 0; port != nof_rx_ports; ++port) {
        for (unsigned symbol = 0; symbol != nof_ofdm_symbols; ++symbol) {
          auto row = rx_grid->get_reader().get_view(port, symbol).first(nof_prb * NOF_SUBCARRIERS_PER_RB);
          observed_input.insert(observed_input.end(), row.begin(), row.end());
        }
      }
      input_hash   = dapp::to_hex(dapp::sha256_bytes(reinterpret_cast<const uint8_t*>(observed_input.data()),
                                                   observed_input.size() * sizeof(cbf16_t)));
      payload_hash = dapp::to_hex(dapp::sha256_bytes(tx_data.data(), tx_data.size()));
#endif
      fmt::print(samples.get(),
                 "{{\"kind\":\"frame\",\"frame\":{},\"warmup\":{},\"qm\":{},\"tb_bits\":{},"
                 "\"gpu_wall_us\":{},\"cpu_wall_us\":{},\"cpu_ran\":{},\"gpu_crc\":{},\"matches_tx\":{},"
                 "\"resident\":{},\"device_grid\":{},\"owned_grid\":{},\"dapp_invocations\":{},\"dapp_fallbacks\":{},"
                 "\"sinr_dB\":{},\"evm\":{},\"epre_dB\":{},\"rsrp_dB\":{},\"ta_s\":{},\"cfo_Hz\":{},"
                 "\"epoch_s\":{},\"input_sha256\":\"{}\",\"payload_sha256\":\"{}\",\"dapp_deadline_misses\":{},"
                 "\"cpu_crc\":{},\"cpu_matches_tx\":{},\"cpu_sinr_dB\":{},"
                 "\"demod_call_us\":{},\"decode_call_us\":{},\"completion_wait_us\":{},\"sinr_readback_us\":{}}}\n",
                 iter,
                 is_warmup,
                 get_bits_per_symbol(mcs_descr.modulation),
                 tbs,
                 gpu_us,
                 cpu_us,
                 run_cpu,
                 gpu_result.data.tb_crc_ok,
                 matches_tx,
                 timing.valid,
                 timing.device_grid_used,
                 timing.owned_device_grid_used,
                 invoke_delta,
                 fallback_delta,
                 number(gpu_result.csi.get_sinr_dB()),
                 number(gpu_result.csi.get_total_evm()),
                 number(gpu_result.csi.get_epre_dB()),
                 number(gpu_result.csi.get_rsrp_dB()),
                 gpu_result.csi.get_time_alignment()
                     ? fmt::format("{}", gpu_result.csi.get_time_alignment()->to_seconds())
                     : std::string("null"),
                 number(gpu_result.csi.get_cfo_Hz()),
                 epoch_seconds(),
                 input_hash,
                 payload_hash,
                 deadline_delta,
                 run_cpu && cpu_result_ptr->data.tb_crc_ok,
                 run_cpu && cpu_rx_data == tx_data,
                 run_cpu ? number(cpu_result_ptr->csi.get_sinr_dB()) : std::string("null"),
                 timing.demod_call_us,
                 timing.decode_call_us,
                 timing.completion_wait_us,
                 timing.sinr_readback_us);
      if (std::fflush(samples.get()))
        throw std::runtime_error("Writing PUSCH samples failed.");
    }
    if (!dapp_manifest.empty() && (invoke_delta != 1 || fallback_delta != 0 || deadline_delta != 0)) {
      throw std::runtime_error(
          fmt::format("Receiver hook qualification failed: invocations={}, fallbacks={}, deadline_misses={}, reasons=[{}]",
                      invoke_delta,
                      fallback_delta,
                      deadline_delta,
                      fallback_detail));
    }
    if (require_pilot_csi) {
      auto finite = [](std::optional<float> value) { return value && std::isfinite(*value); };
      const auto& csi = gpu_result.csi;
      if (!finite(csi.get_sinr_dB()) || !finite(csi.get_epre_dB()) || !finite(csi.get_rsrp_dB()) ||
          !finite(csi.get_cfo_Hz()) || !csi.get_time_alignment() ||
          !std::isfinite(csi.get_time_alignment()->to_seconds())) {
        throw std::runtime_error("Incomplete selected pilot CSI after actual receiver/decoder completion");
      }
    }

    size_t  first_bad_guard_offset = 0;
    uint8_t bad_guard_value        = 0;
    if (!gpu_rx_data->guard_ok(&first_bad_guard_offset, &bad_guard_value)) {
      ++results.output_guard_failures;
      fmt::print("OUTPUT GUARD CORRUPTION at iteration {}: mode={} first_guard_offset={} value=0x{:02x}\n",
                 is_warmup ? 0 : iter - nof_warmup,
                 gpu_output_buffer_mode,
                 first_bad_guard_offset,
                 bad_guard_value);
    }

    // Skip warmup iterations for statistics; validate GPU E2E path.
    if (is_warmup) {
      if (gpu_result.data.acceleration_pipeline_timing.valid) {
        gpu_e2e_validated = true;
      }
      if (!quiet_mode) {
        fmt::print("[warmup {:2d}] CPU: CRC={} GPU: CRC={} ({:.0f}/{:.0f} us)\n",
                   iter,
                   cpu_result_ptr->data.tb_crc_ok ? "PASS" : "FAIL",
                   gpu_result.data.tb_crc_ok ? "PASS" : "FAIL",
                   cpu_us,
                   gpu_us);
      }
      if (iter == nof_warmup - 1) {
        fmt::print("E2E GPU Pipeline: {} (warmup complete, resident timing: {})\n\n",
                   gpu_e2e_validated ? "ACTIVE" : "NOT ACTIVE",
                   gpu_e2e_validated ? "enabled" : "missing");
      }
      continue;
    }

    // Accumulate latency.
    if (run_cpu) {
      results.cpu_latency.add(cpu_us);
    }
    results.gpu_latency.add(gpu_us);

    // GPU pipeline timing breakdown (may not be populated on all builds/configs).
    if (gpu_result.data.acceleration_pipeline_timing.valid) {
      results.gpu_e2e_count++;
      if (gpu_result.data.acceleration_pipeline_timing.device_grid_used) {
        results.gpu_device_grid_count++;
      }
      if (gpu_result.data.acceleration_pipeline_timing.owned_device_grid_used) {
        results.gpu_owned_grid_count++;
      }
      results.gpu_grid_staging.add(gpu_result.data.acceleration_pipeline_timing.grid_staging_us);
      results.gpu_demod_sync.add(gpu_result.data.acceleration_pipeline_timing.demod_sync_us);
      results.gpu_deinterleave.add(gpu_result.data.acceleration_pipeline_timing.deinterleave_us);
      results.gpu_rate_dematch.add(gpu_result.data.acceleration_pipeline_timing.rate_dematch_us);
      results.gpu_ldpc_decode.add(gpu_result.data.acceleration_pipeline_timing.ldpc_decode_us);
      results.gpu_crc_check.add(gpu_result.data.acceleration_pipeline_timing.crc_check_us);
      results.gpu_d2h_transfer.add(gpu_result.data.acceleration_pipeline_timing.d2h_transfer_us);
      results.gpu_extract_bits.add(gpu_result.data.acceleration_pipeline_timing.extract_bits_us);
      results.gpu_ch_estimate.add(gpu_result.data.acceleration_pipeline_timing.ch_estimate_us);
      results.gpu_process_data_setup.add(gpu_result.data.acceleration_pipeline_timing.process_data_setup_us);
      results.gpu_demod_call.add(gpu_result.data.acceleration_pipeline_timing.demod_call_us);
      results.gpu_decode_call.add(gpu_result.data.acceleration_pipeline_timing.decode_call_us);
      results.gpu_join_notify.add(gpu_result.data.acceleration_pipeline_timing.join_notify_us);
      results.gpu_sinr_readback.add(gpu_result.data.acceleration_pipeline_timing.sinr_readback_us);
      results.gpu_decoder_cfg.add(gpu_result.data.acceleration_pipeline_timing.decoder_config_us);
      results.gpu_rm_cfg.add(gpu_result.data.acceleration_pipeline_timing.rate_match_config_us);
      results.gpu_completion_wait.add(gpu_result.data.acceleration_pipeline_timing.completion_wait_us);
      results.gpu_tb_output_copy.add(gpu_result.data.acceleration_pipeline_timing.tb_output_copy_us);
      results.gpu_avg_iters_query.add(gpu_result.data.acceleration_pipeline_timing.avg_iters_query_us);
    }

    // CRC results.
    if (run_cpu) {
      if (cpu_result_ptr->data.tb_crc_ok) {
        results.cpu_pass++;
      } else {
        results.cpu_fail++;
      }
    }

    if (gpu_result.data.tb_crc_ok) {
      results.gpu_pass++;
    } else {
      results.gpu_fail++;
    }

    // LDPC iteration tracking.
    if (run_cpu && cpu_result_ptr->data.ldpc_decoder_stats.get_nof_observations() > 0) {
      results.cpu_iters_sum += cpu_result_ptr->data.ldpc_decoder_stats.get_mean();
      results.cpu_iters_count++;
    }
    if (gpu_result.data.ldpc_decoder_stats.get_nof_observations() > 0) {
      results.gpu_iters_sum += gpu_result.data.ldpc_decoder_stats.get_mean();
    }
    results.iters_count++;

    // CRC disagreement.
    if (run_cpu && cpu_result_ptr->data.tb_crc_ok != gpu_result.data.tb_crc_ok) {
      results.disagreements++;
      fmt::print("DISAGREEMENT at iteration {}: CPU={} GPU={}\n",
                 iter - nof_warmup,
                 cpu_result_ptr->data.tb_crc_ok ? "PASS" : "FAIL",
                 gpu_result.data.tb_crc_ok ? "PASS" : "FAIL");
    }

    // Byte-level TB comparison (only when both CRCs pass).
    if (run_cpu && cpu_result_ptr->data.tb_crc_ok && gpu_result.data.tb_crc_ok) {
      if (cpu_rx_data != gpu_rx_payload) {
        results.byte_mismatches++;
        // Find and report first differing byte.
        for (unsigned b = 0; b < cpu_rx_data.size(); ++b) {
          if (cpu_rx_data[b] != gpu_rx_payload[b]) {
            fmt::print("BYTE MISMATCH at iteration {}: first diff at byte {}/{} (CPU=0x{:02x} GPU=0x{:02x})\n",
                       iter - nof_warmup,
                       b,
                       cpu_rx_data.size(),
                       cpu_rx_data[b],
                       gpu_rx_payload[b]);
            break;
          }
        }
      }
    }

    // CSI comparison.
    {
      const auto& gpu_csi = gpu_result.csi;

      auto gpu_sinr_val = gpu_csi.get_sinr_dB();
      auto gpu_evm_val  = gpu_csi.get_total_evm();
      auto gpu_epre_val = gpu_csi.get_epre_dB();
      auto gpu_rsrp_val = gpu_csi.get_rsrp_dB();
      auto gpu_cfo_val  = gpu_csi.get_cfo_Hz();

      auto                 gpu_ta_raw = gpu_csi.get_time_alignment();
      std::optional<float> gpu_ta_us =
          gpu_ta_raw.has_value() ? std::optional<float>(static_cast<float>(gpu_ta_raw.value().to_seconds() * 1e6))
                                 : std::nullopt;

      std::optional<float> cpu_sinr_val;
      std::optional<float> cpu_evm_val;
      std::optional<float> cpu_epre_val;
      std::optional<float> cpu_rsrp_val;
      std::optional<float> cpu_cfo_val;
      std::optional<float> cpu_ta_us;
      if (run_cpu) {
        const auto& cpu_csi = cpu_result_ptr->csi;
        cpu_sinr_val        = cpu_csi.get_sinr_dB();
        cpu_evm_val         = cpu_csi.get_total_evm();
        cpu_epre_val        = cpu_csi.get_epre_dB();
        cpu_rsrp_val        = cpu_csi.get_rsrp_dB();
        cpu_cfo_val         = cpu_csi.get_cfo_Hz();
        auto cpu_ta_raw     = cpu_csi.get_time_alignment();
        cpu_ta_us           = cpu_ta_raw.has_value()
                                  ? std::optional<float>(static_cast<float>(cpu_ta_raw.value().to_seconds() * 1e6))
                                  : std::nullopt;

        results.cpu_sinr.add(cpu_sinr_val);
        results.cpu_evm.add(cpu_evm_val);
        results.cpu_epre.add(cpu_epre_val);
        results.cpu_ta.add(cpu_ta_us);
        results.cpu_rsrp.add(cpu_rsrp_val);
        results.cpu_cfo.add(cpu_cfo_val);
      }
      results.gpu_sinr.add(gpu_sinr_val);
      results.gpu_evm.add(gpu_evm_val);
      results.gpu_epre.add(gpu_epre_val);
      results.gpu_ta.add(gpu_ta_us);
      results.gpu_rsrp.add(gpu_rsrp_val);
      results.gpu_cfo.add(gpu_cfo_val);

      if (max_ta_offset_us > 0.0f) {
        if (run_cpu) {
          results.cpu_ta_error.add(cpu_ta_us.has_value() ? std::optional<float>(cpu_ta_us.value() - injected_ta_us)
                                                         : std::nullopt);
        }
        results.gpu_ta_error.add(gpu_ta_us.has_value() ? std::optional<float>(gpu_ta_us.value() - injected_ta_us)
                                                       : std::nullopt);
      }

      if (cfo_std_hz > 0.0f) {
        if (run_cpu) {
          results.cpu_cfo_error.add(
              cpu_cfo_val.has_value() ? std::optional<float>(cpu_cfo_val.value() - injected_cfo_hz) : std::nullopt);
        }
        results.gpu_cfo_error.add(gpu_cfo_val.has_value() ? std::optional<float>(gpu_cfo_val.value() - injected_cfo_hz)
                                                          : std::nullopt);
      }

      // Per-iteration output (unless --quiet).
      if (!quiet_mode) {
        auto fmt_opt = [](std::optional<float> v, const char* fmt_str) -> std::string {
          if (v.has_value()) {
            return fmt::format(fmt::runtime(fmt_str), v.value());
          }
          return "N/A";
        };

        fmt::print("[iter {:3d}] CPU: CRC={} SINR={:>6s}dB EPRE={:>6s}dB | "
                   "GPU: CRC={} SINR={:>6s}dB EPRE={:>6s}dB | "
                   "{:.0f}/{:.0f} us",
                   iter - nof_warmup,
                   run_cpu ? (cpu_result_ptr->data.tb_crc_ok ? "PASS" : "FAIL") : "SKIP",
                   fmt_opt(cpu_sinr_val, "{:.2f}"),
                   fmt_opt(cpu_epre_val, "{:.2f}"),
                   gpu_result.data.tb_crc_ok ? "PASS" : "FAIL",
                   fmt_opt(gpu_sinr_val, "{:.2f}"),
                   fmt_opt(gpu_epre_val, "{:.2f}"),
                   cpu_us,
                   gpu_us);

        if (run_cpu && cpu_result_ptr->data.tb_crc_ok && gpu_result.data.tb_crc_ok) {
          fmt::print(" TB={}", cpu_rx_data == gpu_rx_payload ? "MATCH" : "MISMATCH");
        }
        fmt::print("\n");
      }
    }
  }

  cold_trace.record("frame_loop", "end");

  // === Summary Report ===
  unsigned measured_iters = nof_iterations;

  fmt::print("\n=== Results ({} iterations, {} warmup skipped) ===\n", measured_iters, nof_warmup);
  fmt::print(
      "CPU: {}/{} pass ({:.1f}% BLER)\n", results.cpu_pass, measured_iters, 100.0 * results.cpu_fail / measured_iters);
  fmt::print("GPU: {}/{} pass ({:.1f}% BLER)  [resident GPU timing: {}/{}]\n",
             results.gpu_pass,
             measured_iters,
             100.0 * results.gpu_fail / measured_iters,
             results.gpu_e2e_count,
             measured_iters);
  fmt::print("GPU producer-owned grid: {}/{} measured iterations\n", results.gpu_device_grid_count, measured_iters);
  fmt::print("GPU immutable owned snapshot: {}/{} measured iterations\n", results.gpu_owned_grid_count, measured_iters);
  fmt::print("Disagreements: {}\n", results.disagreements);
  fmt::print("Byte mismatches: {}\n", results.byte_mismatches);
  fmt::print("Output guard failures: {}\n", results.output_guard_failures);

  if (results.iters_count > 0 && results.cpu_iters_count > 0) {
    fmt::print("\n=== LDPC Iterations (max={}) ===\n", nof_ldpc_iterations);
    fmt::print("CPU avg: {:.2f}\n", results.cpu_iters_sum / results.cpu_iters_count);
    fmt::print("GPU avg: {:.2f}\n", results.gpu_iters_sum / results.iters_count);
  }

  // === Latency ===
  fmt::print("\n=== Latency (microseconds) ===\n");
  fmt::print("{:<16s} {:>8s} {:>8s} {:>8s}\n", "", "Min", "Mean", "Max");
  fmt::print("{:<16s} {:8.1f} {:8.1f} {:8.1f}\n",
             "CPU total:",
             results.cpu_latency.min_us,
             results.cpu_latency.mean(),
             results.cpu_latency.max_us);
  fmt::print("{:<16s} {:8.1f} {:8.1f} {:8.1f}\n",
             "GPU total:",
             results.gpu_latency.min_us,
             results.gpu_latency.mean(),
             results.gpu_latency.max_us);

  double speedup = results.cpu_latency.mean() / results.gpu_latency.mean();
  fmt::print("Speedup:                  {:.2f}x\n", speedup);

  // === GPU Pipeline Breakdown ===
  if (results.gpu_grid_staging.count > 0) {
    double demod_inner_total  = results.gpu_grid_staging.mean() + results.gpu_demod_sync.mean();
    double decode_inner_total = results.gpu_deinterleave.mean() + results.gpu_rate_dematch.mean() +
                                results.gpu_ldpc_decode.mean() + results.gpu_crc_check.mean() +
                                results.gpu_d2h_transfer.mean() + results.gpu_extract_bits.mean();
    double demod_wrapper_only = std::max(results.gpu_demod_call.mean() - demod_inner_total, 0.0);
    double decode_wrapper_only =
        std::max(results.gpu_decode_call.mean() -
                     (decode_inner_total + results.gpu_join_notify.mean() + results.gpu_sinr_readback.mean() +
                      results.gpu_decoder_cfg.mean() + results.gpu_rm_cfg.mean() + results.gpu_tb_output_copy.mean() +
                      results.gpu_avg_iters_query.mean()),
                 0.0);
    double accounted_total =
        results.gpu_ch_estimate.mean() + results.gpu_process_data_setup.mean() + demod_inner_total +
        demod_wrapper_only + decode_inner_total + results.gpu_join_notify.mean() + results.gpu_sinr_readback.mean() +
        results.gpu_decoder_cfg.mean() + results.gpu_rm_cfg.mean() + results.gpu_tb_output_copy.mean() +
        results.gpu_avg_iters_query.mean() + decode_wrapper_only;

    fmt::print("\n=== GPU Pipeline Breakdown (microseconds) ===\n");
    fmt::print("{:<18s} {:>8s} {:>8s} {:>8s} {:>8s}\n", "", "Min", "Mean", "Max", "%");

    auto print_stage = [&](const char* name, const latency_stats& s) {
      double pct = accounted_total > 0 ? 100.0 * s.mean() / accounted_total : 0;
      fmt::print("{:<18s} {:8.1f} {:8.1f} {:8.1f} {:7.1f}%\n", name, s.min_us, s.mean(), s.max_us, pct);
    };

    print_stage("Ch estimate:", results.gpu_ch_estimate);
    print_stage("Setup:", results.gpu_process_data_setup);
    print_stage("Grid staging:", results.gpu_grid_staging);
    print_stage("Demod sync:", results.gpu_demod_sync);
    print_stage("Deinterleave:", results.gpu_deinterleave);
    print_stage("Rate dematch:", results.gpu_rate_dematch);
    print_stage("LDPC decode:", results.gpu_ldpc_decode);
    print_stage("CRC check:", results.gpu_crc_check);
    print_stage("D2H transfer:", results.gpu_d2h_transfer);
    print_stage("Extract bits:", results.gpu_extract_bits);
    fmt::print("{:<18s} {:8s} {:8.1f} {:8s} {:7.1f}%\n",
               "Demod wrapper:",
               "",
               demod_wrapper_only,
               "",
               accounted_total > 0 ? 100.0 * demod_wrapper_only / accounted_total : 0);
    fmt::print("{:<18s} {:8s} {:8.1f} {:8s} {:7.1f}%\n",
               "Decode wrapper:",
               "",
               decode_wrapper_only,
               "",
               accounted_total > 0 ? 100.0 * decode_wrapper_only / accounted_total : 0);
    print_stage("Join/notify:", results.gpu_join_notify);
    print_stage("SINR readback:", results.gpu_sinr_readback);
    print_stage("Decoder cfg:", results.gpu_decoder_cfg);
    print_stage("RM cfg:", results.gpu_rm_cfg);
    print_stage("Completion wait*:", results.gpu_completion_wait);
    print_stage("TB host copy:", results.gpu_tb_output_copy);
    print_stage("Avg iters query:", results.gpu_avg_iters_query);
    fmt::print("{:<18s} {:8s} {:8.1f}\n", "Accounted total:", "", accounted_total);
    fmt::print("{:<18s} {:8s} {:8.1f}\n", "Demod call:", "", results.gpu_demod_call.mean());
    fmt::print("{:<18s} {:8s} {:8.1f}\n", "Decode call:", "", results.gpu_decode_call.mean());

    double unaccounted = results.gpu_latency.mean() - accounted_total;
    fmt::print("{:<18s} {:8s} {:8.1f}\n", "Unaccounted:", "", unaccounted);
  }

  // === CSI Metric Parity ===
  {
    auto mean_str = [](const csi_metric_stats& s, const char* fmt_str) -> std::string {
      if (s.has_data()) {
        return fmt::format(fmt::runtime(fmt_str), s.mean());
      }
      return "N/A";
    };

    auto delta_str =
        [](const csi_metric_stats& cpu_s, const csi_metric_stats& gpu_s, const char* fmt_str) -> std::string {
      if (cpu_s.has_data() && gpu_s.has_data()) {
        return fmt::format(fmt::runtime(fmt_str), gpu_s.mean() - cpu_s.mean());
      }
      return "N/A";
    };

    auto pop_str = [](const csi_metric_stats& gpu_s) -> std::string {
      if (gpu_s.populated == gpu_s.count) {
        return "Yes";
      }
      if (gpu_s.populated == 0) {
        return "NO";
      }
      return fmt::format("Partial ({:.0f}%)", gpu_s.population_rate());
    };

    fmt::print("\n=== CSI Metric Parity ===\n");
    fmt::print("{:<16s} {:>10s} {:>10s} {:>10s}   {}\n", "", "CPU Mean", "GPU Mean", "Delta", "GPU Populated?");
    fmt::print("{:<16s} {:>10s} {:>10s} {:>10s}   {}\n",
               "SINR (dB):",
               mean_str(results.cpu_sinr, "{:.2f}"),
               mean_str(results.gpu_sinr, "{:.2f}"),
               delta_str(results.cpu_sinr, results.gpu_sinr, "{:+.2f}"),
               pop_str(results.gpu_sinr));
    fmt::print("{:<16s} {:>10s} {:>10s} {:>10s}   {}\n",
               "EVM (ratio):",
               mean_str(results.cpu_evm, "{:.2f}"),
               mean_str(results.gpu_evm, "{:.2f}"),
               delta_str(results.cpu_evm, results.gpu_evm, "{:+.2f}"),
               pop_str(results.gpu_evm));
    fmt::print("{:<16s} {:>10s} {:>10s} {:>10s}   {}\n",
               "EPRE (dB):",
               mean_str(results.cpu_epre, "{:.2f}"),
               mean_str(results.gpu_epre, "{:.2f}"),
               delta_str(results.cpu_epre, results.gpu_epre, "{:+.2f}"),
               pop_str(results.gpu_epre));
    fmt::print("{:<16s} {:>10s} {:>10s} {:>10s}   {}\n",
               "TA (us):",
               mean_str(results.cpu_ta, "{:.2f}"),
               mean_str(results.gpu_ta, "{:.2f}"),
               delta_str(results.cpu_ta, results.gpu_ta, "{:+.2f}"),
               pop_str(results.gpu_ta));
    fmt::print("{:<16s} {:>10s} {:>10s} {:>10s}   {}\n",
               "RSRP (dB):",
               mean_str(results.cpu_rsrp, "{:.1f}"),
               mean_str(results.gpu_rsrp, "{:.1f}"),
               delta_str(results.cpu_rsrp, results.gpu_rsrp, "{:+.1f}"),
               pop_str(results.gpu_rsrp));
    fmt::print("{:<16s} {:>10s} {:>10s} {:>10s}   {}\n",
               "CFO (Hz):",
               mean_str(results.cpu_cfo, "{:.1f}"),
               mean_str(results.gpu_cfo, "{:.1f}"),
               delta_str(results.cpu_cfo, results.gpu_cfo, "{:+.1f}"),
               pop_str(results.gpu_cfo));

    if (max_ta_offset_us > 0.0f) {
      fmt::print("\n=== TA Estimation Accuracy (injected range: +/-{:.1f}us) ===\n", max_ta_offset_us);
      fmt::print("  CPU mean error: {}us\n", mean_str(results.cpu_ta_error, "{:+.4f}"));
      fmt::print("  GPU mean error: {}us\n", mean_str(results.gpu_ta_error, "{:+.4f}"));
    }

    if (cfo_std_hz > 0.0f) {
      fmt::print("\n=== CFO Estimation Accuracy (injected std: {:.1f}Hz) ===\n", cfo_std_hz);
      fmt::print("  CPU mean error: {}Hz\n", mean_str(results.cpu_cfo_error, "{:+.2f}"));
      fmt::print("  GPU mean error: {}Hz\n", mean_str(results.gpu_cfo_error, "{:+.2f}"));
    }
  }

  // === Final verdict ===
  // Sensitivity compares independent CRC outcomes at a fixed waveform/SNR.
  // Different algorithms may decode different blocks; accepted payloads and
  // all resident/ownership/metric checks remain mandatory in either mode.
  const bool check_reference_crc = dapp_manifest.empty() && !decoded_sensitivity;
  bool resident_requirement_failed = require_gpu_resident && (results.gpu_e2e_count != measured_iters);
  bool device_grid_requirement_failed =
      (rx_device_grid_mode == "direct-managed") && (results.gpu_device_grid_count != measured_iters);
  bool owned_grid_requirement_failed =
      (rx_device_grid_mode == "direct-managed") && (results.gpu_owned_grid_count != measured_iters);

  if (resident_requirement_failed) {
    fmt::print("\n*** FAIL: resident GPU pipeline active for {}/{} measured iterations ***\n",
               results.gpu_e2e_count,
               measured_iters);
  } else if (device_grid_requirement_failed) {
    fmt::print("\n*** FAIL: producer-owned CUDA grid consumed directly for {}/{} measured iterations ***\n",
               results.gpu_device_grid_count,
               measured_iters);
  } else if (owned_grid_requirement_failed) {
    fmt::print("\n*** FAIL: immutable owned snapshot consumed for {}/{} measured iterations ***\n",
               results.gpu_owned_grid_count,
               measured_iters);
  } else if (results.output_guard_failures > 0) {
    fmt::print("\n*** FAIL: {} GPU output guard corruptions ***\n", results.output_guard_failures);
  } else if (results.disagreements > 0 && check_reference_crc) {
    fmt::print("\n*** FAIL: {} CRC disagreements between CPU and GPU ***\n", results.disagreements);
  } else if (results.byte_mismatches > 0) {
    fmt::print("\n*** FAIL: {} byte-level mismatches (CRCs agreed but TB data differed) ***\n",
               results.byte_mismatches);
  } else if (results.gpu_truth_mismatches > 0 || (require_all_tb && results.gpu_fail)) {
    fmt::print("\n*** FAIL: GPU decoded-payload or all-TB requirement ***\n");
  } else if (decoded_sensitivity) {
    fmt::print("\nDecoded sensitivity: GPU {}/{} measured TBs decoded; {} CPU CRC disagreements recorded.\n",
               results.gpu_pass, measured_iters, results.disagreements);
  } else if (!dapp_manifest.empty()) {
    fmt::print("\nReceiver dApp: {}/{} measured TBs decoded, {} CPU CRC disagreements (not a parity requirement).\n",
               results.gpu_pass,
               measured_iters,
               results.disagreements);
  } else if (results.cpu_fail == 0 && results.gpu_fail == 0) {
    fmt::print("\nPASS: Both paths decoded all {} iterations, 0 byte mismatches\n", measured_iters);
  } else {
    fmt::print("\nPASS: Both paths agree on BLER, 0 byte mismatches\n");
  }

  if (phase_observation.count()) {
    cold_trace.measure("observe_before_retirement", [] { std::this_thread::sleep_for(phase_observation); });
  }
  cold_trace.measure("executor_stop", [&] { worker_pool->stop(); });
  cold_trace.measure("gpu_processor_destroy", [&] { gpu_proc.reset(); });
  cold_trace.measure("gpu_factory_destroy", [&] { gpu_factory.reset(); });
#ifdef OCUDU_PUSCH_TEST_DAPP
  if (dapp_fixture)
    cold_trace.measure("dapp_unload", [&] { dapp_fixture->unload(); });
#endif
  cold_trace.measure("output_release", [&] { gpu_rx_data.reset(); });
  const bool failed = resident_requirement_failed || device_grid_requirement_failed || owned_grid_requirement_failed ||
                      results.output_guard_failures || results.gpu_truth_mismatches || results.byte_mismatches ||
                      (check_reference_crc && results.disagreements) ||
                      (require_all_tb && results.gpu_fail);
  if (samples) {
    fmt::print(samples.get(),
               "{{\"kind\":\"end\",\"passed\":{},\"gpu_pass\":{},\"gpu_fail\":{},\"truth_mismatches\":{},\"guard_"
               "failures\":{},\"epoch_s\":{}}}\n",
               !failed,
               results.gpu_pass,
               results.gpu_fail,
               results.gpu_truth_mismatches,
               results.output_guard_failures,
               epoch_seconds());
    if (std::fflush(samples.get()))
      throw std::runtime_error("Writing PUSCH summary failed.");
  }
  if (failed)
    throw std::runtime_error("PUSCH resident/correctness requirements failed.");
}

} // namespace

int main(int argc, char** argv)
try {
  unsigned      nof_prb = 106;
  float         sinr_dB = 20.0f;
  sch_mcs_index mcs_idx = static_cast<sch_mcs_index>(20);

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--prb" && i + 1 < argc) {
      nof_prb = std::stoi(argv[++i]);
    } else if (arg == "--sinr" && i + 1 < argc) {
      sinr_dB = std::stof(argv[++i]);
    } else if (arg == "--mcs" && i + 1 < argc) {
      mcs_idx = static_cast<sch_mcs_index>(std::stoi(argv[++i]));
    } else if (arg == "--iterations" && i + 1 < argc) {
      nof_iterations = std::stoi(argv[++i]);
    } else if (arg == "--warmup" && i + 1 < argc) {
      nof_warmup = std::stoi(argv[++i]);
    } else if (arg == "--cpu-every" && i + 1 < argc) {
      cpu_every = std::max(1, std::stoi(argv[++i]));
    } else if (arg == "--workers" && i + 1 < argc) {
      benchmark_workers = parse_u32(argv[++i], 32);
      if (!benchmark_workers || benchmark_workers > 32)
        return 1;
    } else if (arg == "--pace-ms" && i + 1 < argc) {
      const auto value = parse_u32(argv[++i], 1000);
      if (value > 1000)
        return 1;
      iteration_pause = std::chrono::milliseconds{value};
    } else if (arg == "--seed" && i + 1 < argc) {
      payload_seed = parse_u32(argv[++i], std::numeric_limits<uint32_t>::max());
    } else if (arg == "--sample-jsonl" && i + 1 < argc) {
      sample_jsonl_path = argv[++i];
    } else if (arg == "--cold-jsonl" && i + 1 < argc) {
      cold_jsonl_path = argv[++i];
    } else if (arg == "--dapp-manifest" && i + 1 < argc) {
      dapp_manifest = argv[++i];
    } else if (arg == "--dapp-receiver-completion") {
      receiver_completion = true;
    } else if (arg == "--cuda-owned-context") {
      cuda_owned_context = true;
    } else if (arg == "--cuda-init-only") {
      cuda_init_only = true;
    } else if (arg == "--trace-terminate") {
      trace_terminate = true;
    } else if (arg == "--post-teardown-hold-ms" && i + 1 < argc) {
      post_teardown_hold = std::chrono::milliseconds{parse_u32(argv[++i], 5000)};
    } else if (arg == "--phase-observation-ms" && i + 1 < argc) {
      phase_observation = std::chrono::milliseconds{parse_u32(argv[++i], 5000)};
    } else if (arg == "--receiver-capture-prefix" && i + 1 < argc) {
      receiver_capture_prefix = argv[++i];
      if (receiver_capture_prefix.empty()) throw std::runtime_error("Empty receiver capture prefix");
    } else if (arg == "--require-pilot-csi") {
      require_pilot_csi = true;
    } else if (arg == "--sinr-method" && i + 1 < argc) {
      const std::string method = argv[++i];
      if (method == "channel_estimator") {
        sinr_method = channel_state_information::sinr_type::channel_estimator;
      } else if (method == "post_equalization") {
        sinr_method = channel_state_information::sinr_type::post_equalization;
      } else {
        throw std::runtime_error("--sinr-method requires channel_estimator or post_equalization");
      }
    } else if (arg == "--require-all-tb") {
      require_all_tb = true;
    } else if (arg == "--decoded-sensitivity") {
      decoded_sensitivity = true;
    } else if (arg == "--channel-profile" && i + 1 < argc) {
      channel_profile = argv[++i];
      if (channel_profile != "single-tap" && channel_profile != "TDLA" &&
          channel_profile != "TDLB" && channel_profile != "TDLC") {
        throw std::runtime_error("--channel-profile requires single-tap, TDLA, TDLB or TDLC");
      }
    } else if (arg == "--fading-distribution" && i + 1 < argc) {
      fading_distribution = argv[++i];
      if (fading_distribution != "uniform-phase" && fading_distribution != "rayleigh") {
        throw std::runtime_error("--fading-distribution requires uniform-phase or rayleigh");
      }
    } else if (arg == "--mcs-table" && i + 1 < argc) {
      std::string table_str = argv[++i];
      if (table_str == "qam64") {
        mcs_table = pusch_mcs_table::qam64;
      } else if (table_str == "qam256") {
        mcs_table = pusch_mcs_table::qam256;
      } else if (table_str == "qam64LowSe") {
        mcs_table = pusch_mcs_table::qam64LowSe;
      } else {
        fmt::print("Unknown MCS table: {}. Use qam64, qam256, or qam64LowSe\n", table_str);
        return 1;
      }
    } else if (arg == "--ta-offset" && i + 1 < argc) {
      max_ta_offset_us = std::stof(argv[++i]);
    } else if (arg == "--cfo-std" && i + 1 < argc) {
      cfo_std_hz = std::stof(argv[++i]);
    } else if (arg == "--rnti" && i + 1 < argc) {
      rnti = static_cast<uint16_t>(std::stoi(argv[++i]));
    } else if (arg == "--n-id" && i + 1 < argc) {
      n_id = std::stoi(argv[++i]);
    } else if (arg == "--scrambling-id" && i + 1 < argc) {
      scrambling_id = std::stoi(argv[++i]);
    } else if (arg == "--n-scid" && i + 1 < argc) {
      n_scid = (std::stoi(argv[++i]) != 0);
    } else if (arg == "--transform-precoding") {
      enable_transform_precoding = true;
    } else if (arg == "--transform-deprecoder" && i + 1 < argc) {
      transform_deprecoder_backend = argv[++i];
      if ((transform_deprecoder_backend != "custom") && (transform_deprecoder_backend != "vkfft") &&
          (transform_deprecoder_backend != "auto")) {
        fmt::print("Unknown transform deprecoder: {}. Use custom, vkfft, or auto\n", transform_deprecoder_backend);
        return 1;
      }
    } else if (arg == "--n-rs-id" && i + 1 < argc) {
      n_rs_id = std::stoi(argv[++i]);
    } else if (arg == "--ports" && i + 1 < argc) {
      nof_rx_ports = std::stoi(argv[++i]);
    } else if (arg == "--layers" && i + 1 < argc) {
      nof_layers = std::stoi(argv[++i]);
    } else if (arg == "--dmrs-symbols" && i + 1 < argc) {
      dmrs_symbols_mask = parse_dmrs_symbols(argv[++i]);
    } else if (arg == "--dmrs-type" && i + 1 < argc) {
      dmrs = parse_dmrs_type(argv[++i]);
    } else if (arg == "--quiet") {
      quiet_mode = true;
    } else if (arg == "--debug-logs") {
      debug_logs = true;
    } else if (arg == "--require-gpu-resident") {
      require_gpu_resident = true;
    } else if (arg == "--rx-device-grid" && i + 1 < argc) {
      rx_device_grid_mode = argv[++i];
      if ((rx_device_grid_mode != "device") && (rx_device_grid_mode != "managed") &&
          (rx_device_grid_mode != "direct-managed") && (rx_device_grid_mode != "auto") &&
          (rx_device_grid_mode != "off")) {
        fmt::print("Unknown RX device grid mode: {}. Use off, device, managed, direct-managed, or auto\n",
                   rx_device_grid_mode);
        return 1;
      }
      if (rx_device_grid_mode == "off") {
        rx_device_grid_mode.clear();
      }
    } else if (arg == "--gpu-output-buffer" && i + 1 < argc) {
      gpu_output_buffer_mode = argv[++i];
      if ((gpu_output_buffer_mode != "vector") && (gpu_output_buffer_mode != "pinned") &&
          (gpu_output_buffer_mode != "managed")) {
        fmt::print("Unknown GPU output buffer mode: {}. Use vector, pinned, or managed\n", gpu_output_buffer_mode);
        return 1;
      }
    } else if (arg == "--help") {
      fmt::print("Usage: {} [options]\n", argv[0]);
      fmt::print("Options:\n");
      fmt::print("  --prb N         Number of PRBs (default: 106)\n");
      fmt::print("  --sinr N        SINR in dB (default: 20.0)\n");
      fmt::print("  --mcs N         MCS index (default: 20)\n");
      fmt::print("  --mcs-table T   MCS table: qam64, qam256, qam64LowSe (default: qam64)\n");
      fmt::print("  --iterations N  Number of measured iterations (default: 100)\n");
      fmt::print("  --warmup N      Number of warmup iterations before measurement (default: 5)\n");
      fmt::print("  --workers N     Background executor threads, 1-32 (default: up to 8)\n");
      fmt::print("  --seed N        Explicit payload/TA/CFO seed; use one worker for paired input replay\n");
      fmt::print("  --pace-ms N     Pause before each iteration, 0-1000 ms (default: 0)\n");
      fmt::print("  --sample-jsonl PATH  New per-frame timing/metric evidence file (never overwritten)\n");
      fmt::print("  --decoded-sensitivity  Record independent CPU/CUDA CRC outcomes; requires resident sample evidence\n");
      fmt::print("  --post-teardown-hold-ms N  Diagnostic only: hold 0..5000 ms after explicit owned-context cleanup\n");
      fmt::print("  --phase-observation-ms N  Test-only 0..5000-ms gaps after context/setup and before retirement; outside frame timing\n");
      fmt::print("  --receiver-capture-prefix PATH  Diagnostic-only raw grid/LLR captures; timing is perturbed, four PRB SISO, at most 64 frames\n");
      fmt::print("  --channel-profile STR  single-tap (default), TDLA, TDLB or TDLC; frequency-domain proxies, no Doppler\n");
      fmt::print("  --fading-distribution STR  uniform-phase (default) or rayleigh\n");
      fmt::print("  --cold-jsonl PATH  New flushed startup/teardown trace; separate from frame timing\n");
      fmt::print("  --dapp-manifest PATH  Explicit local CUDA receiver package; requires resident mode and samples\n");
      fmt::print("  --dapp-receiver-completion  Opt in only this isolated service to the real completion adapter\n");
      fmt::print("  --cuda-owned-context  Initialize/retain the test primary context before constructing PHY factories\n");
      fmt::print("  --cuda-init-only  With owned context and cold trace: initialize/retire only; no decode or dApp\n");
      fmt::print("  --trace-terminate  Isolated test only: print bounded abort stack; never resume after failure\n");
      fmt::print("  --sinr-method STR  channel_estimator or post_equalization (default) for both CPU/GPU arms\n");
      fmt::print("  --require-pilot-csi  Require selected channel-estimator SINR/EPRE/RSRP/TA/CFO after completion\n");
      fmt::print("  --require-all-tb  Fail unless all measured GPU transport blocks decode successfully\n");
      fmt::print("  --cpu-every N   Run the CPU reference decode every Nth measured iteration (default: 1).\n");
      fmt::print("                  N>1 keeps the GPU on a tighter slot cadence (the ~10ms CPU decode\n");
      fmt::print("                  otherwise lets the GPU idle/downclock between GPU slots).\n");
      fmt::print("  --ta-offset N   Max random TA offset in us (default: 0, disabled)\n");
      fmt::print("  --cfo-std N     CFO standard deviation in Hz (default: 0, disabled)\n");
      fmt::print("  --ports N       Number of RX ports (default: 1)\n");
      fmt::print("  --layers N      Number of TX layers (default: 1; 3-layer tests use at least 4 RX ports)\n");
      fmt::print("  --dmrs-symbols CSV  DMRS OFDM symbols, comma-separated, 1-4 entries (default: 2,11)\n");
      fmt::print("  --dmrs-type STR  DMRS type1 or type2 (default: type1)\n");
      fmt::print("  --transform-precoding  Enable one-layer DFT-s-OFDM PUSCH with low-PAPR DMRS\n");
      fmt::print("  --transform-deprecoder STR  Transform backend: custom, vkfft, auto (default: library default)\n");
      fmt::print("  --n-rs-id N      Low-PAPR DMRS n_RS_ID for transform precoding (default: 0)\n");
      fmt::print("  --rx-device-grid MODE  CUDA-visible RX grid mode: off, device, managed, direct-managed, auto\n");
      fmt::print("                         (default: auto with CUDA — managed on integrated GPUs like GB10)\n");
      fmt::print("  --gpu-output-buffer MODE  GPU output payload buffer: vector, pinned, managed (default: vector)\n");
      fmt::print("  --debug-logs    Enable PHY debug logs for path selection checks\n");
      fmt::print("  --require-gpu-resident  Fail unless every measured iteration uses resident GPU demod+decode\n");
      fmt::print("  --quiet         Suppress per-iteration output, only show summary\n");
      fmt::print("  --rnti N        RNTI value (default: 0x1234)\n");
      fmt::print("  --n-id N        Data scrambling n_ID (default: 0)\n");
      fmt::print("  --scrambling-id N  DMRS scrambling ID (default: 0)\n");
      fmt::print("  --n-scid N      DMRS n_SCID 0 or 1 (default: 0)\n");
      return 0;
    } else {
      throw std::runtime_error("Unknown option or missing value: " + arg);
    }
  }

  if (!nof_prb || nof_prb > MAX_NOF_PRBS || !nof_iterations || nof_iterations > 100000 || nof_warmup > 1000 ||
      !std::isfinite(sinr_dB))
    return 1;
  if (decoded_sensitivity && (!require_gpu_resident || sample_jsonl_path.empty() || !require_pilot_csi)) {
    throw std::runtime_error("--decoded-sensitivity requires --require-gpu-resident, --require-pilot-csi and --sample-jsonl");
  }
  if (receiver_completion && dapp_manifest.empty()) {
    throw std::runtime_error("--dapp-receiver-completion requires an explicit receiver manifest");
  }
  if (!receiver_capture_prefix.empty()) {
#ifndef OCUDU_PUSCH_TEST_DAPP
    throw std::runtime_error("Receiver capture requires CUDA/dApp test support");
#endif
    if (!cuda_owned_context || cuda_init_only || !decoded_sensitivity || cold_jsonl_path.empty() ||
        !payload_seed || benchmark_workers != 1 || nof_prb != 4 || nof_layers != 1 || nof_rx_ports != 1 ||
        nof_warmup + nof_iterations > 64 || enable_transform_precoding || dmrs != dmrs_config_type::type1 ||
        rnti != 0x1234 || n_id != 0 || scrambling_id != 0 || n_scid ||
        dmrs_symbols_mask.to_uint64() != ((1U << 2) | (1U << 11)) ||
        (!dapp_manifest.empty() && !receiver_completion)) {
      throw std::runtime_error("Receiver capture requires seeded, owned, traced, serialized four-PRB SISO decoded test; at most 64 frames");
    }
  }
  if (post_teardown_hold.count() && (!cuda_owned_context || cold_jsonl_path.empty() || cuda_init_only)) {
    throw std::runtime_error("Post-teardown hold requires a full owned-context run and --cold-jsonl");
  }
  if (phase_observation.count() && (!cuda_owned_context || cold_jsonl_path.empty() ||
                                     sample_jsonl_path.empty() || cuda_init_only)) {
    throw std::runtime_error("Phase observation requires a full owned-context run, --cold-jsonl and --sample-jsonl");
  }
  if (cuda_init_only && (!cuda_owned_context || !dapp_manifest.empty() || !sample_jsonl_path.empty())) {
    throw std::runtime_error("--cuda-init-only requires owned context and forbids receiver/sample arguments");
  }
  if (cuda_owned_context && cold_jsonl_path.empty()) {
    throw std::runtime_error("--cuda-owned-context requires an exclusive --cold-jsonl trace");
  }
#ifndef ENABLE_CUDA
  if (cuda_owned_context || cuda_init_only) {
    throw std::runtime_error("Owned context qualification requires a CUDA build");
  }
#endif
  if (require_pilot_csi && (sinr_method != channel_state_information::sinr_type::channel_estimator ||
                            !require_gpu_resident || sample_jsonl_path.empty())) {
    throw std::runtime_error("--require-pilot-csi requires channel_estimator SINR, resident mode and samples");
  }
  if (!dapp_manifest.empty()) {
#ifndef OCUDU_PUSCH_TEST_DAPP
    fmt::print("Receiver dApp test support is not compiled in.\n");
    return 1;
#endif
    if (!require_gpu_resident || sample_jsonl_path.empty()) {
      fmt::print("--dapp-manifest requires --require-gpu-resident and --sample-jsonl.\n");
      return 1;
    }
  }
  // Requiring matched pilot CSI must select its producer in BOTH test arms.
  // Checking only for finite conventional metrics lets the baseline pass while
  // an inline receiver correctly leaves unavailable physical CSI unset.
  // This affects only this test process and runs before any PHY factory can
  // cache the environment option. Production defaults remain unchanged.
  if (require_pilot_csi && setenv("OCUDU_PUSCH_COMMON_PILOT_CSI", "1", 1) != 0) {
    throw std::runtime_error("Failed to enable required common-pilot CSI in the isolated test");
  }
  test_support::pusch_cold_trace cold_trace(cold_jsonl_path);
  if (trace_terminate) {
    test_support::enable_test_termination_trace();
  }
  cold_trace.measure("logging_init", [] { ocudulog::init(); });
#ifdef ENABLE_CUDA
  test_support::pusch_test_cuda_context_operations context_operations(cold_trace);
  test_support::pusch_test_primary_context         context_owner(context_operations);
  if (cuda_owned_context) {
    cold_trace.measure("cuda_owned_context_initialize", [&] {
      if (!context_owner.initialize()) {
        throw std::runtime_error("Owned primary context initialization/identity validation failed");
      }
    });
  }
  if (cuda_init_only) {
    cold_trace.measure("cuda_owned_context_close", [&] {
      if (!context_owner.close()) {
        throw std::runtime_error("Owned primary context restoration/retirement failed");
      }
    });
    cold_trace.record("process", "complete");
    fmt::print("{{\"cuda_initialization_only\":true,\"decoded_frames\":0,\"receiver_invocations\":0}}\n");
    return 0;
  }
  // Test-only separation of cold startup, frame execution and retirement.
  // Never called from production L1; these gaps cannot qualify full-rate isolation.
  if (phase_observation.count()) {
    cold_trace.measure("observe_context", [] { std::this_thread::sleep_for(phase_observation); });
  }
#endif
  if ((nof_layers == 3) && (nof_rx_ports < 4)) {
    nof_rx_ports = 4;
  } else if (nof_rx_ports < nof_layers) {
    nof_rx_ports = nof_layers;
  }
  if (!transform_deprecoder_backend.empty()) {
#ifdef ENABLE_CUDA
    if (setenv(PUSCH_TRANSFORM_DEPRECODER_ENV, transform_deprecoder_backend.c_str(), 1) != 0) {
      throw std::runtime_error("Failed to set requested CUDA transform-deprecoder backend");
    }
#endif
  }
  if (require_gpu_resident && std::getenv("OCUDU_PUSCH_ACCELERATION_TIMING") == nullptr) {
    setenv("OCUDU_PUSCH_ACCELERATION_TIMING", "1", 1);
  }
  run_e2e_pipeline_test(nof_prb, sinr_dB, mcs_idx, cold_trace);
#ifdef ENABLE_CUDA
  if (cuda_owned_context) {
    cold_trace.measure("cuda_owned_context_close", [&] {
      if (!context_owner.close()) {
        throw std::runtime_error("Owned primary context restoration/retirement failed");
      }
    });
  }
#endif
  // Includes local grid/service destruction after the function's explicit teardown.
  // Optional control-only diagnostic: distinguish explicit CUDA cleanup from
  // runtime/static destruction at process exit. Never part of per-frame timing.
  if (post_teardown_hold.count()) {
    cold_trace.measure("post_teardown_hold", [] { std::this_thread::sleep_for(post_teardown_hold); });
  }
  cold_trace.record("process", "complete");
  return 0;
} catch (const std::exception& error) {
  fmt::print(stderr, "PUSCH test failed: {}\n", error.what());
  return 2;
}
