// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "../support/resource_grid_test_doubles.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/phy/upper/dapp/pusch_dapp_channel_estimator.h"
#include "ocudu/phy/upper/dapp/pusch_dapp_demodulator.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_data_router.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/signal_processors/pusch/factories.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
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

struct fallback_state {
  unsigned calls{};
};

class fallback_estimator final : public dmrs_pusch_estimator
{
public:
  explicit fallback_estimator(std::shared_ptr<fallback_state> state_) : state(std::move(state_)) {}

  void estimate(dmrs_pusch_estimator_notifier& notifier, const resource_grid_reader&, const configuration&) override
  {
    ++state->calls;
    notifier.on_estimation_complete(results);
  }

private:
  std::shared_ptr<fallback_state> state;
  empty_results                   results;
};

class fallback_factory final : public dmrs_pusch_estimator_factory
{
public:
  explicit fallback_factory(std::shared_ptr<fallback_state> state_) : state(std::move(state_)) {}

  std::unique_ptr<dmrs_pusch_estimator> create() override { return std::make_unique<fallback_estimator>(state); }

private:
  std::shared_ptr<fallback_state> state;
};

class target_spy final : public pusch_dapp_channel_estimator_target
{
public:
  explicit target_spy(uint32_t lanes_) : lanes(lanes_) {}

  bool     active() const noexcept override { return enabled; }
  uint32_t lane_count() const noexcept override { return lanes; }
  uint32_t worker_id(uint32_t lane) const noexcept override { return 100 + lane; }

  dapp::native_invocation_result invoke(uint32_t                                     lane,
                                        const ocudu_dapp_channel_estimator_input_v1& input,
                                        ocudu_dapp_channel_estimator_output_v1&      output) noexcept override
  {
    ++calls;
    invoked_lanes.push_back(lane);
    invocation                    = input.invocation;
    const auto* pusch_source      = static_cast<const ocudu_dapp_pusch_metadata_v1*>(input.pusch.metadata.data);
    const auto* dmrs_source       = static_cast<const ocudu_dapp_dmrs_metadata_v1*>(input.dmrs.metadata.data);
    pusch                         = *pusch_source;
    dmrs                          = *dmrs_source;
    const auto* coordinate_source = reinterpret_cast<const uint16_t*>(dmrs.coordinates.native_handle);
    coordinates.assign(coordinate_source, coordinate_source + 2 * dmrs.nof_pilots);
    const auto* reference_source = reinterpret_cast<const cf_t*>(dmrs.reference_symbols.native_handle);
    references.assign(reference_source, reference_source + pusch.nof_layers * dmrs.nof_pilots);
    const auto* grid_source = reinterpret_cast<const cf_t*>(input.rx_grid.native_handle);
    first_grid_value        = *grid_source;

    if (status != OCUDU_DAPP_OK_V1) {
      return {status, true};
    }
    const size_t channel_count = output.channel_estimates.byte_length / sizeof(cf_t);
    auto*        channel       = reinterpret_cast<cf_t*>(output.channel_estimates.native_handle);
    std::fill_n(channel, channel_count, cf_t{2.0F, 1.0F});
    const size_t noise_count = output.noise_variances.byte_length / sizeof(float);
    auto*        noise       = reinterpret_cast<float*>(output.noise_variances.native_handle);
    std::fill_n(noise, noise_count, 0.25F);
    if (non_finite_output) {
      channel[0] = {std::numeric_limits<float>::quiet_NaN(), 0.0F};
    }
    if (replace_output) {
      ++output.channel_estimates.allocation_id;
    }
    return {OCUDU_DAPP_OK_V1, false};
  }

  void report_invalid_output(uint32_t lane, const ocudu_dapp_invocation_v1&) noexcept override
  {
    invalid_output_lanes.push_back(lane);
  }

  void report_fallback(uint32_t lane, dapp::class_a_fallback_reason reason) noexcept override
  {
    fallback_reasons.emplace_back(lane, reason);
  }

