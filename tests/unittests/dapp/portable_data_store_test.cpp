// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/portable_data_store.h"
#include "ocudu/dapp/e3/telemetry_types.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <numeric>
#include <thread>

using namespace ocudu;
using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;

namespace {

management::identifier make_id(uint8_t suffix)
{
  management::identifier result;
  result.bytes[0]  = 0xa5;
  result.bytes[15] = suffix;
  return result;
}

portable_data_store_config test_config()
{
  portable_data_store_config config;
  config.maximum_subscriptions  = 4;
  config.maximum_retained_bytes = 1024 * 1024;
  config.maximum_object_bytes   = 2048;
  config.maximum_metadata_bytes = 1024;
  config.minimum_chunk_bytes    = 1024;
  config.maximum_chunk_bytes    = 1024;
  config.minimum_lease_us       = 1;
  config.maximum_lease_us       = 1'000'000;
  return config;
}

portable_subscription_request request(uint8_t suffix, portable_drop_policy policy = portable_drop_policy::drop_newest)
{
  portable_subscription_request result;
  result.subscription_id     = make_id(suffix);
  result.interface_id        = OCUDU_DAPP_INTERFACE_SPECTRUM_V1;
  result.queue_depth_chunks  = 2;
  result.maximum_chunk_bytes = 1024;
  result.lease_us            = 1'000'000;
  result.drop_policy         = policy;
  return result;
}

portable_publication_source publication(uint64_t                    sequence,
                                        const std::vector<uint8_t>& metadata,
                                        const std::vector<uint8_t>& tensor,
                                        uint32_t                    cell_id = 0)
{
  return {OCUDU_DAPP_INTERFACE_SPECTRUM_V1, cell_id, sequence, 17, metadata, tensor};
}

} // namespace

TEST(portable_data_store_test, rejects_invalid_bounds_and_subscriptions)
{
  auto invalid                  = test_config();
  invalid.maximum_subscriptions = 0;
  EXPECT_FALSE(portable_data_store::create(invalid).first);

  auto [store, detail] = portable_data_store::create(test_config());
  ASSERT_NE(store, nullptr) << detail;
  auto invalid_request            = request(1);
  invalid_request.subscription_id = {};
  EXPECT_EQ(store->subscribe(invalid_request).code, portable_subscription_code::invalid_request);
  invalid_request              = request(1);
  invalid_request.interface_id = 0x1234;
  EXPECT_EQ(store->subscribe(invalid_request).code, portable_subscription_code::invalid_request);
  invalid_request                    = request(1);
  invalid_request.queue_depth_chunks = 0;
  EXPECT_EQ(store->subscribe(invalid_request).code, portable_subscription_code::invalid_request);

  // A one-chunk queue is admitted; objects that need more chunks are dropped
  // as too large rather than refused at subscription time.
  auto shallow               = request(2);
  shallow.queue_depth_chunks = 1;
  ASSERT_TRUE(store->subscribe(shallow));
  const std::vector<uint8_t> metadata(16, 0x11);
  const std::vector<uint8_t> small(512, 0x22);
  const std::vector<uint8_t> large(2048, 0x33);
  EXPECT_EQ(store->publish(publication(1, metadata, small)), portable_publish_result::published);
  EXPECT_EQ(store->publish(publication(2, metadata, large)), portable_publish_result::object_too_large);
  auto polled = store->poll(make_id(2), 8);
  ASSERT_TRUE(polled);
  ASSERT_EQ(polled.chunks.size(), 1U);
  EXPECT_EQ(polled.chunks[0].tensor_chunk, small);
}

TEST(portable_data_store_test, chunks_and_reassembles_one_complete_publication)
{
  auto [store, detail] = portable_data_store::create(test_config());
  ASSERT_NE(store, nullptr) << detail;
  ASSERT_TRUE(store->subscribe(request(1)));
  EXPECT_TRUE(store->has_active(OCUDU_DAPP_INTERFACE_SPECTRUM_V1));

  std::vector<uint8_t> metadata{1, 2, 3, 4};
  std::vector<uint8_t> tensor(1536);
  std::iota(tensor.begin(), tensor.end(), uint8_t{});
  EXPECT_EQ(store->publish(publication(9, metadata, tensor)), portable_publish_result::published);

  auto polled = store->poll(make_id(1), 8);
  ASSERT_TRUE(polled) << polled.detail;
  ASSERT_EQ(polled.chunks.size(), 2U);
  EXPECT_EQ(polled.chunks[0].metadata, metadata);
  EXPECT_TRUE(polled.chunks[1].metadata.empty());
  EXPECT_EQ(polled.chunks[0].chunk_index, 0U);
  EXPECT_EQ(polled.chunks[1].chunk_index, 1U);
  EXPECT_EQ(polled.chunks[0].chunk_count, 2U);
  EXPECT_EQ(polled.chunks[0].tensor_size, tensor.size());
  std::vector<uint8_t> restored = polled.chunks[0].tensor_chunk;
  restored.insert(restored.end(), polled.chunks[1].tensor_chunk.begin(), polled.chunks[1].tensor_chunk.end());
  EXPECT_EQ(restored, tensor);
  ASSERT_TRUE(polled.subscription);
  EXPECT_EQ(polled.subscription->published_objects, 1U);
  EXPECT_EQ(polled.subscription->published_tensor_bytes, tensor.size());
  EXPECT_EQ(polled.subscription->queued_chunks, 0U);
}

