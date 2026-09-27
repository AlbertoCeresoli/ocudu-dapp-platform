// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief GPU vs CPU PDSCH end-to-end resource-grid comparison test.

#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ofh/compression/compression_factory.h"
#include "ocudu/ofh/compression/compression_properties.h"
#include "ocudu/phy/antenna_ports.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_processors/pdsch/factories.h"
#include "ocudu/phy/upper/channel_processors/pdsch/pdsch_processor.h"
#include "ocudu/phy/upper/dmrs_mapping.h"
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
#include "ocudu/support/ocudu_test.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <gtest/gtest.h>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <thread>

#ifdef ENABLE_CUDA
#include "cuda/pdsch_device_grid_writer_cuda.h"
#include "cuda/pdsch_resource_grid_mapper_cuda.h"
#include "resource_grid_cuda_visible_impl.h"
#include <cuda_runtime.h>
#endif

using namespace ocudu;

static constexpr subcarrier_spacing scs                   = subcarrier_spacing::kHz30;
static constexpr uint16_t           default_rnti          = 0x1234;
static constexpr unsigned           bwp_start_rb          = 0;
static constexpr unsigned           nof_ofdm_symbols      = 14;
static constexpr cyclic_prefix      cy_prefix             = cyclic_prefix::NORMAL;
static constexpr unsigned           default_n_id          = 0;
static constexpr unsigned           default_scrambling_id = 0;
static constexpr bool               default_n_scid        = false;
static constexpr unsigned           default_slot_index    = 0;
static constexpr float              grid_tolerance        = 1e-3F;

enum class dmrs_profile : uint8_t { single_symbol, double_symbol, triple_symbol };

struct pdsch_gpu_e2e_params {
  pdsch_mcs_table    mcs_table;
  sch_mcs_index      mcs_index;
  unsigned           nof_prb;
  unsigned           nof_layers;
  unsigned           rv;
  dmrs_profile       dmrs_symbols;
  unsigned           nof_cdm_groups_without_data;
  unsigned           nof_ports     = 0;
  dmrs_config_type   dmrs_cfg_type = dmrs_config_type::type1;
  uint16_t           rnti          = default_rnti;
  unsigned           n_id          = default_n_id;
  unsigned           scrambling_id = default_scrambling_id;
  bool               n_scid        = default_n_scid;
  unsigned           slot_index    = default_slot_index;
  unsigned           start_symbol  = 0;
  unsigned           nof_symbols   = nof_ofdm_symbols;
  subcarrier_spacing pdu_scs       = scs;
};

namespace ocudu {

std::ostream& operator<<(std::ostream& os, pdsch_mcs_table table)
{
  switch (table) {
    case pdsch_mcs_table::qam64:
      return os << "qam64";
    case pdsch_mcs_table::qam256:
      return os << "qam256";
    case pdsch_mcs_table::qam64LowSe:
      return os << "qam64LowSe";
  }
  return os << "unknown";
}

std::ostream& operator<<(std::ostream& os, const sch_mcs_index& index)
{
  return os << "mcs" << index.value();
}

} // namespace ocudu

