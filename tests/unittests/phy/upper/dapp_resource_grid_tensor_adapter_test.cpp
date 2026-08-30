// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "../support/resource_grid_test_doubles.h"
#include "ocudu/dapp/runtime/abi_validation.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/upper/dapp/resource_grid_tensor_adapter.h"
#ifdef ENABLE_CUDA
#include <cuda_runtime.h>
#endif
#include <gtest/gtest.h>

using namespace ocudu;
using namespace ocudu::dapp;

namespace {

/// Device reader implementing the owned-snapshot contract the adapter consumes
/// (the live managed grid must never be handed to Class-C, see the Xid 31 note
/// in resource_grid_tensor_adapter.cpp). Under ENABLE_CUDA the snapshot, ready
/// event and the test's execution stream are real CUDA objects because the
/// adapter enqueues cudaStreamWaitEvent on them.
class device_resource_grid_reader : public resource_grid_reader_spy
{
public:
  device_resource_grid_reader() : resource_grid_reader_spy(2, 14, 2)
  {
#ifdef ENABLE_CUDA
    valid = cudaMalloc(&snapshot, 2UL * 14 * 2 * 12 * sizeof(cbf16_t)) == cudaSuccess &&
            cudaEventCreateWithFlags(&ready_event, cudaEventDisableTiming) == cudaSuccess &&
            cudaEventRecord(ready_event, nullptr) == cudaSuccess;
#endif
  }

  ~device_resource_grid_reader() override
  {
#ifdef ENABLE_CUDA
    if (ready_event != nullptr) {
      static_cast<void>(cudaEventSynchronize(ready_event));
      static_cast<void>(cudaEventDestroy(ready_event));
    }
    if (snapshot != nullptr) {
      static_cast<void>(cudaFree(snapshot));
    }
#endif
  }

  bool        supports_device_grid_reading() const override { return true; }
  const void* get_owned_device_snapshot_cbf16() const override
  {
    ++snapshot_reads;
    return snapshot_present ? snapshot_pointer() : nullptr;
  }
  void* get_owned_device_snapshot_ready_event() const override
  {
    return snapshot_present ? ready_event_pointer() : nullptr;
  }
  bool stage_owned_snapshot_from_host() const override
  {
    ++stage_calls;
    if (stage_succeeds) {
      snapshot_present = true;
    }
    return stage_succeeds;
  }
  bool on_device_grid_reading_enqueued(void* stream) const override
  {
    finished_stream = stream;
    return true;
  }

  const void* snapshot_pointer() const
  {
#ifdef ENABLE_CUDA
    return snapshot;
#else
    return &host_placeholder;
#endif
  }
  void* ready_event_pointer() const
  {
#ifdef ENABLE_CUDA
    return ready_event;
#else
    return const_cast<int*>(&host_placeholder);
#endif
  }

  bool             valid{true};
  mutable void*    finished_stream{};
  mutable bool     snapshot_present{true};
  bool             stage_succeeds{false};
  mutable unsigned snapshot_reads{};
  mutable unsigned stage_calls{};

private:
#ifdef ENABLE_CUDA
  void*       snapshot{};
  cudaEvent_t ready_event{};
#else
  int host_placeholder{};
#endif
};

/// A real CUDA stream for the lease under ENABLE_CUDA (the adapter enqueues a
/// wait on it); a plain int elsewhere.
class test_execution_stream
{
public:
  test_execution_stream()
  {
#ifdef ENABLE_CUDA
    valid = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess;
#endif
  }
  ~test_execution_stream()
  {
#ifdef ENABLE_CUDA
    if (stream != nullptr) {
      static_cast<void>(cudaStreamSynchronize(stream));
      static_cast<void>(cudaStreamDestroy(stream));
    }
#endif
  }
  void* handle()
  {
#ifdef ENABLE_CUDA
    return stream;
#else
    return &placeholder;
#endif
  }
  bool valid{true};

private:
#ifdef ENABLE_CUDA
  cudaStream_t stream{};
#else
  int placeholder{};
#endif
};

resource_grid_lease valid_host_lease()
{
  return {.allocation_id = 11, .allocation_generation = 3, .lease_token = 17};
}

accelerator_resource_grid_lease valid_accelerator_lease(void* stream)
{
  accelerator_resource_grid_lease lease{};
  lease.allocation_id         = 21;
  lease.allocation_generation = 5;
  lease.lease_token           = 23;
  lease.backend_id            = OCUDU_DAPP_BACKEND_CUDA_V1;
  lease.device_ordinal        = 2;
  lease.execution_stream      = stream;
  lease.completion_token      = 29;
  return lease;
}

} // namespace

