// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "../support/resource_grid_test_doubles.h"
#include "ocudu/dapp/runtime/abi_validation.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_data_router.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_configuration.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include "ocudu/ran/srs/srs_information.h"
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace ocudu;

namespace {

class publication_target_spy : public upper_phy_dapp_publication_target
{
public:
  explicit publication_target_spy(uint32_t interface_id_) : id(interface_id_) {}

  uint32_t interface_id() const noexcept override { return id; }
  bool     active() const noexcept override { return enabled; }

  void publish(const ocudu_dapp_spectrum_input_v1& input) noexcept override
  {
    ++publications;
    spectrum = input;
    if (input.slot_metadata.metadata.data != nullptr &&
        input.slot_metadata.metadata.size >= sizeof(spectrum_metadata)) {
      spectrum_metadata = *static_cast<const ocudu_dapp_spectrum_slot_metadata_v1*>(input.slot_metadata.metadata.data);
    }
  }

  void publish(const ocudu_dapp_srs_isac_input_v1& input) noexcept override
  {
    ++publications;
    srs = input;
    if (input.srs_metadata.metadata.data != nullptr && input.srs_metadata.metadata.size >= sizeof(srs_metadata)) {
      srs_metadata = *static_cast<const ocudu_dapp_srs_isac_metadata_v1*>(input.srs_metadata.metadata.data);
    }
  }

  uint32_t                             id;
  bool                                 enabled = true;
  unsigned                             publications{};
  ocudu_dapp_spectrum_input_v1         spectrum{};
  ocudu_dapp_srs_isac_input_v1         srs{};
  ocudu_dapp_spectrum_slot_metadata_v1 spectrum_metadata{};
  ocudu_dapp_srs_isac_metadata_v1      srs_metadata{};
};

class contiguous_reader final : public resource_grid_reader_spy
{
public:
  contiguous_reader(unsigned ports, unsigned symbols, unsigned prbs) :
    resource_grid_reader_spy(ports, symbols, prbs), storage(ports * symbols * prbs * NOF_SUBCARRIERS_PER_RB)
  {
  }

  span<const cbf16_t> get_contiguous_view() const override { return storage; }

private:
  std::vector<cbf16_t> storage;
};

class counting_contiguous_reader final : public resource_grid_reader_spy
{
public:
  counting_contiguous_reader(unsigned ports, unsigned symbols, unsigned prbs) :
    resource_grid_reader_spy(ports, symbols, prbs), storage(ports * symbols * prbs * NOF_SUBCARRIERS_PER_RB)
  {
  }

  span<const cbf16_t> get_contiguous_view() const override
  {
    ++host_borrows;
    return storage;
  }

  mutable unsigned     host_borrows{};
  std::vector<cbf16_t> storage;
};

class accelerator_spectrum_target_spy final : public publication_target_spy
{
public:
  accelerator_spectrum_target_spy() : publication_target_spy(OCUDU_DAPP_INTERFACE_SPECTRUM_V1) {}

  bool uses_accelerator_spectrum() const noexcept override { return true; }

  void publish_accelerator_spectrum(const ocudu_dapp_spectrum_input_v1& input,
                                    const resource_grid_reader&         source,
                                    const dapp::resource_grid_lease&    source_lease) noexcept override
  {
    ++accelerator_publications;
    spectrum = input;
    reader   = &source;
    lease    = source_lease;
  }

  unsigned                    accelerator_publications{};
  const resource_grid_reader* reader{};
  dapp::resource_grid_lease   lease{};
};

class accelerator_srs_target_spy final : public publication_target_spy
{
public:
  accelerator_srs_target_spy() : publication_target_spy(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) {}

  bool uses_accelerator_srs() const noexcept override { return true; }

  void publish_accelerator_srs(const ocudu_dapp_srs_isac_input_v1& input,
                               const resource_grid_reader&         source,
                               const srs_estimator_configuration&,
                               const dapp::resource_grid_lease& source_lease) noexcept override
  {
    ++accelerator_publications;
    srs    = input;
    reader = &source;
    lease  = source_lease;
    if (input.srs_metadata.metadata.data != nullptr && input.srs_metadata.metadata.size >= sizeof(srs_metadata)) {
      srs_metadata = *static_cast<const ocudu_dapp_srs_isac_metadata_v1*>(input.srs_metadata.metadata.data);
    }
  }

