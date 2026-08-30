// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "../support/resource_grid_test_doubles.h"
#include "ocudu/dapp/e3/portable_data_store.h"
#include "ocudu/dapp/ipc/class_c_metadata.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_portable_e3_target.h"
#include "ocudu/ran/slot_point.h"
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <numeric>
#include <vector>

using namespace ocudu;

namespace {

class contiguous_portable_reader final : public resource_grid_reader_spy
{
public:
  contiguous_portable_reader() : resource_grid_reader_spy(1, 2, 1), storage(24, to_cbf16(cf_t{1.0F, -1.0F})) {}
  span<const cbf16_t> get_contiguous_view() const override { return storage; }
  span<const cbf16_t> view() const { return storage; }

private:
  std::vector<cbf16_t> storage;
};

dapp::management::identifier subscription_id(uint8_t suffix = 1)
{
  dapp::management::identifier result;
  result.bytes[0]  = 0xe3;
  result.bytes[15] = suffix;
  return result;
}

template <typename T>
ocudu_dapp_typed_metadata_v1 typed_metadata(uint32_t schema, T& value)
{
  ocudu_dapp_typed_metadata_v1 result{};
  result.struct_size             = sizeof(result);
  result.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  result.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
  result.metadata_schema_id      = schema;
  result.metadata_schema_version = 1;
  result.metadata                = {&value, sizeof(value), 0};
  return result;
}

std::shared_ptr<dapp::e3::portable_data_store> make_store()
{
  dapp::e3::portable_data_store_config config;
  config.maximum_subscriptions  = 1;
  config.maximum_retained_bytes = 1024 * 1024;
  config.maximum_object_bytes   = 4096;
  config.maximum_metadata_bytes = 8192;
  config.minimum_chunk_bytes    = 1024;
  config.maximum_chunk_bytes    = 1024;
  auto created                  = dapp::e3::portable_data_store::create(config);
  EXPECT_NE(created.first, nullptr) << created.second;
  return std::move(created.first);
}

TEST(dapp_portable_e3_target, srs_channel_estimates_and_metadata_reassemble_exactly)
{
  auto store = make_store();
  ASSERT_NE(store, nullptr);
  dapp::e3::portable_subscription_request request;
  request.subscription_id     = subscription_id(2);
  request.interface_id        = OCUDU_DAPP_INTERFACE_SRS_ISAC_V1;
  request.queue_depth_chunks  = 4;
  request.maximum_chunk_bytes = 1024;
  ASSERT_TRUE(store->subscribe(request));

  dapp::ipc::class_c_encoder_config encoder;
  encoder.producer_instance   = {51, 52};
  encoder.ran_node            = {53, 54};
  encoder.host_object_profile = dapp::ipc::class_c_host_object_profile::embedded_payload;
  auto target =
      create_upper_phy_dapp_portable_e3_target({OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, 23, std::move(encoder), store});
  ASSERT_TRUE(target) << target.detail;

  std::array<cf_t, 4>       estimates{{{1.0F, -1.0F}, {2.0F, -2.0F}, {3.0F, -3.0F}, {4.0F, -4.0F}}};
  ocudu_dapp_tensor_view_v1 tensor{};
  tensor.struct_size           = sizeof(tensor);
  tensor.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  tensor.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  tensor.domain                = OCUDU_DAPP_MEM_HOST_V1;
  tensor.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
  tensor.element_type          = OCUDU_DAPP_ELEMENT_CF32_V1;
  tensor.rank                  = 3;
  tensor.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  tensor.allocation_id         = 61;
  tensor.allocation_generation = 1;
  tensor.native_handle         = reinterpret_cast<uintptr_t>(estimates.data());
  tensor.byte_length           = estimates.size() * sizeof(cf_t);
  tensor.dimensions[0]         = 1;
  tensor.dimensions[1]         = 1;
  tensor.dimensions[2]         = estimates.size();
  tensor.byte_strides[0]       = tensor.byte_length;
  tensor.byte_strides[1]       = tensor.byte_length;
  tensor.byte_strides[2]       = sizeof(cf_t);
  tensor.lease_token           = 1;

  ocudu_dapp_srs_isac_metadata_v1 metadata{};
  metadata.struct_size           = sizeof(metadata);
  metadata.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  metadata.srs_resource_id       = 2;
  metadata.srs_config_id         = 3;
  metadata.rx_ports              = 1;
  metadata.tx_ports              = 1;
  metadata.comb_size             = 2;
  metadata.first_subcarrier      = 12;
  metadata.subcarrier_spacing_hz = 30'000;
  metadata.frequency_points      = estimates.size();
  metadata.noise_variance        = 0.125F;

  ocudu_dapp_srs_isac_input_v1 input{};
  input.invocation.struct_size      = sizeof(input.invocation);
  input.invocation.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
  input.invocation.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
  input.invocation.invocation_id    = 7;
  input.invocation.cell_id          = 4;
  input.invocation.radio_time       = {10, 6, 1, 0};
  input.invocation.input_generation = 5;
  input.channel_estimates           = tensor;
  input.srs_metadata                = typed_metadata(OCUDU_DAPP_METADATA_SRS_ISAC_V1, metadata);
  input.source_sequence             = 9;

  target.target->publish(input);
  auto polled = store->poll(subscription_id(2), 8);
  ASSERT_TRUE(polled) << polled.detail;
  ASSERT_EQ(polled.chunks.size(), 1U);
  EXPECT_EQ(polled.chunks[0].interface_id, OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  EXPECT_EQ(polled.chunks[0].cell_id, 4U);
  EXPECT_EQ(polled.chunks[0].publication_sequence, 9U);
  EXPECT_EQ(polled.chunks[0].route_epoch, 23U);
  EXPECT_EQ(dapp::ipc::validate_class_c_metadata(polled.chunks[0].metadata), dapp::ipc::metadata_error::none);
  ASSERT_EQ(polled.chunks[0].tensor_chunk.size(), sizeof(estimates));
  EXPECT_EQ(std::memcmp(polled.chunks[0].tensor_chunk.data(), estimates.data(), sizeof(estimates)), 0);
}

} // namespace

TEST(dapp_portable_e3_target, disabled_path_is_constant_until_a_subscription_and_spectrum_reassembles_exactly)
{
  auto store = make_store();
  ASSERT_NE(store, nullptr);
  dapp::ipc::class_c_encoder_config encoder;
  encoder.producer_instance   = {41, 42};
  encoder.ran_node            = {43, 44};
  encoder.host_object_profile = dapp::ipc::class_c_host_object_profile::embedded_payload;
  auto target =
      create_upper_phy_dapp_portable_e3_target({OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 17, std::move(encoder), store});
  ASSERT_TRUE(target) << target.detail;
  auto router = create_upper_phy_dapp_data_router({{{3, 7, 0}, target.target}});
  ASSERT_TRUE(router) << router.detail;
  EXPECT_FALSE(router.router->is_enabled(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, {3, 7, 0}));

  dapp::e3::portable_subscription_request request;
  request.subscription_id     = subscription_id();
  request.interface_id        = OCUDU_DAPP_INTERFACE_SPECTRUM_V1;
  request.cell_id             = 3;
  request.queue_depth_chunks  = 4;
  request.maximum_chunk_bytes = 1024;
  ASSERT_TRUE(store->subscribe(request));
  EXPECT_TRUE(router.router->is_enabled(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, {3, 7, 0}));

  contiguous_portable_reader reader;
  router.router->publish_spectrum({3, 7, 0}, reader, slot_point{1, 7}, {});
  auto polled = store->poll(subscription_id(), 8);
  ASSERT_TRUE(polled) << polled.detail;
  ASSERT_EQ(polled.chunks.size(), 1U);
  EXPECT_EQ(polled.chunks[0].interface_id, OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  EXPECT_EQ(polled.chunks[0].cell_id, 3U);
  EXPECT_EQ(polled.chunks[0].publication_sequence, 1U);
  EXPECT_EQ(polled.chunks[0].route_epoch, 17U);
  EXPECT_EQ(dapp::ipc::validate_class_c_metadata(polled.chunks[0].metadata), dapp::ipc::metadata_error::none);
  ASSERT_EQ(polled.chunks[0].tensor_chunk.size(), reader.view().size_bytes());
  EXPECT_EQ(std::memcmp(polled.chunks[0].tensor_chunk.data(), reader.view().data(), reader.view().size_bytes()), 0);

  ASSERT_TRUE(store->unsubscribe(subscription_id()));
  EXPECT_FALSE(router.router->is_enabled(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, {3, 7, 0}));
}
