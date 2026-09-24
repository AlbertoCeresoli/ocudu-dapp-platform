// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "srs_dapp_test_occasions.h"
#include "ocudu/phy/upper/dapp/srs_dapp_channel_estimator.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_data_router.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_hook_provider.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_factory.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include <gtest/gtest.h>
#include <limits>

using namespace ocudu;
using namespace ocudu::srs_dapp_test;

namespace {

constexpr float not_provided = std::numeric_limits<float>::quiet_NaN();

/// Records every invocation and answers with scripted outputs.
class scripted_target final : public srs_dapp_channel_estimator_target
{
public:
  bool     active() const noexcept override { return is_active; }
  uint32_t lane_count() const noexcept override { return 2; }
  uint32_t worker_id(uint32_t lane) const noexcept override { return 10 + lane; }
  uint32_t backend_id() const noexcept override { return OCUDU_DAPP_BACKEND_HOST_V1; }

  dapp::native_invocation_result invoke(uint32_t                                         lane,
                                        const ocudu_dapp_srs_channel_estimator_input_v1& input,
                                        ocudu_dapp_srs_channel_estimator_output_v1&      output) noexcept override
  {
    ++invocations;
    last_lane      = lane;
    metadata       = *static_cast<const ocudu_dapp_srs_channel_estimator_metadata_v1*>(input.srs.metadata.data);
    rx_tensor      = input.rx_symbols;
    const auto* rx = reinterpret_cast<const cf_t*>(static_cast<uintptr_t>(input.rx_symbols.native_handle));
    rx_symbols.assign(rx, rx + input.rx_symbols.byte_length / sizeof(cf_t));
    const auto* ref = reinterpret_cast<const cf_t*>(static_cast<uintptr_t>(input.reference_symbols.native_handle));
    references.assign(ref, ref + input.reference_symbols.byte_length / sizeof(cf_t));
    if (status != OCUDU_DAPP_OK_V1) {
      return {status, true};
    }
    auto* channel = reinterpret_cast<cf_t*>(static_cast<uintptr_t>(output.channel_matrix.native_handle));
    for (unsigned index = 0; index != output.channel_matrix.byte_length / sizeof(cf_t); ++index) {
      channel[index] = coefficient + cf_t(static_cast<float>(index), 0.0F);
    }
    auto* values = reinterpret_cast<float*>(static_cast<uintptr_t>(output.measurements.native_handle));
    std::copy(measurements.begin(), measurements.end(), values);
    return {OCUDU_DAPP_OK_V1, false};
  }

  void report_completion_deadline(uint32_t, const ocudu_dapp_invocation_v1&, uint64_t, uint64_t) noexcept override {}
  void report_invalid_output(uint32_t, const ocudu_dapp_invocation_v1&) noexcept override { ++invalid_outputs; }
  void report_fallback(uint32_t, dapp::class_a_fallback_reason reason) noexcept override
  {
    ++fallbacks;
    last_reason = reason;
  }

  bool                 is_active = true;
  ocudu_dapp_status_v1 status    = OCUDU_DAPP_OK_V1;
  cf_t                 coefficient{1.0F, 0.5F};
  std::array<float, OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1>
                                measurements{0.01F, -3.0F, -6.0F, 1.0e-7F, 3.0e-8F, -1.0e-6F, 1.0e-6F, not_provided};
  unsigned                      invocations{};
  unsigned                      invalid_outputs{};
  unsigned                      fallbacks{};
  uint32_t                      last_lane{};
  dapp::class_a_fallback_reason last_reason{};
  ocudu_dapp_srs_channel_estimator_metadata_v1 metadata{};
  ocudu_dapp_tensor_view_v1                    rx_tensor{};
  std::vector<cf_t>                            rx_symbols;
  std::vector<cf_t>                            references;
};

/// Host estimator stand-in with a recognisable result.
class fallback_estimator final : public srs_estimator
{
public:
  explicit fallback_estimator(unsigned& calls_) : calls(calls_) {}

  srs_estimator_result estimate(const resource_grid_reader&, const srs_estimator_configuration& config) override
  {
    ++calls;
    srs_estimator_result result;
    result.channel_matrix =
        srs_channel_matrix(config.ports.size(), static_cast<unsigned>(config.resource.nof_antenna_ports));
    result.noise_variance = 42.0F;
    return result;
  }

private:
  unsigned& calls;
};

class fallback_factory final : public srs_estimator_factory
{
public:
  std::unique_ptr<srs_estimator> create() override { return std::make_unique<fallback_estimator>(calls); }
  std::unique_ptr<srs_estimator_configuration_validator> create_validator() override { return nullptr; }