namespace {

symbol_slot_mask make_dmrs_symbol_mask(dmrs_profile profile)
{
  switch (profile) {
    case dmrs_profile::single_symbol:
      return {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    case dmrs_profile::double_symbol:
      return {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0};
    case dmrs_profile::triple_symbol:
      return {0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0};
  }
  return {};
}

const char* dmrs_profile_name(dmrs_profile profile)
{
  switch (profile) {
    case dmrs_profile::single_symbol:
      return "singleDmrs";
    case dmrs_profile::double_symbol:
      return "doubleDmrs";
    case dmrs_profile::triple_symbol:
      return "tripleDmrs";
  }
  return "unknownDmrs";
}

const char* dmrs_type_name(dmrs_config_type value)
{
  return (value == dmrs_config_type::type1) ? "type1" : "type2";
}

std::string format_params(const pdsch_gpu_e2e_params& params)
{
  unsigned          nof_ports = params.nof_ports == 0 ? params.nof_layers : params.nof_ports;
  std::stringstream ss;
  ss << params.mcs_table << "," << params.mcs_index << ",prb" << params.nof_prb << ",layers" << params.nof_layers
     << ",ports" << nof_ports << ",rv" << params.rv << "," << dmrs_profile_name(params.dmrs_symbols) << ","
     << dmrs_type_name(params.dmrs_cfg_type) << ",cdm" << params.nof_cdm_groups_without_data << ",rnti0x" << std::hex
     << params.rnti << std::dec << ",nid" << params.n_id << ",scrambling" << params.scrambling_id << ",nscid"
     << params.n_scid << ",slot" << params.slot_index << ",symbols" << params.start_symbol << ":"
     << params.start_symbol + params.nof_symbols << ",mu" << to_numerology_value(params.pdu_scs);
  return ss.str();
}

class pdsch_processor_notifier_adaptor : public pdsch_processor_notifier
{
public:
  void on_finish_processing() override { completed = true; }

  void wait_for_completion() const
  {
    while (!completed.load()) {
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  }

private:
  std::atomic<bool> completed = {false};
};

std::shared_ptr<pdsch_processor_factory>
create_test_pdsch_processor_factory(std::shared_ptr<pdsch_block_processor_factory> block_processor_factory,
                                    task_executor&                                 executor,
                                    unsigned                                       cb_batch_length        = 1,
                                    unsigned                                       nof_concurrent_threads = 1)
{
  std::shared_ptr<crc_calculator_factory> crc_factory = create_crc_calculator_factory_sw("auto");
  TESTASSERT(crc_factory);

  std::shared_ptr<ldpc_segmenter_tx_factory> segmenter_factory = create_ldpc_segmenter_tx_factory_sw(crc_factory);
  TESTASSERT(segmenter_factory);

  std::shared_ptr<pseudo_random_generator_factory> prg_factory = create_pseudo_random_generator_sw_factory();
  TESTASSERT(prg_factory);

  std::shared_ptr<channel_precoder_factory> precoder_factory = create_channel_precoder_factory("auto");
  TESTASSERT(precoder_factory);

  std::shared_ptr<resource_grid_mapper_factory> rg_mapper_factory =
      create_resource_grid_mapper_factory(precoder_factory);
  TESTASSERT(rg_mapper_factory);

  std::shared_ptr<dmrs_pdsch_processor_factory> dmrs_factory =
      create_dmrs_pdsch_processor_factory_sw(prg_factory, rg_mapper_factory);
  TESTASSERT(dmrs_factory);

  std::shared_ptr<ptrs_pdsch_generator_factory> ptrs_factory =
      create_ptrs_pdsch_generator_generic_factory(prg_factory, rg_mapper_factory);
  TESTASSERT(ptrs_factory);

  return create_pdsch_flexible_processor_factory_sw(segmenter_factory,
                                                    block_processor_factory,
                                                    rg_mapper_factory,
                                                    dmrs_factory,
                                                    ptrs_factory,
                                                    executor,
                                                    nof_concurrent_threads,
                                                    cb_batch_length);
}

std::shared_ptr<pdsch_block_processor_factory> create_cpu_pdsch_block_processor_factory()
{
  std::shared_ptr<ldpc_encoder_factory> ldpc_encoder_factory = create_ldpc_encoder_factory_sw("auto");
  TESTASSERT(ldpc_encoder_factory);

  std::shared_ptr<ldpc_rate_matcher_factory> ldpc_rate_matcher_factory = create_ldpc_rate_matcher_factory_sw();
  TESTASSERT(ldpc_rate_matcher_factory);

  std::shared_ptr<pseudo_random_generator_factory> prg_factory = create_pseudo_random_generator_sw_factory();
  TESTASSERT(prg_factory);

  std::shared_ptr<modulation_mapper_factory> modulation_factory = create_modulation_mapper_factory();
  TESTASSERT(modulation_factory);

  return create_pdsch_block_processor_factory_sw(
      ldpc_encoder_factory, ldpc_rate_matcher_factory, prg_factory, modulation_factory);
}

unsigned calculate_tbs(const sch_mcs_description& mcs_descr,
                       unsigned                   nof_prb,
                       unsigned                   nof_layers,
                       dmrs_config_type           dmrs_cfg_type,
                       const symbol_slot_mask&    dmrs_symbol_mask,
                       unsigned                   nof_cdm_groups_without_data,
                       unsigned                   nof_symbols = nof_ofdm_symbols)
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
                                       unsigned                   nof_layers,
                                       unsigned                   nof_ports,
                                       unsigned                   rv,
                                       dmrs_config_type           dmrs_cfg_type,
                                       const symbol_slot_mask&    dmrs_symbol_mask,
                                       unsigned                   nof_cdm_groups_without_data,
                                       unsigned                   tbs,
                                       uint16_t                   rnti          = default_rnti,
                                       unsigned                   n_id          = default_n_id,
                                       unsigned                   scrambling_id = default_scrambling_id,
                                       bool                       n_scid        = default_n_scid,
                                       unsigned                   slot_index    = default_slot_index,
                                       unsigned                   start_symbol  = 0,
                                       unsigned                   nof_symbols   = nof_ofdm_symbols,
                                       subcarrier_spacing         pdu_scs       = scs)
{
  pdsch_processor::pdu_t pdu;
  pdu.context                     = std::nullopt;
  pdu.slot                        = slot_point(to_numerology_value(pdu_scs), slot_index);
  pdu.rnti                        = to_rnti(rnti);
  pdu.bwp_size_rb                 = nof_prb;
  pdu.bwp_start_rb                = bwp_start_rb;
  pdu.cp                          = cy_prefix;
  pdu.n_id                        = n_id;
  pdu.ref_point                   = pdsch_processor::pdu_t::PRB0;
  pdu.dmrs_symbol_mask            = dmrs_symbol_mask;
  pdu.dmrs                        = dmrs_cfg_type;
  pdu.scrambling_id               = scrambling_id;
  pdu.n_scid                      = n_scid;
  pdu.nof_cdm_groups_without_data = nof_cdm_groups_without_data;
  pdu.freq_alloc                  = rb_allocation::make_type1(bwp_start_rb, nof_prb, std::nullopt);
  pdu.start_symbol_index          = start_symbol;
  pdu.nof_symbols                 = nof_symbols;
  pdu.tbs_lbrm                    = tbs_lbrm_default;
  pdu.reserved                    = {};
  pdu.ptrs                        = std::nullopt;
  pdu.ratio_pdsch_data_to_sss_dB  = 0.0F;
  pdu.ratio_pdsch_dmrs_to_sss_dB  = get_sch_to_dmrs_ratio_dB(nof_cdm_groups_without_data);
  pdu.precoding                   = precoding_configuration::make_wideband(
      (nof_layers == 1 && nof_ports != 1) ? make_one_layer_all_ports(nof_ports) : make_identity(nof_layers));
  pdu.codewords.emplace_back(pdsch_processor::codeword_description{
      mcs_descr.modulation, rv, get_ldpc_base_graph(mcs_descr.get_normalised_target_code_rate(), units::bits(tbs))});

  return pdu;
}

struct grid_diff_stats {
  unsigned nof_mismatches = 0;
  float    max_abs_error  = 0.0F;
  unsigned port           = 0;
  unsigned symbol         = 0;
  unsigned subcarrier     = 0;
  cf_t     cpu_value      = {};
  cf_t     gpu_value      = {};
};

grid_diff_stats compare_resource_grids(const resource_grid_reader& cpu_reader, const resource_grid_reader& gpu_reader)
{
  grid_diff_stats stats;

  EXPECT_EQ(cpu_reader.get_nof_ports(), gpu_reader.get_nof_ports());
  EXPECT_EQ(cpu_reader.get_nof_symbols(), gpu_reader.get_nof_symbols());
  EXPECT_EQ(cpu_reader.get_nof_subc(), gpu_reader.get_nof_subc());

  for (unsigned port = 0; port != cpu_reader.get_nof_ports(); ++port) {
    for (unsigned symbol = 0; symbol != cpu_reader.get_nof_symbols(); ++symbol) {
      span<const cbf16_t> cpu_symbol = cpu_reader.get_view(port, symbol);
      span<const cbf16_t> gpu_symbol = gpu_reader.get_view(port, symbol);
      EXPECT_EQ(cpu_symbol.size(), gpu_symbol.size());

      for (unsigned subcarrier = 0; subcarrier != cpu_symbol.size(); ++subcarrier) {
        cf_t  cpu_value = to_cf(cpu_symbol[subcarrier]);
        cf_t  gpu_value = to_cf(gpu_symbol[subcarrier]);
        float error =
            std::max(std::abs(cpu_value.real() - gpu_value.real()), std::abs(cpu_value.imag() - gpu_value.imag()));

        if (error > stats.max_abs_error) {
          stats.max_abs_error = error;
          stats.port          = port;
          stats.symbol        = symbol;
          stats.subcarrier    = subcarrier;
          stats.cpu_value     = cpu_value;
          stats.gpu_value     = gpu_value;
        }
        if (error > grid_tolerance) {
          ++stats.nof_mismatches;
        }
      }
    }
  }

  return stats;
}

#ifdef ENABLE_CUDA
class env_var_guard
{
public:
  explicit env_var_guard(const char* name_) : name(name_)
  {
    const char* current = std::getenv(name);
    if (current != nullptr) {
      had_value = true;
      old_value = current;
    }
  }

  ~env_var_guard()
  {
    if (had_value) {
      setenv(name, old_value.c_str(), 1);
    } else {
      unsetenv(name);
    }
  }

private:
  const char* name;
  bool        had_value = false;
  std::string old_value;
};

TEST(PdschGpuE2e, ReusedGpuProcessorHandlesMixedBurstShapesWithCudaGraphs)
{
  if (!is_pdsch_block_processor_acceleration_available()) {
    GTEST_SKIP() << "CUDA PDSCH GPU block processor is not available in this build/runtime.";
  }

  env_var_guard graph_guard("OCUDU_PDSCH_CUDA_GRAPHS");
  setenv("OCUDU_PDSCH_CUDA_GRAPHS", "1", 1);

  inline_task_executor                     executor;
  std::shared_ptr<pdsch_processor_factory> cpu_proc_factory =
      create_test_pdsch_processor_factory(create_cpu_pdsch_block_processor_factory(), executor);
  std::shared_ptr<pdsch_processor_factory> gpu_proc_factory =
      create_test_pdsch_processor_factory(create_pdsch_block_processor_factory_accelerated(), executor);
  ASSERT_NE(cpu_proc_factory, nullptr);
  ASSERT_NE(gpu_proc_factory, nullptr);

  std::shared_ptr<resource_grid_factory> grid_factory = create_resource_grid_factory();
  ASSERT_NE(grid_factory, nullptr);

  std::unique_ptr<pdsch_processor> cpu_proc = cpu_proc_factory->create();
  std::unique_ptr<pdsch_processor> gpu_proc = gpu_proc_factory->create();
  ASSERT_NE(cpu_proc, nullptr);
  ASSERT_NE(gpu_proc, nullptr);

  std::vector<pdsch_gpu_e2e_params> burst_sequence = {
      pdsch_gpu_e2e_params{pdsch_mcs_table::qam256, sch_mcs_index(20), 106, 1, 0, dmrs_profile::double_symbol, 2},
      pdsch_gpu_e2e_params{pdsch_mcs_table::qam256, sch_mcs_index(20), 106, 1, 0, dmrs_profile::double_symbol, 2},
      pdsch_gpu_e2e_params{pdsch_mcs_table::qam256, sch_mcs_index(20), 106, 1, 0, dmrs_profile::double_symbol, 2},
      pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 52, 4, 0, dmrs_profile::single_symbol, 1},
      pdsch_gpu_e2e_params{pdsch_mcs_table::qam256, sch_mcs_index(20), 106, 1, 0, dmrs_profile::double_symbol, 2},
      pdsch_gpu_e2e_params{pdsch_mcs_table::qam256, sch_mcs_index(20), 106, 1, 0, dmrs_profile::double_symbol, 2},
      pdsch_gpu_e2e_params{pdsch_mcs_table::qam256, sch_mcs_index(20), 106, 1, 0, dmrs_profile::double_symbol, 2}};

  for (unsigned i_burst = 0; i_burst != burst_sequence.size(); ++i_burst) {
    const pdsch_gpu_e2e_params& params           = burst_sequence[i_burst];
    symbol_slot_mask            dmrs_symbol_mask = make_dmrs_symbol_mask(params.dmrs_symbols);
    unsigned                    nof_ports        = params.nof_ports == 0 ? params.nof_layers : params.nof_ports;
    SCOPED_TRACE("burst=" + std::to_string(i_burst) + " " + format_params(params));

    sch_mcs_description mcs_descr = pdsch_mcs_get_config(params.mcs_table, params.mcs_index);
    unsigned            tbs       = calculate_tbs(mcs_descr,
                                 params.nof_prb,
                                 params.nof_layers,
                                 params.dmrs_cfg_type,
                                 dmrs_symbol_mask,
                                 params.nof_cdm_groups_without_data);
    ASSERT_EQ(tbs % 8, 0U);

    pdsch_processor::pdu_t pdu = build_pdsch_pdu(mcs_descr,
                                                 params.nof_prb,
                                                 params.nof_layers,
                                                 nof_ports,
                                                 params.rv,
                                                 params.dmrs_cfg_type,
                                                 dmrs_symbol_mask,
                                                 params.nof_cdm_groups_without_data,
                                                 tbs,
                                                 params.rnti,
                                                 params.n_id,
                                                 params.scrambling_id,
                                                 params.n_scid,
                                                 params.slot_index);

    std::vector<uint8_t> tx_data(tbs / 8);
    std::mt19937         rgen(0x670a9U + i_burst * 7919U + params.nof_prb * 17U + params.mcs_index.value() * 101U +
                      params.nof_layers * 1009U);
    std::generate(tx_data.begin(), tx_data.end(), [&rgen]() { return static_cast<uint8_t>(rgen() & 0xff); });

    std::unique_ptr<resource_grid> cpu_grid = grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
    std::unique_ptr<resource_grid> gpu_grid = grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
    ASSERT_NE(cpu_grid, nullptr);
    ASSERT_NE(gpu_grid, nullptr);

    pdsch_processor_notifier_adaptor cpu_notifier;
    pdsch_processor_notifier_adaptor gpu_notifier;
    cpu_proc->process(cpu_grid->get_writer(), cpu_notifier, {shared_transport_block(tx_data)}, pdu);
    gpu_proc->process(gpu_grid->get_writer(), gpu_notifier, {shared_transport_block(tx_data)}, pdu);
    cpu_notifier.wait_for_completion();
    gpu_notifier.wait_for_completion();

    grid_diff_stats diff = compare_resource_grids(cpu_grid->get_reader(), gpu_grid->get_reader());
    ASSERT_EQ(diff.nof_mismatches, 0U) << "max_abs_error=" << diff.max_abs_error << " at port=" << diff.port
                                       << " symbol=" << diff.symbol << " subcarrier=" << diff.subcarrier
                                       << " cpu=" << diff.cpu_value << " gpu=" << diff.gpu_value;
  }
}

std::vector<uint32_t> generate_data_re_offsets(const resource_grid_mapper::allocation_configuration& allocation,
                                               const re_pattern_list&                                reserved,
                                               unsigned                                              nof_subc)
{
  auto crb_indices = allocation.freq_alloc.get_crb_indices(allocation.bwp.start(), allocation.bwp.length());

  bounded_bitset<MAX_NOF_SUBCARRIERS> base_crb_re_mask(allocation.bwp.stop() * NOF_SUBCARRIERS_PER_RB);
  std::for_each(crb_indices.begin(), crb_indices.end(), [&base_crb_re_mask](uint16_t crb_idx) {
    base_crb_re_mask.fill(crb_idx * NOF_SUBCARRIERS_PER_RB, (crb_idx + 1) * NOF_SUBCARRIERS_PER_RB);
  });

  std::vector<uint32_t> offsets;
  for (unsigned i_symbol = allocation.time_alloc.start(), i_symbol_end = allocation.time_alloc.stop();
       i_symbol != i_symbol_end;
       ++i_symbol) {
    bounded_bitset<MAX_NOF_SUBCARRIERS> symbol_re_mask = base_crb_re_mask;
    reserved.get_exclusion_mask(symbol_re_mask, i_symbol);
    symbol_re_mask.for_each(0, symbol_re_mask.size(), [&](unsigned i_subcarrier) {
      offsets.push_back(i_symbol * nof_subc + i_subcarrier);
    });
  }
  return offsets;
}

unsigned count_offsets_in_symbol(span<const uint32_t> offsets, unsigned symbol, unsigned nof_subc)
{
  return static_cast<unsigned>(std::count_if(
      offsets.begin(), offsets.end(), [symbol, nof_subc](uint32_t offset) { return (offset / nof_subc) == symbol; }));
}

std::vector<ci8_t> generate_mapper_symbols(unsigned nof_re, unsigned nof_layers = 1)
{
  std::vector<ci8_t> symbols(nof_re * nof_layers);
  for (unsigned i_re = 0; i_re != nof_re; ++i_re) {
    for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
      unsigned symbol_index = i_re * nof_layers + i_layer;
      symbols[symbol_index] = ci8_t(static_cast<int8_t>(static_cast<int>((7U * symbol_index) % 61U) - 30),
                                    static_cast<int8_t>(static_cast<int>((11U * symbol_index) % 59U) - 29));
    }
  }
  return symbols;
}

void assert_real_grid_matches_host(const std::vector<uint16_t>& gpu_grid_bf16,
                                   const resource_grid_reader&  host_grid_reader,
                                   unsigned                     nof_ports,
                                   unsigned                     nof_grid_re_per_port)
{
  for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
    for (unsigned i_symbol = 0; i_symbol != MAX_NSYMB_PER_SLOT; ++i_symbol) {
      span<const cbf16_t> host_symbol = host_grid_reader.get_view(i_port, i_symbol);
      for (unsigned i_subc = 0; i_subc != host_symbol.size(); ++i_subc) {
        unsigned grid_re_index = i_port * nof_grid_re_per_port + i_symbol * MAX_NOF_SUBCARRIERS + i_subc;
        ASSERT_EQ(gpu_grid_bf16[2U * grid_re_index], host_symbol[i_subc].real.value())
            << "real mismatch port=" << i_port << " symbol=" << i_symbol << " subcarrier=" << i_subc;
        ASSERT_EQ(gpu_grid_bf16[2U * grid_re_index + 1U], host_symbol[i_subc].imag.value())
            << "imag mismatch port=" << i_port << " symbol=" << i_symbol << " subcarrier=" << i_subc;
      }
    }
  }
}

