// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/ipc/publication_route.h"
#include "ocudu/dapp/ipc/seqpacket_channel.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include <array>
#include <cstring>
#include <gtest/gtest.h>

using namespace ocudu::dapp::ipc;
using namespace std::chrono_literals;

TEST(dapp_publication_route, wakes_consumer_and_preserves_zero_copy_lease)
{
  auto producer = publication_producer::create(2, 32);
  ASSERT_TRUE(producer) << producer.detail;
  auto capabilities = producer.producer->export_capabilities();
  ASSERT_TRUE(capabilities);
  auto consumer = publication_consumer::attach(
      capabilities.ring.value(), capabilities.notification.value(), capabilities.completion.value());
  ASSERT_TRUE(consumer) << consumer.detail;

  const std::array<uint8_t, 4> payload{1, 2, 3, 4};
  EXPECT_EQ(producer.producer->publish(payload, 9), publish_result::published);
  EXPECT_TRUE(consumer.consumer->wait(100ms));
  EXPECT_EQ(consumer.consumer->drain_notifications(), 1U);
  auto lease = consumer.consumer->try_acquire();
  ASSERT_TRUE(lease);
  EXPECT_EQ(lease->flags(), 9U);
  EXPECT_EQ(std::memcmp(lease->payload().data(), payload.data(), payload.size()), 0);
}

TEST(dapp_publication_route, reports_backpressure_and_invalid_payload)
{
  auto producer = publication_producer::create(2, 4);
  ASSERT_TRUE(producer);
  const std::array<uint8_t, 4> valid{};
  const std::array<uint8_t, 5> oversized{};
  EXPECT_EQ(producer.producer->publish(valid), publish_result::published);
  EXPECT_EQ(producer.producer->publish(valid), publish_result::published);
  EXPECT_EQ(producer.producer->publish(valid), publish_result::full);
  EXPECT_EQ(producer.producer->publish(oversized), publish_result::invalid_payload);
}

TEST(dapp_publication_route, publishes_fragments_and_alignment_padding_directly_into_the_ring)
{
  auto producer = publication_producer::create(2, 16);
  ASSERT_TRUE(producer);
  EXPECT_EQ(producer.producer->payload_capacity(), 16U);
  EXPECT_EQ(producer.producer->route_epoch(), 1U);
  auto capabilities = producer.producer->export_capabilities();
  auto consumer     = publication_consumer::attach(
      capabilities.ring.value(), capabilities.notification.value(), capabilities.completion.value());
  ASSERT_TRUE(consumer);

  const std::array<uint8_t, 3>          header{1, 2, 3};
  const std::array<uint8_t, 2>          tensor{4, 5};
  const std::array<payload_fragment, 2> fragments{{{header, 3}, {tensor, 0}}};
  ASSERT_EQ(producer.producer->publish(fragments), publish_result::published);
  ASSERT_TRUE(consumer.consumer->wait(100ms));
  consumer.consumer->drain_notifications();
  auto lease = consumer.consumer->try_acquire();
  ASSERT_TRUE(lease);
  const std::array<uint8_t, 8> expected{1, 2, 3, 0, 0, 0, 4, 5};
  EXPECT_EQ(lease->payload().size(), expected.size());
  EXPECT_EQ(std::memcmp(lease->payload().data(), expected.data(), expected.size()), 0);

  const std::array<payload_fragment, 1> oversized{{{header, 14}}};
  EXPECT_EQ(producer.producer->publish(oversized), publish_result::invalid_payload);
}

