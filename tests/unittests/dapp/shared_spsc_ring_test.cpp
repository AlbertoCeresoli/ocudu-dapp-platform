// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/ipc/shared_spsc_ring.h"
#include <array>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace ocudu::dapp::ipc;

namespace {

ocudu::span<const uint8_t> bytes(const char* value)
{
  return {reinterpret_cast<const uint8_t*>(value), std::strlen(value)};
}

} // namespace

TEST(dapp_shared_spsc_ring, consumer_borrows_in_order_and_backpressure_is_bounded)
{
  auto created = shared_spsc_ring::create(2, 32);
  ASSERT_TRUE(created) << created.detail;
  const int fd         = created.ring->export_fd();
  const int completion = created.ring->export_completion_fd();
  ASSERT_GE(fd, 0);
  ASSERT_GE(completion, 0);
  auto attached = shared_spsc_ring::attach(fd, completion, endpoint_role::consumer);
  ::close(fd);
  ::close(completion);
  ASSERT_TRUE(attached) << attached.detail;

  ASSERT_TRUE(created.ring->try_publish(bytes("first"), 7));
  ASSERT_TRUE(created.ring->try_publish(bytes("second")));
  EXPECT_FALSE(created.ring->try_publish(bytes("full")));

  auto first = attached.ring->try_acquire();
  ASSERT_TRUE(first);
  EXPECT_EQ(first->flags(), 7U);
  EXPECT_EQ(first->sequence(), 0U);
  EXPECT_EQ(std::memcmp(first->payload().data(), "first", first->payload().size()), 0);
  EXPECT_FALSE(attached.ring->try_acquire());
  first.reset();

  auto second = attached.ring->try_acquire();
  ASSERT_TRUE(second);
  EXPECT_EQ(second->sequence(), 1U);
  EXPECT_EQ(std::memcmp(second->payload().data(), "second", second->payload().size()), 0);
  second.reset();
  EXPECT_TRUE(created.ring->try_publish(bytes("third")));
}

TEST(dapp_shared_spsc_ring, restart_epoch_prevents_stale_lease_from_advancing_new_consumer)
{
  auto created = shared_spsc_ring::create(2, 16);
  ASSERT_TRUE(created);
  const int fd         = created.ring->export_fd();
  const int completion = created.ring->export_completion_fd();
  auto      consumer   = shared_spsc_ring::attach(fd, completion, endpoint_role::consumer);
  ASSERT_TRUE(consumer);

  ASSERT_TRUE(created.ring->try_publish(bytes("old")));
  auto stale = consumer.ring->try_acquire();
  ASSERT_TRUE(stale);
  const auto old_epoch = created.ring->epoch();
  ASSERT_TRUE(created.ring->reset_after_consumer_restart());
  EXPECT_GT(created.ring->epoch(), old_epoch);
  consumer.ring.reset();
  stale.reset();

  consumer = shared_spsc_ring::attach(fd, completion, endpoint_role::consumer);
  ::close(fd);
  ::close(completion);
  ASSERT_TRUE(consumer);
  ASSERT_TRUE(created.ring->try_publish(bytes("new")));
  auto current = consumer.ring->try_acquire();
  ASSERT_TRUE(current);
  EXPECT_EQ(std::memcmp(current->payload().data(), "new", current->payload().size()), 0);
}

TEST(dapp_shared_spsc_ring, memfd_transport_works_across_processes)
{
  auto created = shared_spsc_ring::create(4, 32);
  ASSERT_TRUE(created);
  const int fd         = created.ring->export_fd();
  const int completion = created.ring->export_completion_fd();
  ASSERT_GE(fd, 0);
  ASSERT_GE(completion, 0);
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    auto consumer = shared_spsc_ring::attach(fd, completion, endpoint_role::consumer);
    if (!consumer) {
      _exit(2);
    }
    for (unsigned attempt = 0; attempt != 1000; ++attempt) {
      auto lease = consumer.ring->try_acquire();
      if (lease) {
        const bool valid = lease->payload().size() == 5 && std::memcmp(lease->payload().data(), "child", 5) == 0;
        lease.reset();
        _exit(valid ? 0 : 3);
      }
      ::usleep(1000);
    }
    _exit(4);
  }
  ::close(fd);
  ::close(completion);
  ASSERT_TRUE(created.ring->try_publish(bytes("child")));
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(dapp_shared_spsc_ring, endpoint_roles_and_input_bounds_are_enforced)
{
  EXPECT_FALSE(shared_spsc_ring::create(1, 16));
  EXPECT_FALSE(shared_spsc_ring::attach(-1, -1, endpoint_role::consumer));
  auto created = shared_spsc_ring::create(2, 4);
  ASSERT_TRUE(created);
  EXPECT_FALSE(created.ring->try_publish(bytes("oversized")));
  EXPECT_FALSE(created.ring->try_acquire());
}

TEST(dapp_shared_spsc_ring, exported_data_capability_is_read_only)
{
  auto created = shared_spsc_ring::create(2, 16);
  ASSERT_TRUE(created);
  const int fd = created.ring->export_fd();
  ASSERT_GE(fd, 0);
  EXPECT_EQ(::fcntl(fd, F_GETFL) & O_ACCMODE, O_RDONLY);
  struct stat status {};
  ASSERT_EQ(::fstat(fd, &status), 0);
  void* writable = ::mmap(nullptr, status.st_size, PROT_WRITE, MAP_SHARED, fd, 0);
  EXPECT_EQ(writable, MAP_FAILED);
  if (writable != MAP_FAILED) {
    ::munmap(writable, status.st_size);
  }
  ::close(fd);
}

TEST(dapp_shared_spsc_ring, accidental_concurrent_producers_drop_without_corrupting_sequence)
{
  auto producer = shared_spsc_ring::create(64, 1);
  ASSERT_TRUE(producer);
  const int data       = producer.ring->export_fd();
  const int completion = producer.ring->export_completion_fd();
  auto      consumer   = shared_spsc_ring::attach(data, completion, endpoint_role::consumer);
  ::close(data);
  ::close(completion);
  ASSERT_TRUE(consumer);

  std::atomic<unsigned> finished{0};
  std::atomic<uint64_t> accepted{0};
  auto                  publish = [&](uint8_t value) {
    for (unsigned i = 0; i != 2000; ++i) {
      if (producer.ring->try_publish({&value, 1})) {
        accepted.fetch_add(1, std::memory_order_relaxed);
      }
    }
    finished.fetch_add(1, std::memory_order_release);
  };
  std::thread first(publish, 1);
  std::thread second(publish, 2);

  uint64_t received = 0;
  uint64_t sequence = 0;
  while (finished.load(std::memory_order_acquire) != 2) {
    auto lease = consumer.ring->try_acquire();
    if (!lease) {
      std::this_thread::yield();
      continue;
    }
    EXPECT_EQ(lease->sequence(), sequence++);
    EXPECT_EQ(lease->payload().size(), 1U);
    if (lease->payload().size() == 1) {
      EXPECT_TRUE(lease->payload()[0] == 1 || lease->payload()[0] == 2);
    }
    ++received;
  }
  first.join();
  second.join();
  while (auto lease = consumer.ring->try_acquire()) {
    EXPECT_EQ(lease->sequence(), sequence++);
    ++received;
  }
  EXPECT_EQ(received, accepted.load(std::memory_order_relaxed));
}