  bool                                                            enabled = true;
  bool                                                            non_finite_output{};
  bool                                                            replace_output{};
  ocudu_dapp_status_v1                                            status{OCUDU_DAPP_OK_V1};
  unsigned                                                        calls{};
  std::vector<uint32_t>                                           invoked_lanes;
  std::vector<uint32_t>                                           invalid_output_lanes;
  std::vector<std::pair<uint32_t, dapp::class_a_fallback_reason>> fallback_reasons;
  ocudu_dapp_invocation_v1                                        invocation{};
  ocudu_dapp_pusch_metadata_v1                                    pusch{};
  ocudu_dapp_dmrs_metadata_v1                                     dmrs{};
  std::vector<uint16_t>                                           coordinates;
  std::vector<cf_t>                                               references;
  cf_t                                                            first_grid_value{};

private:
  uint32_t lanes;
};

class demodulator_mode_spy final : public pusch_dapp_demodulator_target
{
public:
  pusch_dapp_demodulator_mode active_mode() const noexcept override { return mode; }
  uint32_t                    lane_count() const noexcept override { return 2; }
  uint32_t worker_id(uint32_t lane, pusch_dapp_demodulator_mode) const noexcept override { return lane; }
  dapp::native_invocation_result
  invoke(uint32_t, const ocudu_dapp_equalizer_input_v1&, ocudu_dapp_equalizer_output_v1&) noexcept override
  {
    return {OCUDU_DAPP_BYPASS_V1, true};
  }
  dapp::native_invocation_result
  invoke(uint32_t, const ocudu_dapp_receiver_input_v1&, ocudu_dapp_receiver_output_v1&) noexcept override
  {
    return {OCUDU_DAPP_BYPASS_V1, true};
  }

  pusch_dapp_demodulator_mode mode{pusch_dapp_demodulator_mode::receiver};
};

class notifier_spy final : public dmrs_pusch_estimator_notifier
{
public:
  void on_estimation_complete(const dmrs_pusch_estimator_results& value) override
  {
    ++calls;
    results = &value;
  }

  unsigned                            calls{};
  const dmrs_pusch_estimator_results* results{};
};

dmrs_pusch_estimator::configuration make_config(unsigned nof_layers = 1)
{
  dmrs_pusch_estimator::configuration config{};
  config.cell_id         = 7;
  config.slot            = slot_point{1, 22};
  config.sequence_config = dmrs_pusch_estimator::pseudo_random_sequence_configuration{
      .type = dmrs_config_type::type1, .nof_tx_layers = nof_layers, .scrambling_id = 19, .n_scid = false};
  config.scaling      = std::sqrt(2.0F);
  config.symbols_mask = bounded_bitset<MAX_NSYMB_PER_SLOT>(MAX_NSYMB_PER_SLOT);
  config.symbols_mask.set(2);
  config.rb_mask = crb_bitmap(1);
  config.rb_mask.set(0);
  config.first_symbol = 2;
  config.nof_symbols  = 2;
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

std::shared_ptr<dmrs_pusch_estimator_factory> make_factory(const std::shared_ptr<target_spy>&     target,
                                                           const std::shared_ptr<fallback_state>& fallback,
                                                           unsigned                               lanes      = 2,
                                                           unsigned                               max_layers = 1)
{
  return create_pusch_dapp_channel_estimator_factory(
      std::make_shared<fallback_factory>(fallback),
      target,
      create_pseudo_random_generator_sw_factory(),
      create_low_papr_sequence_generator_sw_factory(),
      {.nof_lanes = lanes, .max_nof_prb = 1, .max_nof_rx_ports = 1, .max_nof_layers = max_layers});
}

} // namespace

