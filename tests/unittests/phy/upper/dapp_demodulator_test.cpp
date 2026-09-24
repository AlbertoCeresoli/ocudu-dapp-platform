// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "../support/resource_grid_test_doubles.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/phy/upper/channel_modulation/channel_modulation_factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_codeword_buffer.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator_notifier.h"
#include "ocudu/phy/upper/dapp/pusch_dapp_demodulator.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_data_router.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <thread>
#include <tuple>
#include <vector>

using namespace ocudu;

namespace {

class empty_results final : public dmrs_pusch_estimator_results
{
public:
  float                           get_noise_variance(unsigned) const override { return 0; }
  float                           get_rsrp(unsigned, unsigned = 0) const override { return 0; }
  static_vector<float, MAX_PORTS> get_rsrp_all_ports(unsigned = 0) const override { return {}; }
  float                           get_epre(unsigned) const override { return 0; }
  float                           get_snr(unsigned) const override { return 0; }
  float                           get_layer_average_snr(unsigned = 0) const override { return 0; }
  phy_time_unit                   get_time_alignment(unsigned) const override { return {}; }
  std::optional<float>            get_cfo_Hz(unsigned) const override { return std::nullopt; }
  void                            get_symbol_ch_estimate(span<cbf16_t>, unsigned, unsigned, unsigned) const override {}
  void                            get_symbol_ch_estimate(span<cbf16_t>,
                                                         unsigned,
                                                         unsigned,
                                                         unsigned,
                                                         const bounded_bitset<MAX_NOF_SUBCARRIERS>&) const override
  {
  }
  void get_channel_state_information(channel_state_information&) const override {}
};

class codeword_spy final : public pusch_codeword_buffer
{
public:
  span<log_likelihood_ratio> get_next_block_view(unsigned block_size) override
  {
    scratch.resize(block_size);
    return scratch;
  }

  void on_new_block(span<const log_likelihood_ratio> block, const bit_buffer& sequence) override
  {
    for (unsigned index = 0; index != block.size(); ++index) {
      data.push_back(block[index].to_int());
      scrambling.push_back(sequence.extract(index, 1));
    }
  }

  void on_end_codeword() override { ++end_calls; }

  std::vector<log_likelihood_ratio> scratch;
  std::vector<int>                  data;
  std::vector<uint8_t>              scrambling;
  unsigned                          end_calls{};
};

class notifier_spy final : public pusch_demodulator_notifier
{
public:
  void on_provisional_stats(unsigned symbol, const demodulation_stats& stats) override
  {
    symbols.push_back(symbol);
    provisional.push_back(stats);
  }
  void on_end_stats(const demodulation_stats& stats) override
  {
    ++end_calls;
    final = stats;
  }

  std::vector<unsigned>           symbols;
  std::vector<demodulation_stats> provisional;
  demodulation_stats              final;
  unsigned                        end_calls{};
};

struct fallback_state {
  unsigned calls{};
  bool     resident{};
  bool     device_grid{};
  bool     owned_device_grid{};
};

class fallback_demodulator final : public pusch_demodulator
{
public:
  explicit fallback_demodulator(std::shared_ptr<fallback_state> state_) : state(std::move(state_)) {}

  void demodulate(pusch_codeword_buffer&      codeword,
                  pusch_demodulator_notifier& notifier,
                  const resource_grid_reader&,
                  const dmrs_pusch_estimator_results&,
                  const configuration&) override
  {
    ++state->calls;
    static_bit_buffer<8> sequence(2);
    auto                 output = codeword.get_next_block_view(2);
    output[0]                   = 3;
    output[1]                   = -3;
    codeword.on_new_block(output, sequence);
    codeword.on_end_codeword();
    notifier.on_end_stats({});
  }

  bool supports_resident_mode() const override { return true; }
  void enable_resident_mode() override { state->resident = true; }
  void disable_resident_mode() override { state->resident = false; }
  bool is_resident_mode_enabled() const override { return state->resident; }
  bool used_device_grid() const override { return state->device_grid; }
  bool used_owned_device_grid() const override { return state->owned_device_grid; }

private:
  std::shared_ptr<fallback_state> state;
};

class fallback_factory final : public pusch_demodulator_factory
{
public:
  explicit fallback_factory(std::shared_ptr<fallback_state> state_) : state(std::move(state_)) {}
  std::unique_ptr<pusch_demodulator> create() override { return std::make_unique<fallback_demodulator>(state); }

private:
  std::shared_ptr<fallback_state> state;
};

class target_spy final : public pusch_dapp_demodulator_target
{
public:
  explicit target_spy(uint32_t lanes_) : lanes(lanes_) {}

  pusch_dapp_demodulator_mode active_mode() const noexcept override { return mode; }
  uint32_t                    lane_count() const noexcept override { return lanes; }
  uint32_t                    backend_id(pusch_dapp_demodulator_mode) const noexcept override { return backend; }
  uint32_t                    worker_id(uint32_t lane, pusch_dapp_demodulator_mode selected) const noexcept override
  {
    return (selected == pusch_dapp_demodulator_mode::equalizer ? 100U : 200U) + lane;
  }

