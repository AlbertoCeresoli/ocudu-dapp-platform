// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Standalone PDSCH GPU end-to-end latency benchmark.

#include "ocudu/adt/scope_exit.h"
#include "ocudu/phy/antenna_ports.h"
#include "ocudu/phy/support/precoding_configuration.h"
#include "ocudu/phy/support/re_pattern.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/resource_grid_mapper.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_segmenter_buffer.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_segmenter_tx.h"
#include "ocudu/phy/upper/channel_modulation/modulation_mapper.h"
#include "ocudu/phy/upper/channel_processors/pdsch/factories.h"
#include "ocudu/phy/upper/channel_processors/pdsch/pdsch_processor.h"
#include "ocudu/phy/upper/signal_processors/pdsch/factories.h"
#include "ocudu/phy/upper/signal_processors/ptrs/ptrs_pdsch_generator_factory.h"
#include "ocudu/ran/pdsch/pdsch_mcs.h"
#include "ocudu/ran/precoding/precoding_codebooks.h"
#include "ocudu/ran/resource_allocation/rb_interval.h"
#include "ocudu/ran/sch/sch_dmrs_power.h"
#include "ocudu/ran/sch/sch_mcs.h"
#include "ocudu/ran/sch/sch_segmentation.h"
#include "ocudu/ran/sch/tbs_calculator.h"
#include "ocudu/support/executors/inline_task_executor.h"
#include "ocudu/support/math/math_utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef ENABLE_CUDA
#include "channel_processors/pdsch/pdsch_processor_helpers.h"
#include "cuda/cuda_rt_utils.h"
#include "cuda/pdsch_device_grid_writer_cuda.h"
#include "cuda/pdsch_resource_grid_mapper_cuda.h"
#include "resource_grid_cuda_visible_impl.h"
#include "transport_block.h"
#include <cuda_runtime.h>
#endif

using namespace ocudu;

static constexpr subcarrier_spacing scs           = subcarrier_spacing::kHz30;
static constexpr uint16_t           rnti          = 0x1234;
static constexpr unsigned           bwp_start_rb  = 0;
static constexpr cyclic_prefix      cy_prefix     = cyclic_prefix::NORMAL;
static constexpr unsigned           n_id          = 0;
static constexpr unsigned           scrambling_id = 0;
static constexpr bool               n_scid        = false;

namespace {

class pdsch_processor_notifier_adaptor : public pdsch_processor_notifier
{
public:
  void reset() { completed.store(false, std::memory_order_release); }

  void on_finish_processing() override { completed.store(true, std::memory_order_release); }

