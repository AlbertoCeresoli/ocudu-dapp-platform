// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/portable_data_store.h"
#include "ocudu/dapp/e3/telemetry_types.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <tuple>
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

TEST(portable_data_store_test, fanout_copy_budget_drops_whole_objects_and_rotates_admission)
{
  auto config                           = test_config();
  config.maximum_publication_copy_bytes = 1040;
  auto [store, detail]                  = portable_data_store::create(config);
  ASSERT_NE(store, nullptr) << detail;
  ASSERT_TRUE(store->subscribe(request(1)));
  ASSERT_TRUE(store->subscribe(request(2)));
  const std::vector<uint8_t> metadata(16, 7), tensor(1024, 9);
  EXPECT_EQ(store->publish(publication(1, metadata, tensor)), portable_publish_result::published);
  auto first  = store->poll(make_id(1), 8);
  auto second = store->poll(make_id(2), 8);
  ASSERT_EQ(first.chunks.size(), 1U);
  EXPECT_TRUE(second.chunks.empty());
  EXPECT_EQ(first.chunks[0].tensor_chunk, tensor);
  EXPECT_EQ(store->publish(publication(2, metadata, tensor)), portable_publish_result::published);
  EXPECT_TRUE(store->poll(make_id(1), 8).chunks.empty());
  EXPECT_EQ(store->poll(make_id(2), 8).chunks.size(), 1U);
}

TEST(portable_data_store_test, concurrent_removal_and_replacement_do_not_reclaim_a_borrowed_producer_slot)
{
  auto [store, detail] = portable_data_store::create(test_config());
  ASSERT_NE(store, nullptr) << detail;
  ASSERT_TRUE(store->subscribe(request(1)));
  const std::vector<uint8_t> metadata(16, 1), tensor(1024, 2);
  std::atomic<bool>          stop{false};
  std::thread                producer([&, &producer_store = store] {
    uint64_t sequence = 1;
    while (!stop.load(std::memory_order_relaxed)) {
      producer_store->publish(publication(sequence++, metadata, tensor));
    }
  });
  for (unsigned iteration = 0; iteration != 200; ++iteration) {
    EXPECT_TRUE(store->unsubscribe(make_id(1)));
    EXPECT_TRUE(store->subscribe(request(1)));
  }
  stop.store(true, std::memory_order_relaxed);
  producer.join();
  EXPECT_TRUE(store->unsubscribe(make_id(1)));
  EXPECT_FALSE(store->has_active(OCUDU_DAPP_INTERFACE_SPECTRUM_V1));
}

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

// [srs-sharing] Resource-grid region filter: slot rule, box crop, clipping, chunk-split gather and refusals.
namespace {

/// Full grid [port][symbol][subcarrier] of 4-byte elements whose value encodes its own coordinates.
std::vector<uint8_t> coordinate_grid(uint32_t ports, uint32_t symbols, uint32_t subcarriers)
{
  std::vector<uint8_t> bytes(static_cast<std::size_t>(ports) * symbols * subcarriers * 4);
  for (uint32_t port = 0; port != ports; ++port) {
    for (uint32_t symbol = 0; symbol != symbols; ++symbol) {
      for (uint32_t subcarrier = 0; subcarrier != subcarriers; ++subcarrier) {
        const uint32_t value = (port << 24U) | (symbol << 16U) | subcarrier;
        std::memcpy(&bytes[((static_cast<std::size_t>(port) * symbols + symbol) * subcarriers + subcarrier) * 4],
                    &value,
                    sizeof(value));
      }
    }
  }
  return bytes;
}

/// Expected crop of coordinate_grid(), built independently of the store's gather.
std::vector<uint8_t> expected_window(uint32_t                    ports,
                                     uint32_t                    symbols,
                                     uint32_t                    subcarriers,
                                     const portable_grid_window& window)
{
  const auto           full = coordinate_grid(ports, symbols, subcarriers);
  std::vector<uint8_t> result;
  for (uint32_t port = 0; port != ports; ++port) {
    for (uint32_t symbol = window.first_symbol; symbol != window.first_symbol + window.nof_symbols; ++symbol) {
      const auto begin =
          full.begin() + ((static_cast<std::size_t>(port) * symbols + symbol) * subcarriers + window.first_subcarrier) * 4;
      result.insert(result.end(), begin, begin + static_cast<std::ptrdiff_t>(window.nof_subcarriers) * 4);
    }
  }
  return result;
}

portable_grid_layout grid_layout(uint16_t sfn, uint16_t slot, uint32_t ports, uint32_t subcarriers)
{
  return {sfn, slot, 1, ports, 14, subcarriers, 4};
}

portable_data_store_config grid_config()
{
  auto config                   = test_config();
  config.maximum_retained_bytes = 16 * 1024 * 1024;
  config.maximum_object_bytes   = 4 * 1024 * 1024;
  config.maximum_chunk_bytes    = 4096;
  return config;
}

portable_subscription_request grid_request(uint8_t suffix, portable_grid_region region)
{
  auto result                = request(suffix, portable_drop_policy::drop_oldest);
  result.queue_depth_chunks  = 64;
  result.maximum_chunk_bytes = 4096;
  result.grid_region         = region;
  return result;
}

std::vector<uint8_t> reassemble(const std::vector<portable_publication_chunk>& chunks)
{
  std::vector<uint8_t> tensor;
  for (const auto& chunk : chunks) {
    tensor.insert(tensor.end(), chunk.tensor_chunk.begin(), chunk.tensor_chunk.end());
  }
  return tensor;
}

} // namespace