void assert_real_grid_data_re_matches_host(const std::vector<uint16_t>& gpu_grid_bf16,
                                           const resource_grid_reader&  host_grid_reader,
                                           span<const uint32_t>         re_offsets,
                                           unsigned                     nof_ports,
                                           unsigned                     nof_grid_re_per_port,
                                           unsigned                     nof_subc)
{
  for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
    for (uint32_t re_offset : re_offsets) {
      unsigned i_symbol      = re_offset / nof_subc;
      unsigned i_subc        = re_offset % nof_subc;
      cbf16_t  host_re       = host_grid_reader.get_view(i_port, i_symbol)[i_subc];
      unsigned grid_re_index = i_port * nof_grid_re_per_port + re_offset;
      ASSERT_EQ(gpu_grid_bf16[2U * grid_re_index], host_re.real.value())
          << "real mismatch port=" << i_port << " symbol=" << i_symbol << " subcarrier=" << i_subc;
      ASSERT_EQ(gpu_grid_bf16[2U * grid_re_index + 1U], host_re.imag.value())
          << "imag mismatch port=" << i_port << " symbol=" << i_symbol << " subcarrier=" << i_subc;
    }
  }
}

#endif

class PdschGpuE2eFixture : public ::testing::TestWithParam<pdsch_gpu_e2e_params>
{
protected:
  void SetUp() override
  {
    if (!is_pdsch_block_processor_acceleration_available()) {
      GTEST_SKIP() << "CUDA PDSCH GPU block processor is not available in this build/runtime.";
    }

    std::shared_ptr<pdsch_block_processor_factory> cpu_block_factory = create_cpu_pdsch_block_processor_factory();
    ASSERT_NE(cpu_block_factory, nullptr);

    std::shared_ptr<pdsch_block_processor_factory> gpu_block_factory =
        create_pdsch_block_processor_factory_accelerated();
    ASSERT_NE(gpu_block_factory, nullptr);

    cpu_proc_factory = create_test_pdsch_processor_factory(cpu_block_factory, executor);
    gpu_proc_factory = create_test_pdsch_processor_factory(gpu_block_factory, executor);
    ASSERT_NE(cpu_proc_factory, nullptr);
    ASSERT_NE(gpu_proc_factory, nullptr);

    grid_factory = create_resource_grid_factory();
    ASSERT_NE(grid_factory, nullptr);
  }

  inline_task_executor                     executor;
  std::shared_ptr<pdsch_processor_factory> cpu_proc_factory;
  std::shared_ptr<pdsch_processor_factory> gpu_proc_factory;
  std::shared_ptr<resource_grid_factory>   grid_factory;
};

} // namespace

TEST_P(PdschGpuE2eFixture, CpuAndGpuResourceGridsMatch)
{
  pdsch_gpu_e2e_params params           = GetParam();
  symbol_slot_mask     dmrs_symbol_mask = make_dmrs_symbol_mask(params.dmrs_symbols);
  unsigned             nof_ports        = params.nof_ports == 0 ? params.nof_layers : params.nof_ports;
  SCOPED_TRACE(format_params(params));

  sch_mcs_description mcs_descr = pdsch_mcs_get_config(params.mcs_table, params.mcs_index);
  unsigned            tbs       = calculate_tbs(mcs_descr,
                               params.nof_prb,
                               params.nof_layers,
                               params.dmrs_cfg_type,
                               dmrs_symbol_mask,
                               params.nof_cdm_groups_without_data,
                               params.nof_symbols);
  ASSERT_EQ(tbs % 8, 0U);

  pdsch_processor::pdu_t pdu = build_pdsch_pdu(mcs_descr,
                                               params.nof_prb,
                                               params.nof_layers,
                                               nof_ports,
                                               params.rv,
                                               params.dmrs_cfg_type,
                                               dmrs_symbol_mask,
                                               params.nof_cdm_groups_without_data,
                                               tbs,
                                               params.rnti,
                                               params.n_id,
                                               params.scrambling_id,
                                               params.n_scid,
                                               params.slot_index,
                                               params.start_symbol,
                                               params.nof_symbols,
                                               params.pdu_scs);

  std::vector<uint8_t> tx_data(tbs / 8);
  unsigned             seed = 0x5eed1234U + params.nof_prb * 17U + params.mcs_index.value() * 101U;
  seed += params.nof_layers * 1009U + params.rv * 7919U + static_cast<unsigned>(params.dmrs_symbols) * 8191U;
  seed += static_cast<unsigned>(get_nof_re_per_prb(params.dmrs_cfg_type)) * 4099U;
  seed += params.nof_cdm_groups_without_data + nof_ports * 127U + params.rnti * 3U + params.n_id * 5U;
  seed += params.scrambling_id * 7U + static_cast<unsigned>(params.n_scid) * 11U + params.slot_index * 13U;
  std::mt19937 rgen(seed);
  std::generate(tx_data.begin(), tx_data.end(), [&rgen]() { return static_cast<uint8_t>(rgen() & 0xff); });

  std::unique_ptr<pdsch_processor> cpu_proc = cpu_proc_factory->create();
  std::unique_ptr<pdsch_processor> gpu_proc = gpu_proc_factory->create();
  ASSERT_NE(cpu_proc, nullptr);
  ASSERT_NE(gpu_proc, nullptr);

  std::unique_ptr<resource_grid> cpu_grid = grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
  std::unique_ptr<resource_grid> gpu_grid = grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
  ASSERT_NE(cpu_grid, nullptr);
  ASSERT_NE(gpu_grid, nullptr);

  pdsch_processor_notifier_adaptor cpu_notifier;
  pdsch_processor_notifier_adaptor gpu_notifier;

  cpu_proc->process(cpu_grid->get_writer(), cpu_notifier, {shared_transport_block(tx_data)}, pdu);
  gpu_proc->process(gpu_grid->get_writer(), gpu_notifier, {shared_transport_block(tx_data)}, pdu);

  cpu_notifier.wait_for_completion();
  gpu_notifier.wait_for_completion();

  grid_diff_stats diff = compare_resource_grids(cpu_grid->get_reader(), gpu_grid->get_reader());
  ASSERT_EQ(diff.nof_mismatches, 0U) << "max_abs_error=" << diff.max_abs_error << " at port=" << diff.port
                                     << " symbol=" << diff.symbol << " subcarrier=" << diff.subcarrier
                                     << " cpu=" << diff.cpu_value << " gpu=" << diff.gpu_value;
}

#ifdef ENABLE_CUDA
// Limited-buffer rate matching bounds the circular buffer of large transport blocks (TS 38.212 Section 5.4.2.1): the MAC
// always signals tbs_lbrm_default, which binds for transport blocks above ~80 kB (100 MHz, 4 layers, high MCS), and a
// TBS_LBRM equal to the TBS binds for every multi-codeblock transport block. Every redundancy version starts at a k0
// derived from the bounded N_cb, so all of them must match the CPU reference, not only RV 0.
TEST_P(PdschGpuE2eFixture, LimitedBufferRateMatchingMatchesAcrossRedundancyVersions)
{
  pdsch_gpu_e2e_params params           = GetParam();
  symbol_slot_mask     dmrs_symbol_mask = make_dmrs_symbol_mask(params.dmrs_symbols);
  unsigned             nof_ports        = params.nof_ports == 0 ? params.nof_layers : params.nof_ports;
  SCOPED_TRACE(format_params(params));

  sch_mcs_description mcs_descr = pdsch_mcs_get_config(params.mcs_table, params.mcs_index);
  unsigned            tbs       = calculate_tbs(mcs_descr,
                               params.nof_prb,
                               params.nof_layers,
                               params.dmrs_cfg_type,
                               dmrs_symbol_mask,
                               params.nof_cdm_groups_without_data,
                               params.nof_symbols);
  ASSERT_EQ(tbs % 8, 0U);

  std::vector<uint8_t> tx_data(tbs / 8);
  std::mt19937         rgen(0x1b12a000U + tbs);
  std::generate(tx_data.begin(), tx_data.end(), [&rgen]() { return static_cast<uint8_t>(rgen() & 0xff); });

  std::unique_ptr<pdsch_processor> cpu_proc = cpu_proc_factory->create();
  std::unique_ptr<pdsch_processor> gpu_proc = gpu_proc_factory->create();
  ASSERT_NE(cpu_proc, nullptr);
  ASSERT_NE(gpu_proc, nullptr);

  std::vector<units::bytes> lbrm_sizes = {tbs_lbrm_default};
  if (tbs >= 20000) {
    lbrm_sizes.push_back(units::bytes(tbs / 8));
  }
  for (units::bytes tbs_lbrm : lbrm_sizes)
  for (unsigned rv : {0U, 2U, 3U, 1U}) {
    SCOPED_TRACE(fmt::format("rv={} tbs_lbrm={}", rv, tbs_lbrm.value()));
    pdsch_processor::pdu_t pdu = build_pdsch_pdu(mcs_descr,
                                                 params.nof_prb,
                                                 params.nof_layers,
                                                 nof_ports,
                                                 rv,
                                                 params.dmrs_cfg_type,
                                                 dmrs_symbol_mask,
                                                 params.nof_cdm_groups_without_data,
                                                 tbs,
                                                 params.rnti,
                                                 params.n_id,
                                                 params.scrambling_id,
                                                 params.n_scid,
                                                 params.slot_index,
                                                 params.start_symbol,
                                                 params.nof_symbols,
                                                 params.pdu_scs);
    pdu.tbs_lbrm = tbs_lbrm;

    std::unique_ptr<resource_grid> cpu_grid = grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
    std::unique_ptr<resource_grid> gpu_grid = grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
    ASSERT_NE(cpu_grid, nullptr);
    ASSERT_NE(gpu_grid, nullptr);

    pdsch_processor_notifier_adaptor cpu_notifier;
    pdsch_processor_notifier_adaptor gpu_notifier;
    cpu_proc->process(cpu_grid->get_writer(), cpu_notifier, {shared_transport_block(tx_data)}, pdu);
    gpu_proc->process(gpu_grid->get_writer(), gpu_notifier, {shared_transport_block(tx_data)}, pdu);
    cpu_notifier.wait_for_completion();
    gpu_notifier.wait_for_completion();

    grid_diff_stats diff = compare_resource_grids(cpu_grid->get_reader(), gpu_grid->get_reader());
    ASSERT_EQ(diff.nof_mismatches, 0U) << "max_abs_error=" << diff.max_abs_error << " at port=" << diff.port
                                       << " symbol=" << diff.symbol << " subcarrier=" << diff.subcarrier
                                       << " cpu=" << diff.cpu_value << " gpu=" << diff.gpu_value;
  }
}