  dapp::native_invocation_result invoke(uint32_t                             lane,
                                        const ocudu_dapp_equalizer_input_v1& input,
                                        ocudu_dapp_equalizer_output_v1&      output) noexcept override
  {
    ++equalizer_calls;
    record(lane, input.invocation, input.pusch, input.dmrs);
    if (status != OCUDU_DAPP_OK_V1) {
      return {status, true};
    }
    auto* channel = reinterpret_cast<cf_t*>(output.channel_estimates.native_handle);
    std::fill_n(channel, output.channel_estimates.byte_length / sizeof(cf_t), cf_t{1.0F, 0.0F});
    auto* symbols = reinterpret_cast<cf_t*>(output.equalized_symbols.native_handle);
    std::fill_n(symbols, output.equalized_symbols.byte_length / sizeof(cf_t), cf_t{std::sqrt(0.5F), std::sqrt(0.5F)});
    auto* noise = reinterpret_cast<float*>(output.post_equalization_noise.native_handle);
    std::fill_n(noise, output.post_equalization_noise.byte_length / sizeof(float), 0.25F);
    if (invalid_content) {
      symbols[0] = {std::numeric_limits<float>::quiet_NaN(), 0.0F};
    }
    if (replace_output) {
      ++output.equalized_symbols.allocation_generation;
    }
    return {OCUDU_DAPP_OK_V1, false};
  }

  dapp::native_invocation_result invoke(uint32_t                            lane,
                                        const ocudu_dapp_receiver_input_v1& input,
                                        ocudu_dapp_receiver_output_v1&      output) noexcept override
  {
    ++receiver_calls;
    record(lane, input.invocation, input.pusch, input.dmrs);
    if (status != OCUDU_DAPP_OK_V1) {
      return {status, true};
    }
    auto* llrs = reinterpret_cast<int8_t*>(output.llrs.native_handle);
    std::fill_n(llrs, output.llrs.byte_length, int8_t{10});
    if (output.metrics.metadata_schema_id == OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1 &&
        output.metrics.metadata.data != nullptr) {
      const auto* metrics = static_cast<const ocudu_dapp_receiver_metrics_v1*>(output.metrics.metadata.data);
      auto*       noise   = reinterpret_cast<float*>(metrics->post_equalization_noise.native_handle);
      std::fill_n(noise, metrics->post_equalization_noise.byte_length / sizeof(float), 0.25F);
      if (invalid_receiver_noise) {
        noise[0] = std::numeric_limits<float>::quiet_NaN();
      }
      if (publish_receiver_metrics) {
        output.reserved0 |= OCUDU_DAPP_RECEIVER_METRICS_VALID_V1;
      }
      requested_receiver_symbols = metrics->equalized_symbols.struct_size != 0;
      if (requested_receiver_symbols && publish_receiver_symbols) {
        auto* symbols = reinterpret_cast<cf_t*>(metrics->equalized_symbols.native_handle);
        std::fill_n(symbols, metrics->equalized_symbols.dimensions[0], cf_t{0.31F, -0.57F});
        if (invalid_receiver_symbols) {
          symbols[0] = {std::numeric_limits<float>::quiet_NaN(), 0.0F};
        }
        output.reserved0 |= OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_VALID_V1;
      }
    }
    if (invalid_content) {
      llrs[0] = std::numeric_limits<int8_t>::min();
    }
    if (replace_output) {
      ++output.llrs.allocation_id;
    }
    output.confidence_q16 = 32768;
    return {OCUDU_DAPP_OK_V1, false};
  }

  void report_invalid_output(uint32_t                    lane,
                             pusch_dapp_demodulator_mode selected,
                             const ocudu_dapp_invocation_v1&) noexcept override
  {
    invalid_outputs.emplace_back(lane, selected);
  }

  void report_fallback(uint32_t                      lane,
                       pusch_dapp_demodulator_mode   selected,
                       dapp::class_a_fallback_reason reason) noexcept override
  {
    fallback_reasons.emplace_back(lane, selected, reason);
  }

  void record(uint32_t                            lane,
              const ocudu_dapp_invocation_v1&     invocation_,
              const ocudu_dapp_typed_metadata_v1& pusch_wrapper,
              const ocudu_dapp_typed_metadata_v1& dmrs_wrapper)
  {
    invoked_lanes.push_back(lane);
    invocation       = invocation_;
    pusch            = *static_cast<const ocudu_dapp_pusch_metadata_v1*>(pusch_wrapper.metadata.data);
    dmrs             = *static_cast<const ocudu_dapp_dmrs_metadata_v1*>(dmrs_wrapper.metadata.data);
    const auto* mask = reinterpret_cast<const uint8_t*>(pusch.data_re_mask.native_handle);
    data_mask.assign(mask, mask + pusch.data_re_mask.byte_length);
    const auto* indices = reinterpret_cast<const uint32_t*>(pusch.data_re_indices.native_handle);
    data_re_indices.assign(indices, indices + pusch.nof_data_re);
    const auto* coordinate_source = reinterpret_cast<const uint16_t*>(dmrs.coordinates.native_handle);
    coordinates.assign(coordinate_source, coordinate_source + 2 * dmrs.nof_pilots);
    const auto* reference_source = reinterpret_cast<const cf_t*>(dmrs.reference_symbols.native_handle);
    references.assign(reference_source, reference_source + dmrs.nof_pilots);
  }