TEST(portable_data_store_test, sampling_and_cell_filter_do_not_enqueue)
{
  auto [store, detail] = portable_data_store::create(test_config());
  ASSERT_NE(store, nullptr) << detail;
  auto selected           = request(2);
  selected.cell_id        = 3;
  selected.sample_every_n = 2;
  ASSERT_TRUE(store->subscribe(selected));
  const std::vector<uint8_t> metadata{1};
  const std::vector<uint8_t> tensor(1024, 7);

  EXPECT_EQ(store->publish(publication(1, metadata, tensor, 2)), portable_publish_result::no_subscription);
  EXPECT_EQ(store->publish(publication(2, metadata, tensor, 3)), portable_publish_result::published);
  EXPECT_EQ(store->publish(publication(3, metadata, tensor, 3)), portable_publish_result::sampled);
  auto polled = store->poll(make_id(2), 8);
  ASSERT_TRUE(polled);
  ASSERT_EQ(polled.chunks.size(), 1U);
  EXPECT_EQ(polled.chunks.front().publication_sequence, 2U);
  EXPECT_EQ(polled.subscription->sampled_objects, 1U);
}

TEST(portable_data_store_test, drop_policies_preserve_whole_object_boundaries)
{
  const std::vector<uint8_t> metadata{1};
  const std::vector<uint8_t> tensor(1024, 7);
  for (const auto policy :
       {portable_drop_policy::drop_newest, portable_drop_policy::drop_oldest, portable_drop_policy::coalesce_latest}) {
    auto [store, detail] = portable_data_store::create(test_config());
    ASSERT_NE(store, nullptr) << detail;
    ASSERT_TRUE(store->subscribe(request(3, policy)));
    EXPECT_EQ(store->publish(publication(1, metadata, tensor)), portable_publish_result::published);
    EXPECT_EQ(store->publish(publication(2, metadata, tensor)), portable_publish_result::published);
    const auto third = store->publish(publication(3, metadata, tensor));
    EXPECT_EQ(third,
              policy == portable_drop_policy::drop_newest ? portable_publish_result::full
                                                          : portable_publish_result::published);
    auto polled = store->poll(make_id(3), 8);
    ASSERT_TRUE(polled);
    if (policy == portable_drop_policy::coalesce_latest) {
      ASSERT_EQ(polled.chunks.size(), 1U);
      EXPECT_EQ(polled.chunks[0].publication_sequence, 3U);
    } else if (policy == portable_drop_policy::drop_newest) {
      ASSERT_EQ(polled.chunks.size(), 2U);
      EXPECT_EQ(polled.chunks[0].publication_sequence, 1U);
      EXPECT_EQ(polled.chunks[1].publication_sequence, 2U);
    } else {
      ASSERT_EQ(polled.chunks.size(), 2U);
      EXPECT_EQ(polled.chunks[0].publication_sequence, 2U);
      EXPECT_EQ(polled.chunks[1].publication_sequence, 3U);
    }
    EXPECT_EQ(polled.subscription->dropped_objects, policy == portable_drop_policy::coalesce_latest ? 2U : 1U);
  }
}

TEST(portable_data_store_test, unsubscribe_and_expiry_disable_the_fast_predicate)
{
  auto [store, detail] = portable_data_store::create(test_config());
  ASSERT_NE(store, nullptr) << detail;
  auto short_lived     = request(4);
  short_lived.lease_us = 1;
  ASSERT_TRUE(store->subscribe(short_lived));
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const std::vector<uint8_t> metadata{1};
  const std::vector<uint8_t> tensor(1024, 7);
  EXPECT_EQ(store->publish(publication(1, metadata, tensor)), portable_publish_result::no_subscription);
  EXPECT_FALSE(store->has_active(OCUDU_DAPP_INTERFACE_SPECTRUM_V1));

  ASSERT_TRUE(store->unsubscribe(make_id(4)));
  short_lived.lease_us = 1'000'000;
  ASSERT_TRUE(store->subscribe(short_lived));
  EXPECT_TRUE(store->has_active(OCUDU_DAPP_INTERFACE_SPECTRUM_V1));
  EXPECT_TRUE(store->unsubscribe(make_id(4)));
  EXPECT_FALSE(store->has_active(OCUDU_DAPP_INTERFACE_SPECTRUM_V1));
  EXPECT_EQ(store->poll(make_id(4), 1).code, portable_subscription_code::not_found);
}