TEST(PdschGpuIssueProfile, ManagedGridMatchesHostAcrossHarqRvsAndRepeatedSlots)
{
  if (!is_pdsch_block_processor_acceleration_available()) {
    GTEST_SKIP() << "CUDA PDSCH GPU block processor is not available in this build/runtime.";
  }

  env_var_guard grid_mode("OCUDU_DL_CUDA_VISIBLE_GRID");
  ASSERT_EQ(setenv("OCUDU_DL_CUDA_VISIBLE_GRID", "managed", 1), 0);

  inline_task_executor executor;
  auto                 cpu_proc_factory = create_test_pdsch_processor_factory(
      create_cpu_pdsch_block_processor_factory(), executor, std::numeric_limits<unsigned>::max());
  auto gpu_proc_factory = create_test_pdsch_processor_factory(
      create_pdsch_block_processor_factory_accelerated(), executor, std::numeric_limits<unsigned>::max());
  ASSERT_NE(cpu_proc_factory, nullptr);
  ASSERT_NE(gpu_proc_factory, nullptr);

  std::unique_ptr<pdsch_processor> cpu_proc = cpu_proc_factory->create();
  std::unique_ptr<pdsch_processor> gpu_proc = gpu_proc_factory->create();
  ASSERT_NE(cpu_proc, nullptr);
  ASSERT_NE(gpu_proc, nullptr);

  constexpr unsigned nof_prb         = 106;
  constexpr unsigned nof_layers      = 4;
  constexpr unsigned nof_ports       = 4;
  constexpr unsigned nof_subc        = nof_prb * NOF_SUBCARRIERS_PER_RB;
  constexpr unsigned start_symbol    = 2;
  constexpr unsigned nof_symbols     = 12;
  constexpr unsigned nof_repetitions = 8;

  symbol_slot_mask    dmrs_symbol_mask = make_dmrs_symbol_mask(dmrs_profile::triple_symbol);
  sch_mcs_description mcs_descr        = pdsch_mcs_get_config(pdsch_mcs_table::qam256, sch_mcs_index(26));
  unsigned            tbs =
      calculate_tbs(mcs_descr, nof_prb, nof_layers, dmrs_config_type::type1, dmrs_symbol_mask, 2, nof_symbols);
  ASSERT_EQ(tbs % 8, 0U);

  std::shared_ptr<resource_grid_factory> host_grid_factory = create_resource_grid_factory();
  resource_grid_cuda_visible_factory     managed_grid_factory(resource_grid_cuda_visible_factory::direction::downlink);
  std::unique_ptr<resource_grid>         cpu_grid = host_grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, nof_subc);
  std::unique_ptr<resource_grid> gpu_grid = managed_grid_factory.create(nof_ports, MAX_NSYMB_PER_SLOT, nof_subc);
  ASSERT_NE(cpu_grid, nullptr);
  ASSERT_NE(gpu_grid, nullptr);
  ASSERT_TRUE(gpu_grid->get_writer().supports_device_grid_mapping());
  ASSERT_TRUE(gpu_grid->get_writer().device_grid_mapping_aliases_host_grid());
  ASSERT_TRUE(gpu_grid->get_reader().supports_device_grid_reading());

  constexpr float            issue_iq_scaling = 0.25118864F;
  ofh::ru_compression_params compression_params{ofh::compression_type::BFP, 9};
  unsigned                compressed_symbol_bytes = nof_prb * ofh::get_compressed_prb_size(compression_params).value();
  ocudulog::basic_logger& logger                  = ocudulog::fetch_basic_logger("TEST", false);
  std::unique_ptr<ofh::iq_compressor> cpu_compressor =
      ofh::create_iq_compressor(ofh::compression_type::BFP, logger, issue_iq_scaling, "generic");
  std::unique_ptr<ofh::iq_compressor> gpu_compressor =
      ofh::create_iq_compressor(ofh::compression_type::BFP, logger, issue_iq_scaling, "cuda");
  ASSERT_NE(cpu_compressor, nullptr);
  ASSERT_NE(gpu_compressor, nullptr);

  for (unsigned i_tx = 0; i_tx != nof_repetitions; ++i_tx) {
    SCOPED_TRACE("transmission=" + std::to_string(i_tx));

    cpu_grid->set_all_zero();
    gpu_grid->set_all_zero();

    unsigned               rv  = std::array<unsigned, 4>{0, 2, 3, 1}[i_tx % 4];
    pdsch_processor::pdu_t pdu = build_pdsch_pdu(mcs_descr,
                                                 nof_prb,
                                                 nof_layers,
                                                 nof_ports,
                                                 rv,
                                                 dmrs_config_type::type1,
                                                 dmrs_symbol_mask,
                                                 2,
                                                 tbs,
                                                 0x4601,
                                                 10,
                                                 10,
                                                 false,
                                                 i_tx,
                                                 start_symbol,
                                                 nof_symbols,
                                                 subcarrier_spacing::kHz15);

    std::vector<uint8_t> tx_data(tbs / 8);
    std::mt19937         rgen(0x1f1f0000U + i_tx);
    std::generate(tx_data.begin(), tx_data.end(), [&rgen]() { return static_cast<uint8_t>(rgen() & 0xff); });

    pdsch_processor_notifier_adaptor cpu_notifier;
    pdsch_processor_notifier_adaptor gpu_notifier;
    cpu_proc->process(cpu_grid->get_writer(), cpu_notifier, {shared_transport_block(tx_data)}, pdu);
    gpu_proc->process(gpu_grid->get_writer(), gpu_notifier, {shared_transport_block(tx_data)}, pdu);
    cpu_notifier.wait_for_completion();
    gpu_notifier.wait_for_completion();

    grid_diff_stats diff = compare_resource_grids(cpu_grid->get_reader(), gpu_grid->get_reader());
    ASSERT_EQ(diff.nof_mismatches, 0U) << "max_abs_error=" << diff.max_abs_error << " at port=" << diff.port
                                       << " symbol=" << diff.symbol << " subcarrier=" << diff.subcarrier
                                       << " cpu=" << diff.cpu_value << " gpu=" << diff.gpu_value;

    // Mirror the split-7.2 handoff from the completed CUDA-visible DL grid into 9-bit BFP OFH payloads.
    for (unsigned symbol = 0; symbol != MAX_NSYMB_PER_SLOT; ++symbol) {
      for (unsigned port = 0; port != nof_ports; ++port) {
        SCOPED_TRACE("symbol=" + std::to_string(symbol) + " port=" + std::to_string(port));
        std::vector<uint8_t> cpu_compressed(compressed_symbol_bytes);
        std::vector<uint8_t> gpu_compressed(compressed_symbol_bytes);
        cpu_compressor->compress(cpu_compressed, cpu_grid->get_reader().get_view(port, symbol), compression_params);
        ASSERT_TRUE(gpu_compressor->compress_device_symbol(
            gpu_compressed, gpu_grid->get_reader(), port, symbol, 0, nof_prb, compression_params));
        ASSERT_EQ(cpu_compressed, gpu_compressed);
      }
    }
  }
}

TEST(PdschGpuIssueProfile, DisjointPdschsShareManagedGridWithoutIntermediateSynchronization)
{
  if (!is_pdsch_block_processor_acceleration_available()) {
    GTEST_SKIP() << "CUDA PDSCH GPU block processor is not available in this build/runtime.";
  }

  env_var_guard grid_mode("OCUDU_DL_CUDA_VISIBLE_GRID");
  env_var_guard prefetch_mode("OCUDU_CUDA_VISIBLE_GRID_PREFETCH");
  env_var_guard direct_grid_mode("OCUDU_PDSCH_DIRECT_DEVICE_GRID");
  env_var_guard graph_mode("OCUDU_PDSCH_CUDA_GRAPHS");
  ASSERT_EQ(setenv("OCUDU_DL_CUDA_VISIBLE_GRID", "managed", 1), 0);
  ASSERT_EQ(unsetenv("OCUDU_CUDA_VISIBLE_GRID_PREFETCH"), 0);
  ASSERT_EQ(setenv("OCUDU_PDSCH_DIRECT_DEVICE_GRID", "1", 1), 0);
  ASSERT_EQ(unsetenv("OCUDU_PDSCH_CUDA_GRAPHS"), 0);

  inline_task_executor executor;
  auto                 cpu_proc_factory = create_test_pdsch_processor_factory(
      create_cpu_pdsch_block_processor_factory(), executor, std::numeric_limits<unsigned>::max());
  auto gpu_proc_factory = create_test_pdsch_processor_factory(
      create_pdsch_block_processor_factory_accelerated(), executor, std::numeric_limits<unsigned>::max(), 3);
  ASSERT_NE(cpu_proc_factory, nullptr);
  ASSERT_NE(gpu_proc_factory, nullptr);

  std::unique_ptr<pdsch_processor>                cpu_proc = cpu_proc_factory->create();
  std::array<std::unique_ptr<pdsch_processor>, 3> gpu_procs;
  for (std::unique_ptr<pdsch_processor>& gpu_proc : gpu_procs) {
    gpu_proc = gpu_proc_factory->create();
  }
  ASSERT_NE(cpu_proc, nullptr);
  for (const std::unique_ptr<pdsch_processor>& gpu_proc : gpu_procs) {
    ASSERT_NE(gpu_proc, nullptr);
  }

  constexpr unsigned nof_pdus        = 3;
  constexpr unsigned cell_nof_prb    = 51;
  constexpr unsigned pdu_nof_prb     = cell_nof_prb / nof_pdus;
  constexpr unsigned nof_ports       = 1;
  constexpr unsigned nof_layers      = 1;
  constexpr unsigned nof_subc        = cell_nof_prb * NOF_SUBCARRIERS_PER_RB;
  constexpr unsigned start_symbol    = 2;
  constexpr unsigned nof_symbols     = 12;
  constexpr unsigned nof_repetitions = 128;
  symbol_slot_mask   dmrs_symbols    = make_dmrs_symbol_mask(dmrs_profile::single_symbol);

  std::shared_ptr<resource_grid_factory> host_grid_factory = create_resource_grid_factory();
  resource_grid_cuda_visible_factory     managed_grid_factory(resource_grid_cuda_visible_factory::direction::downlink);
  std::unique_ptr<resource_grid>         cpu_grid = host_grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, nof_subc);
  std::unique_ptr<resource_grid> gpu_grid = managed_grid_factory.create(nof_ports, MAX_NSYMB_PER_SLOT, nof_subc);
  ASSERT_NE(cpu_grid, nullptr);
  ASSERT_NE(gpu_grid, nullptr);
  ASSERT_TRUE(gpu_grid->get_writer().device_grid_mapping_aliases_host_grid());

  for (unsigned i_tx = 0; i_tx != nof_repetitions; ++i_tx) {
    SCOPED_TRACE("transmission=" + std::to_string(i_tx));
    cpu_grid->set_all_zero();
    gpu_grid->set_all_zero();

    std::array<pdsch_processor::pdu_t, nof_pdus> pdus;
    std::array<std::vector<uint8_t>, nof_pdus>   tx_data;
    for (unsigned i_pdu = 0; i_pdu != nof_pdus; ++i_pdu) {
      sch_mcs_description mcs_descr =
          pdsch_mcs_get_config(pdsch_mcs_table::qam64, sch_mcs_index(19 + ((i_tx + 2 * i_pdu) % 8)));
      unsigned tbs =
          calculate_tbs(mcs_descr, pdu_nof_prb, nof_layers, dmrs_config_type::type1, dmrs_symbols, 1, nof_symbols);
      ASSERT_EQ(tbs % 8, 0U);

      pdus[i_pdu]             = build_pdsch_pdu(mcs_descr,
                                    pdu_nof_prb,
                                    nof_layers,
                                    nof_ports,
                                    std::array<unsigned, 4>{0, 2, 3, 1}[(i_tx + i_pdu) % 4],
                                    dmrs_config_type::type1,
                                    dmrs_symbols,
                                    1,
                                    tbs,
                                    0x4601 + i_pdu,
                                    10,
                                    10,
                                    false,
                                    i_tx,
                                    start_symbol,
                                    nof_symbols,
                                    subcarrier_spacing::kHz30);
      pdus[i_pdu].bwp_size_rb = cell_nof_prb;
      pdus[i_pdu].freq_alloc  = rb_allocation::make_type1(i_pdu * pdu_nof_prb, pdu_nof_prb, std::nullopt);

      tx_data[i_pdu].resize(tbs / 8);
      std::mt19937 rgen(0x1f1fc000U + i_tx * nof_pdus + i_pdu);
      std::generate(
          tx_data[i_pdu].begin(), tx_data[i_pdu].end(), [&rgen]() { return static_cast<uint8_t>(rgen() & 0xff); });

      pdsch_processor_notifier_adaptor cpu_notifier;
      cpu_proc->process(cpu_grid->get_writer(), cpu_notifier, {shared_transport_block(tx_data[i_pdu])}, pdus[i_pdu]);
      cpu_notifier.wait_for_completion();
    }

    // Submit from distinct worker threads, matching the live multi-UE executor. Submission remains ordered, but the
    // processor pool may reuse a CUDA lane while its earlier grid kernel is still in flight. The final reader joins the
    // recorded producer chain without an intermediate host synchronization.
    std::atomic<unsigned>             submit_turn = 0;
    std::array<std::thread, nof_pdus> submit_threads;
    for (unsigned i_pdu = 0; i_pdu != nof_pdus; ++i_pdu) {
      submit_threads[i_pdu] = std::thread([&, i_pdu]() {
        while (submit_turn.load(std::memory_order_acquire) != i_pdu) {
          std::this_thread::yield();
        }
        pdsch_processor_notifier_adaptor gpu_notifier;
        gpu_procs[i_pdu]->process(
            gpu_grid->get_writer(), gpu_notifier, {shared_transport_block(tx_data[i_pdu])}, pdus[i_pdu]);
        gpu_notifier.wait_for_completion();
        submit_turn.store(i_pdu + 1, std::memory_order_release);
        while (submit_turn.load(std::memory_order_acquire) != nof_pdus) {
          std::this_thread::yield();
        }
      });
    }
    for (std::thread& submit_thread : submit_threads) {
      submit_thread.join();
    }

    grid_diff_stats diff = compare_resource_grids(cpu_grid->get_reader(), gpu_grid->get_reader());
    ASSERT_EQ(diff.nof_mismatches, 0U) << "max_abs_error=" << diff.max_abs_error << " at port=" << diff.port
                                       << " symbol=" << diff.symbol << " subcarrier=" << diff.subcarrier
                                       << " cpu=" << diff.cpu_value << " gpu=" << diff.gpu_value;
  }
}