TEST(dapp_channel_estimator, publishes_compact_metadata_and_commits_caller_owned_results)
{
  auto         target    = std::make_shared<target_spy>(2);
  auto         fallback  = std::make_shared<fallback_state>();
  auto         factory   = make_factory(target, fallback);
  auto         estimator = factory->create();
  auto         grid      = make_grid();
  const auto   config    = make_config();
  notifier_spy notify;

  estimator->estimate(notify, grid, config);

  ASSERT_EQ(notify.calls, 1U);
  ASSERT_NE(notify.results, nullptr);
  EXPECT_EQ(fallback->calls, 0U);
  EXPECT_EQ(target->calls, 1U);
  EXPECT_EQ(target->invoked_lanes, std::vector<uint32_t>{0});
  EXPECT_EQ(target->invocation.cell_id, config.cell_id);
  EXPECT_EQ(target->invocation.worker_id, 100U);
  EXPECT_EQ(target->invocation.radio_time.sfn, config.slot.sfn());
  EXPECT_EQ(target->invocation.radio_time.slot, config.slot.slot_index());
  EXPECT_EQ(target->pusch.nof_rx_ports, 1U);
  EXPECT_EQ(target->pusch.nof_layers, 1U);
  EXPECT_EQ(target->pusch.start_symbol, 2U);
  EXPECT_EQ(target->pusch.nof_symbols, 2U);
  EXPECT_EQ(target->pusch.start_subcarrier, 0U);
  EXPECT_EQ(target->pusch.nof_subcarriers, NOF_SUBCARRIERS_PER_RB);
  EXPECT_EQ(target->pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_SUBCARRIER_SPACING_HZ_V1], 30000U);
  EXPECT_EQ(target->pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_GRID_NOF_SUBCARRIERS_V1],
            NOF_SUBCARRIERS_PER_RB);
  EXPECT_EQ(target->pusch.flags, 0U);
  EXPECT_EQ(target->dmrs.nof_pilots, 6U);
  EXPECT_EQ(target->dmrs.flags,
            OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1);
  EXPECT_EQ(target->coordinates, (std::vector<uint16_t>{0, 0, 0, 2, 0, 4, 0, 6, 0, 8, 0, 10}));
  ASSERT_EQ(target->references.size(), 6U);
  for (cf_t reference : target->references) {
    EXPECT_NEAR(std::abs(reference), config.scaling, 1.0e-5F);
  }
  EXPECT_EQ(target->first_grid_value, cf_t(200.0F, 0.0F));
  EXPECT_FLOAT_EQ(notify.results->get_noise_variance(0), 0.25F);
  EXPECT_NEAR(notify.results->get_rsrp(0), 5.0F, 0.05F);

  std::array<cbf16_t, NOF_SUBCARRIERS_PER_RB> channel{};
  notify.results->get_symbol_ch_estimate(channel, 2, 0, 0);
  for (cbf16_t value : channel) {
    EXPECT_NEAR(to_cf(value).real(), 2.0F, 0.02F);
    EXPECT_NEAR(to_cf(value).imag(), 1.0F, 0.02F);
  }
}

TEST(dapp_channel_estimator, assigns_each_dependency_a_unique_lane_and_never_wraps)
{
  auto target   = std::make_shared<target_spy>(2);
  auto fallback = std::make_shared<fallback_state>();
  auto factory  = make_factory(target, fallback);
  auto first    = factory->create();
  auto second   = factory->create();
  EXPECT_EQ(factory->create(), nullptr);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  auto         grid   = make_grid();
  const auto   config = make_config();
  notifier_spy first_notify;
  notifier_spy second_notify;
  first->estimate(first_notify, grid, config);
  second->estimate(second_notify, grid, config);
  EXPECT_EQ(target->invoked_lanes, (std::vector<uint32_t>{0, 1}));
}

TEST(dapp_channel_estimator, falls_back_on_inactive_failed_or_untrusted_output)
{
  auto       target   = std::make_shared<target_spy>(4);
  auto       fallback = std::make_shared<fallback_state>();
  auto       factory  = make_factory(target, fallback, 4);
  auto       grid     = make_grid();
  const auto config   = make_config();

  target->enabled = false;
  notifier_spy inactive_notify;
  factory->create()->estimate(inactive_notify, grid, config);
  EXPECT_EQ(target->calls, 0U);
  EXPECT_EQ(fallback->calls, 1U);

  target->enabled = true;
  target->status  = OCUDU_DAPP_EXCEPTION_V1;
  notifier_spy failed_notify;
  factory->create()->estimate(failed_notify, grid, config);
  EXPECT_EQ(fallback->calls, 2U);

  target->status            = OCUDU_DAPP_OK_V1;
  target->non_finite_output = true;
  notifier_spy non_finite_notify;
  factory->create()->estimate(non_finite_notify, grid, config);
  EXPECT_EQ(fallback->calls, 3U);

  target->non_finite_output = false;
  target->replace_output    = true;
  notifier_spy replaced_notify;
  factory->create()->estimate(replaced_notify, grid, config);
  EXPECT_EQ(fallback->calls, 4U);
  EXPECT_EQ(target->invalid_output_lanes, (std::vector<uint32_t>{2, 3}));
}

