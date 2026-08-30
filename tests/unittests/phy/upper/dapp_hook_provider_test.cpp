// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "../support/resource_grid_test_doubles.h"
#include "phy_tap_test_doubles.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_data_router.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_configuration.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_factory.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include <algorithm>
#include <gtest/gtest.h>

using namespace ocudu;

namespace {

class router_spy final : public upper_phy_dapp_data_router
{
public:
  bool is_enabled(uint32_t interface_id, upper_phy_dapp_producer_context) const noexcept override
  {
    return interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 ? spectrum_enabled : srs_enabled;
  }

  void publish_spectrum(upper_phy_dapp_producer_context context,
                        const resource_grid_reader&     grid,
                        slot_point                      slot,
                        upper_phy_dapp_expected_ul_plan plan) noexcept override
  {
    ++spectrum_publications;
    last_context = context;
    last_grid    = &grid;
    last_slot    = slot;
    last_plan.assign(plan.cells.begin(), plan.cells.end());
    last_plan_symbols  = plan.nof_symbols;
    last_plan_prbs     = plan.nof_prbs;
    last_plan_complete = plan.complete;
  }

  void publish_srs_isac(upper_phy_dapp_producer_context    context,
                        const resource_grid_reader&        grid,
                        const srs_estimator_configuration& config,
                        const srs_estimator_result&        result) noexcept override
  {
    ++srs_publications;
    last_context        = context;
    last_grid           = &grid;
    last_slot           = config.slot;
    last_noise_variance = result.noise_variance;
  }

  bool                            spectrum_enabled = true;
  bool                            srs_enabled      = true;
  unsigned                        spectrum_publications{};
  unsigned                        srs_publications{};
  upper_phy_dapp_producer_context last_context{};
  const resource_grid_reader*     last_grid{};
  slot_point                      last_slot;
  std::optional<float>            last_noise_variance;
  std::vector<uint8_t>            last_plan;
  uint32_t                        last_plan_symbols{};
  uint32_t                        last_plan_prbs{};
  bool                            last_plan_complete{};
};

class estimator_spy final : public srs_estimator
{
public:
  srs_estimator_result estimate(const resource_grid_reader&, const srs_estimator_configuration& config) override
  {
    ++calls;
    last_config = config;
    srs_estimator_result result{};
    result.noise_variance = 0.25F;
    return result;
  }

  unsigned                                   calls{};
  std::optional<srs_estimator_configuration> last_config;
};

class validator_spy final : public srs_estimator_configuration_validator
{
public:
  error_type<std::string> is_valid(const srs_estimator_configuration&) const override { return default_success_t{}; }
};

class estimator_factory_spy final : public srs_estimator_factory
{
public:
  std::unique_ptr<srs_estimator> create() override
  {
    auto result    = std::make_unique<estimator_spy>();
    last_estimator = result.get();
    return result;
  }

  std::unique_ptr<srs_estimator_configuration_validator> create_validator() override
  {
    ++validator_creations;
    return std::make_unique<validator_spy>();
  }

  estimator_spy* last_estimator{};
  unsigned       validator_creations{};
};

srs_estimator_configuration make_srs_config()
{
  srs_estimator_configuration config{};
  config.resource = {.nof_antenna_ports   = srs_resource_configuration::one_two_four_enum::one,
                     .nof_symbols         = srs_nof_symbols::n1,
                     .start_symbol        = 13,
                     .configuration_index = 0,
                     .sequence_id         = 0,
                     .bandwidth_index     = 0,
                     .comb_size           = tx_comb_size::n2,
                     .comb_offset         = 0,
                     .cyclic_shift        = 0,
                     .freq_position       = 0,
                     .freq_shift          = 0,
                     .freq_hopping        = 0,
                     .hopping             = srs_group_or_sequence_hopping::neither,
                     .periodicity         = std::nullopt};
  return config;
}

} // namespace