TEST(PdschGpuIssueProfile, BackToBackManagedGridSubmissionsPreserveEveryTransportBlock)
{
  if (!is_pdsch_block_processor_acceleration_available()) {
    GTEST_SKIP() << "CUDA PDSCH GPU block processor is not available in this build/runtime.";
  }

  env_var_guard grid_mode("OCUDU_DL_CUDA_VISIBLE_GRID");
  ASSERT_EQ(setenv("OCUDU_DL_CUDA_VISIBLE_GRID", "managed", 1), 0);

  inline_task_executor executor;
  auto                 cpu_proc_factory = create_test_pdsch_processor_factory(
      create_cpu_pdsch_block_processor_factory(), executor, std::numeric_limits<unsigned>::max());
  auto gpu_proc_factory = create_test_pdsch_processor_factory(
      create_pdsch_block_processor_factory_accelerated(), executor, std::numeric_limits<unsigned>::max());
  ASSERT_NE(cpu_proc_factory, nullptr);
  ASSERT_NE(gpu_proc_factory, nullptr);

  std::unique_ptr<pdsch_processor> cpu_proc = cpu_proc_factory->create();
  std::unique_ptr<pdsch_processor> gpu_proc = gpu_proc_factory->create();
  ASSERT_NE(cpu_proc, nullptr);
  ASSERT_NE(gpu_proc, nullptr);

  constexpr unsigned nof_prb           = 106;
  constexpr unsigned nof_layers        = 4;
  constexpr unsigned nof_ports         = 4;
  constexpr unsigned nof_subc          = nof_prb * NOF_SUBCARRIERS_PER_RB;
  constexpr unsigned start_symbol      = 2;
  constexpr unsigned nof_symbols       = 12;
  constexpr unsigned nof_transmissions = 12;

  symbol_slot_mask    dmrs_symbol_mask = make_dmrs_symbol_mask(dmrs_profile::triple_symbol);
  sch_mcs_description mcs_descr        = pdsch_mcs_get_config(pdsch_mcs_table::qam256, sch_mcs_index(26));
  unsigned            tbs =
      calculate_tbs(mcs_descr, nof_prb, nof_layers, dmrs_config_type::type1, dmrs_symbol_mask, 2, nof_symbols);
  ASSERT_EQ(tbs % 8, 0U);

  std::shared_ptr<resource_grid_factory> host_grid_factory = create_resource_grid_factory();
  resource_grid_cuda_visible_factory     managed_grid_factory(resource_grid_cuda_visible_factory::direction::downlink);
  std::vector<std::unique_ptr<resource_grid>> cpu_grids;
  std::vector<std::unique_ptr<resource_grid>> gpu_grids;
  std::vector<std::vector<uint8_t>>           tx_data;
  std::vector<pdsch_processor::pdu_t>         pdus;
  cpu_grids.reserve(nof_transmissions);
  gpu_grids.reserve(nof_transmissions);
  tx_data.reserve(nof_transmissions);
  pdus.reserve(nof_transmissions);

  // Build all reference grids first. This lets the CUDA submissions below run back-to-back without CPU encoding
  // between them, matching a reused acceleration lane with multiple slots already queued on its stream.
  for (unsigned i_tx = 0; i_tx != nof_transmissions; ++i_tx) {
    cpu_grids.push_back(host_grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, nof_subc));
    gpu_grids.push_back(managed_grid_factory.create(nof_ports, MAX_NSYMB_PER_SLOT, nof_subc));
    ASSERT_NE(cpu_grids.back(), nullptr);
    ASSERT_NE(gpu_grids.back(), nullptr);

    pdus.push_back(build_pdsch_pdu(mcs_descr,
                                   nof_prb,
                                   nof_layers,
                                   nof_ports,
                                   0,
                                   dmrs_config_type::type1,
                                   dmrs_symbol_mask,
                                   2,
                                   tbs,
                                   0x4601,
                                   10,
                                   10,
                                   false,
                                   i_tx,
                                   start_symbol,
                                   nof_symbols,
                                   subcarrier_spacing::kHz15));

    tx_data.emplace_back(tbs / 8);
    std::mt19937 rgen(0x1f1f8000U + i_tx);
    std::generate(
        tx_data.back().begin(), tx_data.back().end(), [&rgen]() { return static_cast<uint8_t>(rgen() & 0xff); });

    pdsch_processor_notifier_adaptor cpu_notifier;
    cpu_proc->process(
        cpu_grids.back()->get_writer(), cpu_notifier, {shared_transport_block(tx_data.back())}, pdus.back());
    cpu_notifier.wait_for_completion();
  }

  for (unsigned i_tx = 0; i_tx != nof_transmissions; ++i_tx) {
    pdsch_processor_notifier_adaptor gpu_notifier;
    gpu_proc->process(gpu_grids[i_tx]->get_writer(), gpu_notifier, {shared_transport_block(tx_data[i_tx])}, pdus[i_tx]);
    gpu_notifier.wait_for_completion();
  }

  // Reading each managed grid waits for its recorded CUDA event. No synchronization is allowed between submissions.
  for (unsigned i_tx = 0; i_tx != nof_transmissions; ++i_tx) {
    SCOPED_TRACE("transmission=" + std::to_string(i_tx));
    grid_diff_stats diff = compare_resource_grids(cpu_grids[i_tx]->get_reader(), gpu_grids[i_tx]->get_reader());
    ASSERT_EQ(diff.nof_mismatches, 0U) << "max_abs_error=" << diff.max_abs_error << " at port=" << diff.port
                                       << " symbol=" << diff.symbol << " subcarrier=" << diff.subcarrier
                                       << " cpu=" << diff.cpu_value << " gpu=" << diff.gpu_value;
  }
}

class PdschGpuDeviceGridWriter : public ::testing::TestWithParam<unsigned>
{};

