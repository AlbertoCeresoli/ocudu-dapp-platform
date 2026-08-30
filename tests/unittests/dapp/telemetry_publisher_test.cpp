// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/e3ap_codec.h"
#include "ocudu/dapp/e3/telemetry_publisher.h"
#include <complex>
#include <cstring>
#include <gtest/gtest.h>
#include <thread>

using namespace ocudu;
using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;

namespace {

management::identifier make_id(uint8_t suffix)
{
  management::identifier result;
  result.bytes[0]  = 0x7e;
  result.bytes[15] = suffix;
  return result;
}

std::shared_ptr<portable_data_store> make_store()
{
  portable_data_store_config config;
  config.maximum_subscriptions  = 8;
  config.maximum_retained_bytes = 8 * 1024 * 1024;
  config.maximum_object_bytes   = 512 * 1024;
  config.maximum_metadata_bytes = 1024;
  config.minimum_chunk_bytes    = 1024;
  config.maximum_chunk_bytes    = 64 * 1024;
  config.minimum_lease_us       = 1;
  config.maximum_lease_us       = 10'000'000;
  auto created                  = portable_data_store::create(config);
  EXPECT_TRUE(created.first) << created.second;
  return created.first;
}

portable_subscription_request
subscribe_request(uint8_t suffix, uint32_t interface_id, uint64_t requested_interval_us = portable_interval_unspecified)
{
  portable_subscription_request request;
  request.requested_interval_us = requested_interval_us;
  request.subscription_id       = make_id(suffix);
  request.interface_id          = interface_id;
  request.queue_depth_chunks    = 16;
  request.maximum_chunk_bytes   = 64 * 1024;
  request.lease_us              = 5'000'000;
  request.drop_policy           = portable_drop_policy::coalesce_latest;
  return request;
}

/// Polls until one complete object is available and returns its metadata and tensor.
bool drain_one(portable_data_store&          store,
               const management::identifier& id,
               std::vector<uint8_t>&         metadata,
               std::vector<uint8_t>&         tensor,
               portable_publication_chunk&   first)
{
  metadata.clear();
  tensor.clear();
  auto result = store.poll(id, 8);
  if (!result || result.chunks.empty()) {
    return false;
  }
  first    = result.chunks.front();
  metadata = first.metadata;
  for (const auto& chunk : result.chunks) {
    tensor.insert(tensor.end(), chunk.tensor_chunk.begin(), chunk.tensor_chunk.end());
  }
  return tensor.size() == first.tensor_size;
}

} // namespace

TEST(dapp_telemetry_publisher, gates_on_active_subscriptions_and_rate_limits_per_rnti)
{
  auto                       store = make_store();
  telemetry_publisher_config config;
  config.store                          = store;
  config.pusch_symbols_minimum_interval = std::chrono::milliseconds(200);
  auto created                          = telemetry_publisher::create(config);
  ASSERT_TRUE(created.first) << created.second;
  auto& publisher = *created.first;

  // Without a subscriber, no capture is requested and nothing is copied.
  EXPECT_FALSE(publisher.should_collect(phy_diagnostic_stream::pusch_symbols, 0x4601));
  EXPECT_FALSE(publisher.is_collect_due(phy_diagnostic_stream::pusch_crc, 0x4601));
  EXPECT_FALSE(publisher.metrics_json_active());

  ASSERT_TRUE(store->subscribe(subscribe_request(1, telemetry_stream_pusch_symbols)));
  EXPECT_TRUE(publisher.is_collect_due(phy_diagnostic_stream::pusch_symbols, 0x4601));
  EXPECT_TRUE(publisher.should_collect(phy_diagnostic_stream::pusch_symbols, 0x4601));
  // The token was consumed: the same RNTI is rate limited, another RNTI is not.
  EXPECT_FALSE(publisher.should_collect(phy_diagnostic_stream::pusch_symbols, 0x4601));
  EXPECT_TRUE(publisher.should_collect(phy_diagnostic_stream::pusch_symbols, 0x4602));
  EXPECT_FALSE(publisher.should_collect(phy_diagnostic_stream::pusch_crc, 0x4601));
  EXPECT_EQ(publisher.metrics().rate_limited, 1U);
}