TEST(dapp_hook_provider, spectrum_wrap_preserves_legacy_tap_and_publishes_only_a_complete_slot)
{
  auto router   = std::make_shared<router_spy>();
  auto provider = create_upper_phy_dapp_hook_provider(router);
  ASSERT_NE(provider, nullptr);

  auto  fallback     = std::make_unique<phy_tap_spy>();
  auto* fallback_spy = fallback.get();
  auto  tap          = provider->wrap_phy_tap({3, 7}, std::move(fallback));
  ASSERT_NE(tap, nullptr);

  resource_grid_reader_spy reader(1, 14, 1);
  resource_grid_writer_spy writer(1, 14, 1);
  const slot_point         slot{1, 22};
  tap->handle_ul_symbol(writer, reader, slot, 12, {}, {}, {}, {});
  EXPECT_EQ(router->spectrum_publications, 0U);
  tap->handle_ul_symbol(writer, reader, slot, 13, {}, {}, {}, {});

  EXPECT_EQ(fallback_spy->get_handle_ul_symbol_count(), 2U);
  EXPECT_EQ(router->spectrum_publications, 1U);
  EXPECT_EQ(router->last_context.cell_id, 3U);
  EXPECT_EQ(router->last_context.producer_shard, 7U);
  EXPECT_EQ(router->last_grid, &reader);
  EXPECT_EQ(router->last_slot, slot);
  EXPECT_FALSE(router->last_plan_complete);
}

TEST(dapp_hook_provider, spectrum_wrap_handles_quiet_slots_and_a_missing_legacy_tap)
{
  auto router   = std::make_shared<router_spy>();
  auto provider = create_upper_phy_dapp_hook_provider(router);
  auto tap      = provider->wrap_phy_tap({5, 9}, nullptr);
  ASSERT_NE(tap, nullptr);
  resource_grid_reader_spy reader(1, 14, 1);
  const slot_point         slot{0, 4};

  tap->handle_quiet_grid(reader, slot);
  EXPECT_EQ(router->spectrum_publications, 1U);
  EXPECT_EQ(router->last_context.cell_id, 5U);
  EXPECT_EQ(router->last_context.producer_shard, 9U);
  EXPECT_TRUE(router->last_plan_complete);
  EXPECT_EQ(router->last_plan_symbols, 14U);
  EXPECT_EQ(router->last_plan_prbs, 1U);
  EXPECT_TRUE(std::all_of(router->last_plan.begin(), router->last_plan.end(), [](uint8_t value) {
    return value == OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1;
  }));

  router->spectrum_enabled = false;
  tap->handle_quiet_grid(reader, slot);
  EXPECT_EQ(router->spectrum_publications, 1U);
}

TEST(dapp_hook_provider, spectrum_plan_accumulates_pusch_pucch_and_srs_that_end_on_different_symbols)
{
  auto router   = std::make_shared<router_spy>();
  auto provider = create_upper_phy_dapp_hook_provider(router);
  auto tap      = provider->wrap_phy_tap({1, 2}, nullptr);
  ASSERT_NE(tap, nullptr);
  resource_grid_reader_spy reader(1, 14, 4);
  resource_grid_writer_spy writer(1, 14, 4);
  const slot_point         slot{1, 9};

  uplink_pdu_slot_repository::pusch_pdu pusch{};
  pusch.pdu.bwp_start_rb       = 0;
  pusch.pdu.bwp_size_rb        = 4;
  pusch.pdu.freq_alloc         = rb_allocation::make_type1(1, 1);
  pusch.pdu.start_symbol_index = 0;
  pusch.pdu.nof_symbols        = 2;
  pusch.pdu.dmrs_symbol_mask.set(0);
  const std::array<uplink_pdu_slot_repository::pusch_pdu, 1> pusch_at_symbol_one{{pusch}};

  uplink_pdu_slot_repository::pucch_pdu  pucch{};
  pucch_processor::format0_configuration pucch_config{};
  pucch_config.bwp_start_rb       = 0;
  pucch_config.bwp_size_rb        = 4;
  pucch_config.starting_prb       = 0;
  pucch_config.second_hop_prb     = 1;
  pucch_config.start_symbol_index = 2;
  pucch_config.nof_symbols        = 2;
  pucch.config                    = pucch_config;
  const std::array<uplink_pdu_slot_repository::pucch_pdu, 1> pucch_at_symbol_three{{pucch}};

  uplink_pdu_slot_repository::srs_pdu srs{};
  srs.config = make_srs_config();
  const std::array<uplink_pdu_slot_repository::srs_pdu, 1> srs_at_symbol_thirteen{{srs}};

  for (unsigned symbol = 0; symbol != 14; ++symbol) {
    const span<const uplink_pdu_slot_repository::pusch_pdu> pdus =
        symbol == 1 ? span<const uplink_pdu_slot_repository::pusch_pdu>(pusch_at_symbol_one)
                    : span<const uplink_pdu_slot_repository::pusch_pdu>{};
    const span<const uplink_pdu_slot_repository::pucch_pdu> pucch_pdus =
        symbol == 3 ? span<const uplink_pdu_slot_repository::pucch_pdu>(pucch_at_symbol_three)
                    : span<const uplink_pdu_slot_repository::pucch_pdu>{};
    const span<const uplink_pdu_slot_repository::srs_pdu> srs_pdus =
        symbol == 13 ? span<const uplink_pdu_slot_repository::srs_pdu>(srs_at_symbol_thirteen)
                     : span<const uplink_pdu_slot_repository::srs_pdu>{};
    tap->handle_ul_symbol(writer, reader, slot, symbol, pdus, pucch_pdus, {}, srs_pdus);
  }

  ASSERT_EQ(router->spectrum_publications, 1U);
  ASSERT_TRUE(router->last_plan_complete);
  ASSERT_EQ(router->last_plan.size(), 56U);
  EXPECT_EQ(router->last_plan[0], OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1);
  EXPECT_EQ(router->last_plan[1], OCUDU_DAPP_UL_USE_REFERENCE_SIGNAL_V1);
  EXPECT_EQ(router->last_plan[5], OCUDU_DAPP_UL_USE_PUSCH_V1);
  EXPECT_EQ(router->last_plan[8], OCUDU_DAPP_UL_USE_PUCCH_V1);
  EXPECT_EQ(router->last_plan[13], OCUDU_DAPP_UL_USE_PUCCH_V1);
  EXPECT_TRUE(std::all_of(router->last_plan.begin() + 52, router->last_plan.end(), [](uint8_t value) {
    return value == OCUDU_DAPP_UL_USE_SRS_V1;
  }));
}

