// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Unit tests for CUDA-visible PHY support buffers.

#include "phy_acceleration_prach_buffer_factory.h"
#include "phy_acceleration_resource_grid_factory.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/phy/support/support_factories.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cuda_runtime.h>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace ocudu;

namespace {

class scoped_env_var
{
public:
  explicit scoped_env_var(const char* name_) : name(name_)
  {
    const char* value = std::getenv(name.c_str());
    if (value != nullptr) {
      original_value = value;
    }
  }

  ~scoped_env_var()
  {
    if (original_value.has_value()) {
      (void)setenv(name.c_str(), original_value->c_str(), 1);
    } else {
      (void)unsetenv(name.c_str());
    }
  }

  bool set(const char* value) { return setenv(name.c_str(), value, 1) == 0; }

  bool unset() { return unsetenv(name.c_str()) == 0; }

private:
  std::string                name;
  std::optional<std::string> original_value;
};

class cuda_stream_guard
{
public:
  bool create() { return cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess; }

  ~cuda_stream_guard()
  {
    if (stream != nullptr) {
      (void)cudaStreamDestroy(stream);
    }
  }

  cudaStream_t get() const { return stream; }

private:
  cudaStream_t stream = nullptr;
};

testing::AssertionResult cuda_managed_memory_available()
{
  int         device_count = 0;
  cudaError_t status       = cudaGetDeviceCount(&device_count);
  if ((status != cudaSuccess) || (device_count == 0)) {
    return testing::AssertionFailure() << "CUDA runtime device is not available.";
  }

  int device_id = 0;
  status        = cudaGetDevice(&device_id);
  if (status != cudaSuccess) {
    return testing::AssertionFailure() << "Failed to query active CUDA device: " << cudaGetErrorString(status);
  }

  int managed_memory = 0;
  status             = cudaDeviceGetAttribute(&managed_memory, cudaDevAttrManagedMemory, device_id);
  if (status != cudaSuccess) {
    return testing::AssertionFailure() << "Failed to query CUDA managed-memory support: " << cudaGetErrorString(status);
  }
  if (managed_memory == 0) {
    return testing::AssertionFailure() << "CUDA managed memory is not supported by the active device.";
  }

  return testing::AssertionSuccess();
}

testing::AssertionResult cuda_concurrent_managed_access_available()
{
  testing::AssertionResult managed_available = cuda_managed_memory_available();
  if (!managed_available) {
    return managed_available;
  }

  int device_id = 0;
  if (cudaGetDevice(&device_id) != cudaSuccess) {
    return testing::AssertionFailure() << "Failed to query the active CUDA device.";
  }

  int concurrent_managed_access       = 0;
  int host_page_table_access          = 0;
  int direct_managed_access_from_host = 0;
  if ((cudaDeviceGetAttribute(&concurrent_managed_access, cudaDevAttrConcurrentManagedAccess, device_id) !=
       cudaSuccess) ||
      (cudaDeviceGetAttribute(&host_page_table_access, cudaDevAttrPageableMemoryAccessUsesHostPageTables, device_id) !=
       cudaSuccess) ||
      (cudaDeviceGetAttribute(&direct_managed_access_from_host, cudaDevAttrDirectManagedMemAccessFromHost, device_id) !=
       cudaSuccess)) {
    return testing::AssertionFailure() << "Failed to query concurrent managed-memory support.";
  }
  if ((concurrent_managed_access == 0) || (host_page_table_access == 0) || (direct_managed_access_from_host == 0)) {
    return testing::AssertionFailure() << "Concurrent managed host/device access is not supported.";
  }

  return testing::AssertionSuccess();
}

bool wait_for_flag(const std::atomic<bool>& flag, std::chrono::milliseconds timeout)
{
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!flag.load(std::memory_order_acquire) && (std::chrono::steady_clock::now() < deadline)) {
    std::this_thread::yield();
  }
  return flag.load(std::memory_order_acquire);
}

struct cuda_callback_gate {
  std::atomic<bool> entered  = false;
  std::atomic<bool> released = false;
};