TEST(dapp_telemetry_publisher, publishes_pusch_symbols_as_decodable_records)
{
  auto                       store = make_store();
  telemetry_publisher_config config;
  config.store = store;
  auto created = telemetry_publisher::create(config);
  ASSERT_TRUE(created.first) << created.second;
  auto& publisher = *created.first;
  ASSERT_TRUE(store->subscribe(subscribe_request(2, telemetry_stream_pusch_symbols)));

  std::vector<std::complex<float>> symbols(106 * 12);
  for (std::size_t index = 0; index != symbols.size(); ++index) {
    symbols[index] = {static_cast<float>(index), -static_cast<float>(index)};
  }
  pusch_symbols_diagnostic event;
  event.rnti              = 0x4601;
  event.slot              = slot_point{1, 100, 5};
  event.nof_prb           = 106;
  event.nof_data_symbols  = 12;
  event.nof_layers        = 1;
  event.mod_bits          = 6;
  event.sinr_dB           = 18.0F;
  event.epre_dB           = std::numeric_limits<float>::infinity();
  event.equalized_symbols = symbols;
  event.grant_valid       = true;
  event.grant.tbs_bytes   = 4096;
  event.grant.mcs_known   = true;
  event.grant.mcs_index   = 9;
  event.grant.mcs_table   = 0;
  event.grant.nof_symbols = 14;
  event.grant.bwp_size_rb = 106;
  event.grant.prb_mask[5] = 0xa5;
  event.crc_known         = true;
  event.crc_ok            = true;
  publisher.on_pusch_symbols(event);
  EXPECT_EQ(publisher.metrics().published, 1U);

  std::vector<uint8_t>       metadata;
  std::vector<uint8_t>       tensor;
  portable_publication_chunk first;
  ASSERT_TRUE(drain_one(*store, make_id(2), metadata, tensor, first));
  EXPECT_EQ(metadata.size(), sizeof(pusch_symbols_metadata_v2));
  EXPECT_EQ(tensor.size(), symbols.size() * sizeof(std::complex<float>));
  auto indication = e3ap_codec::from_publication(first.interface_id,
                                                 first.cell_id,
                                                 first.publication_sequence,
                                                 first.route_epoch,
                                                 first.dropped_objects,
                                                 metadata,
                                                 tensor);
  ASSERT_TRUE(indication) << indication.detail;
  const auto& record = std::get<pusch_symbols_record>(indication.value->object);
  EXPECT_EQ(record.rnti, 0x4601);
  EXPECT_EQ(record.radio_slot.sfn, 100U);
  EXPECT_EQ(record.radio_slot.slot, 5U);
  EXPECT_EQ(record.nof_prb, 106U);
  ASSERT_TRUE(record.sinr_db);
  EXPECT_FLOAT_EQ(static_cast<float>(*record.sinr_db), 18.0F);
  EXPECT_FALSE(record.epre_db) << "non-finite measurements are omitted";
  EXPECT_EQ(std::memcmp(record.equalized_symbols.data(), symbols.data(), tensor.size()), 0);
  EXPECT_GT(indication.value->monotonic_ns, 0U);
  ASSERT_TRUE(record.grant) << "metadata v2 carries the grant";
  EXPECT_EQ(record.grant->tbs_bytes, 4096U);
  ASSERT_TRUE(record.grant->mcs_index);
  EXPECT_EQ(*record.grant->mcs_index, 9U);
  EXPECT_EQ(record.grant->prb_mask[5], 0xa5U);
  ASSERT_TRUE(record.crc_ok);
  EXPECT_TRUE(*record.crc_ok);
  EXPECT_FALSE(record.symbols_truncated);

  auto encoded = e3ap_codec::encode(*indication.value);
  ASSERT_TRUE(encoded) << encoded.detail;
  auto decoded = e3ap_codec::decode_telemetry(*encoded.value);
  ASSERT_TRUE(decoded) << decoded.detail;
  EXPECT_EQ(std::get<pusch_symbols_record>(decoded.value->object).equalized_symbols, record.equalized_symbols);
}