TEST(dapp_publication_route, capabilities_transfer_over_authenticated_channel)
{
  auto producer = publication_producer::create(2, 16);
  auto channel  = seqpacket_channel::create_pair();
  ASSERT_TRUE(producer && channel);
  auto                                                  capabilities = producer.producer->export_capabilities();
  std::array<int, OCUDU_DAPP_ROUTE_DESCRIPTOR_COUNT_V1> descriptors{
      capabilities.ring.value(), capabilities.notification.value(), capabilities.completion.value()};
  ocudu_dapp_route_offer_v1 offer{};
  offer.struct_size          = sizeof(offer);
  offer.interface_id         = OCUDU_DAPP_INTERFACE_SPECTRUM_V1;
  offer.route_id             = 7;
  offer.producer_shard_count = 1;
  ASSERT_TRUE(channel.first->send(
      OCUDU_DAPP_CONTROL_ROUTE_OFFER_V1, 1, {reinterpret_cast<const uint8_t*>(&offer), sizeof(offer)}, descriptors));
  auto packet = channel.second->receive();
  ASSERT_TRUE(packet);
  ASSERT_EQ(packet.packet->type, OCUDU_DAPP_CONTROL_ROUTE_OFFER_V1);
  ASSERT_EQ(packet.packet->payload.size(), sizeof(offer));
  ASSERT_EQ(packet.packet->descriptors.size(), OCUDU_DAPP_ROUTE_DESCRIPTOR_COUNT_V1);
  auto consumer =
      publication_consumer::attach(packet.packet->descriptors[OCUDU_DAPP_ROUTE_DESCRIPTOR_DATA_V1].value(),
                                   packet.packet->descriptors[OCUDU_DAPP_ROUTE_DESCRIPTOR_NOTIFICATION_V1].value(),
                                   packet.packet->descriptors[OCUDU_DAPP_ROUTE_DESCRIPTOR_COMPLETION_V1].value());
  ASSERT_TRUE(consumer);

  ocudu_dapp_route_ready_v1 ready{};
  ready.struct_size          = sizeof(ready);
  ready.interface_id         = offer.interface_id;
  ready.route_generation     = 1;
  ready.producer_shard_count = 1;
  ASSERT_TRUE(channel.second->send(
      OCUDU_DAPP_CONTROL_ROUTE_READY_V1, 1, {reinterpret_cast<const uint8_t*>(&ready), sizeof(ready)}));
  auto acknowledged = channel.first->receive();
  ASSERT_TRUE(acknowledged);
  EXPECT_EQ(acknowledged.packet->type, OCUDU_DAPP_CONTROL_ROUTE_READY_V1);
  EXPECT_EQ(acknowledged.packet->sequence, 1U);
  EXPECT_EQ(acknowledged.packet->payload.size(), sizeof(ready));
  EXPECT_TRUE(acknowledged.packet->descriptors.empty());

  const std::array<uint8_t, 1> payload{42};
  ASSERT_EQ(producer.producer->publish(payload), publish_result::published);
  ASSERT_TRUE(consumer.consumer->wait(100ms));
  EXPECT_EQ(consumer.consumer->drain_notifications(), 1U);
  ASSERT_TRUE(consumer.consumer->try_acquire());
}

TEST(dapp_publication_route, restart_discards_stale_publications_and_wakeups)
{
  auto producer = publication_producer::create(2, 16);
  ASSERT_TRUE(producer);
  auto capabilities = producer.producer->export_capabilities();
  ASSERT_TRUE(capabilities);

  const std::array<uint8_t, 1> stale{1};
  ASSERT_EQ(producer.producer->publish(stale), publish_result::published);
  ASSERT_TRUE(producer.producer->reset_after_consumer_restart());
  EXPECT_EQ(producer.producer->route_epoch(), 2U);

  auto consumer = publication_consumer::attach(
      capabilities.ring.value(), capabilities.notification.value(), capabilities.completion.value());
  ASSERT_TRUE(consumer);
  EXPECT_FALSE(consumer.consumer->wait(0ms));
  EXPECT_FALSE(consumer.consumer->try_acquire());

  const std::array<uint8_t, 1> current{2};
  ASSERT_EQ(producer.producer->publish(current), publish_result::published);
  ASSERT_TRUE(consumer.consumer->wait(100ms));
  EXPECT_EQ(consumer.consumer->drain_notifications(), 1U);
  auto lease = consumer.consumer->try_acquire();
  ASSERT_TRUE(lease);
  EXPECT_EQ(lease->payload()[0], current[0]);
}

TEST(dapp_publication_route, external_resources_are_reclaimed_explicitly_in_ring_completion_order)
{
  auto producer = publication_producer::create(2, 16);
  ASSERT_TRUE(producer);
  auto capabilities = producer.producer->export_capabilities();
  auto consumer     = publication_consumer::attach(
      capabilities.ring.value(), capabilities.notification.value(), capabilities.completion.value());
  ASSERT_TRUE(consumer);

  const std::array<uint8_t, 1> payload{1};
  ASSERT_EQ(producer.producer->publish(payload), publish_result::published);
  ASSERT_EQ(producer.producer->publish(payload), publish_result::published);
  auto first = consumer.consumer->try_acquire();
  ASSERT_TRUE(first);
  first.reset();

  unsigned                              callbacks = 0;
  const std::array<payload_fragment, 1> fragments{{{payload, 0}}};
  EXPECT_EQ(producer.producer->publish_external(
                fragments, 0, [](void* context) noexcept { ++*static_cast<unsigned*>(context); }, &callbacks),
            publish_result::full);
  EXPECT_EQ(callbacks, 0U);
  EXPECT_EQ(producer.producer->reclaim_completed(), 1U);
  EXPECT_EQ(producer.producer->publish_external(
                fragments, 0, [](void* context) noexcept { ++*static_cast<unsigned*>(context); }, &callbacks),
            publish_result::published);
  EXPECT_EQ(callbacks, 1U);

  auto second = consumer.consumer->try_acquire();
  ASSERT_TRUE(second);
  second.reset();
  EXPECT_EQ(producer.producer->reclaim_completed(), 1U);
  auto third = consumer.consumer->try_acquire();
  ASSERT_TRUE(third);
  third.reset();
  EXPECT_EQ(producer.producer->reclaim_completed(), 1U);
  EXPECT_EQ(producer.producer->reclaim_completed(), 0U);
}