  pusch_dapp_demodulator_mode                                   mode{pusch_dapp_demodulator_mode::receiver};
  uint32_t                                                      backend{OCUDU_DAPP_BACKEND_HOST_V1};
  ocudu_dapp_status_v1                                          status{OCUDU_DAPP_OK_V1};
  bool                                                          invalid_content{};
  bool                                                          invalid_receiver_noise{};
  bool                                                          publish_receiver_metrics{true};
  bool                                                          publish_receiver_symbols{true};
  bool                                                          requested_receiver_symbols{};
  bool                                                          invalid_receiver_symbols{};
  bool                                                          replace_output{};
  unsigned                                                      equalizer_calls{};
  unsigned                                                      receiver_calls{};
  std::vector<uint32_t>                                         invoked_lanes;
  std::vector<std::pair<uint32_t, pusch_dapp_demodulator_mode>> invalid_outputs;
  std::vector<std::tuple<uint32_t, pusch_dapp_demodulator_mode, dapp::class_a_fallback_reason>> fallback_reasons;
  ocudu_dapp_invocation_v1                                                                      invocation{};
  ocudu_dapp_pusch_metadata_v1                                                                  pusch{};
  ocudu_dapp_dmrs_metadata_v1                                                                   dmrs{};
  std::vector<uint8_t>                                                                          data_mask;
  std::vector<uint32_t>                                                                         data_re_indices;
  std::vector<uint16_t>                                                                         coordinates;
  std::vector<cf_t>                                                                             references;

private:
  uint32_t lanes;
};

class concurrent_target final : public pusch_dapp_demodulator_target
{
public:
  pusch_dapp_demodulator_mode active_mode() const noexcept override { return pusch_dapp_demodulator_mode::receiver; }
  uint32_t                    lane_count() const noexcept override { return 2; }
  uint32_t worker_id(uint32_t lane, pusch_dapp_demodulator_mode) const noexcept override { return 300 + lane; }
  dapp::native_invocation_result
  invoke(uint32_t, const ocudu_dapp_equalizer_input_v1&, ocudu_dapp_equalizer_output_v1&) noexcept override
  {
    return {OCUDU_DAPP_BYPASS_V1, true};
  }
  dapp::native_invocation_result
  invoke(uint32_t, const ocudu_dapp_receiver_input_v1&, ocudu_dapp_receiver_output_v1& output) noexcept override
  {
    entered.fetch_add(1, std::memory_order_acq_rel);
    const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (entered.load(std::memory_order_acquire) != 2 && std::chrono::steady_clock::now() < timeout) {
      std::this_thread::yield();
    }
    if (entered.load(std::memory_order_acquire) == 2) {
      overlapped.fetch_add(1, std::memory_order_relaxed);
    }
    std::fill_n(reinterpret_cast<int8_t*>(output.llrs.native_handle), output.llrs.byte_length, int8_t{10});
    return {OCUDU_DAPP_OK_V1, false};
  }

  std::atomic<unsigned> entered{};
  std::atomic<unsigned> overlapped{};
};

pusch_demodulator::configuration make_config(unsigned nof_layers = 1)
{
  pusch_demodulator::configuration config{};
  config.cell_id = 7;
  config.rnti    = to_rnti(0x1234);
  config.rb_mask = crb_bitmap(1);
  config.rb_mask.set(0);
  config.modulation         = modulation_scheme::QPSK;
  config.start_symbol_index = 2;
  config.nof_symbols        = 2;
  config.dmrs_symb_pos      = symbol_slot_mask(MAX_NSYMB_PER_SLOT);
  config.dmrs_symb_pos.set(2);
  config.dmrs_type                   = dmrs_config_type::type1;
  config.nof_cdm_groups_without_data = 1;
  config.n_id                        = 17;
  config.dmrs_scrambling_id          = 19;
  config.n_scid                      = false;
  config.dmrs_scaling                = std::sqrt(2.0F);
  config.c_prefix                    = cyclic_prefix::NORMAL;
  config.nof_tx_layers               = nof_layers;
  config.enable_transform_precoding  = false;
  config.slot                        = slot_point{1, 22};
  config.rx_ports.push_back(0);
  return config;
}

resource_grid_reader_spy make_grid()
{
  resource_grid_reader_spy grid(1, MAX_NSYMB_PER_SLOT, 1);
  for (unsigned symbol = 2; symbol != 4; ++symbol) {
    for (unsigned subcarrier = 0; subcarrier != NOF_SUBCARRIERS_PER_RB; ++subcarrier) {
      grid.write({0,
                  static_cast<uint8_t>(symbol),
                  static_cast<uint16_t>(subcarrier),
                  {static_cast<float>(100 * symbol + subcarrier), -static_cast<float>(subcarrier)}});
    }
  }
  return grid;
}

std::shared_ptr<pusch_demodulator_factory> make_factory(const std::shared_ptr<target_spy>&     target,
                                                        const std::shared_ptr<fallback_state>& fallback,
                                                        unsigned                               lanes      = 2,
                                                        unsigned                               max_layers = 1)
{
  return create_pusch_dapp_demodulator_factory(
      std::make_shared<fallback_factory>(fallback),
      target,
      create_pseudo_random_generator_sw_factory(),
      create_demodulation_mapper_factory(),
      {.nof_lanes = lanes, .max_nof_prb = 1, .max_nof_rx_ports = 1, .max_nof_layers = max_layers});
}

void run(pusch_demodulator&                      demodulator,
         const pusch_demodulator::configuration& config,
         codeword_spy&                           codeword,
         notifier_spy&                           notifier)
{
  auto          grid = make_grid();
  empty_results results;
  demodulator.demodulate(codeword, notifier, grid, results, config);
}

} // namespace