  void wait_for_completion() const
  {
    while (!completed.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

private:
  std::atomic<bool> completed = {false};
};

template <typename T>
std::shared_ptr<T> require_factory(std::shared_ptr<T> factory, const char* name)
{
  if (!factory) {
    throw std::runtime_error(std::string("Failed to create ") + name);
  }
  return factory;
}

std::shared_ptr<pdsch_processor_factory>
create_benchmark_pdsch_processor_factory(std::shared_ptr<pdsch_block_processor_factory> block_processor_factory,
                                         task_executor&                                 executor,
                                         unsigned                                       cb_batch_length)
{
  auto crc_factory       = require_factory(create_crc_calculator_factory_sw("auto"), "CRC factory");
  auto segmenter_factory = require_factory(create_ldpc_segmenter_tx_factory_sw(crc_factory), "LDPC segmenter factory");
  auto prg_factory       = require_factory(create_pseudo_random_generator_sw_factory(), "PRG factory");
  auto precoder_factory  = require_factory(create_channel_precoder_factory("auto"), "precoder factory");
  auto rg_mapper_factory = require_factory(create_resource_grid_mapper_factory(precoder_factory), "RG mapper factory");
  auto dmrs_factory =
      require_factory(create_dmrs_pdsch_processor_factory_sw(prg_factory, rg_mapper_factory), "DMRS factory");
  auto ptrs_factory =
      require_factory(create_ptrs_pdsch_generator_generic_factory(prg_factory, rg_mapper_factory), "PTRS factory");

  return require_factory(create_pdsch_flexible_processor_factory_sw(segmenter_factory,
                                                                    block_processor_factory,
                                                                    rg_mapper_factory,
                                                                    dmrs_factory,
                                                                    ptrs_factory,
                                                                    executor,
                                                                    1,
                                                                    cb_batch_length),
                         "PDSCH processor factory");
}

std::shared_ptr<pdsch_block_processor_factory> create_cpu_pdsch_block_processor_factory()
{
  auto ldpc_encoder_factory       = require_factory(create_ldpc_encoder_factory_sw("auto"), "LDPC encoder factory");
  auto ldpc_rate_matcher_factory  = require_factory(create_ldpc_rate_matcher_factory_sw(), "LDPC rate matcher factory");
  auto prg_factory                = require_factory(create_pseudo_random_generator_sw_factory(), "PRG factory");
  auto modulation_mapper_factory  = require_factory(create_modulation_mapper_factory(), "modulation mapper factory");
  auto pdsch_block_processor_fact = create_pdsch_block_processor_factory_sw(
      ldpc_encoder_factory, ldpc_rate_matcher_factory, prg_factory, modulation_mapper_factory);
  return require_factory(pdsch_block_processor_fact, "CPU PDSCH block processor factory");
}

unsigned calculate_tbs(const sch_mcs_description& mcs_descr,
                       unsigned                   nof_prb,
                       unsigned                   nof_layers,
                       dmrs_config_type           dmrs_cfg_type,
                       const symbol_slot_mask&    dmrs_symbol_mask,
                       unsigned                   nof_cdm_groups_without_data,
                       unsigned                   nof_symbols)
{
  tbs_calculator_configuration tbs_config = {};
  tbs_config.mcs_descr                    = mcs_descr;
  tbs_config.n_prb                        = nof_prb;
  tbs_config.nof_layers                   = nof_layers;
  tbs_config.nof_symb_sh                  = nof_symbols;
  tbs_config.nof_dmrs_prb = get_nof_re_per_prb(dmrs_cfg_type) * dmrs_symbol_mask.count() * nof_cdm_groups_without_data;
  return tbs_calculator_calculate(tbs_config).to_bits().value();
}

pdsch_processor::pdu_t build_pdsch_pdu(const sch_mcs_description& mcs_descr,
                                       unsigned                   nof_prb,
                                       unsigned                   bwp_size_rb,
                                       unsigned                   vrb_start_rb,
                                       unsigned                   nof_layers,
                                       unsigned                   start_symbol_index,
                                       unsigned                   nof_symbols,
                                       unsigned                   rv,
                                       dmrs_config_type           dmrs_cfg_type,
                                       const symbol_slot_mask&    dmrs_symbol_mask,
                                       unsigned                   nof_cdm_groups_without_data,
                                       unsigned                   tbs)
{
  pdsch_processor::pdu_t pdu;
  pdu.context                     = std::nullopt;
  pdu.slot                        = slot_point(to_numerology_value(scs), 0);
  pdu.rnti                        = to_rnti(rnti);
  pdu.bwp_size_rb                 = bwp_size_rb;
  pdu.bwp_start_rb                = bwp_start_rb;
  pdu.cp                          = cy_prefix;
  pdu.n_id                        = n_id;
  pdu.ref_point                   = pdsch_processor::pdu_t::PRB0;
  pdu.dmrs_symbol_mask            = dmrs_symbol_mask;
  pdu.dmrs                        = dmrs_cfg_type;
  pdu.scrambling_id               = scrambling_id;
  pdu.n_scid                      = n_scid;
  pdu.nof_cdm_groups_without_data = nof_cdm_groups_without_data;
  pdu.freq_alloc                  = rb_allocation::make_type1(vrb_start_rb, nof_prb, std::nullopt);
  pdu.start_symbol_index          = start_symbol_index;
  pdu.nof_symbols                 = nof_symbols;
  pdu.tbs_lbrm                    = tbs_lbrm_default;
  pdu.reserved                    = {};
  pdu.ptrs                        = std::nullopt;
  pdu.ratio_pdsch_data_to_sss_dB  = 0.0F;
  pdu.ratio_pdsch_dmrs_to_sss_dB  = get_sch_to_dmrs_ratio_dB(nof_cdm_groups_without_data);
  pdu.precoding                   = precoding_configuration::make_wideband(make_identity(nof_layers));
  pdu.codewords.emplace_back(pdsch_processor::codeword_description{
      mcs_descr.modulation, rv, get_ldpc_base_graph(mcs_descr.get_normalised_target_code_rate(), units::bits(tbs))});

  return pdu;
}

unsigned parse_unsigned_arg(const char* arg, const char* prefix, unsigned fallback)
{
  std::string_view value(arg);
  std::string_view key(prefix);
  if (value.size() < key.size() || value.substr(0, key.size()) != key) {
    return fallback;
  }
  return static_cast<unsigned>(std::strtoul(std::string(value.substr(key.size())).c_str(), nullptr, 10));
}

unsigned parse_cb_batch_arg(const char* arg, unsigned fallback)
{
  static constexpr std::string_view prefix = "--cb-batch=";
  std::string_view                  value(arg);
  if (value.size() < prefix.size() || value.substr(0, prefix.size()) != prefix) {
    return fallback;
  }

  value.remove_prefix(prefix.size());
  if (value == "all" || value == "sync") {
    return std::numeric_limits<unsigned>::max();
  }
  return static_cast<unsigned>(std::strtoul(std::string(value).c_str(), nullptr, 10));
}

std::string parse_string_arg(const char* arg, const char* prefix, std::string fallback)
{
  std::string_view value(arg);
  std::string_view key(prefix);
  if (value.size() < key.size() || value.substr(0, key.size()) != key) {
    return fallback;
  }
  return std::string(value.substr(key.size()));
}

pdsch_mcs_table parse_mcs_table(std::string_view value)
{
  if (value == "qam64") {
    return pdsch_mcs_table::qam64;
  }
  if (value == "qam256") {
    return pdsch_mcs_table::qam256;
  }
  if (value == "qam64LowSe") {
    return pdsch_mcs_table::qam64LowSe;
  }
  throw std::runtime_error("Unsupported MCS table. Use qam64, qam256, or qam64LowSe.");
}

dmrs_config_type parse_dmrs_type(std::string_view value)
{
  if ((value == "type1") || (value == "1")) {
    return dmrs_config_type::type1;
  }
  if ((value == "type2") || (value == "2")) {
    return dmrs_config_type::type2;
  }
  throw std::runtime_error("Unsupported DMRS type. Use type1 or type2.");
}

const char* dmrs_type_name(dmrs_config_type value)
{
  return (value == dmrs_config_type::type1) ? "type1" : "type2";
}

symbol_slot_mask make_dmrs_symbol_mask(std::string_view profile)
{
  if (profile == "single") {
    return {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  }
  if (profile == "double") {
    return {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0};
  }
  if ((profile == "triple") || (profile == "three")) {
    return {0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0};
  }
  throw std::runtime_error("Unsupported DMRS profile. Use single, double, or triple.");
}

precoding_weight_matrix make_precoding_weights(std::string_view profile,
                                               unsigned         nof_layers,
                                               unsigned         nof_ports,
                                               unsigned         active_port,
                                               unsigned         codebook_index)
{
  if (profile == "all-ports") {
    if (nof_layers != 1) {
      throw std::runtime_error("--precoding=all-ports requires --layers=1.");
    }
    return make_one_layer_all_ports(nof_ports);
  }
  if (profile == "one-port") {
    if (nof_layers != 1) {
      throw std::runtime_error("--precoding=one-port requires --layers=1.");
    }
    if (active_port >= nof_ports) {
      throw std::runtime_error("--active-port must be lower than --ports.");
    }
    return make_one_layer_one_port(nof_ports, active_port);
  }
  if (profile == "identity") {
    if (nof_layers != nof_ports) {
      throw std::runtime_error("--precoding=identity requires --layers to equal --ports.");
    }
    return make_identity(nof_layers);
  }
  if (profile == "one-layer-two-ports") {
    if ((nof_layers != 1) || (nof_ports != 2) || (codebook_index > 3)) {
      throw std::runtime_error("--precoding=one-layer-two-ports requires --layers=1 --ports=2 --precoding-index=0..3.");
    }
    return make_one_layer_two_ports(codebook_index);
  }
  if (profile == "two-layer-two-ports") {
    if ((nof_layers != 2) || (nof_ports != 2) || (codebook_index > 1)) {
      throw std::runtime_error("--precoding=two-layer-two-ports requires --layers=2 --ports=2 --precoding-index=0..1.");
    }
    return make_two_layer_two_ports(codebook_index);
  }
  throw std::runtime_error(
      "Unsupported precoding. Use all-ports, one-port, identity, one-layer-two-ports, or two-layer-two-ports.");
}

double percentile_us(const std::vector<double>& sorted, double percentile)
{
  if (sorted.empty()) {
    return 0.0;
  }
  size_t index = static_cast<size_t>((static_cast<double>(sorted.size() - 1) * percentile) / 100.0);
  return sorted[std::min(index, sorted.size() - 1)];
}

#ifdef ENABLE_CUDA
void throw_on_cuda(cudaError_t status, const char* operation)
{
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string(operation) + " failed: " + cudaGetErrorString(status));
  }
}

void throw_on_ldpc(nr_ldpc_status_t status, const char* operation)
{
  if (status != NR_LDPC_SUCCESS) {
    throw std::runtime_error(std::string(operation) + " failed with status " + std::to_string(status));
  }
}

class encoder_batch_device_symbol_buffer : public resource_grid_mapper::symbol_buffer
{
public:
  encoder_batch_device_symbol_buffer(unsigned     nof_symbols_,
                                     unsigned     nof_ports_,
                                     unsigned     nof_layers_,
                                     cudaStream_t stream_) :
    nof_symbols(nof_symbols_), nof_ports(nof_ports_), nof_layers(nof_layers_), stream(stream_)
  {
  }

  ~encoder_batch_device_symbol_buffer() override
  {
    if (d_re_offsets != nullptr) {
      cudaFree(d_re_offsets);
    }
  }

  encoder_batch_device_symbol_buffer(const encoder_batch_device_symbol_buffer&)            = delete;
  encoder_batch_device_symbol_buffer& operator=(const encoder_batch_device_symbol_buffer&) = delete;

  void reset(const int8_t* d_symbols_)
  {
    d_symbols = d_symbols_;
    consumed  = false;
  }

  unsigned get_max_block_size() const override { return consumed ? 0 : nof_symbols; }

