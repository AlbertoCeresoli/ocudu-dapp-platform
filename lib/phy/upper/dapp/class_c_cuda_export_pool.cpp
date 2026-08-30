// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "class_c_cuda_export_pool.h"
#include "ocudu/dapp/process/v1/cuda_export.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cuda_runtime_api.h>
#include <fcntl.h>
#include <limits>
#include <new>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace ocudu;
using namespace ocudu::dapp;

namespace {

static_assert(sizeof(cudaIpcMemHandle_t) == OCUDU_DAPP_CUDA_IPC_HANDLE_BYTES_V1);
static_assert(sizeof(cudaIpcEventHandle_t) == OCUDU_DAPP_CUDA_IPC_HANDLE_BYTES_V1);

std::string cuda_error(const char* operation, cudaError_t status)
{
  return std::string(operation) + ": " + cudaGetErrorString(status);
}

std::string system_error(const char* operation)
{
  return std::string(operation) + ": " + std::strerror(errno);
}

class pool_impl final : public class_c_cuda_export_pool
{
  enum class slot_state : uint8_t { free, reserved, committed };

  struct slot {
    void*       memory{};
    cudaEvent_t ready{};
    slot_state  state{slot_state::free};
  };

public:
  static class_c_cuda_export_pool_create_result create(uint32_t count, uint64_t capacity)
  {
    if (count < 2 || count > OCUDU_DAPP_CUDA_EXPORT_MAX_SLOTS_V1 || capacity == 0 ||
        capacity > std::numeric_limits<std::size_t>::max()) {
      return {OCUDU_DAPP_INVALID_INPUT_V1, "invalid CUDA export pool bounds", {}};
    }
    int          device = 0;
    cudaStream_t stream = nullptr;
    cudaError_t  status = cudaGetDevice(&device);
    if (status == cudaSuccess) {
      status = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
    }
    if (status != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      return {OCUDU_DAPP_UNSUPPORTED_V1, cuda_error("create CUDA export stream", status), {}};
    }
    try {
      auto        result = std::unique_ptr<pool_impl>(new pool_impl(stream, device, count, capacity));
      std::string detail;
      if (!result->initialize(detail)) {
        return {OCUDU_DAPP_UNSUPPORTED_V1, std::move(detail), {}};
      }
      return {OCUDU_DAPP_OK_V1, {}, std::move(result)};
    } catch (const std::bad_alloc&) {
      static_cast<void>(cudaStreamDestroy(stream));
      return {OCUDU_DAPP_RESOURCE_V1, "unable to allocate CUDA export pool state", {}};
    } catch (...) {
      static_cast<void>(cudaStreamDestroy(stream));
      return {OCUDU_DAPP_INTERNAL_ERROR_V1, "unknown CUDA export pool construction failure", {}};
    }
  }

  ~pool_impl() override
  {
    static_cast<void>(cudaSetDevice(device));
    static_cast<void>(cudaStreamSynchronize(copy_stream));
    for (auto& item : slots) {
      if (item.ready != nullptr) {
        static_cast<void>(cudaEventDestroy(item.ready));
      }
      if (item.memory != nullptr) {
        static_cast<void>(cudaFree(item.memory));
      }
    }
    static_cast<void>(cudaStreamDestroy(copy_stream));
  }

  unique_fd export_descriptor() const noexcept override
  {
    char      path[64];
    const int length = std::snprintf(path, sizeof(path), "/proc/self/fd/%d", descriptor.value());
    return length > 0 && static_cast<std::size_t>(length) < sizeof(path) ? unique_fd(::open(path, O_RDONLY | O_CLOEXEC))
                                                                         : unique_fd{};
  }
  void*                            stream() const noexcept override { return copy_stream; }
  uint32_t                         device_ordinal() const noexcept override { return static_cast<uint32_t>(device); }
  uint32_t                         slot_count() const noexcept override { return slots.size(); }
  uint64_t                         slot_capacity() const noexcept override { return capacity; }
  class_c_cuda_export_pool_metrics snapshot_metrics() const noexcept override
  {
    return {slots.size(),
            in_use.load(std::memory_order_relaxed),
            high_watermark.load(std::memory_order_relaxed),
            reservation_failures.load(std::memory_order_relaxed)};
  }

  uint32_t reserve() noexcept override
  {
    for (uint32_t index = 0; index != slots.size(); ++index) {
      if (slots[index].state == slot_state::free) {
        slots[index].state     = slot_state::reserved;
        const uint64_t current = in_use.fetch_add(1, std::memory_order_relaxed) + 1;
        uint64_t       high    = high_watermark.load(std::memory_order_relaxed);
        while (current > high && !high_watermark.compare_exchange_weak(high, current, std::memory_order_relaxed)) {
        }
        return index;
      }
    }
    reservation_failures.fetch_add(1, std::memory_order_relaxed);
    return std::numeric_limits<uint32_t>::max();
  }

  bool snapshot(uint32_t index, const ocudu_dapp_tensor_view_v1& source) noexcept override
  {
    if (index >= slots.size() || slots[index].state != slot_state::reserved ||
        source.domain != OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1 || source.backend_id != OCUDU_DAPP_BACKEND_CUDA_V1 ||
        source.device_ordinal != static_cast<uint32_t>(device) || source.native_handle == 0 ||
        source.byte_length == 0 || source.byte_length > capacity ||
        source.byte_offset > std::numeric_limits<uintptr_t>::max() - source.native_handle) {
      return false;
    }
    const auto* input =
        reinterpret_cast<const void*>(static_cast<uintptr_t>(source.native_handle + source.byte_offset));
    const cudaError_t copied =
        cudaMemcpyAsync(slots[index].memory, input, source.byte_length, cudaMemcpyDeviceToDevice, copy_stream);
    const cudaError_t recorded = copied == cudaSuccess ? cudaEventRecord(slots[index].ready, copy_stream) : copied;
    if (recorded != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      return false;
    }
    return true;
  }