TEST(dapp_demodulator, forwards_current_grid_provenance_without_caching_or_inventing_it)
{
  auto target      = std::make_shared<target_spy>(2);
  auto fallback    = std::make_shared<fallback_state>();
  auto demodulator = make_factory(target, fallback)->create();
  target->backend  = OCUDU_DAPP_BACKEND_CUDA_V1;
  EXPECT_FALSE(demodulator->used_device_grid());
  EXPECT_FALSE(demodulator->used_owned_device_grid());
  for (auto mode : {pusch_dapp_demodulator_mode::inactive,
                    pusch_dapp_demodulator_mode::receiver,
                    pusch_dapp_demodulator_mode::equalizer}) {
    target->mode = mode;
    for (const auto& flags :
         {std::pair{false, false}, std::pair{true, false}, std::pair{true, true}, std::pair{false, false}}) {
      fallback->device_grid       = flags.first;
      fallback->owned_device_grid = flags.second;
      codeword_spy codeword;
      notifier_spy notifier;
      run(*demodulator, make_config(), codeword, notifier);
      EXPECT_EQ(demodulator->used_device_grid(), flags.first);
      EXPECT_EQ(demodulator->used_owned_device_grid(), flags.second);
    }
  }
  // A successful CPU receiver must not inherit a previous CUDA grant's flags.
  fallback->device_grid = fallback->owned_device_grid = true;
  target->backend                                     = OCUDU_DAPP_BACKEND_HOST_V1;
  target->mode                                        = pusch_dapp_demodulator_mode::receiver;
  codeword_spy codeword;
  notifier_spy notifier;
  run(*demodulator, make_config(), codeword, notifier);
  EXPECT_FALSE(demodulator->used_device_grid());
  EXPECT_FALSE(demodulator->used_owned_device_grid());
}

TEST(dapp_demodulator, receiver_publishes_typed_inputs_and_rejoins_the_codeword_pipeline)
{
  auto         target      = std::make_shared<target_spy>(2);
  auto         fallback    = std::make_shared<fallback_state>();
  auto         demodulator = make_factory(target, fallback)->create();
  const auto   config      = make_config();
  codeword_spy codeword;
  notifier_spy notifier;

  run(*demodulator, config, codeword, notifier);

  EXPECT_EQ(fallback->calls, 0U);
  EXPECT_EQ(target->receiver_calls, 1U);
  EXPECT_EQ(target->equalizer_calls, 0U);
  EXPECT_EQ(target->invoked_lanes, std::vector<uint32_t>{0});
  EXPECT_EQ(target->invocation.cell_id, 7U);
  EXPECT_EQ(target->invocation.worker_id, 200U);
  EXPECT_EQ(target->pusch.nof_symbols, 2U);
  EXPECT_EQ(target->pusch.nof_subcarriers, NOF_SUBCARRIERS_PER_RB);
  EXPECT_EQ(target->pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_SUBCARRIER_SPACING_HZ_V1], 30000U);
  EXPECT_EQ(target->pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_GRID_NOF_SUBCARRIERS_V1], NOF_SUBCARRIERS_PER_RB);
  EXPECT_EQ(target->pusch.flags, OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1);
  EXPECT_EQ(std::count(target->data_mask.begin(), target->data_mask.end(), uint8_t{1}), 18);
  EXPECT_EQ(target->pusch.data_re_mask.layout_id, OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_MASK_U8_V1);
  EXPECT_EQ(target->pusch.data_re_indices.layout_id, OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1);
  EXPECT_EQ(target->data_re_indices,
            (std::vector<uint32_t>{1, 3, 5, 7, 9, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23}));
  EXPECT_EQ(target->dmrs.nof_pilots, 6U);
  EXPECT_EQ(target->dmrs.flags,
            OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1);
  EXPECT_EQ(target->coordinates, (std::vector<uint16_t>{0, 0, 0, 2, 0, 4, 0, 6, 0, 8, 0, 10}));
  for (cf_t reference : target->references) {
    EXPECT_NEAR(std::abs(reference), config.dmrs_scaling, 1.0e-5F);
  }
  ASSERT_EQ(codeword.data.size(), 36U);
  ASSERT_EQ(codeword.scrambling.size(), codeword.data.size());
  for (unsigned index = 0; index != codeword.data.size(); ++index) {
    EXPECT_EQ(codeword.data[index], codeword.scrambling[index] == 0 ? 10 : -10);
  }
  EXPECT_EQ(codeword.end_calls, 1U);
  EXPECT_EQ(notifier.end_calls, 1U);
  EXPECT_EQ(notifier.symbols, (std::vector<unsigned>{2, 3}));
  ASSERT_TRUE(notifier.final.sinr_dB.has_value());
  // Physical post-equalization noise is already in the scheduler convention;
  // the host must not add a global optimism offset that can drive PUSCH TPC down.
  EXPECT_NEAR(*notifier.final.sinr_dB, 6.0206F, 0.01F);
}

