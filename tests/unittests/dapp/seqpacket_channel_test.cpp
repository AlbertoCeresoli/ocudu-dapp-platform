// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/ipc/seqpacket_channel.h"
#include "ocudu/dapp/ipc/shared_spsc_ring.h"
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace ocudu::dapp::ipc;

TEST(dapp_seqpacket_channel, preserves_framing_and_kernel_credentials)
{
  auto pair = seqpacket_channel::create_pair();
  ASSERT_TRUE(pair) << pair.detail;
  const std::array<uint8_t, 3> payload{1, 2, 3};
  ASSERT_TRUE(pair.first->send(7, 42, payload));
  auto received = pair.second->receive();
  ASSERT_TRUE(received) << received.detail;
  EXPECT_EQ(received.packet->type, 7U);
  EXPECT_EQ(received.packet->sequence, 42U);
  EXPECT_EQ(received.packet->payload, std::vector<uint8_t>(payload.begin(), payload.end()));
  EXPECT_EQ(received.packet->sender.pid, ::getpid());
  EXPECT_EQ(received.packet->sender.uid, ::geteuid());
}

TEST(dapp_seqpacket_channel, passes_ring_capability_without_copying_tensor_data)
{
  auto pair = seqpacket_channel::create_pair();
  auto ring = shared_spsc_ring::create(2, 32);
  ASSERT_TRUE(pair && ring);
  ocudu::unique_fd exported(ring.ring->export_fd());
  ocudu::unique_fd completion(ring.ring->export_completion_fd());
  ASSERT_TRUE(exported.is_open());
  ASSERT_TRUE(completion.is_open());
  const std::array<int, 2> descriptors{exported.value(), completion.value()};
  ASSERT_TRUE(pair.first->send(1, 1, {}, descriptors));
  auto received = pair.second->receive();
  ASSERT_TRUE(received) << received.detail;
  ASSERT_EQ(received.packet->descriptors.size(), 2U);
  auto consumer = shared_spsc_ring::attach(
      received.packet->descriptors[0].value(), received.packet->descriptors[1].value(), endpoint_role::consumer);
  ASSERT_TRUE(consumer);

  const char value[] = "tensor-metadata";
  ASSERT_TRUE(ring.ring->try_publish({reinterpret_cast<const uint8_t*>(value), sizeof(value)}));
  auto lease = consumer.ring->try_acquire();
  ASSERT_TRUE(lease);
  EXPECT_EQ(std::memcmp(lease->payload().data(), value, sizeof(value)), 0);
}

TEST(dapp_seqpacket_channel, enforces_expected_forked_peer_pid)
{
  auto pair = seqpacket_channel::create_pair();
  ASSERT_TRUE(pair);
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    pair.first.reset();
    const bool sent = pair.second->send(2, 9);
    _exit(sent ? 0 : 2);
  }
  pair.second.reset();
  pair.first->require_peer(child, ::geteuid());
  auto received = pair.first->receive();
  ASSERT_TRUE(received) << received.detail;
  EXPECT_EQ(received.packet->sender.pid, child);
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(dapp_seqpacket_channel, rejects_wrong_peer_and_oversized_messages)
{
  auto pair = seqpacket_channel::create_pair();
  ASSERT_TRUE(pair);
  pair.second->require_peer(::getpid() + 1, ::geteuid());
  ASSERT_TRUE(pair.first->send(1, 1));
  auto rejected = pair.second->receive();
  EXPECT_FALSE(rejected);
  EXPECT_FALSE(rejected.peer_closed);

  std::array<uint8_t, maximum_control_payload + 1> oversized{};
  EXPECT_FALSE(pair.first->send(1, 2, oversized));
  pair.first.reset();
  EXPECT_TRUE(pair.second->receive().peer_closed);
}
