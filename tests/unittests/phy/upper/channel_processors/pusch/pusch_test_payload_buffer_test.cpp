// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// CUDA entry points below are host-only fakes. This executable must not link cudart.
#include "pusch_test_payload_buffer.h"
#include <cstdlib>
#include <gtest/gtest.h>
#include <type_traits>

namespace {
unsigned allocations = 0, releases = 0;
bool     fail_allocation = false;

cudaError_t allocate(void** output, size_t bytes)
{
  ++allocations;
  *output = fail_allocation ? nullptr : std::malloc(bytes);
  return *output ? cudaSuccess : cudaErrorMemoryAllocation;
}
} // namespace

extern "C" cudaError_t CUDARTAPI cudaHostAlloc(void** output, size_t bytes, unsigned int)
{
  return allocate(output, bytes);
}
extern "C" cudaError_t CUDARTAPI cudaMallocManaged(void** output, size_t bytes, unsigned int)
{
  return allocate(output, bytes);
}
extern "C" cudaError_t CUDARTAPI cudaFree(void* pointer)
{
  ++releases;
  std::free(pointer);
  return cudaSuccess;
}
extern "C" cudaError_t CUDARTAPI cudaFreeHost(void* pointer)
{
  return cudaFree(pointer);
}
extern "C" const char* CUDARTAPI cudaGetErrorString(cudaError_t)
{
  return "injected allocation failure";
}

using ocudu::test_support::guarded_payload_buffer;
static_assert(!std::is_copy_constructible_v<guarded_payload_buffer>);
static_assert(!std::is_move_constructible_v<guarded_payload_buffer>);

class PuschTestPayloadBuffer : public ::testing::TestWithParam<const char*>
{
  void SetUp() override
  {
    allocations = releases = 0;
    fail_allocation        = false;
  }
};

TEST_P(PuschTestPayloadBuffer, ReusesOneAllocationAndGuardsShrinkingPayloads)
{
  const bool cuda_storage = std::string(GetParam()) != "vector";
  {
    guarded_payload_buffer buffer(127, GetParam());
    auto*                  base = buffer.as_span().data();
    for (unsigned i = 0; i != 100; ++i) {
      const size_t size = 1 + (i * 43) % 127;
      buffer.reset(size);
      ASSERT_EQ(buffer.as_span().data(), base);
      ASSERT_EQ(buffer.as_span().size(), size);
      EXPECT_EQ(buffer.payload_vector(), std::vector<uint8_t>(size, 0xa5));
      ASSERT_TRUE(buffer.guard_ok());
      base[size]     = 0;
      size_t  offset = 99;
      uint8_t value  = 99;
      EXPECT_FALSE(buffer.guard_ok(&offset, &value));
      EXPECT_EQ(offset, 0);
      EXPECT_EQ(value, 0);
      buffer.reset();
      EXPECT_TRUE(buffer.guard_ok());
      // Unused capacity and final canary both remain checked on smaller TBs.
      base[127 + 63] = 0;
      EXPECT_FALSE(buffer.guard_ok());
      buffer.reset(127);
      EXPECT_TRUE(buffer.guard_ok());
      EXPECT_EQ(allocations, cuda_storage ? 1U : 0U);
      EXPECT_EQ(releases, 0);
    }
  }
  EXPECT_EQ(releases, cuda_storage ? 1U : 0U);
}

TEST_P(PuschTestPayloadBuffer, InvalidExtentPreservesPriorContentsAndOwnership)
{
  guarded_payload_buffer buffer(32, GetParam());
  buffer.as_span()[0] = 42;
  for (size_t bad : {size_t{0}, size_t{33}, std::numeric_limits<size_t>::max()}) {
    EXPECT_THROW(buffer.reset(bad), std::runtime_error);
    EXPECT_EQ(buffer.as_span().size(), 32);
    EXPECT_EQ(buffer.as_span()[0], 42);
    EXPECT_TRUE(buffer.guard_ok());
  }
}

INSTANTIATE_TEST_SUITE_P(Storage, PuschTestPayloadBuffer, ::testing::Values("vector", "managed", "pinned"));

TEST(PuschTestPayloadAdmission, RejectsBadCapacityAndModeBeforeCuda)
{
  allocations = releases = 0;
  fail_allocation        = false;
  for (size_t bad : {size_t{0}, std::numeric_limits<size_t>::max()}) {
    EXPECT_THROW(guarded_payload_buffer(bad, "managed"), std::runtime_error);
  }
  EXPECT_THROW(guarded_payload_buffer(32, "unknown"), std::runtime_error);
  EXPECT_EQ(allocations, 0);
  EXPECT_EQ(releases, 0);
}

TEST(PuschTestPayloadAdmission, AllocationFailureDoesNotPublishOrReleaseAnInvalidBuffer)
{
  allocations = releases = 0;
  fail_allocation        = true;
  EXPECT_THROW(guarded_payload_buffer(32, "managed"), std::runtime_error);
  EXPECT_THROW(guarded_payload_buffer(32, "pinned"), std::runtime_error);
  EXPECT_EQ(allocations, 2);
  EXPECT_EQ(releases, 0);
  fail_allocation = false;
}