TEST(dapp_hook_provider, srs_wrap_publishes_after_the_conventional_result_without_changing_it)
{
  auto router           = std::make_shared<router_spy>();
  auto provider         = create_upper_phy_dapp_hook_provider(router);
  auto fallback_factory = std::make_shared<estimator_factory_spy>();
  auto factory          = provider->decorate_srs_estimator(fallback_factory, 2);
  ASSERT_NE(factory, nullptr);
  EXPECT_NE(factory->create_validator(), nullptr);
  EXPECT_EQ(fallback_factory->validator_creations, 1U);
  auto estimator = factory->create();
  ASSERT_NE(estimator, nullptr);
  ASSERT_NE(fallback_factory->last_estimator, nullptr);

  resource_grid_reader_spy    reader(1, 14, 1);
  srs_estimator_configuration config{};
  config.cell_id        = 11;
  config.producer_shard = 4;
  config.slot           = slot_point{1, 31};
  const auto result     = estimator->estimate(reader, config);

  EXPECT_EQ(fallback_factory->last_estimator->calls, 1U);
  ASSERT_TRUE(result.noise_variance.has_value());
  EXPECT_FLOAT_EQ(*result.noise_variance, 0.25F);
  EXPECT_EQ(router->srs_publications, 1U);
  EXPECT_EQ(router->last_context.cell_id, 11U);
  EXPECT_EQ(router->last_context.producer_shard, 4U);
  EXPECT_EQ(router->last_context.worker_lane, 0U);
  ASSERT_TRUE(router->last_noise_variance.has_value());
  EXPECT_FLOAT_EQ(*router->last_noise_variance, 0.25F);

  router->srs_enabled = false;
  estimator->estimate(reader, config);
  EXPECT_EQ(fallback_factory->last_estimator->calls, 2U);
  EXPECT_EQ(router->srs_publications, 1U);

  router->srs_enabled = true;
  auto second         = factory->create();
  ASSERT_NE(second, nullptr);
  second->estimate(reader, config);
  EXPECT_EQ(router->last_context.worker_lane, 1U);
  EXPECT_EQ(factory->create(), nullptr);
}

TEST(dapp_hook_provider, rejects_construction_without_a_router)
{
  EXPECT_EQ(create_upper_phy_dapp_hook_provider(nullptr), nullptr);
}