  bool empty() const override { return consumed; }

  span<const ci8_t> pop_symbols(unsigned) override
  {
    throw std::runtime_error("Encoder-batch device symbol buffer unexpectedly fell back to host mapping.");
  }

  bool supports_device_grid_mapping() const override { return d_symbols != nullptr; }

  bool map_to_device_grid(span<const uint32_t> re_offsets,
                          void*                d_grid_bf16,
                          unsigned             mapped_nof_ports,
                          unsigned             mapped_nof_layers,
                          unsigned             nof_grid_re_per_port,
                          float                weight_real) override
  {
    if (consumed || (d_symbols == nullptr) || (d_grid_bf16 == nullptr) || re_offsets.empty() ||
        (mapped_nof_ports != nof_ports) || (mapped_nof_layers != nof_layers) ||
        (re_offsets.size() * nof_layers != nof_symbols)) {
      return false;
    }

    size_t offsets_bytes = re_offsets.size() * sizeof(uint32_t);
    if (offsets_bytes > d_re_offsets_capacity) {
      if (d_re_offsets != nullptr) {
        cudaFree(d_re_offsets);
        d_re_offsets = nullptr;
      }
      if (cudaMalloc(&d_re_offsets, offsets_bytes) != cudaSuccess) {
        d_re_offsets_capacity = 0;
        return false;
      }
      d_re_offsets_capacity = offsets_bytes;
      cached_re_offsets.clear();
    }

    bool offsets_cached = (cached_re_offsets.size() == re_offsets.size()) &&
                          (std::memcmp(cached_re_offsets.data(), re_offsets.data(), offsets_bytes) == 0);
    if (!offsets_cached) {
      if (cudaMemcpyAsync(d_re_offsets, re_offsets.data(), offsets_bytes, cudaMemcpyHostToDevice, stream) !=
          cudaSuccess) {
        return false;
      }
      cached_re_offsets.assign(re_offsets.begin(), re_offsets.end());
    }

    bool launch_ok = pdsch_map_layers_int8_to_bf16_real_grid(d_symbols,
                                                             static_cast<const uint32_t*>(d_re_offsets),
                                                             static_cast<uint16_t*>(d_grid_bf16),
                                                             re_offsets.size(),
                                                             nof_ports,
                                                             nof_layers,
                                                             nof_grid_re_per_port,
                                                             weight_real,
                                                             stream);
    if (!launch_ok) {
      return false;
    }

    consumed = true;
    return true;
  }

