// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "../support/resource_grid_test_doubles.h"
#include "ocudu/adt/complex.h"
#include "ocudu/dapp/ipc/class_c_metadata.h"
#include "ocudu/dapp/process/v1/control.h"
#include "ocudu/dapp/process/v1/publication_record.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_process_target.h"
#include "ocudu/ran/slot_point.h"
#include <array>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <utility>
#include <vector>

using namespace ocudu;
using namespace ocudu::dapp::ipc;
using namespace std::chrono_literals;

namespace {

class contiguous_process_reader final : public resource_grid_reader_spy
{
public:
  contiguous_process_reader(unsigned ports, unsigned symbols, unsigned prbs) :
    resource_grid_reader_spy(ports, symbols, prbs),
    storage(ports * symbols * prbs * NOF_SUBCARRIERS_PER_RB, to_cbf16(cf_t{1.0F, -1.0F}))
  {
  }

  span<const cbf16_t> get_contiguous_view() const override { return storage; }
  span<const cbf16_t> view() const { return storage; }

private:
  std::vector<cbf16_t> storage;
};

upper_phy_dapp_process_target_create_result create_target(uint32_t interface_id, uint32_t slots = 2, uint32_t flags = 0)
{
  upper_phy_dapp_process_target_config config{};
  config.interface_id              = interface_id;
  config.route_flags               = flags;
  config.slot_count                = slots;
  config.payload_capacity          = 4096;
  config.encoder.producer_instance = {41, 42};
  config.encoder.ran_node          = {43, 44};
  return create_upper_phy_dapp_process_target(std::move(config));
}

consumer_open_result attach(upper_phy_dapp_process_target& target)
{
  auto capabilities = target.export_capabilities();
  if (!capabilities) {
    return {};
  }
  return publication_consumer::attach(
      capabilities.ring.value(), capabilities.notification.value(), capabilities.completion.value());
}

ocudu_dapp_invocation_v1 invocation(uint32_t cell, uint64_t sequence)
{
  ocudu_dapp_invocation_v1 value{};
  value.struct_size           = sizeof(value);
  value.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  value.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  value.invocation_id         = sequence;
  value.cell_id               = cell;
  value.radio_time.sfn        = 2;
  value.radio_time.slot       = 3;
  value.radio_time.numerology = 1;
  value.input_generation      = sequence;
  return value;
}

template <typename T>
ocudu_dapp_typed_metadata_v1 typed(uint32_t schema_id, T& value)
{
  ocudu_dapp_typed_metadata_v1 result{};
  result.struct_size             = sizeof(result);
  result.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  result.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
  result.metadata_schema_id      = schema_id;
  result.metadata_schema_version = 1;
  result.metadata                = {&value, sizeof(value), 0};
  return result;
}

} // namespace