TEST(dapp_demodulator, receiver_does_not_reuse_stale_sinr_when_metrics_are_not_published)
{
  auto         target      = std::make_shared<target_spy>(1);
  auto         fallback    = std::make_shared<fallback_state>();
  auto         demodulator = make_factory(target, fallback, 1)->create();
  codeword_spy first_codeword;
  notifier_spy first_notifier;
  run(*demodulator, make_config(), first_codeword, first_notifier);
  ASSERT_TRUE(first_notifier.final.sinr_dB.has_value());

  target->publish_receiver_metrics = false;
  codeword_spy second_codeword;
  notifier_spy second_notifier;
  run(*demodulator, make_config(), second_codeword, second_notifier);

  EXPECT_EQ(fallback->calls, 0U);
  EXPECT_EQ(target->receiver_calls, 2U);
  EXPECT_EQ(second_codeword.data.size(), 36U);
  EXPECT_FALSE(second_notifier.final.sinr_dB.has_value());
}

TEST(dapp_demodulator, receiver_symbol_capture_is_optional_and_never_reuses_stale_symbols)
{
  auto target = std::make_shared<target_spy>(1);
  auto fallback = std::make_shared<fallback_state>();
  auto demodulator = make_factory(target, fallback, 1)->create();
  codeword_spy first;
  notifier_spy first_stats;
  run(*demodulator, make_config(), first, first_stats);
  EXPECT_FALSE(target->requested_receiver_symbols);
  EXPECT_TRUE(demodulator->get_captured_symbols().empty());
  demodulator->enable_symbol_capture();
  codeword_spy second;
  notifier_spy second_stats;
  run(*demodulator, make_config(), second, second_stats);
  ASSERT_TRUE(target->requested_receiver_symbols);
  auto symbols = demodulator->get_captured_symbols();
  ASSERT_EQ(symbols.size(), 18U);
  for (cf_t value : symbols) EXPECT_EQ(value, cf_t(0.31F, -0.57F));
  EXPECT_EQ(first.data, second.data);
  EXPECT_EQ(first_stats.final.sinr_dB, second_stats.final.sinr_dB);
  target->publish_receiver_symbols = false; // A direct-bit receiver may decline.
  codeword_spy third;
  notifier_spy third_stats;
  run(*demodulator, make_config(), third, third_stats);
  EXPECT_TRUE(demodulator->get_captured_symbols().empty());
  EXPECT_EQ(third.data, first.data);
  EXPECT_EQ(fallback->calls, 0U);
  demodulator->disable_symbol_capture();
  codeword_spy fourth;
  notifier_spy fourth_stats;
  run(*demodulator, make_config(), fourth, fourth_stats);
  EXPECT_FALSE(target->requested_receiver_symbols);
  EXPECT_TRUE(demodulator->get_captured_symbols().empty());
}

TEST(dapp_demodulator, receiver_rejects_claimed_nonfinite_symbol_output)
{
  auto target = std::make_shared<target_spy>(1);
  auto fallback = std::make_shared<fallback_state>();
  auto demodulator = make_factory(target, fallback, 1)->create();
  demodulator->enable_symbol_capture();
  target->invalid_receiver_symbols = true;
  codeword_spy codeword;
  notifier_spy stats;
  run(*demodulator, make_config(), codeword, stats);
  EXPECT_EQ(fallback->calls, 1U);
  EXPECT_EQ(target->invalid_outputs.size(), 1U);
  EXPECT_TRUE(demodulator->get_captured_symbols().empty());
}

TEST(dapp_demodulator, receiver_symbol_delivery_and_scheduler_metrics_are_independent)
{
  auto target = std::make_shared<target_spy>(1);
  auto fallback = std::make_shared<fallback_state>();
  auto demodulator = make_factory(target, fallback, 1)->create();
  demodulator->enable_symbol_capture();
  std::vector<int> expected_llrs;

  // Reuse the same lane and buffers through all validity combinations. In
  // particular, direct-bit receivers may decline both auxiliary outputs, and
  // a receiver may expose symbols without asserting physical noise authority.
  for (const auto& flags : {std::make_pair(true, true), std::make_pair(true, false),
                           std::make_pair(false, true), std::make_pair(false, false)}) {
    target->publish_receiver_symbols = flags.first;
    target->publish_receiver_metrics = flags.second;
    // Unpublished storage is not authoritative, even if it contains NaNs.
    target->invalid_receiver_noise = !flags.second;
    codeword_spy codeword;
    notifier_spy stats;
    run(*demodulator, make_config(), codeword, stats);

    EXPECT_TRUE(target->requested_receiver_symbols);
    EXPECT_EQ(stats.final.sinr_dB.has_value(), flags.second);
    const auto symbols = demodulator->get_captured_symbols();
    EXPECT_EQ(symbols.size(), flags.first ? 18U : 0U);
    for (cf_t value : symbols) {
      EXPECT_EQ(value, cf_t(0.31F, -0.57F));
    }
    if (expected_llrs.empty()) {
      expected_llrs = codeword.data;
    }
    EXPECT_EQ(codeword.data.size(), 36U);
    EXPECT_EQ(codeword.data, expected_llrs);
    EXPECT_EQ(codeword.end_calls, 1U);
    EXPECT_EQ(fallback->calls, 0U);
    EXPECT_TRUE(target->invalid_outputs.empty());
  }
}

TEST(dapp_demodulator, receiver_rejects_claimed_nonfinite_noise_metrics)
{
  auto target                    = std::make_shared<target_spy>(1);
  target->invalid_receiver_noise = true;
  auto         fallback          = std::make_shared<fallback_state>();
  auto         demodulator       = make_factory(target, fallback, 1)->create();
  codeword_spy codeword;
  notifier_spy notifier;

  run(*demodulator, make_config(), codeword, notifier);

  EXPECT_EQ(target->receiver_calls, 1U);
  EXPECT_EQ(target->invalid_outputs.size(), 1U);
  EXPECT_EQ(fallback->calls, 1U);
  EXPECT_FALSE(notifier.final.sinr_dB.has_value());
}