TEST(dapp_telemetry_publisher, publishes_crc_srs_dmrs_and_json_records)
{
  auto                       store = make_store();
  telemetry_publisher_config config;
  config.store                        = store;
  config.srs_channel_minimum_interval = std::chrono::microseconds(0);
  config.pusch_dmrs_minimum_interval  = std::chrono::microseconds(0);
  auto created                        = telemetry_publisher::create(config);
  ASSERT_TRUE(created.first) << created.second;
  auto& publisher = *created.first;
  ASSERT_TRUE(store->subscribe(subscribe_request(3, telemetry_stream_pusch_crc)));
  ASSERT_TRUE(store->subscribe(subscribe_request(4, telemetry_stream_srs_channel)));
  ASSERT_TRUE(store->subscribe(subscribe_request(5, telemetry_stream_pusch_dmrs_channel)));
  ASSERT_TRUE(store->subscribe(subscribe_request(6, telemetry_stream_gnb_metrics_json)));

  publisher.on_pusch_crc({0x4601, slot_point{1, 3, 4}, false});
  std::vector<std::complex<float>> coefficients(2, {1.0F, 0.5F});
  srs_channel_diagnostic           srs;
  srs.rnti            = 0x4601;
  srs.slot            = slot_point{1, 3, 4};
  srs.nof_subcarriers = 2;
  srs.nof_rx_ports    = 2;
  srs.nof_tx_ports    = 1;
  srs.rx_port_index   = 1;
  srs.rsrp_dB         = -80.0F;
  srs.h_coefficients  = coefficients;
  publisher.on_srs_channel(srs);
  pusch_dmrs_channel_diagnostic dmrs;
  dmrs.rnti        = 0x4601;
  dmrs.slot        = slot_point{1, 3, 4};
  dmrs.nof_prb     = 1;
  dmrs.h_estimates = coefficients;
  publisher.on_pusch_dmrs_channel(dmrs);
  EXPECT_TRUE(publisher.metrics_json_active());
  EXPECT_TRUE(publisher.publish_metrics_json("{\"ok\":true}"));
  EXPECT_FALSE(publisher.publish_metrics_json(""));
  EXPECT_EQ(publisher.metrics().published, 4U);

  std::vector<uint8_t>       metadata;
  std::vector<uint8_t>       tensor;
  portable_publication_chunk first;
  ASSERT_TRUE(drain_one(*store, make_id(3), metadata, tensor, first));
  auto crc = e3ap_codec::from_publication(first.interface_id, 0, 1, first.route_epoch, 0, metadata, tensor);
  ASSERT_TRUE(crc) << crc.detail;
  EXPECT_FALSE(std::get<pusch_crc_record>(crc.value->object).crc_ok);

  ASSERT_TRUE(drain_one(*store, make_id(4), metadata, tensor, first));
  auto channel = e3ap_codec::from_publication(first.interface_id, 0, 1, first.route_epoch, 0, metadata, tensor);
  ASSERT_TRUE(channel) << channel.detail;
  EXPECT_EQ(std::get<srs_channel_record>(channel.value->object).rx_port_index, 1U);
  ASSERT_TRUE(std::get<srs_channel_record>(channel.value->object).rsrp_db);
  EXPECT_EQ(std::get<srs_channel_record>(channel.value->object).coefficients.size(), 16U);

  ASSERT_TRUE(drain_one(*store, make_id(5), metadata, tensor, first));
  auto estimate = e3ap_codec::from_publication(first.interface_id, 0, 1, first.route_epoch, 0, metadata, tensor);
  ASSERT_TRUE(estimate) << estimate.detail;
  EXPECT_EQ(std::get<pusch_dmrs_channel_record>(estimate.value->object).nof_prb, 1U);

  ASSERT_TRUE(drain_one(*store, make_id(6), metadata, tensor, first));
  auto json = e3ap_codec::from_publication(first.interface_id, 0, 1, first.route_epoch, 0, metadata, tensor);
  ASSERT_TRUE(json) << json.detail;
  const auto& text = std::get<gnb_metrics_json_record>(json.value->object).json;
  EXPECT_EQ(std::string(text.begin(), text.end()), "{\"ok\":true}");
}