  bool commit(uint32_t index) noexcept override
  {
    if (index >= slots.size() || slots[index].state != slot_state::reserved || committed_count == committed.size()) {
      return false;
    }
    slots[index].state                                               = slot_state::committed;
    committed[(committed_head + committed_count) % committed.size()] = index;
    ++committed_count;
    return true;
  }

  void cancel(uint32_t index) noexcept override
  {
    if (index < slots.size() && slots[index].state == slot_state::reserved) {
      slots[index].state = slot_state::free;
      in_use.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  bool reclaim(uint64_t completed) noexcept override
  {
    if (completed > committed_count) {
      return false;
    }
    while (completed-- != 0) {
      const uint32_t index = committed[committed_head];
      committed_head       = (committed_head + 1) % committed.size();
      --committed_count;
      if (index >= slots.size() || slots[index].state != slot_state::committed) {
        return false;
      }
      slots[index].state = slot_state::free;
      in_use.fetch_sub(1, std::memory_order_relaxed);
    }
    return true;
  }

  bool reset_after_consumer_restart() noexcept override
  {
    if (cudaSetDevice(device) != cudaSuccess || cudaStreamSynchronize(copy_stream) != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      return false;
    }
    for (auto& item : slots) {
      item.state = slot_state::free;
    }
    committed_head  = 0;
    committed_count = 0;
    in_use.store(0, std::memory_order_relaxed);
    return true;
  }

private:
  pool_impl(cudaStream_t stream, int device_, uint32_t count, uint64_t capacity_) :
    copy_stream(stream), device(device_), capacity(capacity_), slots(count), committed(count)
  {
  }

  bool initialize(std::string& detail)
  {
    if (cudaSetDevice(device) != cudaSuccess) {
      detail = "unable to select CUDA export device";
      return false;
    }
    std::vector<uint8_t> bytes(sizeof(ocudu_dapp_cuda_export_header_v1) +
                               slots.size() * sizeof(ocudu_dapp_cuda_export_slot_v1));
    auto*                header = reinterpret_cast<ocudu_dapp_cuda_export_header_v1*>(bytes.data());
    header->magic               = OCUDU_DAPP_CUDA_EXPORT_MAGIC_V1;
    header->version             = OCUDU_DAPP_CUDA_EXPORT_VERSION_V1;
    header->header_size         = sizeof(*header);
    header->backend_id          = OCUDU_DAPP_BACKEND_CUDA_V1;
    header->device_ordinal      = device;
    header->slot_count          = slots.size();
    header->slot_stride         = sizeof(ocudu_dapp_cuda_export_slot_v1);
    header->slot_capacity       = capacity;

    auto* descriptions = reinterpret_cast<ocudu_dapp_cuda_export_slot_v1*>(bytes.data() + sizeof(*header));
    for (uint32_t index = 0; index != slots.size(); ++index) {
      cudaError_t status = cudaMalloc(&slots[index].memory, capacity);
      if (status == cudaSuccess) {
        status = cudaEventCreateWithFlags(&slots[index].ready, cudaEventDisableTiming | cudaEventInterprocess);
      }
      cudaIpcMemHandle_t   memory_handle{};
      cudaIpcEventHandle_t event_handle{};
      if (status == cudaSuccess) {
        status = cudaIpcGetMemHandle(&memory_handle, slots[index].memory);
      }
      if (status == cudaSuccess) {
        status = cudaIpcGetEventHandle(&event_handle, slots[index].ready);
      }
      if (status != cudaSuccess) {
        detail = cuda_error("create CUDA IPC export slot", status);
        static_cast<void>(cudaGetLastError());
        return false;
      }
      auto& output         = descriptions[index];
      output.struct_size   = sizeof(output);
      output.slot_id       = index;
      output.allocation_id = (static_cast<uint64_t>(device + 1) << 48U) | (index + 1U);
      output.capacity      = capacity;
      std::memcpy(output.memory_handle, &memory_handle, sizeof(memory_handle));
      std::memcpy(output.ready_event_handle, &event_handle, sizeof(event_handle));
    }

    const int fd = ::memfd_create("ocudu-dapp-cuda-export", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
      detail = system_error("create CUDA export descriptor");
      return false;
    }
    descriptor = unique_fd(fd);
    if (::ftruncate(fd, bytes.size()) != 0 ||
        ::pwrite(fd, bytes.data(), bytes.size(), 0) != static_cast<ssize_t>(bytes.size()) ||
        ::fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL) != 0) {
      detail = system_error("seal CUDA export descriptor");
      return false;
    }
    return true;
  }

  cudaStream_t          copy_stream{};
  int                   device{};
  uint64_t              capacity{};
  std::vector<slot>     slots;
  std::vector<uint32_t> committed;
  uint32_t              committed_head{};
  uint32_t              committed_count{};
  std::atomic<uint64_t> in_use{};
  std::atomic<uint64_t> high_watermark{};
  std::atomic<uint64_t> reservation_failures{};
  unique_fd             descriptor;
};

} // namespace

class_c_cuda_export_pool_create_result class_c_cuda_export_pool::create(uint32_t slot_count,
                                                                        uint64_t slot_capacity) noexcept
{
  try {
    return pool_impl::create(slot_count, slot_capacity);
  } catch (const std::bad_alloc&) {
    return {OCUDU_DAPP_RESOURCE_V1, "unable to allocate CUDA export pool", {}};
  } catch (...) {
    return {OCUDU_DAPP_INTERNAL_ERROR_V1, "unknown CUDA export pool failure", {}};
  }
}