TEST(dapp_demodulator, equalizer_uses_the_conventional_demapper_after_validating_results)
{
  auto target           = std::make_shared<target_spy>(1);
  target->mode          = pusch_dapp_demodulator_mode::equalizer;
  auto         fallback = std::make_shared<fallback_state>();
  auto         instance = make_factory(target, fallback, 1)->create();
  codeword_spy codeword;
  notifier_spy notifier;

  run(*instance, make_config(), codeword, notifier);

  EXPECT_EQ(fallback->calls, 0U);
  EXPECT_EQ(target->equalizer_calls, 1U);
  EXPECT_EQ(target->invocation.worker_id, 100U);
  ASSERT_EQ(codeword.data.size(), 36U);
  EXPECT_TRUE(notifier.final.sinr_dB.has_value());
  // (signal-plus-noise - noise) / noise = 3, with no global calibration offset.
  EXPECT_NEAR(*notifier.final.sinr_dB, 4.7712F, 0.01F);
}

TEST(dapp_demodulator, assigns_unique_lanes_without_modulo_reuse)
{
  auto target   = std::make_shared<target_spy>(2);
  auto fallback = std::make_shared<fallback_state>();
  auto factory  = make_factory(target, fallback);
  auto first    = factory->create();
  auto second   = factory->create();
  EXPECT_EQ(factory->create(), nullptr);
  codeword_spy first_codeword;
  codeword_spy second_codeword;
  notifier_spy first_notifier;
  notifier_spy second_notifier;
  run(*first, make_config(), first_codeword, first_notifier);
  run(*second, make_config(), second_codeword, second_notifier);
  EXPECT_EQ(target->invoked_lanes, (std::vector<uint32_t>{0, 1}));
}

TEST(dapp_demodulator, separate_dependency_objects_execute_their_dapp_lanes_concurrently)
{
  auto target   = std::make_shared<concurrent_target>();
  auto fallback = std::make_shared<fallback_state>();
  auto factory  = create_pusch_dapp_demodulator_factory(
      std::make_shared<fallback_factory>(fallback),
      target,
      create_pseudo_random_generator_sw_factory(),
      create_demodulation_mapper_factory(),
      {.nof_lanes = 2, .max_nof_prb = 1, .max_nof_rx_ports = 1, .max_nof_layers = 1});
  auto         first  = factory->create();
  auto         second = factory->create();
  codeword_spy first_codeword;
  codeword_spy second_codeword;
  notifier_spy first_notifier;
  notifier_spy second_notifier;

  std::thread first_thread([&]() { run(*first, make_config(), first_codeword, first_notifier); });
  std::thread second_thread([&]() { run(*second, make_config(), second_codeword, second_notifier); });
  first_thread.join();
  second_thread.join();

  EXPECT_EQ(target->entered.load(), 2U);
  EXPECT_EQ(target->overlapped.load(), 2U);
  EXPECT_EQ(fallback->calls, 0U);
}

TEST(dapp_demodulator, falls_back_for_inactive_conflicting_failed_or_invalid_hooks)
{
  auto target   = std::make_shared<target_spy>(6);
  auto fallback = std::make_shared<fallback_state>();
  auto factory  = make_factory(target, fallback, 6);

  auto run_next = [&](const pusch_demodulator::configuration& config = make_config()) {
    codeword_spy codeword;
    notifier_spy notifier;
    auto         instance = factory->create();
    run(*instance, config, codeword, notifier);
  };

  target->mode = pusch_dapp_demodulator_mode::inactive;
  run_next();
  target->mode = pusch_dapp_demodulator_mode::conflict;
  run_next();
  target->mode   = pusch_dapp_demodulator_mode::receiver;
  target->status = OCUDU_DAPP_EXCEPTION_V1;
  run_next();
  target->status          = OCUDU_DAPP_OK_V1;
  target->invalid_content = true;
  run_next();
  target->invalid_content = false;
  target->replace_output  = true;
  run_next();
  target->replace_output = false;
  run_next(make_config(2));

  EXPECT_EQ(fallback->calls, 6U);
  EXPECT_EQ(target->invalid_outputs.size(), 2U);
  EXPECT_NE(std::find(target->fallback_reasons.begin(),
                      target->fallback_reasons.end(),
                      std::make_tuple(
                          1U, pusch_dapp_demodulator_mode::conflict, dapp::class_a_fallback_reason::overlapped_hook)),
            target->fallback_reasons.end());
  EXPECT_NE(std::find(target->fallback_reasons.begin(),
                      target->fallback_reasons.end(),
                      std::make_tuple(5U,
                                      pusch_dapp_demodulator_mode::receiver,
                                      dapp::class_a_fallback_reason::unsupported_layers)),
            target->fallback_reasons.end());
}