TEST_P(PdschGpuDeviceGridWriter, MapsDataToWriterDeviceGrid)
{
  if (!is_pdsch_block_processor_acceleration_available()) {
    GTEST_SKIP() << "CUDA PDSCH GPU block processor is not available in this build/runtime.";
  }

  env_var_guard skip_host_guard("OCUDU_PDSCH_DEVICE_MAP_SKIP_HOST");
  unsetenv("OCUDU_PDSCH_DEVICE_MAP_SKIP_HOST");

  const unsigned     nof_layers           = GetParam();
  const unsigned     nof_prb              = (nof_layers == 3) ? 52 : 106;
  const unsigned     nof_ports            = (nof_layers == 1) ? 4 : nof_layers;
  constexpr unsigned nof_cdm_groups       = 2;
  constexpr unsigned nof_grid_re_per_port = MAX_NSYMB_PER_SLOT * MAX_NOF_SUBCARRIERS;

  symbol_slot_mask           dmrs_symbol_mask = make_dmrs_symbol_mask(dmrs_profile::double_symbol);
  constexpr dmrs_config_type dmrs_cfg_type    = dmrs_config_type::type1;
  const sch_mcs_index        mcs_index        = (nof_layers == 3) ? sch_mcs_index(10) : sch_mcs_index(20);
  sch_mcs_description        mcs_descr        = pdsch_mcs_get_config(pdsch_mcs_table::qam64, mcs_index);
  unsigned tbs = calculate_tbs(mcs_descr, nof_prb, nof_layers, dmrs_cfg_type, dmrs_symbol_mask, nof_cdm_groups);
  ASSERT_EQ(tbs % 8, 0U);

  pdsch_processor::pdu_t pdu = build_pdsch_pdu(
      mcs_descr, nof_prb, nof_layers, nof_ports, 0, dmrs_cfg_type, dmrs_symbol_mask, nof_cdm_groups, tbs);

  std::vector<uint8_t> tx_data(tbs / 8);
  std::mt19937 rgen(0x5eed1234U + nof_prb * 17U + mcs_index.value() * 101U + nof_layers * 1009U + nof_ports * 127U);
  std::generate(tx_data.begin(), tx_data.end(), [&rgen]() { return static_cast<uint8_t>(rgen() & 0xff); });

  inline_task_executor                     executor;
  std::shared_ptr<pdsch_processor_factory> cpu_proc_factory =
      create_test_pdsch_processor_factory(create_cpu_pdsch_block_processor_factory(), executor);
  std::shared_ptr<pdsch_processor_factory> gpu_proc_factory = create_test_pdsch_processor_factory(
      create_pdsch_block_processor_factory_accelerated(), executor, std::numeric_limits<unsigned>::max());
  ASSERT_NE(cpu_proc_factory, nullptr);
  ASSERT_NE(gpu_proc_factory, nullptr);

  std::shared_ptr<resource_grid_factory> grid_factory = create_resource_grid_factory();
  ASSERT_NE(grid_factory, nullptr);
  std::unique_ptr<resource_grid> cpu_grid = grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
  std::unique_ptr<resource_grid> gpu_grid = grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
  ASSERT_NE(cpu_grid, nullptr);
  ASSERT_NE(gpu_grid, nullptr);

  std::unique_ptr<pdsch_processor> cpu_proc = cpu_proc_factory->create();
  ASSERT_NE(cpu_proc, nullptr);

  pdsch_processor_notifier_adaptor cpu_notifier;
  cpu_proc->process(cpu_grid->get_writer(), cpu_notifier, {shared_transport_block(tx_data)}, pdu);
  cpu_notifier.wait_for_completion();

  gpu_grid->set_all_zero();

  pdsch_device_grid_writer_cuda device_writer(gpu_grid->get_writer());
  ASSERT_TRUE(device_writer.supports_device_grid_mapping());
  ASSERT_TRUE(device_writer.clear_device_grid_async());
  ASSERT_EQ(cudaDeviceSynchronize(), cudaSuccess);

  std::unique_ptr<pdsch_processor> gpu_proc = gpu_proc_factory->create();
  ASSERT_NE(gpu_proc, nullptr);

  pdsch_processor_notifier_adaptor gpu_notifier;
  gpu_proc->process(device_writer, gpu_notifier, {shared_transport_block(tx_data)}, pdu);
  gpu_notifier.wait_for_completion();
  ASSERT_EQ(cudaDeviceSynchronize(), cudaSuccess);

  std::vector<uint16_t> device_grid_bf16(device_writer.get_device_grid_size_bytes() / sizeof(uint16_t));
  ASSERT_TRUE(device_writer.copy_device_grid_to_host(span<uint16_t>(device_grid_bf16)));

  resource_grid_mapper::allocation_configuration allocation = {.bwp = crb_interval{0, nof_prb},
                                                               .freq_alloc =
                                                                   rb_allocation::make_type1(0, nof_prb, std::nullopt),
                                                               .time_alloc = {0, nof_ofdm_symbols}};
  re_pattern_list reserved(get_dmrs_pattern(dmrs_cfg_type, bwp_start_rb, nof_prb, nof_cdm_groups, dmrs_symbol_mask));
  std::vector<uint32_t> re_offsets = generate_data_re_offsets(allocation, reserved, MAX_NOF_SUBCARRIERS);
  ASSERT_FALSE(re_offsets.empty());

  assert_real_grid_data_re_matches_host(device_grid_bf16,
                                        cpu_grid->get_reader(),
                                        span<const uint32_t>(re_offsets),
                                        nof_ports,
                                        nof_grid_re_per_port,
                                        MAX_NOF_SUBCARRIERS);
}

TEST(PdschGpuDeviceGridWriter, WideBandwidthResidentMappingsMatchHost)
{
  if (!is_pdsch_block_processor_acceleration_available()) {
    GTEST_SKIP() << "CUDA PDSCH GPU block processor is not available in this build/runtime.";
  }

  env_var_guard direct_grid_guard("OCUDU_PDSCH_DIRECT_DEVICE_GRID");
  env_var_guard defer_guard("OCUDU_PDSCH_DEFER_SYMBOL_D2H");
  env_var_guard skip_host_guard("OCUDU_PDSCH_DEVICE_MAP_SKIP_HOST");
  setenv("OCUDU_PDSCH_DIRECT_DEVICE_GRID", "1", 1);
  setenv("OCUDU_PDSCH_DEFER_SYMBOL_D2H", "1", 1);
  unsetenv("OCUDU_PDSCH_DEVICE_MAP_SKIP_HOST");

  constexpr unsigned  nof_cdm_groups       = 2;
  constexpr unsigned  nof_grid_re_per_port = MAX_NSYMB_PER_SLOT * MAX_NOF_SUBCARRIERS;
  sch_mcs_description mcs_descr            = pdsch_mcs_get_config(pdsch_mcs_table::qam64, sch_mcs_index(10));

  inline_task_executor                     executor;
  std::shared_ptr<pdsch_processor_factory> cpu_proc_factory =
      create_test_pdsch_processor_factory(create_cpu_pdsch_block_processor_factory(), executor);
  std::shared_ptr<pdsch_processor_factory> gpu_proc_factory = create_test_pdsch_processor_factory(
      create_pdsch_block_processor_factory_accelerated(), executor, std::numeric_limits<unsigned>::max());
  ASSERT_NE(cpu_proc_factory, nullptr);
  ASSERT_NE(gpu_proc_factory, nullptr);

  std::shared_ptr<resource_grid_factory> grid_factory = create_resource_grid_factory();
  ASSERT_NE(grid_factory, nullptr);

  for (dmrs_config_type dmrs_cfg_type : {dmrs_config_type::type1, dmrs_config_type::type2}) {
    for (dmrs_profile profile :
         {dmrs_profile::single_symbol, dmrs_profile::double_symbol, dmrs_profile::triple_symbol}) {
      symbol_slot_mask dmrs_symbol_mask = make_dmrs_symbol_mask(profile);
      for (unsigned nof_prb : {106U, 273U}) {
        for (unsigned nof_layers : {1U, 2U, 3U, 4U}) {
          unsigned nof_ports = (nof_layers == 1) ? 4 : nof_layers;
          SCOPED_TRACE("dmrs=" + std::string(dmrs_profile_name(profile)) + " prb=" + std::to_string(nof_prb) +
                       " layers=" + std::to_string(nof_layers) + " dmrs_config_type=" + dmrs_type_name(dmrs_cfg_type));

          unsigned tbs = calculate_tbs(mcs_descr, nof_prb, nof_layers, dmrs_cfg_type, dmrs_symbol_mask, nof_cdm_groups);
          ASSERT_EQ(tbs % 8, 0U);

          pdsch_processor::pdu_t pdu = build_pdsch_pdu(
              mcs_descr, nof_prb, nof_layers, nof_ports, 0, dmrs_cfg_type, dmrs_symbol_mask, nof_cdm_groups, tbs);

          std::vector<uint8_t> tx_data(tbs / 8);
          std::mt19937         rgen(0x5eed1234U + nof_prb * 17U + nof_layers * 1009U + nof_ports * 127U +
                            static_cast<unsigned>(profile) * 7919U +
                            static_cast<unsigned>(get_nof_re_per_prb(dmrs_cfg_type)) * 4099U);
          std::generate(tx_data.begin(), tx_data.end(), [&rgen]() { return static_cast<uint8_t>(rgen() & 0xff); });

          std::unique_ptr<resource_grid> cpu_grid =
              grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
          std::unique_ptr<resource_grid> gpu_grid =
              grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
          ASSERT_NE(cpu_grid, nullptr);
          ASSERT_NE(gpu_grid, nullptr);

          std::unique_ptr<pdsch_processor> cpu_proc = cpu_proc_factory->create();
          ASSERT_NE(cpu_proc, nullptr);
          pdsch_processor_notifier_adaptor cpu_notifier;
          cpu_proc->process(cpu_grid->get_writer(), cpu_notifier, {shared_transport_block(tx_data)}, pdu);
          cpu_notifier.wait_for_completion();

          gpu_grid->set_all_zero();
          pdsch_device_grid_writer_cuda device_writer(gpu_grid->get_writer());
          ASSERT_TRUE(device_writer.supports_device_grid_mapping());
          ASSERT_TRUE(device_writer.clear_device_grid_async());
          ASSERT_EQ(cudaDeviceSynchronize(), cudaSuccess);

          std::unique_ptr<pdsch_processor> gpu_proc = gpu_proc_factory->create();
          ASSERT_NE(gpu_proc, nullptr);
          pdsch_processor_notifier_adaptor gpu_notifier;
          gpu_proc->process(device_writer, gpu_notifier, {shared_transport_block(tx_data)}, pdu);
          gpu_notifier.wait_for_completion();
          ASSERT_EQ(cudaDeviceSynchronize(), cudaSuccess);

          std::vector<uint16_t> device_grid_bf16(device_writer.get_device_grid_size_bytes() / sizeof(uint16_t));
          ASSERT_TRUE(device_writer.copy_device_grid_to_host(span<uint16_t>(device_grid_bf16)));

          resource_grid_mapper::allocation_configuration allocation = {
              .bwp        = crb_interval{0, nof_prb},
              .freq_alloc = rb_allocation::make_type1(0, nof_prb, std::nullopt),
              .time_alloc = {0, nof_ofdm_symbols}};
          re_pattern_list reserved(
              get_dmrs_pattern(dmrs_cfg_type, bwp_start_rb, nof_prb, nof_cdm_groups, dmrs_symbol_mask));
          std::vector<uint32_t> re_offsets = generate_data_re_offsets(allocation, reserved, MAX_NOF_SUBCARRIERS);
          ASSERT_FALSE(re_offsets.empty());

          assert_real_grid_data_re_matches_host(device_grid_bf16,
                                                cpu_grid->get_reader(),
                                                span<const uint32_t>(re_offsets),
                                                nof_ports,
                                                nof_grid_re_per_port,
                                                MAX_NOF_SUBCARRIERS);
        }
      }
    }
  }
}