TEST(dapp_telemetry_publisher, rejects_invalid_configuration_and_empty_events)
{
  EXPECT_FALSE(telemetry_publisher::create({}).first);
  auto                       store = make_store();
  telemetry_publisher_config config;
  config.store                          = store;
  config.pusch_symbols_minimum_interval = std::chrono::seconds(61);
  EXPECT_FALSE(telemetry_publisher::create(config).first);
  config.pusch_symbols_minimum_interval = std::chrono::seconds(0);
  auto created                          = telemetry_publisher::create(config);
  ASSERT_TRUE(created.first) << created.second;
  ASSERT_TRUE(store->subscribe(subscribe_request(7, telemetry_stream_pusch_symbols)));
  pusch_symbols_diagnostic empty;
  empty.rnti = 1;
  created.first->on_pusch_symbols(empty);
  EXPECT_EQ(created.first->metrics().invalid, 1U);
  EXPECT_EQ(created.first->metrics().published, 0U);
}

TEST(dapp_telemetry_publisher, fastest_subscriber_request_sets_the_floor_down_to_the_hard_floor)
{
  auto                       store = make_store();
  telemetry_publisher_config config;
  config.store                        = store;
  config.srs_channel_minimum_interval = std::chrono::milliseconds(20);
  config.srs_channel_hard_floor       = std::chrono::milliseconds(2);
  auto created                        = telemetry_publisher::create(config);
  ASSERT_TRUE(created.first) << created.second;
  auto& publisher = *created.first;

  // A subscriber without a request keeps the default floor: the second capture is rate limited.
  ASSERT_TRUE(store->subscribe(subscribe_request(10, telemetry_stream_srs_channel)));
  EXPECT_TRUE(publisher.should_collect(phy_diagnostic_stream::srs_channel, 0x4601));
  EXPECT_FALSE(publisher.should_collect(phy_diagnostic_stream::srs_channel, 0x4601));

  // A recorder asking for every event lowers the effective floor to the hard floor (2 ms).
  ASSERT_TRUE(store->subscribe(subscribe_request(11, telemetry_stream_srs_channel, 0)));
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  EXPECT_TRUE(publisher.should_collect(phy_diagnostic_stream::srs_channel, 0x4601)) << "3 ms > hard floor";
  EXPECT_FALSE(publisher.should_collect(phy_diagnostic_stream::srs_channel, 0x4601)) << "hard floor still applies";
  auto demand = store->demand(telemetry_stream_srs_channel);
  EXPECT_EQ(demand.active, 2U);
  EXPECT_EQ(demand.unspecified, 1U);
  EXPECT_EQ(demand.fastest_requested_us, 0U);

  // Releasing the recorder restores the default floor.
  ASSERT_TRUE(store->unsubscribe(make_id(11)));
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  EXPECT_FALSE(publisher.should_collect(phy_diagnostic_stream::srs_channel, 0x4601)) << "back to the 20 ms default";
  EXPECT_EQ(store->demand(telemetry_stream_srs_channel).active, 1U);

  // A slower explicit request never raises the floor above the default.
  ASSERT_TRUE(store->subscribe(subscribe_request(12, telemetry_stream_srs_channel, 500'000)));
  std::this_thread::sleep_for(std::chrono::milliseconds(21));
  EXPECT_TRUE(publisher.should_collect(phy_diagnostic_stream::srs_channel, 0x4601));
}

TEST(dapp_telemetry_publisher, hard_floor_bounds_the_stream_across_rntis)
{
  auto                       store = make_store();
  telemetry_publisher_config config;
  config.store                        = store;
  config.srs_channel_minimum_interval = std::chrono::milliseconds(0);
  config.srs_channel_hard_floor       = std::chrono::milliseconds(50);
  auto created                        = telemetry_publisher::create(config);
  ASSERT_TRUE(created.first) << created.second;
  auto& publisher = *created.first;

  ASSERT_TRUE(store->subscribe(subscribe_request(10, telemetry_stream_srs_channel, 0)));

  // The hard floor is a stream-wide real-time budget: after one publication, captures for a *different* RNTI are
  // also rate limited until the floor elapses.
  EXPECT_TRUE(publisher.should_collect(phy_diagnostic_stream::srs_channel, 0x4601));
  EXPECT_FALSE(publisher.should_collect(phy_diagnostic_stream::srs_channel, 0x4602)) << "hard floor is aggregate";
  EXPECT_FALSE(publisher.should_collect(phy_diagnostic_stream::srs_channel, 0x4601));
  std::this_thread::sleep_for(std::chrono::milliseconds(55));
  EXPECT_TRUE(publisher.should_collect(phy_diagnostic_stream::srs_channel, 0x4602));
}

TEST(dapp_telemetry_publisher, publishes_raw_grid_captures_with_grant_parameters)
{
  auto                       store = make_store();
  telemetry_publisher_config config;
  config.store                       = store;
  config.pusch_grid_minimum_interval = std::chrono::microseconds(0);
  auto created                       = telemetry_publisher::create(config);
  ASSERT_TRUE(created.first) << created.second;
  auto& publisher = *created.first;
  EXPECT_FALSE(publisher.should_collect(phy_diagnostic_stream::pusch_grid, 0x4601));
  ASSERT_TRUE(store->subscribe(subscribe_request(20, telemetry_stream_pusch_grid)));
  EXPECT_TRUE(publisher.should_collect(phy_diagnostic_stream::pusch_grid, 0x4601));

  std::vector<std::complex<float>> res(2 * 14 * 8 * 12, {0.5F, -0.5F});
  pusch_grid_diagnostic            event;
  event.rnti              = 0x4601;
  event.slot              = slot_point{1, 20, 3};
  event.nof_prb           = 8;
  event.nof_rx_ports      = 2;
  event.start_symbol      = 0;
  event.nof_symbols       = 14;
  event.grant.tbs_bytes   = 777;
  event.grant.nof_symbols = 14;
  event.grant.bwp_size_rb = 106;
  event.resource_elements = res;
  publisher.on_pusch_grid(event);
  EXPECT_EQ(publisher.metrics().published, 1U);

  std::vector<uint8_t>       metadata;
  std::vector<uint8_t>       tensor;
  portable_publication_chunk first;
  ASSERT_TRUE(drain_one(*store, make_id(20), metadata, tensor, first));
  EXPECT_EQ(metadata.size(), sizeof(pusch_grid_metadata_v1));
  auto indication = e3ap_codec::from_publication(first.interface_id, 0, 1, first.route_epoch, 0, metadata, tensor);
  ASSERT_TRUE(indication) << indication.detail;
  const auto& record = std::get<pusch_grid_record>(indication.value->object);
  EXPECT_EQ(record.nof_rx_ports, 2U);
  EXPECT_EQ(record.grant.tbs_bytes, 777U);
  EXPECT_EQ(record.resource_elements.size(), res.size() * sizeof(std::complex<float>));
  auto encoded = e3ap_codec::encode(*indication.value);
  ASSERT_TRUE(encoded) << encoded.detail;
  auto decoded = e3ap_codec::decode_telemetry(*encoded.value);
  ASSERT_TRUE(decoded) << decoded.detail;
  EXPECT_EQ(std::get<pusch_grid_record>(decoded.value->object).grant.tbs_bytes, 777U);
}