TEST(dapp_resource_grid_tensor_adapter, borrows_complete_contiguous_host_grid_without_copy)
{
  auto factory = create_resource_grid_factory();
  ASSERT_NE(factory, nullptr);
  auto grid = factory->create(2, 14, 24);
  ASSERT_NE(grid, nullptr);

  auto result = make_host_resource_grid_tensor(grid->get_reader(), valid_host_lease());
  ASSERT_TRUE(result);
  EXPECT_EQ(validate_tensor_view(result.tensor), validation_error::none);
  EXPECT_EQ(result.tensor.domain, OCUDU_DAPP_MEM_HOST_V1);
  EXPECT_EQ(result.tensor.backend_id, OCUDU_DAPP_BACKEND_HOST_V1);
  EXPECT_EQ(result.tensor.element_type, OCUDU_DAPP_ELEMENT_CBF16_V1);
  EXPECT_EQ(result.tensor.layout_id, OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1);
  EXPECT_EQ(result.tensor.native_handle, reinterpret_cast<uintptr_t>(grid->get_reader().get_contiguous_view().data()));
  EXPECT_EQ(result.tensor.dimensions[0], 2U);
  EXPECT_EQ(result.tensor.dimensions[1], 14U);
  EXPECT_EQ(result.tensor.dimensions[2], 24U);
  EXPECT_EQ(result.tensor.byte_strides[2], sizeof(cbf16_t));
  EXPECT_EQ(result.tensor.byte_strides[1], 24U * sizeof(cbf16_t));
  EXPECT_EQ(result.tensor.byte_strides[0], 14U * 24U * sizeof(cbf16_t));
  EXPECT_EQ(result.tensor.byte_length, 2U * 14U * 24U * sizeof(cbf16_t));
}

TEST(dapp_resource_grid_tensor_adapter, rejects_missing_host_ownership_or_noncontiguous_storage)
{
  resource_grid_reader_spy reader(1, 14, 1);
  auto                     lease = valid_host_lease();
  lease.lease_token              = 0;
  EXPECT_EQ(make_host_resource_grid_tensor(reader, lease).status, OCUDU_DAPP_INVALID_INPUT_V1);
  EXPECT_EQ(make_host_resource_grid_tensor(reader, valid_host_lease()).status, OCUDU_DAPP_UNSUPPORTED_V1);
}

TEST(dapp_resource_grid_tensor_adapter, requires_complete_accelerator_identity_ordering_and_lifetime)
{
  device_resource_grid_reader reader;
  test_execution_stream       stream;
  ASSERT_TRUE(reader.valid);
  ASSERT_TRUE(stream.valid);
  auto lease  = valid_accelerator_lease(stream.handle());
  auto result = make_accelerator_resource_grid_tensor(reader, lease);
  ASSERT_TRUE(result);
  EXPECT_EQ(validate_tensor_view(result.tensor), validation_error::none);
  // The adapter must hand out the owned snapshot, never the live grid.
  EXPECT_EQ(result.tensor.native_handle, reinterpret_cast<uintptr_t>(reader.snapshot_pointer()));
  EXPECT_EQ(result.tensor.ready_token, reinterpret_cast<uintptr_t>(reader.ready_event_pointer()));
  EXPECT_EQ(reader.snapshot_reads, 1U);
  EXPECT_EQ(reader.stage_calls, 0U);
  EXPECT_EQ(result.tensor.domain, OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1);
  EXPECT_EQ(result.tensor.backend_id, OCUDU_DAPP_BACKEND_CUDA_V1);
  EXPECT_EQ(result.tensor.device_ordinal, 2U);
  EXPECT_EQ(result.tensor.completion_token, 29U);
  EXPECT_TRUE(finish_accelerator_resource_grid_read(reader, stream.handle()));
  EXPECT_EQ(reader.finished_stream, stream.handle());

  lease.backend_id = OCUDU_DAPP_BACKEND_HOST_V1;
  EXPECT_EQ(make_accelerator_resource_grid_tensor(reader, lease).status, OCUDU_DAPP_INVALID_INPUT_V1);

  // Without an owned snapshot the adapter must stage one rather than fall
  // through to the live managed grid; a failed staging misses the frame.
  lease                   = valid_accelerator_lease(stream.handle());
  reader.snapshot_present = false;
  reader.stage_succeeds   = false;
  EXPECT_EQ(make_accelerator_resource_grid_tensor(reader, lease).status, OCUDU_DAPP_BYPASS_V1);
  EXPECT_GE(reader.stage_calls, 1U);
  reader.snapshot_present = false;
  reader.stage_succeeds   = true;
  result                  = make_accelerator_resource_grid_tensor(reader, lease);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.tensor.native_handle, reinterpret_cast<uintptr_t>(reader.snapshot_pointer()));

  resource_grid_reader_spy host_only_reader(1, 14, 1);
  EXPECT_EQ(make_accelerator_resource_grid_tensor(host_only_reader, lease).status, OCUDU_DAPP_UNSUPPORTED_V1);
}