TEST(dapp_demodulator, bypasses_cpu_hook_while_the_conventional_resident_path_is_enabled)
{
  auto target      = std::make_shared<target_spy>(1);
  auto fallback    = std::make_shared<fallback_state>();
  auto demodulator = make_factory(target, fallback, 1)->create();
  demodulator->enable_resident_mode();
  codeword_spy codeword;
  notifier_spy notifier;

  run(*demodulator, make_config(), codeword, notifier);

  EXPECT_EQ(target->receiver_calls, 0U);
  EXPECT_EQ(fallback->calls, 1U);
  EXPECT_TRUE(demodulator->is_resident_mode_enabled());
  EXPECT_EQ(target->fallback_reasons,
            (std::vector<std::tuple<uint32_t, pusch_dapp_demodulator_mode, dapp::class_a_fallback_reason>>{
                {0, pusch_dapp_demodulator_mode::receiver, dapp::class_a_fallback_reason::overlapped_hook}}));
}

TEST(dapp_demodulator, bypasses_cpu_adapter_for_an_accelerator_backend)
{
  auto target              = std::make_shared<target_spy>(1);
  target->backend          = OCUDU_DAPP_BACKEND_CUDA_V1;
  auto         fallback    = std::make_shared<fallback_state>();
  auto         demodulator = make_factory(target, fallback, 1)->create();
  codeword_spy codeword;
  notifier_spy notifier;

  run(*demodulator, make_config(), codeword, notifier);

  EXPECT_EQ(target->receiver_calls, 0U);
  EXPECT_EQ(fallback->calls, 1U);
}

TEST(dapp_demodulator, native_target_and_provider_preserve_interface_and_lane_identity)
{
  auto equalizer_result = dapp::native_hook_point::create(OCUDU_DAPP_INTERFACE_EQUALIZER_V1, 40, 2);
  auto receiver_result  = dapp::native_hook_point::create(OCUDU_DAPP_INTERFACE_RECEIVER_V1, 80, 2);
  ASSERT_TRUE(equalizer_result);
  ASSERT_TRUE(receiver_result);
  auto target = create_native_pusch_dapp_demodulator_target(
      std::shared_ptr<dapp::native_hook_point>(std::move(equalizer_result.point)),
      std::shared_ptr<dapp::native_hook_point>(std::move(receiver_result.point)));
  ASSERT_NE(target, nullptr);
  EXPECT_EQ(target->active_mode(), pusch_dapp_demodulator_mode::inactive);
  EXPECT_EQ(target->lane_count(), 2U);
  EXPECT_EQ(target->worker_id(1, pusch_dapp_demodulator_mode::equalizer), 41U);
  EXPECT_EQ(target->worker_id(1, pusch_dapp_demodulator_mode::receiver), 81U);

  auto provider = create_upper_phy_dapp_hook_provider(nullptr, nullptr, target);
  ASSERT_NE(provider, nullptr);
  auto decorated = provider->decorate_pusch_demodulator(
      std::make_shared<fallback_factory>(std::make_shared<fallback_state>()),
      {.nof_lanes = 2, .max_nof_prb = 1, .max_nof_rx_ports = 1, .max_nof_layers = 1});
  EXPECT_NE(decorated, nullptr);

  auto wrong = dapp::native_hook_point::create(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 0, 1);
  ASSERT_TRUE(wrong);
  EXPECT_EQ(create_native_pusch_dapp_demodulator_target(
                std::shared_ptr<dapp::native_hook_point>(std::move(wrong.point)), nullptr),
            nullptr);
}

namespace {

/// Two receiver instances and the stock receiver behind one target, selected per UE the way the
/// native routed target selects hook-point slots.
class routing_target final : public pusch_dapp_demodulator_target
{
public:
  routing_target() { stock.mode = pusch_dapp_demodulator_mode::inactive; }

  dapp::pusch_route resolve_route(uint32_t cell_index, uint16_t rnti) const noexcept override
  {
    ++lookups;
    last_cell = cell_index;
    switch (rnti) {
      case 0x4601:
        return dapp::pusch_route::instance(dapp::pusch_route_hook::receiver, 0);
      case 0x4602:
        return dapp::pusch_route::instance(dapp::pusch_route_hook::receiver, 1);
      case 0x4603:
        return dapp::pusch_route::conventional();
      default:
        return dapp::pusch_route::primary();
    }
  }
  pusch_dapp_demodulator_target& route(const dapp::pusch_route& selected) noexcept override
  {
    if (selected == dapp::pusch_route::conventional()) {
      return stock;
    }
    if (selected.type == dapp::pusch_route::kind::instance) {
      return selected.slot == 0 ? first : second;
    }
    return primary;
  }

  pusch_dapp_demodulator_mode active_mode() const noexcept override { return primary.active_mode(); }
  uint32_t                    lane_count() const noexcept override { return primary.lane_count(); }
  uint32_t                    worker_id(uint32_t lane, pusch_dapp_demodulator_mode mode) const noexcept override
  {
    return primary.worker_id(lane, mode);
  }
  dapp::native_invocation_result invoke(uint32_t                             lane,
                                        const ocudu_dapp_equalizer_input_v1& input,
                                        ocudu_dapp_equalizer_output_v1&      output) noexcept override
  {
    return primary.invoke(lane, input, output);
  }
  dapp::native_invocation_result invoke(uint32_t                            lane,
                                        const ocudu_dapp_receiver_input_v1& input,
                                        ocudu_dapp_receiver_output_v1&      output) noexcept override
  {
    return primary.invoke(lane, input, output);
  }

  target_spy       primary{2};
  target_spy       first{2};
  target_spy       second{2};
  target_spy       stock{2};
  mutable unsigned lookups{};
  mutable uint32_t last_cell{};
};

} // namespace