TEST(dapp_process_target, routes_spectrum_with_one_tensor_copy_and_restart_epoch)
{
  auto created = create_target(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  ASSERT_TRUE(created) << created.detail;
  std::shared_ptr<dapp::class_c_process_endpoint> supervised_endpoint = created.target;
  ASSERT_EQ(supervised_endpoint->interface_id(), OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  auto consumer = attach(*created.target);
  ASSERT_TRUE(consumer) << consumer.detail;
  created.target->set_active(true);
  auto router = create_upper_phy_dapp_data_router({{{3, 7, 0}, created.target}});
  ASSERT_TRUE(router) << router.detail;

  contiguous_process_reader    reader(1, 2, 1);
  const std::array<uint8_t, 2> plan{{OCUDU_DAPP_UL_USE_PUSCH_V1, OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1}};
  router.router->publish_spectrum({3, 7, 0}, reader, slot_point{1, 7}, {{plan.data(), plan.size()}, 2, 1, true});
  ASSERT_TRUE(consumer.consumer->wait(100ms));
  EXPECT_EQ(consumer.consumer->drain_notifications(), 1U);
  auto lease = consumer.consumer->try_acquire();
  ASSERT_TRUE(lease);
  ocudu_dapp_publication_record_view_v1 record{};
  ASSERT_EQ(ocudu_dapp_parse_publication_record_v1(lease->payload().data(), lease->payload().size(), &record),
            OCUDU_DAPP_PUBLICATION_RECORD_OK_V1);
  EXPECT_EQ(record.header.route_epoch, 1U);
  EXPECT_EQ(record.header.publication_sequence, 1U);
  EXPECT_EQ(record.header.tensor_size, reader.view().size_bytes());
  EXPECT_EQ(validate_class_c_metadata({record.metadata, record.header.metadata_size}), metadata_error::none);
  EXPECT_EQ(std::memcmp(record.tensor, reader.view().data(), reader.view().size_bytes()), 0);
  lease.reset();

  auto metrics = created.target->snapshot_metrics();
  EXPECT_EQ(metrics.observed_cell_bitmap, uint32_t{1} << 3U);
  EXPECT_EQ(metrics.publications, 1U);
  EXPECT_EQ(metrics.backpressure_drops, 0U);
  EXPECT_EQ(metrics.invalid_drops, 0U);
  EXPECT_EQ(metrics.snapshot_copy_operations, 1U);
  EXPECT_EQ(metrics.snapshot_copy_bytes, reader.view().size_bytes());
  EXPECT_EQ(metrics.lease_operations, 0U);
  const ocudu_dapp_radio_time_v1 created_time{0, 7, 1, 0};
  EXPECT_FALSE(supervised_endpoint->consume_result_source(1, 0, 3, created_time));
  EXPECT_FALSE(supervised_endpoint->consume_result_source(1, 1, 4, created_time));
  EXPECT_TRUE(supervised_endpoint->consume_result_source(1, 1, 3, created_time));
  EXPECT_FALSE(supervised_endpoint->consume_result_source(1, 1, 3, created_time));
  EXPECT_TRUE(supervised_endpoint->consume_quiet_intent_source(1, 1, 3, created_time));
  EXPECT_FALSE(supervised_endpoint->consume_quiet_intent_source(1, 1, 3, created_time));

  created.target->set_active(false);
  ASSERT_TRUE(created.target->reset_after_consumer_restart());
  created.target->set_active(true);
  router.router->publish_spectrum({3, 7, 0}, reader, slot_point{1, 8}, {});
  ASSERT_TRUE(consumer.consumer->wait(100ms));
  consumer.consumer->drain_notifications();
  auto restarted = consumer.consumer->try_acquire();
  ASSERT_TRUE(restarted);
  ASSERT_EQ(ocudu_dapp_parse_publication_record_v1(restarted->payload().data(), restarted->payload().size(), &record),
            OCUDU_DAPP_PUBLICATION_RECORD_OK_V1);
  EXPECT_EQ(record.header.route_epoch, 2U);
  EXPECT_EQ(record.header.publication_sequence, 2U);
}

TEST(dapp_process_target, carries_srs_and_reports_bounded_backpressure)
{
  auto created = create_target(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  ASSERT_TRUE(created) << created.detail;
  auto consumer = attach(*created.target);
  ASSERT_TRUE(consumer) << consumer.detail;
  created.target->set_active(true);

  std::array<cf_t, 4>       response{{{1.0F, 0.0F}, {0.5F, 0.25F}, {-1.0F, 0.0F}, {0.0F, -0.5F}}};
  ocudu_dapp_tensor_view_v1 tensor{};
  tensor.struct_size           = sizeof(tensor);
  tensor.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  tensor.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  tensor.domain                = OCUDU_DAPP_MEM_HOST_V1;
  tensor.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
  tensor.element_type          = OCUDU_DAPP_ELEMENT_CF32_V1;
  tensor.rank                  = 3;
  tensor.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  tensor.allocation_id         = 1;
  tensor.allocation_generation = 1;
  tensor.native_handle         = reinterpret_cast<uintptr_t>(response.data());
  tensor.byte_length           = sizeof(response);
  tensor.dimensions[0]         = 1;
  tensor.dimensions[1]         = 1;
  tensor.dimensions[2]         = response.size();
  tensor.byte_strides[0]       = sizeof(response);
  tensor.byte_strides[1]       = sizeof(response);
  tensor.byte_strides[2]       = sizeof(cf_t);
  tensor.lease_token           = 1;

  ocudu_dapp_srs_isac_metadata_v1 metadata{};
  metadata.struct_size           = sizeof(metadata);
  metadata.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  metadata.srs_resource_id       = 17;
  metadata.srs_config_id         = 19;
  metadata.rx_ports              = 1;
  metadata.tx_ports              = 1;
  metadata.comb_size             = 2;
  metadata.subcarrier_spacing_hz = 30'000;
  metadata.frequency_points      = response.size();
  metadata.noise_variance        = 0.1F;
  ocudu_dapp_srs_isac_input_v1 input{};
  input.invocation        = invocation(8, 1);
  input.channel_estimates = tensor;
  input.srs_metadata      = typed(OCUDU_DAPP_METADATA_SRS_ISAC_V1, metadata);

  for (uint64_t sequence = 1; sequence != 4; ++sequence) {
    input.invocation.cell_id                      = sequence == 1 ? 8 : (sequence == 2 ? 12 : 13);
    input.source_sequence                         = sequence;
    input.invocation.invocation_id                = sequence;
    input.invocation.input_generation             = sequence;
    input.channel_estimates.allocation_generation = sequence;
    created.target->publish(input);
  }
  auto metrics = created.target->snapshot_metrics();
  EXPECT_EQ(metrics.observed_cell_bitmap, (uint32_t{1} << 8U) | (uint32_t{1} << 12U));
  EXPECT_EQ(metrics.publications, 2U);
  EXPECT_EQ(metrics.backpressure_drops, 1U);
  EXPECT_EQ(metrics.snapshot_copy_operations, 2U);
  EXPECT_EQ(metrics.snapshot_copy_bytes, 2U * sizeof(response));

  ASSERT_TRUE(consumer.consumer->wait(100ms));
  consumer.consumer->drain_notifications();
  auto lease = consumer.consumer->try_acquire();
  ASSERT_TRUE(lease);
  ocudu_dapp_publication_record_view_v1 record{};
  ASSERT_EQ(ocudu_dapp_parse_publication_record_v1(lease->payload().data(), lease->payload().size(), &record),
            OCUDU_DAPP_PUBLICATION_RECORD_OK_V1);
  EXPECT_EQ(validate_class_c_metadata({record.metadata, record.header.metadata_size}), metadata_error::none);
  EXPECT_EQ(record.header.tensor_size, sizeof(response));
  EXPECT_EQ(std::memcmp(record.tensor, response.data(), sizeof(response)), 0);
  const auto source_time = input.invocation.radio_time;
  EXPECT_FALSE(created.target->consume_srs_result_source(1, 16, 19, 8, source_time));
  EXPECT_FALSE(created.target->consume_srs_result_source(1, 17, 20, 8, source_time));
  EXPECT_TRUE(created.target->consume_srs_result_source(1, 17, 19, 8, source_time));
  EXPECT_FALSE(created.target->consume_srs_result_source(1, 17, 19, 8, source_time));
}

TEST(dapp_process_target, carries_an_advance_plan_on_a_dedicated_tensorless_route)
{
  auto created = create_target(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 2, OCUDU_DAPP_ROUTE_FLAG_UL_RESOURCE_PLAN_V1);
  ASSERT_TRUE(created) << created.detail;
  auto consumer = attach(*created.target);
  ASSERT_TRUE(consumer) << consumer.detail;
  created.target->set_active(true);

  ocudu_dapp_ul_resource_span_v1 span{};
  span.struct_size   = sizeof(span);
  span.symbol_start  = 2;
  span.symbol_count  = 4;
  span.first_prb     = 10;
  span.nof_prbs      = 8;
  span.use_category  = OCUDU_DAPP_UL_USE_PUSCH_V1;
  span.allocation_id = 1;
  ocudu_dapp_ul_resource_plan_v1 plan{};
  plan.struct_size                     = sizeof(plan);
  plan.abi_major                       = OCUDU_DAPP_ABI_MAJOR_V1;
  plan.abi_minor                       = OCUDU_DAPP_ABI_MINOR_V1;
  plan.context.struct_size             = sizeof(plan.context);
  plan.context.schema_id               = OCUDU_DAPP_CONTEXT_UL_RESOURCE_PLAN_V1;
  plan.context.schema_version          = 1;
  plan.context.producer_instance_id[0] = 1;
  plan.context.producer_generation     = 1;
  plan.context.source_sequence         = 9;
  plan.context.cell_id                 = 3;
  plan.context.confidence_q16          = 1U << 16U;
  plan.context.created                 = {2, 3, 1, 0};
  plan.context.valid_from              = plan.context.created;
  plan.context.valid_until             = plan.context.created;
  plan.planned_time                    = plan.context.created;
  plan.plan_generation                 = 9;
  plan.spans                           = &span;
  plan.nof_spans                       = 1;
  plan.flags                           = OCUDU_DAPP_UL_RESOURCE_PLAN_COMPLETE_V1;
  plan.cell_nof_prbs                   = 52;
  created.target->publish(plan);

  ASSERT_TRUE(consumer.consumer->wait(100ms));
  EXPECT_EQ(consumer.consumer->drain_notifications(), 1U);
  auto lease = consumer.consumer->try_acquire();
  ASSERT_TRUE(lease);
  ocudu_dapp_publication_record_view_v1 record{};
  ASSERT_EQ(ocudu_dapp_parse_publication_record_v1(lease->payload().data(), lease->payload().size(), &record),
            OCUDU_DAPP_PUBLICATION_RECORD_OK_V1);
  EXPECT_EQ(record.header.flags, 0U);
  EXPECT_EQ(record.header.publication_sequence, plan.plan_generation);
  EXPECT_EQ(record.header.tensor_size, 0U);
  EXPECT_EQ(record.tensor, nullptr);
  EXPECT_EQ(validate_class_c_metadata({record.metadata, record.header.metadata_size}), metadata_error::none);
  const auto metrics = created.target->snapshot_metrics();
  EXPECT_EQ(metrics.observed_cell_bitmap, 0U);
  EXPECT_EQ(metrics.publications, 1U);
  EXPECT_EQ(metrics.snapshot_copy_operations, 0U);
  EXPECT_EQ(metrics.snapshot_copy_bytes, 0U);
}