  unsigned calls{};
};

struct adapter_fixture {
  explicit adapter_fixture(occasion_profile profile_ = {}) : profile(profile_)
  {
    config = make_configuration(profile);
    grid   = make_grid(profile);
    EXPECT_TRUE(write_occasion(*grid, profile, config));
    factory = create_srs_dapp_channel_estimator_factory(
        fallback, target, create_low_papr_sequence_generator_sw_factory(), {.nof_lanes = 2, .max_nof_prb = 52});
    EXPECT_NE(factory, nullptr);
    estimator = factory->create();
    EXPECT_NE(estimator, nullptr);
  }

  srs_estimator_result estimate() { return estimator->estimate(grid->get_reader(), config); }

  occasion_profile                       profile;
  srs_estimator_configuration            config;
  std::unique_ptr<resource_grid>         grid;
  std::shared_ptr<scripted_target>       target   = std::make_shared<scripted_target>();
  std::shared_ptr<fallback_factory>      fallback = std::make_shared<fallback_factory>();
  std::shared_ptr<srs_estimator_factory> factory;
  std::unique_ptr<srs_estimator>         estimator;
};

TEST(srs_dapp_channel_estimator, hands_the_module_the_srs_resource_elements_and_reference_sequences)
{
  adapter_fixture test({.nof_rx_ports = 2, .nof_tx_ports = 2, .nof_symbols = srs_nof_symbols::n2, .start_symbol = 12});
  test.estimate();
  ASSERT_EQ(test.target->invocations, 1U);
  EXPECT_EQ(test.target->last_lane, 0U);

  const auto&           srs  = test.target->metadata;
  const srs_information info = get_srs_information(test.config.resource, 0);
  EXPECT_EQ(srs.nof_rx_ports, 2U);
  EXPECT_EQ(srs.nof_tx_ports, 2U);
  EXPECT_EQ(srs.start_symbol, 12U);
  EXPECT_EQ(srs.nof_symbols, 2U);
  EXPECT_EQ(srs.sequence_length, info.sequence_length);
  EXPECT_EQ(srs.comb_size, 4U);
  EXPECT_EQ(srs.n_cs_max, 12U);
  EXPECT_EQ(srs.nof_pilot_sets, 1U);
  EXPECT_EQ(srs.flags, 0U);
  EXPECT_EQ(srs.first_subcarrier[0], info.mapping_initial_subcarrier);
  EXPECT_EQ(srs.subcarrier_spacing_hz, 30000U);
  EXPECT_EQ(srs.numerology, 1U);
  EXPECT_EQ(srs.grid_nof_subcarriers, 52U * NOF_SUBCARRIERS_PER_RB);
  EXPECT_EQ(srs.rnti, 0x4601U);
  EXPECT_DOUBLE_EQ(srs.max_time_alignment_s, 1.0 / (12.0 * 30000.0 * 4.0));
  EXPECT_EQ(test.target->rx_tensor.layout_id, OCUDU_DAPP_LAYOUT_SRS_RX_SET_PORT_SYMBOL_PILOT_CF32_V1);

  // rx_symbols[0][rx][symbol][n] is the grid at the comb subcarriers of the pilot set.
  const auto&       reader = test.grid->get_reader();
  std::vector<cf_t> expected(info.sequence_length);
  for (unsigned rx = 0; rx != 2; ++rx) {
    for (unsigned symbol = 0; symbol != 2; ++symbol) {
      reader.get(expected, rx, 12 + symbol, info.mapping_initial_subcarrier, info.comb_size);
      for (unsigned n = 0; n != info.sequence_length; ++n) {
        ASSERT_EQ(test.target->rx_symbols[(rx * 2 + symbol) * info.sequence_length + n], expected[n]);
      }
    }
  }
  // reference_symbols[tx] is the low-PAPR sequence with the port's cyclic shift.
  auto generator = create_low_papr_sequence_generator_sw_factory()->create();
  for (unsigned tx = 0; tx != 2; ++tx) {
    const srs_information port = get_srs_information(test.config.resource, tx);
    EXPECT_EQ(srs.cyclic_shift[tx], port.n_cs);
    generator->generate(expected, port.sequence_group, port.sequence_number, port.n_cs, port.n_cs_max);
    for (unsigned n = 0; n != info.sequence_length; ++n) {
      ASSERT_EQ(test.target->references[tx * info.sequence_length + n], expected[n]);
    }
  }
}

TEST(srs_dapp_channel_estimator, interleaved_pilots_give_the_odd_ports_their_own_pilot_set)
{
  // Four ports with a cyclic shift in the upper half of the comb-4 range interleave their pilots.
  adapter_fixture test({.nof_rx_ports = 1, .nof_tx_ports = 4, .cyclic_shift = 8});
  test.estimate();
  ASSERT_EQ(test.target->invocations, 1U);
  const auto& srs = test.target->metadata;
  EXPECT_EQ(srs.flags, OCUDU_DAPP_SRS_FLAG_INTERLEAVED_PILOTS_V1);
  EXPECT_EQ(srs.nof_pilot_sets, 2U);
  for (unsigned tx = 0; tx != 4; ++tx) {
    EXPECT_EQ(srs.pilot_set[tx], tx % 2);
  }
  EXPECT_EQ(srs.first_subcarrier[0], get_srs_information(test.config.resource, 0).mapping_initial_subcarrier);
  EXPECT_EQ(srs.first_subcarrier[1], get_srs_information(test.config.resource, 1).mapping_initial_subcarrier);
  EXPECT_NE(srs.first_subcarrier[0], srs.first_subcarrier[1]);
  EXPECT_EQ(test.target->rx_tensor.dimensions[0], 2U);
}

TEST(srs_dapp_channel_estimator, maps_the_module_outputs_to_the_estimator_result)
{
  adapter_fixture test({.nof_rx_ports = 2, .nof_tx_ports = 1});
  const auto      result = test.estimate();
  EXPECT_EQ(test.fallback->calls, 0U);
  ASSERT_EQ(result.channel_matrix.get_nof_rx_ports(), 2U);
  ASSERT_EQ(result.channel_matrix.get_nof_tx_ports(), 1U);
  EXPECT_EQ(result.channel_matrix.get_coefficient(0, 0), cf_t(1.0F, 0.5F));
  EXPECT_EQ(result.channel_matrix.get_coefficient(1, 0), cf_t(2.0F, 0.5F));
  EXPECT_FLOAT_EQ(result.noise_variance.value(), 0.01F);
  EXPECT_FLOAT_EQ(result.epre_dB.value(), -3.0F);
  EXPECT_FLOAT_EQ(result.rsrp_dB.value(), -6.0F);
  EXPECT_NEAR(result.time_alignment.time_alignment, 1.0e-7, 1e-12);
  EXPECT_NEAR(result.time_alignment.resolution, 3.0e-8, 1e-12);
  EXPECT_NEAR(result.time_alignment.min, -1.0e-6, 1e-12);
  EXPECT_NEAR(result.time_alignment.max, 1.0e-6, 1e-12);
}

TEST(srs_dapp_channel_estimator, measurements_the_module_does_not_provide_are_absent)
{
  adapter_fixture test;
  test.target->measurements.fill(not_provided);
  const auto result = test.estimate();
  EXPECT_EQ(test.fallback->calls, 0U);
  EXPECT_FALSE(result.noise_variance.has_value());
  EXPECT_FALSE(result.epre_dB.has_value());
  EXPECT_FALSE(result.rsrp_dB.has_value());
  EXPECT_EQ(result.time_alignment.time_alignment, 0.0);
  EXPECT_EQ(result.time_alignment.max, std::numeric_limits<double>::max());
}

TEST(srs_dapp_channel_estimator, an_invalid_output_falls_back_to_the_host_estimator)
{
  for (const auto corrupt : {+[](scripted_target& target) { target.coefficient = {not_provided, 0.0F}; },
                             +[](scripted_target& target) { target.measurements[0] = -1.0F; },
                             +[](scripted_target& target) { target.measurements[2] = INFINITY; },
                             +[](scripted_target& target) { target.measurements[4] = not_provided; },
                             +[](scripted_target& target) {
                               target.measurements[5] = 1.0F;
                               target.measurements[6] = -1.0F;
                             }}) {
    adapter_fixture test;
    corrupt(*test.target);
    const auto result = test.estimate();
    EXPECT_EQ(test.target->invalid_outputs, 1U);
    EXPECT_EQ(test.fallback->calls, 1U);
    EXPECT_FLOAT_EQ(result.noise_variance.value(), 42.0F);
  }
}

TEST(srs_dapp_channel_estimator, a_declined_invocation_or_an_inactive_hook_keeps_the_host_estimator)
{
  adapter_fixture declined;
  declined.target->status = OCUDU_DAPP_UNSUPPORTED_V1;
  EXPECT_FLOAT_EQ(declined.estimate().noise_variance.value(), 42.0F);
  EXPECT_EQ(declined.target->invocations, 1U);
  EXPECT_EQ(declined.fallback->calls, 1U);

  adapter_fixture inactive;
  inactive.target->is_active = false;
  EXPECT_FLOAT_EQ(inactive.estimate().noise_variance.value(), 42.0F);
  EXPECT_EQ(inactive.target->invocations, 0U);
  EXPECT_EQ(inactive.target->fallbacks, 0U);
}

TEST(srs_dapp_channel_estimator, an_occasion_outside_the_workspace_is_reported_and_kept_on_the_host)
{
  // A 272 PRB sounding bandwidth on a 273 PRB grid does not fit the 52 PRB workspace.
  adapter_fixture test({.configuration_index = 63, .grid_nof_prb = 273});
  EXPECT_FLOAT_EQ(test.estimate().noise_variance.value(), 42.0F);
  EXPECT_EQ(test.target->invocations, 0U);
  EXPECT_EQ(test.target->fallbacks, 1U);
  EXPECT_EQ(test.target->last_reason, dapp::class_a_fallback_reason::unsupported_allocation);
}

TEST(srs_dapp_channel_estimator, estimators_beyond_the_hook_lanes_keep_the_host_estimator)
{
  adapter_fixture test;
  auto            second = test.factory->create();
  auto            third  = test.factory->create();
  ASSERT_NE(second, nullptr);
  ASSERT_NE(third, nullptr);
  second->estimate(test.grid->get_reader(), test.config);
  EXPECT_EQ(test.target->last_lane, 1U);
  third->estimate(test.grid->get_reader(), test.config);
  EXPECT_EQ(test.target->invocations, 1U);
  EXPECT_EQ(test.fallback->calls, 1U);
}

/// Records the SRS publications the Class-C route sees.
class srs_router_spy final : public upper_phy_dapp_data_router
{
public:
  bool is_enabled(uint32_t, upper_phy_dapp_producer_context) const noexcept override { return true; }
  void publish_spectrum(upper_phy_dapp_producer_context,
                        const resource_grid_reader&,
                        slot_point,
                        upper_phy_dapp_expected_ul_plan) noexcept override
  {
  }
  void publish_srs_isac(upper_phy_dapp_producer_context,
                        const resource_grid_reader&,
                        const srs_estimator_configuration&,
                        const srs_estimator_result& result) noexcept override
  {
    published = result.noise_variance;
  }