TEST(portable_data_store_grid_region, delivers_only_matching_slots_and_only_the_box)
{
  auto [store, detail] = portable_data_store::create(grid_config());
  ASSERT_NE(store, nullptr) << detail;
  // A's live SRS resource: period 10 slots, offset 7, symbol 13, PRBs 5..44 of a 51-PRB grid, two rx ports.
  ASSERT_TRUE(store->subscribe(grid_request(1, {10, 7, 13, 1, 5, 40})));
  const std::vector<uint8_t> metadata(16, 7);
  const auto                 grid = coordinate_grid(2, 14, 612);

  auto source = publication(1, metadata, grid);
  source.grid = grid_layout(100, 8, 2, 612); // absolute slot 2008: 2008 % 10 = 8, not an occasion
  EXPECT_EQ(store->publish(source), portable_publish_result::sampled);
  auto skipped = store->poll(make_id(1), 8);
  EXPECT_TRUE(skipped.chunks.empty());
  EXPECT_EQ(skipped.subscription->sampled_objects, 1U);

  source      = publication(2, metadata, grid);
  source.grid = grid_layout(100, 7, 2, 612); // absolute slot 2007: an occasion
  EXPECT_EQ(store->publish(source), portable_publish_result::published);
  auto polled = store->poll(make_id(1), 8);
  ASSERT_EQ(polled.chunks.size(), 1U);
  ASSERT_TRUE(polled.chunks[0].grid_window.has_value());
  const portable_grid_window window = *polled.chunks[0].grid_window;
  EXPECT_EQ(window.first_symbol, 13U);
  EXPECT_EQ(window.nof_symbols, 1U);
  EXPECT_EQ(window.first_subcarrier, 60U);
  EXPECT_EQ(window.nof_subcarriers, 480U);
  EXPECT_EQ(polled.chunks[0].tensor_size, 2U * 480U * 4U);
  EXPECT_EQ(polled.chunks[0].metadata, metadata);
  EXPECT_EQ(polled.chunks[0].tensor_chunk, expected_window(2, 14, 612, window));
  EXPECT_EQ(polled.subscription->published_tensor_bytes, 2U * 480U * 4U);
}