TEST(PdschGpuDeviceGridWriter, MaterializedResidentGridMatchesHostGrid)
{
  if (!is_pdsch_block_processor_acceleration_available()) {
    GTEST_SKIP() << "CUDA PDSCH GPU block processor is not available in this build/runtime.";
  }

  env_var_guard direct_grid_guard("OCUDU_PDSCH_DIRECT_DEVICE_GRID");
  env_var_guard defer_guard("OCUDU_PDSCH_DEFER_SYMBOL_D2H");
  env_var_guard skip_host_guard("OCUDU_PDSCH_DEVICE_MAP_SKIP_HOST");
  setenv("OCUDU_PDSCH_DIRECT_DEVICE_GRID", "1", 1);
  setenv("OCUDU_PDSCH_DEFER_SYMBOL_D2H", "1", 1);
  unsetenv("OCUDU_PDSCH_DEVICE_MAP_SKIP_HOST");

  constexpr unsigned         nof_cdm_groups   = 2;
  symbol_slot_mask           dmrs_symbol_mask = make_dmrs_symbol_mask(dmrs_profile::double_symbol);
  constexpr dmrs_config_type dmrs_cfg_type    = dmrs_config_type::type2;
  sch_mcs_description        mcs_descr        = pdsch_mcs_get_config(pdsch_mcs_table::qam64, sch_mcs_index(10));

  inline_task_executor                     executor;
  std::shared_ptr<pdsch_processor_factory> cpu_proc_factory =
      create_test_pdsch_processor_factory(create_cpu_pdsch_block_processor_factory(), executor);
  std::shared_ptr<pdsch_processor_factory> gpu_proc_factory = create_test_pdsch_processor_factory(
      create_pdsch_block_processor_factory_accelerated(), executor, std::numeric_limits<unsigned>::max());
  ASSERT_NE(cpu_proc_factory, nullptr);
  ASSERT_NE(gpu_proc_factory, nullptr);

  std::shared_ptr<resource_grid_factory> grid_factory = create_resource_grid_factory();
  ASSERT_NE(grid_factory, nullptr);

  for (unsigned nof_prb : {106U, 273U}) {
    for (unsigned nof_layers : {1U, 2U, 3U, 4U}) {
      unsigned nof_ports = (nof_layers == 1) ? 4 : nof_layers;
      SCOPED_TRACE("prb=" + std::to_string(nof_prb) + " layers=" + std::to_string(nof_layers));

      unsigned tbs = calculate_tbs(mcs_descr, nof_prb, nof_layers, dmrs_cfg_type, dmrs_symbol_mask, nof_cdm_groups);
      ASSERT_EQ(tbs % 8, 0U);

      pdsch_processor::pdu_t pdu = build_pdsch_pdu(
          mcs_descr, nof_prb, nof_layers, nof_ports, 0, dmrs_cfg_type, dmrs_symbol_mask, nof_cdm_groups, tbs);

      std::vector<uint8_t> tx_data(tbs / 8);
      std::mt19937         rgen(0x5eed1234U + nof_prb * 17U + nof_layers * 1009U + nof_ports * 127U);
      std::generate(tx_data.begin(), tx_data.end(), [&rgen]() { return static_cast<uint8_t>(rgen() & 0xff); });

      std::unique_ptr<resource_grid> cpu_grid =
          grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
      std::unique_ptr<resource_grid> gpu_grid =
          grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
      ASSERT_NE(cpu_grid, nullptr);
      ASSERT_NE(gpu_grid, nullptr);

      std::unique_ptr<pdsch_processor> cpu_proc = cpu_proc_factory->create();
      ASSERT_NE(cpu_proc, nullptr);
      pdsch_processor_notifier_adaptor cpu_notifier;
      cpu_proc->process(cpu_grid->get_writer(), cpu_notifier, {shared_transport_block(tx_data)}, pdu);
      cpu_notifier.wait_for_completion();

      gpu_grid->set_all_zero();
      pdsch_device_grid_writer_cuda device_writer(gpu_grid->get_writer());
      ASSERT_TRUE(device_writer.supports_device_grid_mapping());
      ASSERT_TRUE(device_writer.clear_device_grid_async());
      ASSERT_TRUE(device_writer.synchronize_device_grid_ready());

      std::unique_ptr<pdsch_processor> gpu_proc = gpu_proc_factory->create();
      ASSERT_NE(gpu_proc, nullptr);
      pdsch_processor_notifier_adaptor gpu_notifier;
      gpu_proc->process(device_writer, gpu_notifier, {shared_transport_block(tx_data)}, pdu);
      gpu_notifier.wait_for_completion();
      ASSERT_TRUE(device_writer.materialize_nonzero_device_grid_to_host());

      grid_diff_stats diff = compare_resource_grids(cpu_grid->get_reader(), gpu_grid->get_reader());
      EXPECT_EQ(diff.nof_mismatches, 0U) << "max_abs_error=" << diff.max_abs_error << " at port=" << diff.port
                                         << " symbol=" << diff.symbol << " subcarrier=" << diff.subcarrier
                                         << " cpu=" << diff.cpu_value << " gpu=" << diff.gpu_value;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Layers, PdschGpuDeviceGridWriter, ::testing::Values(1U, 2U, 3U, 4U));

TEST(PdschGpuRealGridMapper, DataReOffsetsMatchDmrsMasksForType1AndType2)
{
  constexpr unsigned test_bwp_start_rb       = 5;
  constexpr unsigned test_bwp_size_rb        = 32;
  constexpr unsigned vrb_start               = 7;
  constexpr unsigned nof_prb                 = 11;
  constexpr unsigned nof_cdm_groups          = 2;
  constexpr unsigned start_symbol            = 1;
  constexpr unsigned stop_symbol             = 8;
  constexpr unsigned nof_subc                = MAX_NOF_SUBCARRIERS;
  constexpr unsigned first_active_subcarrier = (test_bwp_start_rb + vrb_start) * NOF_SUBCARRIERS_PER_RB;
  constexpr unsigned last_active_subcarrier  = first_active_subcarrier + nof_prb * NOF_SUBCARRIERS_PER_RB - 1;

  symbol_slot_mask dmrs_symbol_mask(MAX_NSYMB_PER_SLOT);
  dmrs_symbol_mask.set(2);
  dmrs_symbol_mask.set(5);

  resource_grid_mapper::allocation_configuration allocation = {
      .bwp        = crb_interval{test_bwp_start_rb, test_bwp_start_rb + test_bwp_size_rb},
      .freq_alloc = rb_allocation::make_type1(vrb_start, nof_prb, std::nullopt),
      .time_alloc = {start_symbol, stop_symbol}};

  for (dmrs_config_type dmrs_cfg_type : {dmrs_config_type::type1, dmrs_config_type::type2}) {
    SCOPED_TRACE(dmrs_type_name(dmrs_cfg_type));

    re_pattern_list reserved(
        get_dmrs_pattern(dmrs_cfg_type, test_bwp_start_rb, test_bwp_size_rb, nof_cdm_groups, dmrs_symbol_mask));
    std::vector<uint32_t> re_offsets = generate_data_re_offsets(allocation, reserved, nof_subc);

    unsigned expected_dmrs_re_per_symbol = nof_prb * get_nof_re_per_prb(dmrs_cfg_type) * nof_cdm_groups;
    unsigned expected_data_re            = nof_prb * NOF_SUBCARRIERS_PER_RB * (stop_symbol - start_symbol) -
                                expected_dmrs_re_per_symbol * dmrs_symbol_mask.count();
    ASSERT_EQ(re_offsets.size(), expected_data_re);
    ASSERT_EQ(re_offsets.front(), start_symbol * nof_subc + first_active_subcarrier);
    ASSERT_EQ(re_offsets.back(), (stop_symbol - 1U) * nof_subc + last_active_subcarrier);

    re_prb_mask dmrs_prb_mask = get_dmrs_prb_mask(dmrs_cfg_type, nof_cdm_groups);
    for (unsigned symbol = start_symbol; symbol != stop_symbol; ++symbol) {
      unsigned expected_symbol_re = nof_prb * NOF_SUBCARRIERS_PER_RB;
      if (dmrs_symbol_mask.test(symbol)) {
        expected_symbol_re -= expected_dmrs_re_per_symbol;
      }
      EXPECT_EQ(count_offsets_in_symbol(span<const uint32_t>(re_offsets.data(), re_offsets.size()), symbol, nof_subc),
                expected_symbol_re)
          << "symbol=" << symbol;
    }

    for (uint32_t re_offset : re_offsets) {
      unsigned symbol     = re_offset / nof_subc;
      unsigned subcarrier = re_offset % nof_subc;
      EXPECT_GE(symbol, start_symbol);
      EXPECT_LT(symbol, stop_symbol);
      EXPECT_GE(subcarrier, first_active_subcarrier);
      EXPECT_LE(subcarrier, last_active_subcarrier);
      if (dmrs_symbol_mask.test(symbol)) {
        EXPECT_FALSE(dmrs_prb_mask.test(subcarrier % NOF_SUBCARRIERS_PER_RB))
            << "offset=" << re_offset << " symbol=" << symbol << " subcarrier=" << subcarrier;
      }
    }
  }
}

TEST(PdschGpuRealGridMapper, SupportedResidentMappingsMatchHostMapper)
{
  if (!is_pdsch_block_processor_acceleration_available()) {
    GTEST_SKIP() << "CUDA PDSCH GPU block processor is not available in this build/runtime.";
  }

  constexpr unsigned nof_prb              = 24;
  constexpr unsigned nof_subc             = MAX_NOF_SUBCARRIERS;
  constexpr unsigned nof_grid_re_per_port = MAX_NSYMB_PER_SLOT * MAX_NOF_SUBCARRIERS;

  std::shared_ptr<channel_precoder_factory> precoder_factory = create_channel_precoder_factory("auto");
  ASSERT_NE(precoder_factory, nullptr);
  std::shared_ptr<resource_grid_mapper_factory> mapper_factory = create_resource_grid_mapper_factory(precoder_factory);
  ASSERT_NE(mapper_factory, nullptr);
  std::shared_ptr<resource_grid_factory> rg_factory = create_resource_grid_factory();
  ASSERT_NE(rg_factory, nullptr);

  symbol_slot_mask dmrs_symbol_mask = make_dmrs_symbol_mask(dmrs_profile::double_symbol);
  re_pattern_list  reserved(get_dmrs_pattern(dmrs_config_type::type2, bwp_start_rb, nof_prb, 2, dmrs_symbol_mask));

  resource_grid_mapper::allocation_configuration allocation = {.bwp = crb_interval{0, nof_prb},
                                                               .freq_alloc =
                                                                   rb_allocation::make_type1(0, nof_prb, std::nullopt),
                                                               .time_alloc = {0, nof_ofdm_symbols}};

  std::vector<uint32_t> re_offsets = generate_data_re_offsets(allocation, reserved, nof_subc);
  ASSERT_FALSE(re_offsets.empty());
  // The host precoding reference currently supports up to four ports. Keep the resident mapper parity test inside the
  // common 1/2/4-port operating envelope until the generic host precoder grows an 8-port matrix representation.
  for (unsigned nof_ports : {1U, 2U, 4U}) {
    SCOPED_TRACE("one-layer ports=" + std::to_string(nof_ports));

    std::vector<ci8_t>      symbols          = generate_mapper_symbols(re_offsets.size());
    precoding_weight_matrix weights          = make_one_layer_all_ports(nof_ports);
    precoding_configuration precoding_config = precoding_configuration::make_wideband(weights);

    std::unique_ptr<resource_grid> host_grid = rg_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
    ASSERT_NE(host_grid, nullptr);
    host_grid->set_all_zero();

    resource_grid_mapper::symbol_buffer_adapter host_buffer{span<const ci8_t>(symbols)};
    std::unique_ptr<resource_grid_mapper>       mapper = mapper_factory->create();
    ASSERT_NE(mapper, nullptr);
    std::vector<uint8_t> port_list(precoding_config.get_nof_ports());
    std::iota(port_list.begin(), port_list.end(), 0);
    mapper->map(host_grid->get_writer(), host_buffer, allocation, reserved, port_list, precoding_config);

    int8_t*   d_symbols    = nullptr;
    uint32_t* d_re_offsets = nullptr;
    uint16_t* d_grid_bf16  = nullptr;
    size_t    grid_words   = static_cast<size_t>(nof_ports) * nof_grid_re_per_port * 2U;
    ASSERT_EQ(cudaMalloc(&d_symbols, symbols.size() * sizeof(ci8_t)), cudaSuccess);
    ASSERT_EQ(cudaMalloc(&d_re_offsets, re_offsets.size() * sizeof(uint32_t)), cudaSuccess);
    ASSERT_EQ(cudaMalloc(&d_grid_bf16, grid_words * sizeof(uint16_t)), cudaSuccess);
    ASSERT_EQ(cudaMemset(d_grid_bf16, 0, grid_words * sizeof(uint16_t)), cudaSuccess);
    ASSERT_EQ(cudaMemcpy(d_symbols, symbols.data(), symbols.size() * sizeof(ci8_t), cudaMemcpyHostToDevice),
              cudaSuccess);
    ASSERT_EQ(cudaMemcpy(d_re_offsets, re_offsets.data(), re_offsets.size() * sizeof(uint32_t), cudaMemcpyHostToDevice),
              cudaSuccess);

    bool launch_ok = pdsch_map_one_layer_all_ports_int8_to_bf16_real_grid(d_symbols,
                                                                          d_re_offsets,
                                                                          d_grid_bf16,
                                                                          re_offsets.size(),
                                                                          nof_ports,
                                                                          nof_grid_re_per_port,
                                                                          weights.get_coefficient(0, 0).real(),
                                                                          nullptr);
    ASSERT_TRUE(launch_ok);
    ASSERT_EQ(cudaDeviceSynchronize(), cudaSuccess);

    std::vector<uint16_t> gpu_grid_bf16(grid_words);
    ASSERT_EQ(cudaMemcpy(gpu_grid_bf16.data(), d_grid_bf16, grid_words * sizeof(uint16_t), cudaMemcpyDeviceToHost),
              cudaSuccess);

    assert_real_grid_matches_host(gpu_grid_bf16, host_grid->get_reader(), nof_ports, nof_grid_re_per_port);

    cudaFree(d_grid_bf16);
    cudaFree(d_re_offsets);
    cudaFree(d_symbols);
  }

  for (unsigned nof_layers : {2U, 3U, 4U}) {
    SCOPED_TRACE("identity layers=" + std::to_string(nof_layers));

    std::vector<ci8_t>      symbols          = generate_mapper_symbols(re_offsets.size(), nof_layers);
    precoding_weight_matrix weights          = make_identity(nof_layers);
    precoding_configuration precoding_config = precoding_configuration::make_wideband(weights);

    std::unique_ptr<resource_grid> host_grid = rg_factory->create(nof_layers, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
    ASSERT_NE(host_grid, nullptr);
    host_grid->set_all_zero();

    resource_grid_mapper::symbol_buffer_adapter host_buffer{span<const ci8_t>(symbols)};
    std::unique_ptr<resource_grid_mapper>       mapper = mapper_factory->create();
    ASSERT_NE(mapper, nullptr);
    std::vector<uint8_t> port_list(precoding_config.get_nof_ports());
    std::iota(port_list.begin(), port_list.end(), 0);
    mapper->map(host_grid->get_writer(), host_buffer, allocation, reserved, port_list, precoding_config);

    int8_t*   d_symbols    = nullptr;
    uint32_t* d_re_offsets = nullptr;
    uint16_t* d_grid_bf16  = nullptr;
    size_t    grid_words   = static_cast<size_t>(nof_layers) * nof_grid_re_per_port * 2U;
    ASSERT_EQ(cudaMalloc(&d_symbols, symbols.size() * sizeof(ci8_t)), cudaSuccess);
    ASSERT_EQ(cudaMalloc(&d_re_offsets, re_offsets.size() * sizeof(uint32_t)), cudaSuccess);
    ASSERT_EQ(cudaMalloc(&d_grid_bf16, grid_words * sizeof(uint16_t)), cudaSuccess);
    ASSERT_EQ(cudaMemset(d_grid_bf16, 0, grid_words * sizeof(uint16_t)), cudaSuccess);
    ASSERT_EQ(cudaMemcpy(d_symbols, symbols.data(), symbols.size() * sizeof(ci8_t), cudaMemcpyHostToDevice),
              cudaSuccess);
    ASSERT_EQ(cudaMemcpy(d_re_offsets, re_offsets.data(), re_offsets.size() * sizeof(uint32_t), cudaMemcpyHostToDevice),
              cudaSuccess);

    bool launch_ok = pdsch_map_layers_int8_to_bf16_real_grid(d_symbols,
                                                             d_re_offsets,
                                                             d_grid_bf16,
                                                             re_offsets.size(),
                                                             nof_layers,
                                                             nof_layers,
                                                             nof_grid_re_per_port,
                                                             weights.get_coefficient(0, 0).real(),
                                                             nullptr);
    ASSERT_TRUE(launch_ok);
    ASSERT_EQ(cudaDeviceSynchronize(), cudaSuccess);

    std::vector<uint16_t> gpu_grid_bf16(grid_words);
    ASSERT_EQ(cudaMemcpy(gpu_grid_bf16.data(), d_grid_bf16, grid_words * sizeof(uint16_t), cudaMemcpyDeviceToHost),
              cudaSuccess);

    assert_real_grid_matches_host(gpu_grid_bf16, host_grid->get_reader(), nof_layers, nof_grid_re_per_port);

    cudaFree(d_grid_bf16);
    cudaFree(d_re_offsets);
    cudaFree(d_symbols);
  }
}
#endif

INSTANTIATE_TEST_SUITE_P(
    PdschGpuE2eSweep,
    PdschGpuE2eFixture,
    ::testing::Values(
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(0), 3, 1, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(0), 25, 1, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(0), 106, 1, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(0), 162, 1, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 52, 1, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 52, 1, 2, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(20), 106, 1, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam256, sch_mcs_index(20), 162, 1, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(27), 273, 1, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(27), 273, 1, 0, dmrs_profile::double_symbol, 2, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(27), 273, 1, 0, dmrs_profile::double_symbol, 2, 4},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64,
                             sch_mcs_index(27),
                             273,
                             1,
                             0,
                             dmrs_profile::double_symbol,
                             2,
                             0,
                             dmrs_config_type::type2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64,
                             sch_mcs_index(10),
                             106,
                             1,
                             0,
                             dmrs_profile::double_symbol,
                             2,
                             4,
                             dmrs_config_type::type1,
                             0x4601,
                             17,
                             17,
                             false,
                             3},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64,
                             sch_mcs_index(10),
                             106,
                             1,
                             0,
                             dmrs_profile::double_symbol,
                             2,
                             4,
                             dmrs_config_type::type2,
                             0x4602,
                             511,
                             1007,
                             true,
                             7},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(0), 162, 1, 1, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(20), 106, 1, 3, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam256, sch_mcs_index(20), 162, 1, 2, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 52, 2, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 106, 2, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 273, 2, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64,
                             sch_mcs_index(10),
                             273,
                             2,
                             0,
                             dmrs_profile::double_symbol,
                             2,
                             0,
                             dmrs_config_type::type2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64,
                             sch_mcs_index(10),
                             106,
                             2,
                             0,
                             dmrs_profile::double_symbol,
                             2,
                             2,
                             dmrs_config_type::type2,
                             0x4702,
                             321,
                             654,
                             true,
                             5},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(20), 106, 2, 1, dmrs_profile::single_symbol, 1},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam256, sch_mcs_index(20), 162, 2, 3, dmrs_profile::single_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 52, 3, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 106, 3, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 273, 3, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64,
                             sch_mcs_index(10),
                             273,
                             3,
                             0,
                             dmrs_profile::double_symbol,
                             2,
                             0,
                             dmrs_config_type::type2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64,
                             sch_mcs_index(10),
                             106,
                             3,
                             0,
                             dmrs_profile::double_symbol,
                             2,
                             3,
                             dmrs_config_type::type1,
                             0x4703,
                             77,
                             999,
                             true,
                             4},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(20), 106, 3, 1, dmrs_profile::single_symbol, 1},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 52, 4, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 106, 4, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 273, 4, 0, dmrs_profile::double_symbol, 2},
        // Maximum-bandwidth, rank-4 profile. This requires more than 52 LDPC codeblocks and guards the resident
        // transport-block encoder's preallocated buffers.
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam256, sch_mcs_index(27), 273, 4, 0, dmrs_profile::double_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64,
                             sch_mcs_index(10),
                             273,
                             4,
                             0,
                             dmrs_profile::double_symbol,
                             2,
                             0,
                             dmrs_config_type::type2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64,
                             sch_mcs_index(10),
                             106,
                             4,
                             0,
                             dmrs_profile::double_symbol,
                             2,
                             4,
                             dmrs_config_type::type2,
                             0x4804,
                             1003,
                             42,
                             false,
                             9},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam256, sch_mcs_index(20), 106, 4, 2, dmrs_profile::single_symbol, 1},
        // Issue #1 OTA profile: 20 MHz FDD, rank 4, qam256 MCS 26 and the scheduler's 12-symbol allocation.
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam256,
                             sch_mcs_index(26),
                             106,
                             4,
                             0,
                             dmrs_profile::triple_symbol,
                             2,
                             4,
                             dmrs_config_type::type1,
                             0x4601,
                             10,
                             10,
                             false,
                             0,
                             2,
                             12,
                             subcarrier_spacing::kHz15},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam256,
                             sch_mcs_index(26),
                             106,
                             4,
                             1,
                             dmrs_profile::triple_symbol,
                             2,
                             4,
                             dmrs_config_type::type1,
                             0x4601,
                             10,
                             10,
                             false,
                             1,
                             2,
                             12,
                             subcarrier_spacing::kHz15},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam256,
                             sch_mcs_index(26),
                             106,
                             4,
                             2,
                             dmrs_profile::triple_symbol,
                             2,
                             4,
                             dmrs_config_type::type1,
                             0x4601,
                             10,
                             10,
                             false,
                             2,
                             2,
                             12,
                             subcarrier_spacing::kHz15},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam256,
                             sch_mcs_index(26),
                             106,
                             4,
                             3,
                             dmrs_profile::triple_symbol,
                             2,
                             4,
                             dmrs_config_type::type1,
                             0x4601,
                             10,
                             10,
                             false,
                             3,
                             2,
                             12,
                             subcarrier_spacing::kHz15},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 106, 1, 0, dmrs_profile::triple_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 106, 2, 0, dmrs_profile::triple_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 106, 3, 0, dmrs_profile::triple_symbol, 2},
        pdsch_gpu_e2e_params{pdsch_mcs_table::qam64, sch_mcs_index(10), 106, 4, 0, dmrs_profile::triple_symbol, 2}));