TEST(dapp_channel_estimator, initial_conventional_reference_falls_back_for_multi_layer_grants)
{
  auto         target    = std::make_shared<target_spy>(1);
  auto         fallback  = std::make_shared<fallback_state>();
  auto         factory   = make_factory(target, fallback, 1, 2);
  auto         estimator = factory->create();
  auto         grid      = make_grid();
  const auto   config    = make_config(2);
  notifier_spy notify;

  estimator->estimate(notify, grid, config);

  EXPECT_EQ(target->calls, 0U);
  EXPECT_EQ(fallback->calls, 1U);
  EXPECT_EQ(notify.calls, 1U);
  EXPECT_EQ(target->fallback_reasons,
            (std::vector<std::pair<uint32_t, dapp::class_a_fallback_reason>>{
                {0, dapp::class_a_fallback_reason::unsupported_layers}}));
}

TEST(dapp_channel_estimator, hook_provider_can_host_inline_hooks_without_a_class_c_router)
{
  auto target   = std::make_shared<target_spy>(2);
  auto provider = create_upper_phy_dapp_hook_provider(nullptr, target);
  ASSERT_NE(provider, nullptr);
  auto fallback  = std::make_shared<fallback_factory>(std::make_shared<fallback_state>());
  auto decorated = provider->decorate_pusch_channel_estimator(
      std::move(fallback), {.nof_lanes = 2, .max_nof_prb = 1, .max_nof_rx_ports = 1, .max_nof_layers = 1});
  EXPECT_NE(decorated, nullptr);
}

TEST(dapp_channel_estimator, deeper_inline_hook_suppresses_the_overlapping_ce_hook)
{
  auto channel_target = std::make_shared<target_spy>(2);
  auto demod_target   = std::make_shared<demodulator_mode_spy>();
  auto provider       = create_upper_phy_dapp_hook_provider(nullptr, channel_target, demod_target);
  auto fallback       = std::make_shared<fallback_state>();
  auto factory        = provider->decorate_pusch_channel_estimator(
      std::make_shared<fallback_factory>(fallback),
      {.nof_lanes = 2, .max_nof_prb = 1, .max_nof_rx_ports = 1, .max_nof_layers = 1});
  auto grid   = make_grid();
  auto config = make_config();

  notifier_spy suppressed;
  factory->create()->estimate(suppressed, grid, config);
  EXPECT_EQ(channel_target->calls, 0U);
  EXPECT_EQ(fallback->calls, 1U);

  demod_target->mode = pusch_dapp_demodulator_mode::inactive;
  notifier_spy active;
  factory->create()->estimate(active, grid, config);
  EXPECT_EQ(channel_target->calls, 1U);
  EXPECT_EQ(fallback->calls, 1U);
}

TEST(dapp_channel_estimator, native_target_preserves_the_preallocated_hook_lane_identity)
{
  auto point = dapp::native_hook_point::create(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 40, 2);
  ASSERT_TRUE(point);
  auto shared_point = std::shared_ptr<dapp::native_hook_point>(std::move(point.point));
  auto target       = create_native_pusch_dapp_channel_estimator_target(shared_point);
  ASSERT_NE(target, nullptr);
  EXPECT_FALSE(target->active());
  EXPECT_EQ(target->lane_count(), 2U);
  EXPECT_EQ(target->worker_id(0), 40U);
  EXPECT_EQ(target->worker_id(1), 41U);

  auto wrong_interface = dapp::native_hook_point::create(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 0, 1);
  ASSERT_TRUE(wrong_interface);
  EXPECT_EQ(create_native_pusch_dapp_channel_estimator_target(
                std::shared_ptr<dapp::native_hook_point>(std::move(wrong_interface.point))),
            nullptr);
}