  unsigned                    accelerator_publications{};
  const resource_grid_reader* reader{};
  dapp::resource_grid_lease   lease{};
};

class counting_srs_reader final : public resource_grid_reader_spy
{
public:
  counting_srs_reader() : resource_grid_reader_spy(1, 14, 275) {}

  void get(span<cf_t> output, unsigned port, unsigned symbol, unsigned first_subcarrier, unsigned stride) const override
  {
    ++get_calls;
    resource_grid_reader_spy::get(output, port, symbol, first_subcarrier, stride);
  }

  mutable unsigned get_calls{};
};

class throwing_reader final : public resource_grid_reader_spy
{
public:
  throwing_reader() : resource_grid_reader_spy(1, 14, 275) {}

  void get(span<cf_t>, unsigned, unsigned, unsigned, unsigned) const override
  {
    throw std::runtime_error("test grid failure");
  }
};

srs_estimator_configuration make_srs_config()
{
  srs_estimator_configuration config{};
  config.cell_id        = 8;
  config.producer_shard = 2;
  config.slot           = slot_point{1, 15};
  config.resource       = {.nof_antenna_ports   = srs_resource_configuration::one_two_four_enum::one,
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
  config.ports          = {0};
  config.context        = srs_context(0, to_rnti(0x4601));
  return config;
}

} // namespace

TEST(dapp_data_router, spectrum_materializes_one_native_bf16_product_for_all_targets)
{
  auto first   = std::make_shared<publication_target_spy>(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  auto second  = std::make_shared<publication_target_spy>(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  auto created = create_upper_phy_dapp_data_router({{{3, 7}, first}, {{3, 7}, second}});
  ASSERT_TRUE(created) << created.detail;
  EXPECT_TRUE(created.router->is_enabled(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, {3, 7}));
  EXPECT_FALSE(created.router->is_enabled(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, {3, 8}));

  contiguous_reader reader(2, 14, 2);
  const slot_point  slot{1, 22};
  created.router->publish_spectrum({3, 7}, reader, slot, {});

  ASSERT_EQ(first->publications, 1U);
  ASSERT_EQ(second->publications, 1U);
  EXPECT_EQ(first->spectrum.source_sequence, second->spectrum.source_sequence);
  EXPECT_EQ(first->spectrum.invocation.cell_id, 3U);
  EXPECT_EQ(first->spectrum.invocation.radio_time.sfn, slot.sfn());
  EXPECT_EQ(first->spectrum.invocation.radio_time.slot, slot.slot_index());
  EXPECT_EQ(first->spectrum.uplink_resource_grid.element_type, OCUDU_DAPP_ELEMENT_CBF16_V1);
  EXPECT_EQ(first->spectrum.uplink_resource_grid.native_handle, second->spectrum.uplink_resource_grid.native_handle);
  EXPECT_EQ(dapp::validate_tensor_view(first->spectrum.uplink_resource_grid), dapp::validation_error::none);
  EXPECT_EQ(first->spectrum_metadata.rx_ports, 2U);
  EXPECT_EQ(first->spectrum_metadata.symbols, 14U);
  EXPECT_EQ(first->spectrum_metadata.subcarriers, 24U);
  EXPECT_EQ(first->spectrum_metadata.bandwidth_hz, 24U * 30'000U);

  first->enabled  = false;
  second->enabled = false;
  EXPECT_FALSE(created.router->is_enabled(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, {3, 7}));
  created.router->publish_spectrum({3, 7}, reader, slot, {});
  EXPECT_EQ(first->publications, 1U);
  EXPECT_EQ(second->publications, 1U);
}

TEST(dapp_data_router, multicell_spectrum_routes_keep_independent_producer_lane_state)
{
  auto first   = std::make_shared<publication_target_spy>(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  auto second  = std::make_shared<publication_target_spy>(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  auto created = create_upper_phy_dapp_data_router({{{3, 7, 0}, first}, {{8, 2, 0}, second}});
  ASSERT_TRUE(created) << created.detail;
  EXPECT_TRUE(created.router->is_enabled(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, {3, 7, 0}));
  EXPECT_TRUE(created.router->is_enabled(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, {8, 2, 0}));
  EXPECT_FALSE(created.router->is_enabled(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, {3, 2, 0}));

  contiguous_reader first_reader(1, 2, 1);
  contiguous_reader second_reader(1, 2, 1);
  auto              run = [&](upper_phy_dapp_producer_context context, contiguous_reader& reader) {
    for (unsigned iteration = 0; iteration != 32; ++iteration) {
      created.router->publish_spectrum(context, reader, slot_point{1, iteration % 20}, {});
    }
  };
  std::thread first_thread(run, upper_phy_dapp_producer_context{3, 7, 0}, std::ref(first_reader));
  std::thread second_thread(run, upper_phy_dapp_producer_context{8, 2, 0}, std::ref(second_reader));
  first_thread.join();
  second_thread.join();

  EXPECT_EQ(first->publications, 32U);
  EXPECT_EQ(first->spectrum.invocation.cell_id, 3U);
  EXPECT_EQ(first->spectrum.source_sequence, 32U);
  EXPECT_EQ(second->publications, 32U);
  EXPECT_EQ(second->spectrum.invocation.cell_id, 8U);
  EXPECT_EQ(second->spectrum.source_sequence, 32U);
}

TEST(dapp_data_router, spectrum_avoids_host_materialization_for_accelerator_only_fanout)
{
  auto accelerator = std::make_shared<accelerator_spectrum_target_spy>();
  auto created     = create_upper_phy_dapp_data_router({{{3, 7}, accelerator}});
  ASSERT_TRUE(created) << created.detail;
  counting_contiguous_reader reader(2, 14, 2);

  created.router->publish_spectrum({3, 7}, reader, slot_point{1, 22}, {});

  EXPECT_EQ(accelerator->accelerator_publications, 1U);
  EXPECT_EQ(accelerator->publications, 0U);
  EXPECT_EQ(accelerator->reader, &reader);
  EXPECT_EQ(accelerator->lease.allocation_generation, accelerator->spectrum.source_sequence);
  EXPECT_EQ(accelerator->lease.lease_token, accelerator->spectrum.source_sequence);
  EXPECT_EQ(reader.host_borrows, 0U);
}

TEST(dapp_data_router, spectrum_materializes_host_view_only_for_mixed_fanout_consumers)
{
  auto accelerator = std::make_shared<accelerator_spectrum_target_spy>();
  auto host        = std::make_shared<publication_target_spy>(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  auto created     = create_upper_phy_dapp_data_router({{{3, 7}, accelerator}, {{3, 7}, host}});
  ASSERT_TRUE(created) << created.detail;
  counting_contiguous_reader reader(1, 2, 2);

  created.router->publish_spectrum({3, 7}, reader, slot_point{1, 22}, {});

  EXPECT_EQ(accelerator->accelerator_publications, 1U);
  EXPECT_EQ(host->publications, 1U);
  EXPECT_EQ(reader.host_borrows, 1U);
  EXPECT_EQ(host->spectrum.uplink_resource_grid.domain, OCUDU_DAPP_MEM_HOST_V1);
}

TEST(dapp_data_router, spectrum_attaches_a_generation_matched_expected_ul_use_tensor)
{
  auto target  = std::make_shared<publication_target_spy>(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  auto created = create_upper_phy_dapp_data_router({{{3, 7}, target}});
  ASSERT_TRUE(created) << created.detail;
  contiguous_reader            reader(1, 2, 2);
  const std::array<uint8_t, 4> plan{{OCUDU_DAPP_UL_USE_REFERENCE_SIGNAL_V1,
                                     OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1,
                                     OCUDU_DAPP_UL_USE_PUSCH_V1,
                                     OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1}};

  created.router->publish_spectrum({3, 7}, reader, slot_point{1, 22}, {{plan.data(), plan.size()}, 2, 2, true});

  ASSERT_EQ(target->publications, 1U);
  EXPECT_EQ(target->spectrum_metadata.flags, OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1);
  EXPECT_EQ(target->spectrum_metadata.source_plan_generation, target->spectrum.source_sequence);
  const auto& expected = target->spectrum.scheduled_re_mask;
  EXPECT_EQ(dapp::validate_tensor_view(expected), dapp::validation_error::none);
  EXPECT_EQ(expected.element_type, OCUDU_DAPP_ELEMENT_U8_V1);
  EXPECT_EQ(expected.rank, 2U);
  EXPECT_EQ(expected.dimensions[0], 2U);
  EXPECT_EQ(expected.dimensions[1], 2U);
  EXPECT_EQ(expected.layout_id, OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1);
  EXPECT_EQ(expected.allocation_generation, target->spectrum_metadata.source_plan_generation);
  EXPECT_EQ(std::memcmp(reinterpret_cast<const void*>(expected.native_handle), plan.data(), plan.size()), 0);
}

TEST(dapp_data_router, srs_forms_a_frequency_response_without_changing_the_estimator)
{
  auto target  = std::make_shared<publication_target_spy>(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  auto created = create_upper_phy_dapp_data_router({{{8, 2}, target}});
  ASSERT_TRUE(created) << created.detail;
  auto config                   = make_srs_config();
  config.context                = srs_context(0, to_rnti(0x4601));
  const srs_information    info = get_srs_information(config.resource, 0);
  resource_grid_reader_spy reader(1, 14, 275);
  for (uint32_t frequency = 0; frequency != info.sequence_length; ++frequency) {
    reader.write({0,
                  config.resource.start_symbol.value(),
                  static_cast<uint16_t>(info.mapping_initial_subcarrier + frequency * info.comb_size),
                  {1.0F, 0.0F}});
  }
  srs_estimator_result result{};
  result.noise_variance                = 0.125F;
  result.rsrp_dB                       = -10.0F;
  result.epre_dB                       = -8.0F;
  result.time_alignment.time_alignment = 2.0e-6;

  created.router->publish_srs_isac({8, 2, 0}, reader, config, result);

  ASSERT_EQ(target->publications, 1U);
  EXPECT_EQ(target->srs.invocation.cell_id, 8U);
  EXPECT_EQ(target->srs.channel_estimates.element_type, OCUDU_DAPP_ELEMENT_CF32_V1);
  EXPECT_EQ(target->srs.channel_estimates.layout_id, OCUDU_DAPP_LAYOUT_SRS_CHANNEL_RX_TX_FREQUENCY_CF32_V1);
  EXPECT_EQ(target->srs.channel_estimates.layout_version, 1U);
  EXPECT_EQ(target->srs.channel_estimates.dimensions[0], 1U);
  EXPECT_EQ(target->srs.channel_estimates.dimensions[1], 1U);
  EXPECT_EQ(target->srs.channel_estimates.dimensions[2], info.sequence_length);
  EXPECT_EQ(dapp::validate_tensor_view(target->srs.channel_estimates), dapp::validation_error::none);
  EXPECT_EQ(target->srs_metadata.frequency_points, info.sequence_length);
  EXPECT_EQ(target->srs_metadata.comb_size, 2U);
  EXPECT_FLOAT_EQ(target->srs_metadata.noise_variance, 0.125F);
  EXPECT_EQ(target->srs_metadata.timing_advance_ns, 2000);
  EXPECT_EQ(target->srs_metadata.rnti, 0x4601U);
  const auto* response = reinterpret_cast<const cf_t*>(target->srs.channel_estimates.native_handle);
  EXPECT_TRUE(std::isfinite(response[0].real()));
  EXPECT_TRUE(std::isfinite(response[0].imag()));

  // A duplicate callback for the same completed observation is suppressed.
  created.router->publish_srs_isac({8, 2, 0}, reader, config, result);
  EXPECT_EQ(target->publications, 1U);
  config.slot = slot_point{1, 16};
  created.router->publish_srs_isac({8, 2, 0}, reader, config, result);
  EXPECT_EQ(target->publications, 2U);
  EXPECT_EQ(target->srs.source_sequence, 2U);
  EXPECT_EQ(target->srs.channel_estimates.allocation_generation, 2U);
}

TEST(dapp_data_router, srs_avoids_host_channel_materialization_for_accelerator_only_fanout)
{
  auto accelerator = std::make_shared<accelerator_srs_target_spy>();
  auto created     = create_upper_phy_dapp_data_router({{{8, 2}, accelerator}});
  ASSERT_TRUE(created) << created.detail;
  throwing_reader reader;

  EXPECT_NO_THROW(created.router->publish_srs_isac({8, 2, 0}, reader, make_srs_config(), {}));

  EXPECT_EQ(accelerator->accelerator_publications, 1U);
  EXPECT_EQ(accelerator->publications, 0U);
  EXPECT_EQ(accelerator->reader, &reader);
  EXPECT_EQ(accelerator->lease.allocation_generation, accelerator->srs.source_sequence);
  EXPECT_EQ(accelerator->lease.lease_token, accelerator->srs.source_sequence);
  EXPECT_EQ(accelerator->srs.channel_estimates.struct_size, 0U);
  EXPECT_GT(accelerator->srs_metadata.frequency_points, 0U);
}

TEST(dapp_data_router, srs_materializes_host_estimates_once_for_mixed_fanout)
{
  auto accelerator = std::make_shared<accelerator_srs_target_spy>();
  auto host        = std::make_shared<publication_target_spy>(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  auto created     = create_upper_phy_dapp_data_router({{{8, 2}, accelerator}, {{8, 2}, host}});
  ASSERT_TRUE(created) << created.detail;
  counting_srs_reader reader;
  const auto          config = make_srs_config();
  const auto          info   = get_srs_information(config.resource, 0);
  for (uint32_t frequency = 0; frequency != info.sequence_length; ++frequency) {
    reader.write({0,
                  config.resource.start_symbol.value(),
                  static_cast<uint16_t>(info.mapping_initial_subcarrier + frequency * info.comb_size),
                  {1.0F, 0.0F}});
  }

  created.router->publish_srs_isac({8, 2, 0}, reader, config, {});

  EXPECT_EQ(accelerator->accelerator_publications, 1U);
  ASSERT_EQ(host->publications, 1U);
  EXPECT_EQ(reader.get_calls, 1U);
  EXPECT_EQ(host->srs.channel_estimates.domain, OCUDU_DAPP_MEM_HOST_V1);
  EXPECT_EQ(host->srs.channel_estimates.allocation_generation, accelerator->srs.source_sequence);
}

TEST(dapp_data_router, srs_normalizes_nonfinite_optional_measurements)
{
  auto target  = std::make_shared<publication_target_spy>(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  auto created = create_upper_phy_dapp_data_router({{{8, 2}, target}});
  ASSERT_TRUE(created) << created.detail;
  auto                     config = make_srs_config();
  const srs_information    info   = get_srs_information(config.resource, 0);
  resource_grid_reader_spy reader(1, 14, 275);
  for (uint32_t frequency = 0; frequency != info.sequence_length; ++frequency) {
    reader.write({0,
                  config.resource.start_symbol.value(),
                  static_cast<uint16_t>(info.mapping_initial_subcarrier + frequency * info.comb_size),
                  {0.0F, 0.0F}});
  }
  srs_estimator_result result{};
  result.noise_variance = -std::numeric_limits<float>::infinity();
  result.rsrp_dB        = -std::numeric_limits<float>::infinity();
  result.epre_dB        = std::numeric_limits<float>::quiet_NaN();

  created.router->publish_srs_isac({8, 2, 0}, reader, config, result);

  ASSERT_EQ(target->publications, 1U);
  EXPECT_EQ(target->srs_metadata.frequency_points, info.sequence_length);
  EXPECT_FLOAT_EQ(target->srs_metadata.noise_variance, 0.0F);
  EXPECT_FLOAT_EQ(target->srs_metadata.rsrp_db, 0.0F);
  EXPECT_FLOAT_EQ(target->srs_metadata.epre_db, 0.0F);
  EXPECT_EQ(target->srs_metadata.rnti, 0x4601U);
}

TEST(dapp_data_router, srs_without_ue_context_is_dropped_before_materialization)
{
  auto target  = std::make_shared<publication_target_spy>(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  auto created = create_upper_phy_dapp_data_router({{{8, 2}, target}});
  ASSERT_TRUE(created) << created.detail;
  auto config = make_srs_config();
  config.context.reset();
  throwing_reader reader;

  EXPECT_NO_THROW(created.router->publish_srs_isac({8, 2, 0}, reader, config, {}));
  EXPECT_EQ(target->publications, 0U);
}

TEST(dapp_data_router, srs_worker_lanes_have_independent_hot_state)
{
  auto first   = std::make_shared<publication_target_spy>(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  auto second  = std::make_shared<publication_target_spy>(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  auto created = create_upper_phy_dapp_data_router({{{8, 2, 0}, first}, {{8, 2, 1}, second}});
  ASSERT_TRUE(created) << created.detail;
  const auto                 config = make_srs_config();
  const srs_information      info   = get_srs_information(config.resource, 0);
  resource_grid_reader_spy   first_reader(1, 14, 275);
  resource_grid_reader_spy   second_reader(1, 14, 275);
  const srs_estimator_result result{};
  for (uint32_t frequency = 0; frequency != info.sequence_length; ++frequency) {
    const resource_grid_reader_spy::expected_entry_t entry{
        0,
        config.resource.start_symbol.value(),
        static_cast<uint16_t>(info.mapping_initial_subcarrier + frequency * info.comb_size),
        {1.0F, 0.0F}};
    first_reader.write(entry);
    second_reader.write(entry);
  }

  auto run = [&](upper_phy_dapp_producer_context context, resource_grid_reader_spy& reader) {
    auto lane_config = config;
    for (unsigned i = 0; i != 20; ++i) {
      lane_config.slot = slot_point{1, i};
      created.router->publish_srs_isac(context, reader, lane_config, result);
    }
  };

  std::thread first_thread(run, upper_phy_dapp_producer_context{8, 2, 0}, std::ref(first_reader));
  std::thread second_thread(run, upper_phy_dapp_producer_context{8, 2, 1}, std::ref(second_reader));
  first_thread.join();
  second_thread.join();

  EXPECT_EQ(first->publications, 20U);
  EXPECT_EQ(first->srs.source_sequence, 20U);
  EXPECT_EQ(first->srs.channel_estimates.allocation_generation, 20U);
  EXPECT_EQ(second->publications, 20U);
  EXPECT_EQ(second->srs.source_sequence, 20U);
  EXPECT_EQ(second->srs.channel_estimates.allocation_generation, 20U);
  EXPECT_NE(first->srs.channel_estimates.native_handle, second->srs.channel_estimates.native_handle);
  EXPECT_NE(first->srs.channel_estimates.allocation_id, second->srs.channel_estimates.allocation_id);
}

TEST(dapp_data_router, pooled_srs_lane_routes_preserve_the_actual_producer_identity)
{
  auto target  = std::make_shared<publication_target_spy>(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  auto created = create_upper_phy_dapp_data_router({{{0, 0, 3}, target, true}});
  ASSERT_TRUE(created) << created.detail;
  EXPECT_TRUE(created.router->is_enabled(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, {7, 91, 3}));
  EXPECT_FALSE(created.router->is_enabled(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, {7, 91, 2}));

  auto config                   = make_srs_config();
  config.cell_id                = 7;
  config.producer_shard         = 91;
  const srs_information    info = get_srs_information(config.resource, 0);
  resource_grid_reader_spy reader(1, 14, 275);
  for (uint32_t frequency = 0; frequency != info.sequence_length; ++frequency) {
    reader.write({0,
                  config.resource.start_symbol.value(),
                  static_cast<uint16_t>(info.mapping_initial_subcarrier + frequency * info.comb_size),
                  {1.0F, 0.0F}});
  }

  created.router->publish_srs_isac({7, 91, 3}, reader, config, {});

  ASSERT_EQ(target->publications, 1U);
  EXPECT_EQ(target->srs.invocation.cell_id, 7U);
  EXPECT_EQ(target->srs.source_sequence, 1U);

  config.cell_id        = 9;
  config.producer_shard = 42;
  created.router->publish_srs_isac({9, 42, 3}, reader, config, {});
  ASSERT_EQ(target->publications, 2U);
  EXPECT_EQ(target->srs.invocation.cell_id, 9U);
  EXPECT_EQ(target->srs.source_sequence, 2U);
}

TEST(dapp_data_router, contains_srs_product_materialization_exceptions)
{
  auto target  = std::make_shared<publication_target_spy>(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  auto created = create_upper_phy_dapp_data_router({{{8, 2, 0}, target}});
  ASSERT_TRUE(created) << created.detail;

  throwing_reader reader;
  EXPECT_NO_THROW(created.router->publish_srs_isac({8, 2, 0}, reader, make_srs_config(), {}));
  EXPECT_EQ(target->publications, 0U);
}

TEST(dapp_data_router, validates_bindings_and_native_target_lane_ranges)
{
  EXPECT_FALSE(create_upper_phy_dapp_data_router({}));
  EXPECT_FALSE(create_upper_phy_dapp_data_router({{{0, 0}, nullptr}}));
  auto spectrum = std::make_shared<publication_target_spy>(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  EXPECT_FALSE(create_upper_phy_dapp_data_router({{{0, 0}, spectrum, true}}));

  auto point_created = dapp::native_hook_point::create(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 20, 2);
  ASSERT_TRUE(point_created);
  auto point  = std::shared_ptr<dapp::native_hook_point>(std::move(point_created.point));
  auto target = create_native_upper_phy_dapp_target(point, 1);
  ASSERT_NE(target, nullptr);
  EXPECT_EQ(target->interface_id(), OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  EXPECT_FALSE(target->active());
  EXPECT_EQ(create_native_upper_phy_dapp_target(point, 2), nullptr);
}