  std::optional<float> published;
};

TEST(srs_dapp_channel_estimator, the_class_c_publication_observes_the_class_a_estimate)
{
  auto router   = std::make_shared<srs_router_spy>();
  auto target   = std::make_shared<scripted_target>();
  auto fallback = std::make_shared<fallback_factory>();
  auto provider = create_upper_phy_dapp_hook_provider(router, {}, {}, 2, target);
  ASSERT_NE(provider, nullptr);
  auto factory = provider->decorate_srs_estimator(fallback, 2, 52);
  ASSERT_NE(factory, nullptr);
  auto estimator = factory->create();
  ASSERT_NE(estimator, nullptr);

  const occasion_profile profile;
  const auto             config = make_configuration(profile);
  auto                   grid   = make_grid(profile);
  ASSERT_TRUE(write_occasion(*grid, profile, config));
  estimator->estimate(grid->get_reader(), config);
  ASSERT_TRUE(router->published.has_value());
  EXPECT_FLOAT_EQ(*router->published, 0.01F);

  target->is_active = false;
  estimator->estimate(grid->get_reader(), config);
  EXPECT_FLOAT_EQ(*router->published, 42.0F);
}

TEST(srs_dapp_channel_estimator, the_factory_rejects_an_unusable_configuration)
{
  auto target   = std::make_shared<scripted_target>();
  auto fallback = std::make_shared<fallback_factory>();
  auto low_papr = create_low_papr_sequence_generator_sw_factory();
  EXPECT_EQ(create_srs_dapp_channel_estimator_factory(nullptr, target, low_papr, {1, 52}), nullptr);
  EXPECT_EQ(create_srs_dapp_channel_estimator_factory(fallback, nullptr, low_papr, {1, 52}), nullptr);
  EXPECT_EQ(create_srs_dapp_channel_estimator_factory(fallback, target, low_papr, {3, 52}), nullptr);
  EXPECT_EQ(create_srs_dapp_channel_estimator_factory(fallback, target, low_papr, {1, 0}), nullptr);
  EXPECT_EQ(create_native_srs_dapp_channel_estimator_target(nullptr), nullptr);
}

} // namespace
