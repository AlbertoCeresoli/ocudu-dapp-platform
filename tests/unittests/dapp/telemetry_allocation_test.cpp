// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/portable_data_store.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <gtest/gtest.h>
#include <new>
#include <thread>

namespace {
thread_local bool     track_producer_memory = false;
thread_local unsigned producer_allocations  = 0;
thread_local unsigned producer_frees        = 0;
} // namespace

// Test-only replacement. noinline keeps GCC from treating our instrumented
// new/delete implementation as a mismatched malloc/delete call site.
[[gnu::noinline]] void* operator new(std::size_t size)
{
  if (track_producer_memory) {
    ++producer_allocations;
  }
  if (void* value = std::malloc(size == 0 ? 1 : size)) {
    return value;
  }
  throw std::bad_alloc();
}
[[gnu::noinline]] void* operator new[](std::size_t size)
{
  return ::operator new(size);
}
[[gnu::noinline]] void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}
[[gnu::noinline]] void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept
{
  return ::operator new(size, tag);
}
[[gnu::noinline]] void operator delete(void* value) noexcept
{
  if (track_producer_memory && value != nullptr) {
    ++producer_frees;
  }
  std::free(value);
}
[[gnu::noinline]] void operator delete[](void* value) noexcept
{
  ::operator delete(value);
}
[[gnu::noinline]] void operator delete(void* value, std::size_t) noexcept
{
  ::operator delete(value);
}
[[gnu::noinline]] void operator delete[](void* value, std::size_t) noexcept
{
  ::operator delete(value);
}
[[gnu::noinline]] void operator delete(void* value, const std::nothrow_t&) noexcept
{
  ::operator delete(value);
}
[[gnu::noinline]] void operator delete[](void* value, const std::nothrow_t&) noexcept
{
  ::operator delete(value);
}

using namespace ocudu::dapp::e3;

TEST(dapp_telemetry_isolation, producer_never_allocates_or_reclaims_during_subscription_churn)
{
  portable_data_store_config config;
  config.maximum_subscriptions = 1;
  auto [store, detail]         = portable_data_store::create(config);
  ASSERT_TRUE(store) << detail;
  portable_subscription_request request;
  request.subscription_id.bytes[15] = 1;
  request.interface_id              = OCUDU_DAPP_INTERFACE_SPECTRUM_V1;
  request.queue_depth_chunks        = 4;
  request.maximum_chunk_bytes       = 4096;
  request.lease_us                  = 1'000'000;
  request.drop_policy               = portable_drop_policy::coalesce_latest;
  ASSERT_TRUE(store->subscribe(request));
  std::array<uint8_t, 16>     metadata{};
  std::array<uint8_t, 1024>   tensor{};
  portable_publication_source source{request.interface_id, 0, 1, 1, metadata, tensor};
  std::atomic<bool>           started{false};
  std::atomic<bool>           done{false};
  std::thread                 management([&] {
    while (!started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (unsigned iteration = 0; iteration != 50; ++iteration) {
      EXPECT_TRUE(store->unsubscribe(request.subscription_id));
      EXPECT_TRUE(store->subscribe(request));
    }
    done.store(true, std::memory_order_release);
  });
  producer_allocations = producer_frees = 0;
  started.store(true, std::memory_order_release);
  do {
    track_producer_memory = true;
    store->publish(source);
    track_producer_memory = false;
    ++source.publication_sequence;
  } while (!done.load(std::memory_order_acquire));
  management.join();
  EXPECT_EQ(producer_allocations, 0U);
  EXPECT_EQ(producer_frees, 0U);
}
