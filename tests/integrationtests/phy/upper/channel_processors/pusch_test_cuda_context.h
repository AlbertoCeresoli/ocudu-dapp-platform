// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include "pusch_test_cold_trace.h"
#include "pusch_test_primary_context.h"
#include <cuda.h>
#include <cuda_runtime_api.h>

namespace ocudu::test_support {

/// Driver-first initialization on explicit test device0. This is the same
/// primary-context ownership used by the standalone package replay, now in the
/// fully linked production-host test image. No allocation, stream or kernel is
/// requested here. Runtime/driver initialization may still block internally.
class pusch_test_cuda_context_operations
{
public:
  using context_type = CUcontext;
  explicit pusch_test_cuda_context_operations(pusch_cold_trace& trace_) : trace(trace_) {}

  bool driver_init()
  {
    return call("cuda_driver_init", [] { return cuInit(0); });
  }
  bool current(CUcontext& value)
  {
    return call("cuda_context_current", [&] { return cuCtxGetCurrent(&value); });
  }
  bool device(int& value)
  {
    return call("cuda_device_select", [&] { return cuDeviceGet(&value, 0); });
  }
  bool retain(CUcontext& value, int device)
  {
    return call("cuda_primary_retain", [&] { return cuDevicePrimaryCtxRetain(&value, device); });
  }
  bool push(CUcontext value)
  {
    return call("cuda_context_push", [&] { return cuCtxPushCurrent(value); });
  }
  bool pop(CUcontext& value)
  {
    return call("cuda_context_pop", [&] { return cuCtxPopCurrent(&value); });
  }
  bool release(int device)
  {
    return call("cuda_primary_release", [&] { return cuDevicePrimaryCtxRelease(device); });
  }
  bool runtime_device(int& value)
  {
    int count = 0;
    if (!call("cuda_runtime_device_count", [&] { return cudaGetDeviceCount(&count); }) || count != 1) {
      return false;
    }
    return call("cuda_runtime_device_current", [&] { return cudaGetDevice(&value); });
  }

private:
  template <typename Function>
  bool call(const char* phase, Function&& function)
  {
    return trace.measure(phase, [&] {
      const int status = static_cast<int>(function());
      if (status != 0) {
        throw std::runtime_error(fmt::format("{} failed with CUDA status {}", phase, status));
      }
      return true;
    });
  }
  pusch_cold_trace& trace;
};
} // namespace ocudu::test_support