void wait_for_cuda_callback_gate(void* opaque)
{
  auto& gate = *static_cast<cuda_callback_gate*>(opaque);
  gate.entered.store(true, std::memory_order_release);
  while (!gate.released.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

std::vector<cbf16_t> generate_grid_payload(unsigned nof_ports, unsigned nof_symbols, unsigned nof_subc)
{
  std::vector<cbf16_t> payload(static_cast<size_t>(nof_ports) * nof_symbols * nof_subc);
  for (size_t i = 0; i != payload.size(); ++i) {
    payload[i] = cbf16_t(0.003F * static_cast<float>(i + 1), -0.002F * static_cast<float>(i + 3));
  }
  return payload;
}

std::vector<cbf16_t> generate_symbol_payload(unsigned nof_samples, float scale)
{
  std::vector<cbf16_t> payload(nof_samples);
  for (unsigned i = 0; i != nof_samples; ++i) {
    payload[i] = cbf16_t(scale * static_cast<float>(i + 1), -scale * static_cast<float>(i + 7));
  }
  return payload;
}

void expect_equal(span<const cbf16_t> actual, span<const cbf16_t> expected)
{
  ASSERT_EQ(actual.size(), expected.size());
  for (unsigned i = 0; i != actual.size(); ++i) {
    EXPECT_EQ(actual[i], expected[i]) << "sample=" << i;
  }
}

} // namespace

TEST(cuda_visible_phy_support_test, resource_grid_factory_can_force_managed_cuda_visible_grid)
{
  testing::AssertionResult cuda_available = cuda_managed_memory_available();
  if (!cuda_available) {
    GTEST_SKIP() << cuda_available.message();
  }

  scoped_env_var grid_mode("OCUDU_UL_CUDA_VISIBLE_GRID");
  ASSERT_TRUE(grid_mode.set("managed"));

  std::shared_ptr<resource_grid_factory> fallback_factory = create_resource_grid_factory();
  std::shared_ptr<resource_grid_factory> factory          = create_phy_acceleration_resource_grid_factory(
      fallback_factory, phy_acceleration_resource_grid_direction::uplink, true);
  ASSERT_NE(factory, nullptr);

  std::unique_ptr<resource_grid> grid = factory->create(2, 4, 24);
  ASSERT_NE(grid, nullptr);

  resource_grid_writer&       writer = grid->get_writer();
  const resource_grid_reader& reader = grid->get_reader();
  EXPECT_TRUE(writer.supports_device_grid_mapping());
  EXPECT_TRUE(writer.device_grid_mapping_aliases_host_grid());
  EXPECT_NE(writer.get_device_grid_bf16(), nullptr);
  EXPECT_TRUE(reader.supports_device_grid_reading());
  EXPECT_EQ(reader.get_device_grid_cbf16(), writer.get_device_grid_bf16());
}

TEST(cuda_visible_phy_support_test, coherent_managed_resource_grid_uses_direct_host_mapping_by_default)
{
  testing::AssertionResult concurrent_access = cuda_concurrent_managed_access_available();
  if (!concurrent_access) {
    GTEST_SKIP() << concurrent_access.message();
  }

  scoped_env_var grid_mode("OCUDU_UL_CUDA_VISIBLE_GRID");
  scoped_env_var prefer_device("OCUDU_CUDA_VISIBLE_GRID_PREFER_DEVICE");
  ASSERT_TRUE(grid_mode.set("managed"));
  ASSERT_TRUE(prefer_device.unset());

  static constexpr unsigned nof_ports   = 2;
  static constexpr unsigned nof_symbols = 4;
  static constexpr unsigned nof_subc    = 24;
  static constexpr size_t   grid_bytes  = nof_ports * nof_symbols * nof_subc * sizeof(cbf16_t);

  auto factory = create_phy_acceleration_resource_grid_factory(
      create_resource_grid_factory(), phy_acceleration_resource_grid_direction::uplink, true);
  std::unique_ptr<resource_grid> grid = factory->create(nof_ports, nof_symbols, nof_subc);
  ASSERT_NE(grid, nullptr);

  void* managed_grid = grid->get_writer().get_device_grid_bf16();
  ASSERT_NE(managed_grid, nullptr);

  int preferred_location = cudaInvalidDeviceId;
  ASSERT_EQ(cudaMemRangeGetAttribute(&preferred_location,
                                     sizeof(preferred_location),
                                     cudaMemRangeAttributePreferredLocation,
                                     managed_grid,
                                     grid_bytes),
            cudaSuccess);

  int device_id = cudaInvalidDeviceId;
  ASSERT_EQ(cudaGetDevice(&device_id), cudaSuccess);
  EXPECT_EQ(preferred_location, device_id);

  int device_count = 0;
  ASSERT_EQ(cudaGetDeviceCount(&device_count), cudaSuccess);
  std::vector<int> accessed_by(static_cast<size_t>(device_count) + 1, cudaInvalidDeviceId);
  ASSERT_EQ(cudaMemRangeGetAttribute(accessed_by.data(),
                                     accessed_by.size() * sizeof(accessed_by.front()),
                                     cudaMemRangeAttributeAccessedBy,
                                     managed_grid,
                                     grid_bytes),
            cudaSuccess);
  EXPECT_NE(std::find(accessed_by.begin(), accessed_by.end(), device_id), accessed_by.end());
  EXPECT_NE(std::find(accessed_by.begin(), accessed_by.end(), cudaCpuDeviceId), accessed_by.end());
}

TEST(cuda_visible_phy_support_test, explicit_managed_mode_overrides_pinned_environment_policy)
{
  testing::AssertionResult cuda_available = cuda_managed_memory_available();
  if (!cuda_available) {
    GTEST_SKIP() << cuda_available.message();
  }

  scoped_env_var grid_mode("OCUDU_UL_CUDA_VISIBLE_GRID");
  ASSERT_TRUE(grid_mode.set("pinned"));

  std::shared_ptr<resource_grid_factory> factory =
      create_phy_acceleration_resource_grid_factory(create_resource_grid_factory(),
                                                    phy_acceleration_resource_grid_direction::uplink,
                                                    true,
                                                    0,
                                                    phy_acceleration_resource_grid_memory_mode::managed);
  ASSERT_NE(factory, nullptr);

  std::unique_ptr<resource_grid> grid = factory->create(1, 1, 24);
  ASSERT_NE(grid, nullptr);
  EXPECT_TRUE(grid->get_writer().supports_device_grid_mapping());
  EXPECT_TRUE(grid->get_reader().supports_device_grid_reading());
}

TEST(cuda_visible_phy_support_test, resource_grid_factory_uses_fallback_when_accelerated_grid_is_disabled)
{
  scoped_env_var grid_mode("OCUDU_UL_CUDA_VISIBLE_GRID");
  ASSERT_TRUE(grid_mode.set("managed"));

  std::shared_ptr<resource_grid_factory> fallback_factory = create_resource_grid_factory();
  std::shared_ptr<resource_grid_factory> factory          = create_phy_acceleration_resource_grid_factory(
      fallback_factory, phy_acceleration_resource_grid_direction::uplink, false);
  ASSERT_EQ(factory, fallback_factory);

  std::unique_ptr<resource_grid> grid = factory->create(2, 4, 24);
  ASSERT_NE(grid, nullptr);
  EXPECT_FALSE(grid->get_writer().supports_device_grid_mapping());
  EXPECT_FALSE(grid->get_reader().supports_device_grid_reading());
}

TEST(cuda_visible_phy_support_test, resource_grid_factory_uses_fallback_when_pinned_mode_is_requested)
{
  scoped_env_var grid_mode("OCUDU_UL_CUDA_VISIBLE_GRID");
  ASSERT_TRUE(grid_mode.set("pinned"));

  std::shared_ptr<resource_grid_factory> factory = create_phy_acceleration_resource_grid_factory(
      create_resource_grid_factory(), phy_acceleration_resource_grid_direction::uplink, true);
  ASSERT_NE(factory, nullptr);

  std::unique_ptr<resource_grid> grid = factory->create(2, 4, 24);
  ASSERT_NE(grid, nullptr);
  EXPECT_FALSE(grid->get_writer().supports_device_grid_mapping());
  EXPECT_FALSE(grid->get_reader().supports_device_grid_reading());
}

TEST(cuda_visible_phy_support_test, resource_grid_device_mapping_is_visible_to_host_reader)
{
  testing::AssertionResult cuda_available = cuda_managed_memory_available();
  if (!cuda_available) {
    GTEST_SKIP() << cuda_available.message();
  }

  scoped_env_var grid_mode("OCUDU_UL_CUDA_VISIBLE_GRID");
  ASSERT_TRUE(grid_mode.set("managed"));

  static constexpr unsigned nof_ports   = 2;
  static constexpr unsigned nof_symbols = 4;
  static constexpr unsigned nof_subc    = 24;
  static constexpr unsigned port        = 1;
  static constexpr unsigned symbol      = 2;

  auto factory = create_phy_acceleration_resource_grid_factory(
      create_resource_grid_factory(), phy_acceleration_resource_grid_direction::uplink, true);
  std::unique_ptr<resource_grid> grid = factory->create(nof_ports, nof_symbols, nof_subc);
  ASSERT_NE(grid, nullptr);

  resource_grid_writer&       writer = grid->get_writer();
  const resource_grid_reader& reader = grid->get_reader();
  ASSERT_TRUE(writer.supports_device_grid_mapping());

  cuda_stream_guard stream;
  ASSERT_TRUE(stream.create());

  std::vector<cbf16_t> payload = generate_grid_payload(nof_ports, nof_symbols, nof_subc);
  ASSERT_TRUE(writer.prepare_device_grid_mapping(stream.get()));
  ASSERT_EQ(cudaMemcpyAsync(writer.get_device_grid_bf16(),
                            payload.data(),
                            payload.size() * sizeof(cbf16_t),
                            cudaMemcpyHostToDevice,
                            stream.get()),
            cudaSuccess);
  ASSERT_TRUE(writer.on_device_grid_mapping_enqueued(stream.get()));

  const size_t offset = (static_cast<size_t>(port) * nof_symbols + symbol) * nof_subc;
  expect_equal(reader.get_view(port, symbol), span<const cbf16_t>(payload.data() + offset, nof_subc));

  grid->set_all_zero();
  ASSERT_TRUE(reader.is_empty());
  span<const cbf16_t> cleared = reader.get_view(port, symbol);
  for (cbf16_t value : cleared) {
    EXPECT_EQ(value, cbf16_t());
  }
}

TEST(cuda_visible_phy_support_test, resource_grid_host_writer_is_visible_to_device_reader)
{
  testing::AssertionResult cuda_available = cuda_managed_memory_available();
  if (!cuda_available) {
    GTEST_SKIP() << cuda_available.message();
  }

  scoped_env_var grid_mode("OCUDU_UL_CUDA_VISIBLE_GRID");
  ASSERT_TRUE(grid_mode.set("managed"));

  static constexpr unsigned nof_ports   = 2;
  static constexpr unsigned nof_symbols = 4;
  static constexpr unsigned nof_subc    = 24;
  static constexpr unsigned port        = 0;
  static constexpr unsigned symbol      = 3;

  auto factory = create_phy_acceleration_resource_grid_factory(
      create_resource_grid_factory(), phy_acceleration_resource_grid_direction::uplink, true);
  std::unique_ptr<resource_grid> grid = factory->create(nof_ports, nof_symbols, nof_subc);
  ASSERT_NE(grid, nullptr);

  resource_grid_writer&       writer = grid->get_writer();
  const resource_grid_reader& reader = grid->get_reader();
  ASSERT_TRUE(reader.supports_device_grid_reading());

  std::vector<cbf16_t> expected = generate_symbol_payload(nof_subc, 0.005F);
  writer.put(port, symbol, 0, 1, span<const cbf16_t>(expected));

  cuda_stream_guard stream;
  ASSERT_TRUE(stream.create());

  ASSERT_TRUE(reader.prepare_device_grid_reading(stream.get()));
  std::vector<cbf16_t> copied(expected.size());
  const cbf16_t*       device_grid = static_cast<const cbf16_t*>(reader.get_device_grid_cbf16());
  ASSERT_NE(device_grid, nullptr);

  const size_t offset = (static_cast<size_t>(port) * nof_symbols + symbol) * nof_subc;
  ASSERT_EQ(
      cudaMemcpyAsync(
          copied.data(), device_grid + offset, copied.size() * sizeof(cbf16_t), cudaMemcpyDeviceToHost, stream.get()),
      cudaSuccess);
  ASSERT_TRUE(reader.on_device_grid_reading_enqueued(stream.get()));
  ASSERT_EQ(cudaStreamSynchronize(stream.get()), cudaSuccess);
  ASSERT_TRUE(reader.synchronize_device_grid_reading());

  EXPECT_EQ(copied, expected);
}

TEST(cuda_visible_phy_support_test, downlink_host_writer_does_not_wait_for_disjoint_device_mapping)
{
  testing::AssertionResult concurrent_access = cuda_concurrent_managed_access_available();
  if (!concurrent_access) {
    GTEST_SKIP() << concurrent_access.message();
  }

  scoped_env_var grid_mode("OCUDU_DL_CUDA_VISIBLE_GRID");
  scoped_env_var prefetch_mode("OCUDU_CUDA_VISIBLE_GRID_PREFETCH");
  ASSERT_TRUE(grid_mode.set("managed"));
  ASSERT_TRUE(prefetch_mode.unset());

  static constexpr unsigned nof_ports   = 1;
  static constexpr unsigned nof_symbols = 2;
  static constexpr unsigned nof_subc    = 24;

  auto factory = create_phy_acceleration_resource_grid_factory(
      create_resource_grid_factory(), phy_acceleration_resource_grid_direction::downlink, true);
  std::unique_ptr<resource_grid> grid = factory->create(nof_ports, nof_symbols, nof_subc);
  ASSERT_NE(grid, nullptr);

  resource_grid_writer&       writer = grid->get_writer();
  const resource_grid_reader& reader = grid->get_reader();
  ASSERT_TRUE(writer.supports_device_grid_mapping());

  cuda_stream_guard stream;
  ASSERT_TRUE(stream.create());

  std::vector<cbf16_t> device_values(nof_subc);
  auto*                device_value_bytes = reinterpret_cast<unsigned char*>(device_values.data());
  std::fill(device_value_bytes, device_value_bytes + device_values.size() * sizeof(cbf16_t), 0x2a);
  std::vector<cbf16_t> host_values = generate_symbol_payload(nof_subc, 0.007F);
  cuda_callback_gate   gate;

  ASSERT_TRUE(writer.prepare_device_grid_mapping(stream.get()));
  ASSERT_EQ(cudaLaunchHostFunc(stream.get(), wait_for_cuda_callback_gate, &gate), cudaSuccess);
  ASSERT_EQ(cudaMemsetAsync(writer.get_device_grid_bf16(), 0x2a, device_values.size() * sizeof(cbf16_t), stream.get()),
            cudaSuccess);
  ASSERT_TRUE(writer.on_device_grid_mapping_enqueued(stream.get()));

  bool callback_entered = wait_for_flag(gate.entered, std::chrono::seconds(1));
  if (!callback_entered) {
    gate.released.store(true, std::memory_order_release);
    (void)cudaStreamSynchronize(stream.get());
    FAIL() << "CUDA callback did not enter before the timeout.";
  }

  std::atomic<bool> host_write_done = false;
  std::thread       host_writer([&]() {
    writer.put(0, 1, 0, 1, span<const cbf16_t>(host_values));
    host_write_done.store(true, std::memory_order_release);
  });

  bool host_write_overlapped = wait_for_flag(host_write_done, std::chrono::seconds(1));
  gate.released.store(true, std::memory_order_release);
  host_writer.join();

  ASSERT_TRUE(host_write_overlapped) << "The disjoint host write waited for the pending CUDA producer.";
  expect_equal(reader.get_view(0, 0), device_values);
  expect_equal(reader.get_view(0, 1), host_values);
}

TEST(cuda_visible_phy_support_test, resource_grid_repeated_device_mapping_and_reading_cycles_are_ordered)
{
  testing::AssertionResult cuda_available = cuda_managed_memory_available();
  if (!cuda_available) {
    GTEST_SKIP() << cuda_available.message();
  }

  scoped_env_var grid_mode("OCUDU_UL_CUDA_VISIBLE_GRID");
  ASSERT_TRUE(grid_mode.set("managed"));

  static constexpr unsigned nof_ports   = 2;
  static constexpr unsigned nof_symbols = 4;
  static constexpr unsigned nof_subc    = 24;

  auto factory = create_phy_acceleration_resource_grid_factory(
      create_resource_grid_factory(), phy_acceleration_resource_grid_direction::uplink, true);
  std::unique_ptr<resource_grid> grid = factory->create(nof_ports, nof_symbols, nof_subc);
  ASSERT_NE(grid, nullptr);

  resource_grid_writer&       writer = grid->get_writer();
  const resource_grid_reader& reader = grid->get_reader();
  ASSERT_TRUE(writer.supports_device_grid_mapping());
  ASSERT_TRUE(reader.supports_device_grid_reading());

  cuda_stream_guard stream;
  ASSERT_TRUE(stream.create());

  for (unsigned iteration = 0; iteration != 12; ++iteration) {
    SCOPED_TRACE("iteration=" + std::to_string(iteration));

    std::vector<cbf16_t> payload = generate_grid_payload(nof_ports, nof_symbols, nof_subc);
    for (cbf16_t& sample : payload) {
      cf_t value = to_cf(sample);
      sample     = cbf16_t(value.real() + 0.001F * static_cast<float>(iteration),
                       value.imag() - 0.001F * static_cast<float>(iteration));
    }

    ASSERT_TRUE(writer.prepare_device_grid_mapping(stream.get()));
    ASSERT_EQ(cudaMemcpyAsync(writer.get_device_grid_bf16(),
                              payload.data(),
                              payload.size() * sizeof(cbf16_t),
                              cudaMemcpyHostToDevice,
                              stream.get()),
              cudaSuccess);
    ASSERT_TRUE(writer.on_device_grid_mapping_enqueued(stream.get()));

    const unsigned port   = iteration % nof_ports;
    const unsigned symbol = iteration % nof_symbols;
    const size_t   offset = (static_cast<size_t>(port) * nof_symbols + symbol) * nof_subc;
    expect_equal(reader.get_view(port, symbol), span<const cbf16_t>(payload.data() + offset, nof_subc));

    ASSERT_TRUE(reader.prepare_device_grid_reading(stream.get()));
    std::vector<cbf16_t> copied(nof_subc);
    const cbf16_t*       device_grid = static_cast<const cbf16_t*>(reader.get_device_grid_cbf16());
    ASSERT_NE(device_grid, nullptr);
    ASSERT_EQ(
        cudaMemcpyAsync(
            copied.data(), device_grid + offset, copied.size() * sizeof(cbf16_t), cudaMemcpyDeviceToHost, stream.get()),
        cudaSuccess);
    ASSERT_TRUE(reader.on_device_grid_reading_enqueued(stream.get()));
    ASSERT_EQ(cudaStreamSynchronize(stream.get()), cudaSuccess);
    ASSERT_TRUE(reader.synchronize_device_grid_reading());
    EXPECT_EQ(copied, std::vector<cbf16_t>(payload.begin() + offset, payload.begin() + offset + nof_subc));

    ASSERT_TRUE(writer.prepare_device_grid_mapping(stream.get()));
    ASSERT_TRUE(writer.cancel_device_grid_mapping());
  }
}

TEST(cuda_visible_phy_support_test, prach_buffer_factory_uses_fallback_when_disabled_or_pinned)
{
  scoped_env_var buffer_mode("OCUDU_UL_CUDA_VISIBLE_PRACH_BUFFER");

  ASSERT_TRUE(buffer_mode.set("managed"));
  std::unique_ptr<prach_buffer> disabled_buffer = create_phy_acceleration_prach_buffer_short(2, 2, 2, false);
  ASSERT_NE(disabled_buffer, nullptr);
  EXPECT_FALSE(disabled_buffer->supports_device_prach_buffer_mapping());
  EXPECT_FALSE(disabled_buffer->supports_device_prach_buffer_reading());

  ASSERT_TRUE(buffer_mode.set("pinned"));
  std::unique_ptr<prach_buffer> pinned_buffer = create_phy_acceleration_prach_buffer_short(2, 2, 2, true);
  ASSERT_NE(pinned_buffer, nullptr);
  EXPECT_FALSE(pinned_buffer->supports_device_prach_buffer_mapping());
  EXPECT_FALSE(pinned_buffer->supports_device_prach_buffer_reading());
}

TEST(cuda_visible_phy_support_test, prach_buffer_device_mapping_is_visible_to_host_reader)
{
  testing::AssertionResult cuda_available = cuda_managed_memory_available();
  if (!cuda_available) {
    GTEST_SKIP() << cuda_available.message();
  }

  scoped_env_var buffer_mode("OCUDU_UL_CUDA_VISIBLE_PRACH_BUFFER");
  ASSERT_TRUE(buffer_mode.set("managed"));

  std::unique_ptr<prach_buffer> buffer = create_phy_acceleration_prach_buffer_short(2, 2, 2, true);
  ASSERT_NE(buffer, nullptr);
  ASSERT_TRUE(buffer->supports_device_prach_buffer_mapping());
  ASSERT_TRUE(buffer->supports_device_prach_buffer_reading());
  ASSERT_NE(buffer->get_device_prach_buffer_cbf16(), nullptr);

  static constexpr unsigned port        = 1;
  static constexpr unsigned td_occasion = 1;
  static constexpr unsigned fd_occasion = 0;
  static constexpr unsigned symbol      = 3;

  cuda_stream_guard stream;
  ASSERT_TRUE(stream.create());

  std::vector<cbf16_t> expected      = generate_symbol_payload(buffer->get_sequence_length(), 0.011F);
  const unsigned       offset        = buffer->get_device_prach_symbol_offset(port, td_occasion, fd_occasion, symbol);
  cbf16_t*             device_buffer = static_cast<cbf16_t*>(buffer->get_device_prach_buffer_cbf16());

  ASSERT_TRUE(buffer->prepare_device_prach_buffer_mapping(stream.get()));
  ASSERT_EQ(cudaMemcpyAsync(device_buffer + offset,
                            expected.data(),
                            expected.size() * sizeof(cbf16_t),
                            cudaMemcpyHostToDevice,
                            stream.get()),
            cudaSuccess);
  ASSERT_TRUE(buffer->on_device_prach_buffer_mapping_enqueued(stream.get()));

  expect_equal(buffer->get_symbol(port, td_occasion, fd_occasion, symbol), span<const cbf16_t>(expected));
}

TEST(cuda_visible_phy_support_test, prach_buffer_host_writer_is_visible_to_device_reader)
{
  testing::AssertionResult cuda_available = cuda_managed_memory_available();
  if (!cuda_available) {
    GTEST_SKIP() << cuda_available.message();
  }

  scoped_env_var buffer_mode("OCUDU_UL_CUDA_VISIBLE_PRACH_BUFFER");
  ASSERT_TRUE(buffer_mode.set("managed"));

  std::unique_ptr<prach_buffer> buffer = create_phy_acceleration_prach_buffer_short(2, 2, 2, true);
  ASSERT_NE(buffer, nullptr);
  ASSERT_TRUE(buffer->supports_device_prach_buffer_reading());

  static constexpr unsigned port        = 0;
  static constexpr unsigned td_occasion = 1;
  static constexpr unsigned fd_occasion = 1;
  static constexpr unsigned symbol      = 4;

  std::vector<cbf16_t> expected = generate_symbol_payload(buffer->get_sequence_length(), 0.013F);
  std::copy(expected.begin(), expected.end(), buffer->get_symbol(port, td_occasion, fd_occasion, symbol).begin());

  cuda_stream_guard stream;
  ASSERT_TRUE(stream.create());

  ASSERT_TRUE(buffer->prepare_device_prach_buffer_reading(stream.get()));
  std::vector<cbf16_t> copied(expected.size());
  const cbf16_t*       device_buffer = static_cast<const cbf16_t*>(buffer->get_device_prach_buffer_cbf16());
  const unsigned       offset        = buffer->get_device_prach_symbol_offset(port, td_occasion, fd_occasion, symbol);

  ASSERT_EQ(
      cudaMemcpyAsync(
          copied.data(), device_buffer + offset, copied.size() * sizeof(cbf16_t), cudaMemcpyDeviceToHost, stream.get()),
      cudaSuccess);
  ASSERT_EQ(cudaStreamSynchronize(stream.get()), cudaSuccess);

  EXPECT_EQ(copied, expected);
}

TEST(cuda_visible_phy_support_test, prach_buffer_repeated_device_mapping_and_reading_cycles_are_ordered)
{
  testing::AssertionResult cuda_available = cuda_managed_memory_available();
  if (!cuda_available) {
    GTEST_SKIP() << cuda_available.message();
  }

  scoped_env_var buffer_mode("OCUDU_UL_CUDA_VISIBLE_PRACH_BUFFER");
  ASSERT_TRUE(buffer_mode.set("managed"));

  std::unique_ptr<prach_buffer> buffer = create_phy_acceleration_prach_buffer_short(2, 2, 2, true);
  ASSERT_NE(buffer, nullptr);
  ASSERT_TRUE(buffer->supports_device_prach_buffer_mapping());
  ASSERT_TRUE(buffer->supports_device_prach_buffer_reading());

  cuda_stream_guard stream;
  ASSERT_TRUE(stream.create());

  for (unsigned iteration = 0; iteration != 12; ++iteration) {
    SCOPED_TRACE("iteration=" + std::to_string(iteration));

    const unsigned port        = iteration % buffer->get_max_nof_ports();
    const unsigned td_occasion = (iteration / 2) % buffer->get_max_nof_td_occasions();
    const unsigned fd_occasion = (iteration / 3) % buffer->get_max_nof_fd_occasions();
    const unsigned symbol      = iteration % buffer->get_max_nof_symbols();

    std::vector<cbf16_t> expected =
        generate_symbol_payload(buffer->get_sequence_length(), 0.003F + 0.001F * static_cast<float>(iteration));
    const unsigned offset        = buffer->get_device_prach_symbol_offset(port, td_occasion, fd_occasion, symbol);
    cbf16_t*       device_buffer = static_cast<cbf16_t*>(buffer->get_device_prach_buffer_cbf16());
    ASSERT_NE(device_buffer, nullptr);

    ASSERT_TRUE(buffer->prepare_device_prach_buffer_mapping(stream.get()));
    ASSERT_EQ(cudaMemcpyAsync(device_buffer + offset,
                              expected.data(),
                              expected.size() * sizeof(cbf16_t),
                              cudaMemcpyHostToDevice,
                              stream.get()),
              cudaSuccess);
    ASSERT_TRUE(buffer->on_device_prach_buffer_mapping_enqueued(stream.get()));
    expect_equal(buffer->get_symbol(port, td_occasion, fd_occasion, symbol), span<const cbf16_t>(expected));

    ASSERT_TRUE(buffer->prepare_device_prach_buffer_reading(stream.get()));
    std::vector<cbf16_t> copied(expected.size());
    ASSERT_EQ(cudaMemcpyAsync(copied.data(),
                              device_buffer + offset,
                              copied.size() * sizeof(cbf16_t),
                              cudaMemcpyDeviceToHost,
                              stream.get()),
              cudaSuccess);
    ASSERT_EQ(cudaStreamSynchronize(stream.get()), cudaSuccess);
    EXPECT_EQ(copied, expected);

    ASSERT_TRUE(buffer->prepare_device_prach_buffer_mapping(stream.get()));
    ASSERT_TRUE(buffer->cancel_device_prach_buffer_mapping());
  }
}

TEST(cuda_visible_phy_support_test, prach_buffer_sustained_device_only_cycles_remain_ordered)
{
  testing::AssertionResult cuda_available = cuda_managed_memory_available();
  if (!cuda_available) {
    GTEST_SKIP() << cuda_available.message();
  }

  scoped_env_var buffer_mode("OCUDU_UL_CUDA_VISIBLE_PRACH_BUFFER");
  ASSERT_TRUE(buffer_mode.set("managed"));

  std::unique_ptr<prach_buffer> buffer = create_phy_acceleration_prach_buffer_short(1, 1, 1, true);
  ASSERT_NE(buffer, nullptr);
  ASSERT_TRUE(buffer->supports_device_prach_buffer_mapping());
  ASSERT_TRUE(buffer->supports_device_prach_buffer_reading());

  cuda_stream_guard producer_stream;
  cuda_stream_guard consumer_stream;
  ASSERT_TRUE(producer_stream.create());
  ASSERT_TRUE(consumer_stream.create());

  cbf16_t* device_buffer = static_cast<cbf16_t*>(buffer->get_device_prach_buffer_cbf16());
  ASSERT_NE(device_buffer, nullptr);
  const unsigned offset = buffer->get_device_prach_symbol_offset(0, 0, 0, 0);

  for (unsigned iteration = 0; iteration != 512; ++iteration) {
    SCOPED_TRACE("iteration=" + std::to_string(iteration));
    std::vector<cbf16_t> expected =
        generate_symbol_payload(buffer->get_sequence_length(), 0.001F * static_cast<float>(iteration + 1));

    ASSERT_TRUE(buffer->prepare_device_prach_buffer_mapping(producer_stream.get()));
    ASSERT_EQ(cudaMemcpyAsync(device_buffer + offset,
                              expected.data(),
                              expected.size() * sizeof(cbf16_t),
                              cudaMemcpyHostToDevice,
                              producer_stream.get()),
              cudaSuccess);
    ASSERT_TRUE(buffer->on_device_prach_buffer_mapping_enqueued(producer_stream.get()));

    ASSERT_TRUE(buffer->prepare_device_prach_buffer_reading(consumer_stream.get()));
    std::vector<cbf16_t> copied(expected.size());
    ASSERT_EQ(cudaMemcpyAsync(copied.data(),
                              device_buffer + offset,
                              copied.size() * sizeof(cbf16_t),
                              cudaMemcpyDeviceToHost,
                              consumer_stream.get()),
              cudaSuccess);
    ASSERT_EQ(cudaStreamSynchronize(consumer_stream.get()), cudaSuccess);
    EXPECT_EQ(copied, expected);
  }
}