TEST(portable_data_store_grid_region, splits_rows_across_chunks_and_clips_the_box_to_the_grid)
{
  auto [store, detail] = portable_data_store::create(grid_config());
  ASSERT_NE(store, nullptr) << detail;
  // 14 symbols x PRBs 30..59 on a 51-PRB grid: clipped to PRBs 30..50; rows of 1008 B straddle 4 KiB chunks.
  ASSERT_TRUE(store->subscribe(grid_request(1, {1, 0, 0, 14, 30, 30})));
  // A plain subscriber on the same stream still gets the full grid.
  auto plain               = request(2, portable_drop_policy::drop_oldest);
  plain.queue_depth_chunks = 64;
  plain.maximum_chunk_bytes = 4096;
  ASSERT_TRUE(store->subscribe(plain));
  const std::vector<uint8_t> metadata(16, 7);
  const auto                 grid   = coordinate_grid(2, 14, 612);
  auto                       source = publication(1, metadata, grid);
  source.grid                       = grid_layout(0, 0, 2, 612);
  EXPECT_EQ(store->publish(source), portable_publish_result::published);

  std::vector<portable_publication_chunk> chunks;
  for (int poll = 0; poll != 16; ++poll) {
    auto polled = store->poll(make_id(1), 8);
    chunks.insert(chunks.end(), polled.chunks.begin(), polled.chunks.end());
  }
  const portable_grid_window expected_box{0, 14, 360, 252};
  ASSERT_FALSE(chunks.empty());
  EXPECT_EQ(chunks.size(), chunks[0].chunk_count);
  EXPECT_GT(chunks.size(), 1U);
  for (const auto& chunk : chunks) {
    ASSERT_TRUE(chunk.grid_window.has_value());
    EXPECT_EQ(chunk.grid_window->first_subcarrier, expected_box.first_subcarrier);
    EXPECT_EQ(chunk.grid_window->nof_subcarriers, expected_box.nof_subcarriers);
  }
  EXPECT_EQ(reassemble(chunks), expected_window(2, 14, 612, expected_box));

  std::vector<portable_publication_chunk> full;
  for (int poll = 0; poll != 16; ++poll) {
    auto polled = store->poll(make_id(2), 8);
    full.insert(full.end(), polled.chunks.begin(), polled.chunks.end());
  }
  ASSERT_FALSE(full.empty());
  EXPECT_FALSE(full[0].grid_window.has_value());
  EXPECT_EQ(reassemble(full), grid);
}

TEST(portable_data_store_grid_region, slot_rule_holds_across_the_sfn_wrap)
{
  auto [store, detail] = portable_data_store::create(grid_config());
  ASSERT_NE(store, nullptr) << detail;
  ASSERT_TRUE(store->subscribe(grid_request(1, {20, 19, 0, 1, 0, 1})));
  const std::vector<uint8_t> metadata(16, 7);
  const auto                 grid = coordinate_grid(1, 14, 12);
  uint64_t                   sequence = 1;
  for (const auto& [sfn, slot, expected] : std::vector<std::tuple<uint16_t, uint16_t, bool>>{
           {1023, 19, true}, {0, 19, true}, {0, 0, false}, {511, 19, true}, {1023, 18, false}}) {
    auto source = publication(sequence++, metadata, grid);
    source.grid = grid_layout(sfn, slot, 1, 12);
    EXPECT_EQ(store->publish(source) == portable_publish_result::published, expected) << sfn << "." << slot;
  }
}

TEST(portable_data_store_grid_region, refuses_bad_regions_and_publications_without_a_layout)
{
  auto [store, detail] = portable_data_store::create(grid_config());
  ASSERT_NE(store, nullptr) << detail;
  auto telemetry         = grid_request(1, {1, 0, 0, 14, 0, 1});
  telemetry.interface_id = telemetry_stream_pusch_grid;
  EXPECT_FALSE(store->subscribe(telemetry)) << "regions are Class-C Spectrum only";
  EXPECT_FALSE(store->subscribe(grid_request(2, {10, 10, 0, 1, 0, 1}))) << "offset must be below the period";
  EXPECT_FALSE(store->subscribe(grid_request(3, {1, 0, 13, 2, 0, 1}))) << "symbols past the slot";
  EXPECT_FALSE(store->subscribe(grid_request(4, {1, 0, 0, 1, 270, 6}))) << "PRBs past 275";
  EXPECT_FALSE(store->subscribe(grid_request(5, {0, 0, 0, 1, 0, 1}))) << "zero period";

  ASSERT_TRUE(store->subscribe(grid_request(6, {1, 0, 0, 1, 0, 1})));
  const std::vector<uint8_t> metadata(16, 7);
  const auto                 grid = coordinate_grid(1, 14, 12);
  EXPECT_EQ(store->publish(publication(1, metadata, grid)), portable_publish_result::invalid) << "no layout";
  auto source = publication(2, metadata, grid);
  source.grid = grid_layout(0, 0, 2, 12); // claims two ports: does not match the tensor size
  EXPECT_EQ(store->publish(source), portable_publish_result::invalid);
  auto polled = store->poll(make_id(6), 8);
  EXPECT_TRUE(polled.chunks.empty());
  EXPECT_EQ(polled.subscription->dropped_objects, 2U);
}