  void* get_device_grid_mapping_stream() const override { return stream; }

private:
  const int8_t*         d_symbols             = nullptr;
  unsigned              nof_symbols           = 0;
  unsigned              nof_ports             = 0;
  unsigned              nof_layers            = 0;
  cudaStream_t          stream                = nullptr;
  bool                  consumed              = false;
  uint32_t*             d_re_offsets          = nullptr;
  size_t                d_re_offsets_capacity = 0;
  std::vector<uint32_t> cached_re_offsets;
};
#endif

} // namespace

int main(int argc, char** argv)
{
  try {
    unsigned    warmup_iterations       = 10;
    unsigned    iterations              = 100;
    unsigned    runs                    = 1;
    unsigned    cb_batch_length         = std::numeric_limits<unsigned>::max();
    unsigned    nof_prb                 = 273;
    unsigned    nof_layers              = 1;
    unsigned    nof_ports               = 0;
    unsigned    mcs_index_value         = 27;
    unsigned    rv                      = 0;
    unsigned    start_symbol            = 0;
    unsigned    nof_symbols             = 14;
    unsigned    nof_cdm_groups          = 2;
    unsigned    active_port             = 0;
    unsigned    precoding_index         = 0;
    unsigned    nof_pdus                = 1;
    unsigned    encoder_batch           = 0;
    unsigned    encoder_batch_map       = 1;
    unsigned    use_device_grid         = 0;
    unsigned    materialize_device_grid = 0;
    unsigned    sync_after_process      = 1;
    std::string backend                 = "gpu";
    std::string mcs_table_name          = "qam64";
    std::string dmrs_profile            = "double";
    std::string dmrs_type_name_arg      = "type1";
    std::string device_grid_memory      = "device";
    std::string resource_grid_memory    = "host";
    std::string precoding_profile       = "auto";

    for (int i = 1; i != argc; ++i) {
      warmup_iterations       = parse_unsigned_arg(argv[i], "--warmup=", warmup_iterations);
      iterations              = parse_unsigned_arg(argv[i], "--iterations=", iterations);
      runs                    = parse_unsigned_arg(argv[i], "--runs=", runs);
      nof_prb                 = parse_unsigned_arg(argv[i], "--prb=", nof_prb);
      nof_layers              = parse_unsigned_arg(argv[i], "--layers=", nof_layers);
      nof_ports               = parse_unsigned_arg(argv[i], "--ports=", nof_ports);
      mcs_index_value         = parse_unsigned_arg(argv[i], "--mcs=", mcs_index_value);
      rv                      = parse_unsigned_arg(argv[i], "--rv=", rv);
      start_symbol            = parse_unsigned_arg(argv[i], "--start-symbol=", start_symbol);
      nof_symbols             = parse_unsigned_arg(argv[i], "--symbols=", nof_symbols);
      nof_cdm_groups          = parse_unsigned_arg(argv[i], "--cdm=", nof_cdm_groups);
      active_port             = parse_unsigned_arg(argv[i], "--active-port=", active_port);
      precoding_index         = parse_unsigned_arg(argv[i], "--precoding-index=", precoding_index);
      nof_pdus                = parse_unsigned_arg(argv[i], "--pdus=", nof_pdus);
      encoder_batch           = parse_unsigned_arg(argv[i], "--encoder-batch=", encoder_batch);
      encoder_batch_map       = parse_unsigned_arg(argv[i], "--encoder-batch-map=", encoder_batch_map);
      use_device_grid         = parse_unsigned_arg(argv[i], "--device-grid=", use_device_grid);
      materialize_device_grid = parse_unsigned_arg(argv[i], "--materialize-device-grid=", materialize_device_grid);
      sync_after_process      = parse_unsigned_arg(argv[i], "--sync-after-process=", sync_after_process);
      cb_batch_length         = parse_cb_batch_arg(argv[i], cb_batch_length);
      backend                 = parse_string_arg(argv[i], "--backend=", backend);
      mcs_table_name          = parse_string_arg(argv[i], "--mcs-table=", mcs_table_name);
      dmrs_profile            = parse_string_arg(argv[i], "--dmrs=", dmrs_profile);
      dmrs_type_name_arg      = parse_string_arg(argv[i], "--dmrs-type=", dmrs_type_name_arg);
      device_grid_memory      = parse_string_arg(argv[i], "--device-grid-memory=", device_grid_memory);
      resource_grid_memory    = parse_string_arg(argv[i], "--resource-grid-memory=", resource_grid_memory);
      precoding_profile       = parse_string_arg(argv[i], "--precoding=", precoding_profile);
    }

    if (backend == "gpu" && !is_pdsch_block_processor_acceleration_available()) {
      std::cerr << "CUDA PDSCH GPU block processor is not available in this build/runtime.\n";
      return 77;
    }
    if ((use_device_grid != 0) && (backend != "gpu")) {
      throw std::runtime_error("--device-grid=1 requires --backend=gpu.");
    }
    if ((encoder_batch != 0) && (backend != "gpu")) {
      throw std::runtime_error("--encoder-batch=1 requires --backend=gpu.");
    }
    if (nof_pdus == 0) {
      throw std::runtime_error("--pdus must be greater than zero.");
    }
    if (nof_prb * nof_pdus > MAX_NOF_PRBS) {
      throw std::runtime_error("--pdus is too large for the requested PRB allocation.");
    }
    if ((device_grid_memory != "device") && (device_grid_memory != "managed")) {
      throw std::runtime_error("Unsupported device grid memory. Use --device-grid-memory=device or managed.");
    }
    if ((resource_grid_memory != "host") && (resource_grid_memory != "managed") && (resource_grid_memory != "auto")) {
      throw std::runtime_error("Unsupported resource grid memory. Use --resource-grid-memory=host, managed or auto.");
    }
#ifndef ENABLE_CUDA
    if (encoder_batch != 0) {
      throw std::runtime_error("--encoder-batch=1 requires a CUDA build.");
    }
    if (use_device_grid != 0) {
      throw std::runtime_error("--device-grid=1 requires a CUDA build.");
    }
    if ((resource_grid_memory == "managed") || (resource_grid_memory == "auto")) {
      throw std::runtime_error("--resource-grid-memory=managed/auto requires a CUDA build.");
    }
#endif

    inline_task_executor                           executor;
    std::shared_ptr<pdsch_block_processor_factory> block_factory;
    if (backend == "gpu") {
      block_factory = require_factory(create_pdsch_block_processor_factory_accelerated(), "GPU block factory");
    } else if (backend == "cpu") {
      block_factory = create_cpu_pdsch_block_processor_factory();
    } else {
      throw std::runtime_error("Unsupported backend. Use --backend=gpu or --backend=cpu.");
    }
    auto proc_factory = create_benchmark_pdsch_processor_factory(block_factory, executor, cb_batch_length);
    auto grid_factory = require_factory(create_resource_grid_factory(), "resource-grid factory");

    if (nof_ports == 0) {
      nof_ports = nof_layers;
    }
    if (precoding_profile == "auto") {
      precoding_profile = (nof_layers == 1) ? "all-ports" : "identity";
    }
    pdsch_mcs_table         mcs_table        = parse_mcs_table(mcs_table_name);
    dmrs_config_type        dmrs_cfg_type    = parse_dmrs_type(dmrs_type_name_arg);
    sch_mcs_index           mcs_index        = sch_mcs_index(mcs_index_value);
    symbol_slot_mask        dmrs_symbol_mask = make_dmrs_symbol_mask(dmrs_profile);
    precoding_weight_matrix precoding_weights =
        make_precoding_weights(precoding_profile, nof_layers, nof_ports, active_port, precoding_index);

    sch_mcs_description mcs_descr = pdsch_mcs_get_config(mcs_table, mcs_index);
    unsigned            tbs =
        calculate_tbs(mcs_descr, nof_prb, nof_layers, dmrs_cfg_type, dmrs_symbol_mask, nof_cdm_groups, nof_symbols);
    if ((tbs % 8) != 0) {
      throw std::runtime_error("Calculated TBS is not byte-aligned.");
    }

    unsigned batch_bwp_size_rb = nof_prb * nof_pdus;

    std::vector<pdsch_processor::pdu_t> pdus;
    pdus.reserve(nof_pdus);
    for (unsigned i_pdu = 0; i_pdu != nof_pdus; ++i_pdu) {
      pdsch_processor::pdu_t pdu = build_pdsch_pdu(mcs_descr,
                                                   nof_prb,
                                                   batch_bwp_size_rb,
                                                   i_pdu * nof_prb,
                                                   nof_layers,
                                                   start_symbol,
                                                   nof_symbols,
                                                   rv,
                                                   dmrs_cfg_type,
                                                   dmrs_symbol_mask,
                                                   nof_cdm_groups,
                                                   tbs);
      pdu.precoding              = precoding_configuration::make_wideband(precoding_weights);
      pdus.push_back(std::move(pdu));
    }

    std::vector<std::vector<uint8_t>> tx_data(nof_pdus, std::vector<uint8_t>(tbs / 8));
    for (unsigned i_pdu = 0; i_pdu != nof_pdus; ++i_pdu) {
      std::mt19937 rgen(0x5eed1234U + nof_prb * 17U + mcs_index.value() * 101U + i_pdu * 997U);
      std::generate(
          tx_data[i_pdu].begin(), tx_data[i_pdu].end(), [&rgen]() { return static_cast<uint8_t>(rgen() & 0xff); });
    }

#ifdef ENABLE_CUDA
    if (encoder_batch != 0) {
      unsigned nof_re_per_prb = NOF_SUBCARRIERS_PER_RB * nof_symbols -
                                get_nof_re_per_prb(dmrs_cfg_type) * dmrs_symbol_mask.count() * nof_cdm_groups;
      unsigned    nof_re_pdsch = nof_prb * nof_re_per_prb;
      unsigned    nof_cb = compute_nof_codeblocks(units::bits(tbs), pdus.front().codewords.front().ldpc_base_graph);
      units::bits Nref   = ldpc::compute_N_ref(pdus.front().tbs_lbrm, nof_cb);

      auto segmenter_crc_factory =
          require_factory(create_crc_calculator_factory_sw("auto"), "encoder-batch CRC factory");
      auto segmenter_factory = require_factory(create_ldpc_segmenter_tx_factory_sw(segmenter_crc_factory),
                                               "encoder-batch LDPC segmenter factory");
      std::unique_ptr<ldpc_segmenter_tx> segmenter = segmenter_factory->create();
      if (!segmenter) {
        throw std::runtime_error("Failed to create encoder-batch LDPC segmenter.");
      }

      segmenter_config             segmenter_cfg  = {.transport_block_size = units::bytes(tbs / 8),
                                                     .base_graph     = pdus.front().codewords.front().ldpc_base_graph,
                                                     .rv             = rv,
                                                     .mod            = mcs_descr.modulation,
                                                     .Nref           = Nref.value(),
                                                     .nof_layers     = nof_layers,
                                                     .nof_ch_symbols = nof_layers * nof_re_pdsch};
      const ldpc_segmenter_buffer& segment_buffer = segmenter->new_transmission(segmenter_cfg);
      const codeblock_metadata     cb_meta        = segment_buffer.get_cb_metadata(0);

      unsigned nof_short = segment_buffer.get_nof_short_segments();
      unsigned E_short   = (nof_short > 0) ? segment_buffer.get_rm_length(0) : 0;
      unsigned E_long =
          (nof_short < segment_buffer.get_nof_codeblocks()) ? segment_buffer.get_rm_length(nof_short) : E_short;
      if (E_short == 0) {
        E_short = E_long;
      }

      tb_encoder_config_t tb_cfg = {};
      tb_cfg.tb_size_bits        = static_cast<int>(tbs);
      tb_cfg.num_layers          = static_cast<int>(nof_layers);
      tb_cfg.modulation_order    = static_cast<int>(get_bits_per_symbol(mcs_descr.modulation));
      tb_cfg.num_allocated_res   = static_cast<int>(segment_buffer.get_cw_length().value());
      tb_cfg.redundancy_version  = static_cast<int>(rv);
      tb_cfg.code_rate           = static_cast<float>(tbs) / static_cast<float>(tb_cfg.num_allocated_res);
      tb_cfg.n_RNTI              = rnti;
      tb_cfg.n_ID                = n_id;
      tb_cfg.q                   = 0;
      tb_cfg.enable_scrambling   = true;
      tb_cfg.base_graph          = static_cast<int>(cb_meta.tb_common.base_graph);
      tb_cfg.num_code_blocks     = static_cast<int>(segment_buffer.get_nof_codeblocks());
      tb_cfg.lifting_size        = static_cast<int>(cb_meta.tb_common.lifting_size);
      tb_cfg.nof_filler_bits     = static_cast<int>(cb_meta.cb_specific.nof_filler_bits);
      tb_cfg.nof_short_segments  = static_cast<int>(nof_short);
      tb_cfg.E_short             = static_cast<int>(E_short);
      tb_cfg.E_long              = static_cast<int>(E_long);

      tb_batch_encoder_config_t batch_cfg = {};
      batch_cfg.tb_cfg                    = tb_cfg;
      batch_cfg.max_batch_size            = static_cast<int>(nof_pdus);

      tb_batch_encoder_handle_t batch_encoder = nullptr;
      throw_on_ldpc(tb_batch_encoder_create(&batch_encoder), "tb_batch_encoder_create");
      auto destroy_batch_encoder = make_scope_exit([&]() { tb_batch_encoder_destroy(batch_encoder); });
      throw_on_ldpc(tb_batch_encoder_configure(batch_encoder, &batch_cfg), "tb_batch_encoder_configure");

      tb_encoder_handle_t single_encoder = nullptr;
      throw_on_ldpc(tb_encoder_create(&single_encoder), "tb_encoder_create");
      auto destroy_single_encoder = make_scope_exit([&]() { tb_encoder_destroy(single_encoder); });
      throw_on_ldpc(tb_encoder_configure(single_encoder, &tb_cfg), "tb_encoder_configure");

      cudaStream_t stream = nullptr;
      throw_on_cuda(cudaStreamCreate(&stream), "cudaStreamCreate");
      auto destroy_stream = make_scope_exit([&]() { cudaStreamDestroy(stream); });

      const size_t tb_bytes           = tbs / 8;
      const size_t batch_input_bytes  = tb_bytes * nof_pdus;
      const size_t symbols_per_tb     = static_cast<size_t>(tb_cfg.num_allocated_res / tb_cfg.modulation_order);
      const size_t batch_symbol_bytes = symbols_per_tb * nof_pdus * sizeof(ci8_t);

      uint8_t* h_batch_input = nullptr;
      throw_on_cuda(cudaMallocHost(&h_batch_input, batch_input_bytes), "cudaMallocHost(batch input)");
      auto free_host_input = make_scope_exit([&]() { cudaFreeHost(h_batch_input); });
      for (unsigned i_pdu = 0; i_pdu != nof_pdus; ++i_pdu) {
        std::memcpy(h_batch_input + i_pdu * tb_bytes, tx_data[i_pdu].data(), tb_bytes);
      }

      uint8_t* d_batch_input    = nullptr;
      int8_t*  d_batch_symbols  = nullptr;
      uint8_t* d_single_input   = nullptr;
      int8_t*  d_single_symbols = nullptr;
      throw_on_cuda(cudaMalloc(&d_batch_input, batch_input_bytes), "cudaMalloc(batch input)");
      auto free_batch_input = make_scope_exit([&]() { cudaFree(d_batch_input); });
      throw_on_cuda(cudaMalloc(&d_batch_symbols, batch_symbol_bytes), "cudaMalloc(batch symbols)");
      auto free_batch_symbols = make_scope_exit([&]() { cudaFree(d_batch_symbols); });
      throw_on_cuda(cudaMalloc(&d_single_input, tb_bytes), "cudaMalloc(single input)");
      auto free_single_input = make_scope_exit([&]() { cudaFree(d_single_input); });
      throw_on_cuda(cudaMalloc(&d_single_symbols, symbols_per_tb * sizeof(ci8_t)), "cudaMalloc(single symbols)");
      auto free_single_symbols = make_scope_exit([&]() { cudaFree(d_single_symbols); });

      throw_on_cuda(cudaMemcpyAsync(d_batch_input, h_batch_input, batch_input_bytes, cudaMemcpyHostToDevice, stream),
                    "cudaMemcpyAsync(batch input verify)");
      throw_on_ldpc(tb_batch_encoder_encode_to_symbols_int8(
                        batch_encoder, d_batch_input, d_batch_symbols, static_cast<int>(nof_pdus), stream),
                    "tb_batch_encoder_encode_to_symbols_int8");
      throw_on_cuda(cudaStreamSynchronizeYielding(stream), "cudaStreamSynchronizeYielding(batch verify)");

      std::vector<int8_t> batch_symbols(batch_symbol_bytes);
      throw_on_cuda(cudaMemcpy(batch_symbols.data(), d_batch_symbols, batch_symbol_bytes, cudaMemcpyDeviceToHost),
                    "cudaMemcpy(batch symbols verify)");

      std::vector<int8_t> single_symbols(symbols_per_tb * sizeof(ci8_t));
      for (unsigned i_pdu = 0; i_pdu != nof_pdus; ++i_pdu) {
        throw_on_cuda(cudaMemcpyAsync(d_single_input, tx_data[i_pdu].data(), tb_bytes, cudaMemcpyHostToDevice, stream),
                      "cudaMemcpyAsync(single input verify)");
        throw_on_ldpc(tb_encoder_encode_to_symbols_int8(single_encoder, d_single_input, d_single_symbols, stream),
                      "tb_encoder_encode_to_symbols_int8");
        throw_on_cuda(
            cudaMemcpyAsync(
                single_symbols.data(), d_single_symbols, single_symbols.size(), cudaMemcpyDeviceToHost, stream),
            "cudaMemcpyAsync(single symbols verify)");
        throw_on_cuda(cudaStreamSynchronizeYielding(stream), "cudaStreamSynchronizeYielding(single verify)");
        if (std::memcmp(batch_symbols.data() + i_pdu * single_symbols.size(),
                        single_symbols.data(),
                        single_symbols.size()) != 0) {
          throw std::runtime_error("Encoder-batch output does not match the single-TB encoder.");
        }
      }

      std::unique_ptr<resource_grid>                                   batch_grid;
      std::unique_ptr<pdsch_device_grid_writer_cuda>                   batch_device_writer;
      std::unique_ptr<resource_grid_mapper>                            batch_mapper;
      std::vector<resource_grid_mapper::allocation_configuration>      batch_allocations;
      std::vector<re_pattern_list>                                     batch_reserved;
      std::vector<precoding_configuration>                             batch_precoding;
      std::vector<std::unique_ptr<encoder_batch_device_symbol_buffer>> batch_symbol_buffers;

      if (encoder_batch_map != 0) {
        auto batch_precoder_factory =
            require_factory(create_channel_precoder_factory("auto"), "encoder-batch precoder factory");
        auto batch_mapper_factory = require_factory(create_resource_grid_mapper_factory(batch_precoder_factory),
                                                    "encoder-batch mapper factory");
        batch_mapper              = batch_mapper_factory->create();
        if (!batch_mapper) {
          throw std::runtime_error("Failed to create encoder-batch resource-grid mapper.");
        }

        batch_grid = grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
        if (!batch_grid) {
          throw std::runtime_error("Failed to create encoder-batch resource grid.");
        }
        auto allocation_mode = (device_grid_memory == "managed")
                                   ? pdsch_device_grid_writer_cuda::grid_allocation_mode::managed
                                   : pdsch_device_grid_writer_cuda::grid_allocation_mode::device;
        batch_device_writer =
            std::make_unique<pdsch_device_grid_writer_cuda>(batch_grid->get_writer(), allocation_mode);
        if (!batch_device_writer->supports_device_grid_mapping()) {
          throw std::runtime_error("Failed to allocate encoder-batch CUDA device resource grid.");
        }

        batch_allocations.reserve(nof_pdus);
        batch_reserved.reserve(nof_pdus);
        batch_precoding.reserve(nof_pdus);
        batch_symbol_buffers.reserve(nof_pdus);
        for (unsigned i_pdu = 0; i_pdu != nof_pdus; ++i_pdu) {
          const pdsch_processor::pdu_t& pdu = pdus[i_pdu];

          resource_grid_mapper::allocation_configuration allocation;
          allocation.bwp        = {pdu.bwp_start_rb, pdu.bwp_start_rb + pdu.bwp_size_rb};
          allocation.freq_alloc = pdu.freq_alloc;
          allocation.time_alloc = {pdu.start_symbol_index, pdu.start_symbol_index + pdu.nof_symbols};
          batch_allocations.push_back(allocation);

          crb_bitmap      rb_mask  = pdu.freq_alloc.get_crb_mask(pdu.bwp_start_rb, pdu.bwp_size_rb);
          re_pattern_list reserved = pdu.reserved;
          pdsch_reserve_ptrs_re(reserved, pdu, rb_mask);
          re_pattern dmrs_pattern = get_dmrs_pattern(
              pdu.dmrs, pdu.bwp_start_rb, pdu.bwp_size_rb, pdu.nof_cdm_groups_without_data, pdu.dmrs_symbol_mask);
          reserved.merge(dmrs_pattern);
          batch_reserved.push_back(reserved);

          float scaling = convert_dB_to_amplitude(-pdu.ratio_pdsch_data_to_sss_dB);
          scaling *= modulation_mapper::get_modulation_scaling(pdu.codewords.front().modulation);
          precoding_configuration precoding = pdu.precoding;
          precoding *= scaling;
          batch_precoding.push_back(precoding);

          batch_symbol_buffers.push_back(
              std::make_unique<encoder_batch_device_symbol_buffer>(symbols_per_tb, nof_ports, nof_layers, stream));
        }
      }

      const bool  encoder_batch_maps_grid = encoder_batch_map != 0;
      const char* encoder_batch_grid_path = encoder_batch_maps_grid ? "encoder-batch-device-map" : "encoder-batch";

      std::vector<double> run_avg_us;
      std::vector<double> run_p50_us;
      std::vector<double> run_p90_us;
      std::vector<double> run_p99_us;
      run_avg_us.reserve(runs);
      run_p50_us.reserve(runs);
      run_p90_us.reserve(runs);
      run_p99_us.reserve(runs);

      for (unsigned i_run = 0; i_run != runs; ++i_run) {
        std::vector<double> latency_us;
        latency_us.reserve(iterations);

        for (unsigned i = 0, i_end = warmup_iterations + iterations; i != i_end; ++i) {
          if (encoder_batch_maps_grid) {
            if (materialize_device_grid != 0) {
              batch_grid->set_all_zero();
            }
            if (!batch_device_writer->clear_device_grid_async(stream) ||
                !batch_device_writer->synchronize_device_grid_ready()) {
              throw std::runtime_error("Failed to clear CUDA device grid before encoder-batch PDSCH mapping.");
            }
          }

          auto start = std::chrono::steady_clock::now();
          throw_on_cuda(
              cudaMemcpyAsync(d_batch_input, h_batch_input, batch_input_bytes, cudaMemcpyHostToDevice, stream),
              "cudaMemcpyAsync(batch input)");
          throw_on_ldpc(tb_batch_encoder_encode_to_symbols_int8(
                            batch_encoder, d_batch_input, d_batch_symbols, static_cast<int>(nof_pdus), stream),
                        "tb_batch_encoder_encode_to_symbols_int8");
          if (encoder_batch_maps_grid) {
            for (unsigned i_pdu = 0; i_pdu != nof_pdus; ++i_pdu) {
              batch_symbol_buffers[i_pdu]->reset(d_batch_symbols + i_pdu * symbols_per_tb * 2U);
              std::vector<uint8_t> port_list(batch_precoding[i_pdu].get_nof_ports());
              std::iota(port_list.begin(), port_list.end(), 0);
              batch_mapper->map(*batch_device_writer,
                                *batch_symbol_buffers[i_pdu],
                                batch_allocations[i_pdu],
                                batch_reserved[i_pdu],
                                port_list,
                                batch_precoding[i_pdu]);
            }
            bool sync_ok = (materialize_device_grid != 0)
                               ? batch_device_writer->materialize_nonzero_device_grid_to_host()
                               : batch_device_writer->synchronize_device_grid_ready();
            if (!sync_ok) {
              throw std::runtime_error("Failed to synchronize CUDA device grid after encoder-batch PDSCH mapping.");
            }
          } else {
            throw_on_cuda(cudaStreamSynchronizeYielding(stream), "cudaStreamSynchronizeYielding(batch encode)");
          }
          auto stop = std::chrono::steady_clock::now();

          if (i >= warmup_iterations) {
            latency_us.push_back(std::chrono::duration<double, std::micro>(stop - start).count());
          }
        }

        std::sort(latency_us.begin(), latency_us.end());
        double sum_us         = std::accumulate(latency_us.begin(), latency_us.end(), 0.0);
        double avg_us         = latency_us.empty() ? 0.0 : sum_us / static_cast<double>(latency_us.size());
        double p50_us         = percentile_us(latency_us, 50.0);
        double p90_us         = percentile_us(latency_us, 90.0);
        double p99_us         = percentile_us(latency_us, 99.0);
        double per_pdu_avg_us = avg_us / static_cast<double>(nof_pdus);
        double per_pdu_p50_us = p50_us / static_cast<double>(nof_pdus);
        double per_pdu_p90_us = p90_us / static_cast<double>(nof_pdus);
        double per_pdu_p99_us = p99_us / static_cast<double>(nof_pdus);

        run_avg_us.push_back(avg_us);
        run_p50_us.push_back(p50_us);
        run_p90_us.push_back(p90_us);
        run_p99_us.push_back(p99_us);

        std::cout << "mcs_table=" << mcs_table_name << " mcs=" << mcs_index_value << " prb=" << nof_prb
                  << " layers=" << nof_layers << " ports=" << nof_ports << " rv=" << rv << " dmrs=" << dmrs_profile
                  << " dmrs_config_type=" << dmrs_type_name(dmrs_cfg_type) << " cdm=" << nof_cdm_groups
                  << " precoding=" << precoding_profile << " active_port=" << active_port
                  << " precoding_index=" << precoding_index << " start_symbol=" << start_symbol
                  << " symbols=" << nof_symbols << " backend=" << backend << " pdus=" << nof_pdus
                  << " batch_bwp_prb=" << batch_bwp_size_rb << " encoder_batch=1 cb_batch="
                  << (cb_batch_length == std::numeric_limits<unsigned>::max() ? std::string("all")
                                                                              : std::to_string(cb_batch_length))
                  << " device_grid=" << (encoder_batch_maps_grid ? 1 : 0)
                  << " device_grid_memory=" << device_grid_memory << " resource_grid_memory=" << resource_grid_memory
                  << " resource_grid_path=" << encoder_batch_grid_path
                  << " materialize_device_grid=" << (encoder_batch_maps_grid ? materialize_device_grid : 0)
                  << " sync_after_process=1 run=" << i_run << " warmup=" << warmup_iterations
                  << " iterations=" << iterations << " tbs_bits=" << tbs << " batch_tbs_bits=" << tbs * nof_pdus
                  << " min_us=" << (latency_us.empty() ? 0.0 : latency_us.front()) << " avg_us=" << avg_us
                  << " p50_us=" << p50_us << " p90_us=" << p90_us << " p99_us=" << p99_us
                  << " max_us=" << (latency_us.empty() ? 0.0 : latency_us.back())
                  << " per_pdu_avg_us=" << per_pdu_avg_us << " per_pdu_p50_us=" << per_pdu_p50_us
                  << " per_pdu_p90_us=" << per_pdu_p90_us << " per_pdu_p99_us=" << per_pdu_p99_us << '\n';
      }

      std::sort(run_avg_us.begin(), run_avg_us.end());
      std::sort(run_p50_us.begin(), run_p50_us.end());
      std::sort(run_p90_us.begin(), run_p90_us.end());
      std::sort(run_p99_us.begin(), run_p99_us.end());

      std::cout << "mcs_table=" << mcs_table_name << " mcs=" << mcs_index_value << " prb=" << nof_prb
                << " layers=" << nof_layers << " ports=" << nof_ports << " rv=" << rv << " dmrs=" << dmrs_profile
                << " dmrs_config_type=" << dmrs_type_name(dmrs_cfg_type) << " cdm=" << nof_cdm_groups
                << " precoding=" << precoding_profile << " active_port=" << active_port
                << " precoding_index=" << precoding_index << " start_symbol=" << start_symbol
                << " symbols=" << nof_symbols << " backend=" << backend << " pdus=" << nof_pdus
                << " batch_bwp_prb=" << batch_bwp_size_rb << " encoder_batch=1 cb_batch="
                << (cb_batch_length == std::numeric_limits<unsigned>::max() ? std::string("all")
                                                                            : std::to_string(cb_batch_length))
                << " device_grid=" << (encoder_batch_maps_grid ? 1 : 0) << " device_grid_memory=" << device_grid_memory
                << " resource_grid_memory=" << resource_grid_memory << " resource_grid_path=" << encoder_batch_grid_path
                << " materialize_device_grid=" << (encoder_batch_maps_grid ? materialize_device_grid : 0)
                << " sync_after_process=1 runs=" << runs << " warmup=" << warmup_iterations
                << " iterations=" << iterations << " tbs_bits=" << tbs << " batch_tbs_bits=" << tbs * nof_pdus
                << " median_run_avg_us=" << percentile_us(run_avg_us, 50.0)
                << " median_run_p50_us=" << percentile_us(run_p50_us, 50.0)
                << " median_run_p90_us=" << percentile_us(run_p90_us, 50.0)
                << " median_run_p99_us=" << percentile_us(run_p99_us, 50.0)
                << " median_run_per_pdu_avg_us=" << percentile_us(run_avg_us, 50.0) / static_cast<double>(nof_pdus)
                << " median_run_per_pdu_p50_us=" << percentile_us(run_p50_us, 50.0) / static_cast<double>(nof_pdus)
                << " median_run_per_pdu_p90_us=" << percentile_us(run_p90_us, 50.0) / static_cast<double>(nof_pdus)
                << " median_run_per_pdu_p99_us=" << percentile_us(run_p99_us, 50.0) / static_cast<double>(nof_pdus)
                << '\n';

      return 0;
    }
#endif

    std::vector<std::unique_ptr<pdsch_processor>> procs;
    procs.reserve(nof_pdus);
    for (unsigned i_pdu = 0; i_pdu != nof_pdus; ++i_pdu) {
      std::unique_ptr<pdsch_processor> proc = proc_factory->create();
      if (!proc) {
        throw std::runtime_error("Failed to create PDSCH processor.");
      }
      procs.push_back(std::move(proc));
    }

    std::unique_ptr<resource_grid> grid;
#ifdef ENABLE_CUDA
    if (resource_grid_memory == "auto") {
      resource_grid_cuda_visible_factory cuda_grid_factory(resource_grid_cuda_visible_factory::direction::downlink);
      grid = cuda_grid_factory.create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
    } else if (resource_grid_memory == "managed") {
      auto managed_grid =
          std::make_unique<cuda_visible_resource_grid>(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
      if (!managed_grid->is_valid()) {
        throw std::runtime_error("Failed to create managed CUDA-visible resource grid.");
      }
      grid = std::move(managed_grid);
    } else
#endif
    {
      grid = grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
    }
    if (!grid) {
      throw std::runtime_error("Failed to create resource grid.");
    }

#ifdef ENABLE_CUDA
    std::unique_ptr<pdsch_device_grid_writer_cuda> device_grid_writer;
    if ((use_device_grid != 0) && !grid->get_writer().supports_device_grid_mapping()) {
      auto allocation_mode = (device_grid_memory == "managed")
                                 ? pdsch_device_grid_writer_cuda::grid_allocation_mode::managed
                                 : pdsch_device_grid_writer_cuda::grid_allocation_mode::device;
      device_grid_writer   = std::make_unique<pdsch_device_grid_writer_cuda>(grid->get_writer(), allocation_mode);
      if (!device_grid_writer->supports_device_grid_mapping()) {
        throw std::runtime_error("Failed to allocate CUDA device resource grid.");
      }
    }
    const char* resource_grid_path =
        grid->get_writer().supports_device_grid_mapping() ? "direct" : (device_grid_writer ? "sidecar" : "host");
#else
    const char* resource_grid_path = "host";
#endif

    std::vector<pdsch_processor_notifier_adaptor> notifiers(nof_pdus);
    std::vector<double>                           run_avg_us;
    std::vector<double>                           run_p50_us;
    std::vector<double>                           run_p90_us;
    std::vector<double>                           run_p99_us;
    run_avg_us.reserve(runs);
    run_p50_us.reserve(runs);
    run_p90_us.reserve(runs);
    run_p99_us.reserve(runs);

    for (unsigned i_run = 0; i_run != runs; ++i_run) {
      std::vector<double> latency_us;
      latency_us.reserve(iterations);

      for (unsigned i = 0, i_end = warmup_iterations + iterations; i != i_end; ++i) {
        grid->set_all_zero();
#ifdef ENABLE_CUDA
        if (device_grid_writer) {
          if (!device_grid_writer->clear_device_grid_async() || !device_grid_writer->synchronize_device_grid_ready()) {
            throw std::runtime_error("Failed to clear CUDA device grid before PDSCH processing.");
          }
        }
        resource_grid_writer* writer =
            device_grid_writer ? static_cast<resource_grid_writer*>(device_grid_writer.get()) : &grid->get_writer();
#else
        resource_grid_writer* writer = &grid->get_writer();
#endif
        for (pdsch_processor_notifier_adaptor& notifier : notifiers) {
          notifier.reset();
        }

        auto start = std::chrono::steady_clock::now();
        for (unsigned i_pdu = 0; i_pdu != nof_pdus; ++i_pdu) {
          static_vector<shared_transport_block, pdsch_processor::MAX_NOF_TRANSPORT_BLOCKS> pdu_data;
          pdu_data.emplace_back(span<const uint8_t>(tx_data[i_pdu].data(), tx_data[i_pdu].size()));
          procs[i_pdu]->process(*writer, notifiers[i_pdu], std::move(pdu_data), pdus[i_pdu]);
        }
        for (const pdsch_processor_notifier_adaptor& notifier : notifiers) {
          notifier.wait_for_completion();
        }
#ifdef ENABLE_CUDA
        if ((sync_after_process != 0) && (backend == "gpu")) {
          bool sync_ok = false;
          if (device_grid_writer && (materialize_device_grid != 0)) {
            sync_ok = device_grid_writer->materialize_nonzero_device_grid_to_host();
          } else {
            sync_ok = device_grid_writer ? device_grid_writer->synchronize_device_grid_ready()
                                         : (grid->get_writer().supports_device_grid_mapping()
                                                ? grid->get_writer().synchronize_device_grid_mapping()
                                                : (cudaDeviceSynchronize() == cudaSuccess));
          }
          if (!sync_ok) {
            throw std::runtime_error("Failed to synchronize CUDA work after PDSCH processing.");
          }
        }
#endif
        auto stop = std::chrono::steady_clock::now();

        if (i >= warmup_iterations) {
          latency_us.push_back(std::chrono::duration<double, std::micro>(stop - start).count());
        }
      }

      std::sort(latency_us.begin(), latency_us.end());
      double sum_us         = std::accumulate(latency_us.begin(), latency_us.end(), 0.0);
      double avg_us         = latency_us.empty() ? 0.0 : sum_us / static_cast<double>(latency_us.size());
      double p50_us         = percentile_us(latency_us, 50.0);
      double p90_us         = percentile_us(latency_us, 90.0);
      double p99_us         = percentile_us(latency_us, 99.0);
      double per_pdu_avg_us = avg_us / static_cast<double>(nof_pdus);
      double per_pdu_p50_us = p50_us / static_cast<double>(nof_pdus);
      double per_pdu_p90_us = p90_us / static_cast<double>(nof_pdus);
      double per_pdu_p99_us = p99_us / static_cast<double>(nof_pdus);

      run_avg_us.push_back(avg_us);
      run_p50_us.push_back(p50_us);
      run_p90_us.push_back(p90_us);
      run_p99_us.push_back(p99_us);

      std::cout << "mcs_table=" << mcs_table_name << " mcs=" << mcs_index_value << " prb=" << nof_prb
                << " layers=" << nof_layers << " ports=" << nof_ports << " rv=" << rv << " dmrs=" << dmrs_profile
                << " dmrs_config_type=" << dmrs_type_name(dmrs_cfg_type) << " cdm=" << nof_cdm_groups
                << " precoding=" << precoding_profile << " active_port=" << active_port
                << " precoding_index=" << precoding_index << " start_symbol=" << start_symbol
                << " symbols=" << nof_symbols << " backend=" << backend << " pdus=" << nof_pdus
                << " batch_bwp_prb=" << batch_bwp_size_rb << " cb_batch="
                << (cb_batch_length == std::numeric_limits<unsigned>::max() ? std::string("all")
                                                                            : std::to_string(cb_batch_length))
                << " device_grid=" << use_device_grid << " device_grid_memory=" << device_grid_memory
                << " resource_grid_memory=" << resource_grid_memory << " resource_grid_path=" << resource_grid_path
                << " materialize_device_grid=" << materialize_device_grid
                << " sync_after_process=" << sync_after_process << " run=" << i_run << " warmup=" << warmup_iterations
                << " iterations=" << iterations << " tbs_bits=" << tbs << " batch_tbs_bits=" << tbs * nof_pdus
                << " min_us=" << (latency_us.empty() ? 0.0 : latency_us.front()) << " avg_us=" << avg_us
                << " p50_us=" << p50_us << " p90_us=" << p90_us << " p99_us=" << p99_us
                << " max_us=" << (latency_us.empty() ? 0.0 : latency_us.back()) << " per_pdu_avg_us=" << per_pdu_avg_us
                << " per_pdu_p50_us=" << per_pdu_p50_us << " per_pdu_p90_us=" << per_pdu_p90_us
                << " per_pdu_p99_us=" << per_pdu_p99_us << '\n';
    }

    std::sort(run_avg_us.begin(), run_avg_us.end());
    std::sort(run_p50_us.begin(), run_p50_us.end());
    std::sort(run_p90_us.begin(), run_p90_us.end());
    std::sort(run_p99_us.begin(), run_p99_us.end());

    std::cout << "mcs_table=" << mcs_table_name << " mcs=" << mcs_index_value << " prb=" << nof_prb
              << " layers=" << nof_layers << " ports=" << nof_ports << " rv=" << rv << " dmrs=" << dmrs_profile
              << " dmrs_config_type=" << dmrs_type_name(dmrs_cfg_type) << " cdm=" << nof_cdm_groups
              << " precoding=" << precoding_profile << " active_port=" << active_port
              << " precoding_index=" << precoding_index << " start_symbol=" << start_symbol
              << " symbols=" << nof_symbols << " backend=" << backend << " pdus=" << nof_pdus
              << " batch_bwp_prb=" << batch_bwp_size_rb << " cb_batch="
              << (cb_batch_length == std::numeric_limits<unsigned>::max() ? std::string("all")
                                                                          : std::to_string(cb_batch_length))
              << " device_grid=" << use_device_grid << " device_grid_memory=" << device_grid_memory
              << " resource_grid_memory=" << resource_grid_memory << " resource_grid_path=" << resource_grid_path
              << " materialize_device_grid=" << materialize_device_grid << " sync_after_process=" << sync_after_process
              << " runs=" << runs << " warmup=" << warmup_iterations << " iterations=" << iterations
              << " tbs_bits=" << tbs << " batch_tbs_bits=" << tbs * nof_pdus
              << " median_run_avg_us=" << percentile_us(run_avg_us, 50.0)
              << " median_run_p50_us=" << percentile_us(run_p50_us, 50.0)
              << " median_run_p90_us=" << percentile_us(run_p90_us, 50.0)
              << " median_run_p99_us=" << percentile_us(run_p99_us, 50.0)
              << " median_run_per_pdu_avg_us=" << percentile_us(run_avg_us, 50.0) / static_cast<double>(nof_pdus)
              << " median_run_per_pdu_p50_us=" << percentile_us(run_p50_us, 50.0) / static_cast<double>(nof_pdus)
              << " median_run_per_pdu_p90_us=" << percentile_us(run_p90_us, 50.0) / static_cast<double>(nof_pdus)
              << " median_run_per_pdu_p99_us=" << percentile_us(run_p99_us, 50.0) / static_cast<double>(nof_pdus)
              << '\n';
  } catch (const std::exception& e) {
    std::cerr << "pdsch_gpu_latency_benchmark failed: " << e.what() << '\n';
    return 1;
  }

  return 0;
}