TEST(dapp_demodulator, each_grant_is_served_by_the_instance_its_ue_is_routed_to)
{
  auto target      = std::make_shared<routing_target>();
  auto fallback    = std::make_shared<fallback_state>();
  auto demodulator = create_pusch_dapp_demodulator_factory(
                         std::make_shared<fallback_factory>(fallback),
                         target,
                         create_pseudo_random_generator_sw_factory(),
                         create_demodulation_mapper_factory(),
                         {.nof_lanes = 2, .max_nof_prb = 1, .max_nof_rx_ports = 1, .max_nof_layers = 1})
                         ->create();
  ASSERT_NE(demodulator, nullptr);

  const auto grant = [&](uint16_t rnti) {
    auto config = make_config();
    config.rnti = to_rnti(rnti);
    codeword_spy codeword;
    notifier_spy notifier;
    run(*demodulator, config, codeword, notifier);
    EXPECT_EQ(notifier.end_calls, 1U);
  };

  grant(0x4601);
  grant(0x4602);
  grant(0x4602);
  grant(0x4603);
  grant(0x1234);

  EXPECT_EQ(target->lookups, 5U);
  EXPECT_EQ(target->last_cell, 7U) << "the grant's cell is part of the lookup";
  EXPECT_EQ(target->first.receiver_calls, 1U);
  EXPECT_EQ(target->second.receiver_calls, 2U);
  EXPECT_EQ(target->primary.receiver_calls, 1U) << "an unlisted UE keeps the primary receiver";
  EXPECT_EQ(target->stock.receiver_calls, 0U);
  EXPECT_EQ(fallback->calls, 1U) << "the stock-receiver route takes the conventional demodulator";
  EXPECT_EQ(target->first.pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_RNTI_V1], 0x4601U);
  EXPECT_EQ(target->second.pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_RNTI_V1], 0x4602U);
}

TEST(dapp_demodulator, native_routed_target_views_each_slot_of_each_hook)
{
  const auto point = [](uint32_t interface_id, uint32_t first_worker) {
    auto created = dapp::native_hook_point::create(interface_id, first_worker, 2);
    EXPECT_TRUE(created) << created.detail;
    return std::shared_ptr<dapp::native_hook_point>(std::move(created.point));
  };
  auto table = std::make_shared<dapp::pusch_route_table>(2);
  auto equalizers =
      std::vector{point(OCUDU_DAPP_INTERFACE_EQUALIZER_V1, 40), point(OCUDU_DAPP_INTERFACE_EQUALIZER_V1, 50)};
  auto receivers =
      std::vector{point(OCUDU_DAPP_INTERFACE_RECEIVER_V1, 80), point(OCUDU_DAPP_INTERFACE_RECEIVER_V1, 90)};

  EXPECT_EQ(create_native_pusch_dapp_demodulator_target(equalizers, {receivers.front()}, table), nullptr)
      << "every slot needs both hooks";
  EXPECT_EQ(
      create_native_pusch_dapp_demodulator_target(equalizers, receivers, std::make_shared<dapp::pusch_route_table>(1)),
      nullptr)
      << "the table bounds the slots";
  EXPECT_EQ(create_native_pusch_dapp_demodulator_target(receivers, equalizers, table), nullptr);
  EXPECT_EQ(create_native_pusch_dapp_demodulator_target(equalizers, receivers, nullptr), nullptr);

  auto target = create_native_pusch_dapp_demodulator_target(equalizers, receivers, table);
  ASSERT_NE(target, nullptr);
  EXPECT_EQ(target->lane_count(), 2U);
  EXPECT_EQ(target->resolve_route(0, 0x4601), dapp::pusch_route::primary()) << "an empty table routes nobody";
  EXPECT_EQ(target->active_mode(), pusch_dapp_demodulator_mode::inactive);

  using dapp::pusch_route;
  using dapp::pusch_route_hook;
  EXPECT_EQ(target->route(pusch_route::primary()).worker_id(1, pusch_dapp_demodulator_mode::receiver), 81U);
  EXPECT_EQ(target->route(pusch_route::primary()).worker_id(1, pusch_dapp_demodulator_mode::equalizer), 41U);
  EXPECT_EQ(target->route(pusch_route::instance(pusch_route_hook::receiver, 1))
                .worker_id(1, pusch_dapp_demodulator_mode::receiver),
            91U);
  EXPECT_EQ(target->route(pusch_route::instance(pusch_route_hook::equalizer, 1))
                .worker_id(0, pusch_dapp_demodulator_mode::equalizer),
            50U);
  auto& stock = target->route(pusch_route::conventional());
  EXPECT_EQ(stock.active_mode(), pusch_dapp_demodulator_mode::inactive);
  EXPECT_EQ(stock.lane_count(), 2U);
  EXPECT_EQ(&target->route(pusch_route::instance(pusch_route_hook::channel_estimator, 0)), &stock)
      << "a channel-estimator route keeps the conventional demodulator";
  EXPECT_EQ(&target->route(pusch_route::instance(pusch_route_hook::receiver, 7)), &stock) << "unknown slot";

  ocudu_dapp_receiver_input_v1  input{};
  ocudu_dapp_receiver_output_v1 output{};
  EXPECT_EQ(stock.invoke(0, input, output).status, OCUDU_DAPP_BYPASS_V1);
}