TEST(portable_data_store_test, a_new_subscription_reclaims_an_expired_slot)
{
  auto config                  = test_config();
  config.maximum_subscriptions = 1;
  auto [store, detail]         = portable_data_store::create(config);
  ASSERT_NE(store, nullptr) << detail;
  auto short_lived     = request(7);
  short_lived.lease_us = 1;
  ASSERT_TRUE(store->subscribe(short_lived));
  std::this_thread::sleep_for(std::chrono::milliseconds(2));

  auto replacement     = request(8);
  replacement.lease_us = 1'000'000;
  ASSERT_TRUE(store->subscribe(replacement));
  EXPECT_EQ(store->poll(make_id(7), 1).code, portable_subscription_code::not_found);
  EXPECT_TRUE(store->has_active(OCUDU_DAPP_INTERFACE_SPECTRUM_V1));
}

TEST(portable_data_store_test, concurrent_producer_and_poller_never_expose_partial_chunks)
{
  auto config                 = test_config();
  config.maximum_object_bytes = 1024;
  auto  created               = portable_data_store::create(config);
  auto& store                 = created.first;
  auto& detail                = created.second;
  ASSERT_NE(store, nullptr) << detail;
  auto subscribed               = request(5, portable_drop_policy::drop_oldest);
  subscribed.queue_depth_chunks = 8;
  ASSERT_TRUE(store->subscribe(subscribed));
  const std::vector<uint8_t> metadata{9, 8, 7};
  const std::vector<uint8_t> tensor(1024, 0x5a);
  std::atomic<bool>          done{};
  std::thread                producer([&] {
    for (uint64_t sequence = 1; sequence != 1001; ++sequence) {
      static_cast<void>(store->publish(publication(sequence, metadata, tensor)));
    }
    done.store(true, std::memory_order_release);
  });
  uint64_t                   last{};
  do {
    auto polled = store->poll(make_id(5), 8);
    if (!polled) {
      EXPECT_EQ(polled.code, portable_subscription_code::busy);
      continue;
    }
    for (const auto& chunk : polled.chunks) {
      EXPECT_EQ(chunk.chunk_index, 0U);
      EXPECT_EQ(chunk.chunk_count, 1U);
      EXPECT_EQ(chunk.metadata, metadata);
      EXPECT_EQ(chunk.tensor_chunk, tensor);
      EXPECT_GT(chunk.publication_sequence, last);
      last = chunk.publication_sequence;
    }
  } while (!done.load(std::memory_order_acquire));
  producer.join();
}

TEST(portable_data_store_test, concurrent_expiry_renewal_and_removal_preserve_the_active_count)
{
  auto config                 = test_config();
  config.maximum_object_bytes = 1024;
  auto  created               = portable_data_store::create(config);
  auto& store                 = created.first;
  auto& detail                = created.second;
  ASSERT_NE(store, nullptr) << detail;
  auto short_lived               = request(6);
  short_lived.queue_depth_chunks = 4;
  short_lived.lease_us           = 1;
  ASSERT_TRUE(store->subscribe(short_lived));
  const std::vector<uint8_t> metadata{1};
  const std::vector<uint8_t> tensor(1024, 7);

  std::thread renewer([&] {
    for (unsigned count = 0; count != 1000; ++count) {
      EXPECT_TRUE(store->subscribe(short_lived));
    }
  });
  std::thread expirer([&] {
    for (uint64_t sequence = 1; sequence != 1001; ++sequence) {
      static_cast<void>(store->publish(publication(sequence, metadata, tensor)));
    }
  });
  renewer.join();
  expirer.join();

  ASSERT_TRUE(store->unsubscribe(make_id(6)));
  EXPECT_FALSE(store->has_active(OCUDU_DAPP_INTERFACE_SPECTRUM_V1));
}

TEST(dapp_portable_data_store, tracks_every_telemetry_interface_including_the_newest)
{
  // Regression: the per-interface tables were sized to the first five telemetry
  // streams, so a pusch.grid subscription indexed past the end of the arrays.
  auto created = portable_data_store::create(test_config());
  ASSERT_TRUE(created.first) << created.second;
  auto& store = *created.first;
  for (uint32_t interface :
       {telemetry_stream_pusch_symbols, telemetry_stream_pusch_grid, telemetry_stream_gnb_metrics_json}) {
    auto request                      = ::request(1, portable_drop_policy::drop_oldest);
    request.subscription_id.bytes[15] = static_cast<uint8_t>(interface & 0xff);
    request.interface_id              = interface;
    request.requested_interval_us     = 5'000;
    ASSERT_TRUE(store.subscribe(request)) << "interface " << interface;
    EXPECT_TRUE(store.has_active(interface));
    const auto demand = store.demand(interface);
    EXPECT_EQ(demand.active, 1U);
    EXPECT_EQ(demand.unspecified, 0U);
    EXPECT_EQ(demand.fastest_requested_us, 5'000U);
  }
  EXPECT_FALSE(store.has_active(telemetry_stream_pusch_crc));
  EXPECT_EQ(store.demand(telemetry_stream_pusch_crc).active, 0U);
}
