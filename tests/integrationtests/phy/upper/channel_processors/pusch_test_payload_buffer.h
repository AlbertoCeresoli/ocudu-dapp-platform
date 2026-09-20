// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include "ocudu/adt/span.h"
#include "fmt/format.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef ENABLE_CUDA
#include <cuda_runtime_api.h>
#endif

namespace ocudu::test_support {

/// Test-owned output allocation, reused only after the preceding receiver has completed.
/// Reset changes the active extent without allocating; the entire unused capacity is guarded.
class guarded_payload_buffer
{
public:
  guarded_payload_buffer(size_t capacity_bytes_, const std::string& mode_) :
    capacity_bytes(capacity_bytes_), mode(mode_)
  {
    if (capacity_bytes == 0 || capacity_bytes > std::numeric_limits<size_t>::max() - guard_bytes) {
      throw std::runtime_error("Invalid GPU payload capacity.");
    }
    total_bytes = capacity_bytes + guard_bytes;
    allocate();
    reset(capacity_bytes);
  }

  guarded_payload_buffer(const guarded_payload_buffer&)            = delete;
  guarded_payload_buffer& operator=(const guarded_payload_buffer&) = delete;
  ~guarded_payload_buffer() { release(); }

  span<uint8_t>        as_span() { return {data_ptr, payload_bytes}; }
  std::vector<uint8_t> payload_vector() const { return {data_ptr, data_ptr + payload_bytes}; }

  void reset() { reset(payload_bytes); }
  void reset(size_t active_bytes)
  {
    // Reject before changing either the previous extent or any sentinel.
    if (active_bytes == 0 || active_bytes > capacity_bytes) {
      throw std::runtime_error("GPU payload exceeds its prepared capacity.");
    }
    payload_bytes = active_bytes;
    std::fill(data_ptr, data_ptr + payload_bytes, fill_value);
    std::fill(data_ptr + payload_bytes, data_ptr + total_bytes, guard_value);
  }

  bool guard_ok(size_t* first_bad_offset = nullptr, uint8_t* bad_value = nullptr) const
  {
    for (size_t i = payload_bytes; i != total_bytes; ++i) {
      if (data_ptr[i] != guard_value) {
        if (first_bad_offset != nullptr) {
          *first_bad_offset = i - payload_bytes;
        }
        if (bad_value != nullptr) {
          *bad_value = data_ptr[i];
        }
        return false;
      }
    }
    return true;
  }

private:
  void allocate()
  {
    if (mode == "vector") {
      vector_storage.resize(total_bytes);
      data_ptr = vector_storage.data();
      return;
    }
#ifdef ENABLE_CUDA
    if (mode == "pinned") {
      const auto status = cudaHostAlloc(reinterpret_cast<void**>(&data_ptr), total_bytes, cudaHostAllocDefault);
      if (status != cudaSuccess) {
        throw std::runtime_error(
            fmt::format("cudaHostAlloc failed for GPU output payload: {}", cudaGetErrorString(status)));
      }
      return;
    }
    if (mode == "managed") {
      const auto status = cudaMallocManaged(reinterpret_cast<void**>(&data_ptr), total_bytes);
      if (status != cudaSuccess) {
        throw std::runtime_error(
            fmt::format("cudaMallocManaged failed for GPU output payload: {}", cudaGetErrorString(status)));
      }
      return;
    }
#endif
    throw std::runtime_error(fmt::format("Unknown GPU output buffer mode '{}'.", mode));
  }

  void release()
  {
#ifdef ENABLE_CUDA
    if (data_ptr != nullptr && mode == "pinned") {
      (void)cudaFreeHost(data_ptr);
    } else if (data_ptr != nullptr && mode == "managed") {
      (void)cudaFree(data_ptr);
    }
#endif
    data_ptr = nullptr;
  }

  static constexpr size_t  guard_bytes    = 64;
  static constexpr uint8_t fill_value     = 0xa5;
  static constexpr uint8_t guard_value    = 0x7e;
  size_t                   capacity_bytes = 0, payload_bytes = 0, total_bytes = 0;
  std::string              mode;
  std::vector<uint8_t>     vector_storage;
  uint8_t*                 data_ptr = nullptr;
};
} // namespace ocudu::test_support
