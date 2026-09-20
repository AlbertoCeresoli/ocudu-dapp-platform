// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "native_cuda_control_context.h"
#include "ocudu/dapp/abi/v1/memory.h"
#include <atomic>
#ifdef ENABLE_CUDA
#include <cuda.h>

namespace {
class cuda_control_context final : public ocudu::dapp::native_control_context
{
public:
  CUcontext context{};
  CUdevice device{-1};
  std::atomic<bool> poisoned{};

  ~cuda_control_context() override
  {
    // On an uncertain stack/driver failure keep our primary reference until
    // process exit. Never reset a context or retry an ambiguous release.
    if (context && !poisoned.load(std::memory_order_acquire))
      static_cast<void>(cuDevicePrimaryCtxRelease(device));
  }
  uint32_t backend_id() const noexcept override { return OCUDU_DAPP_BACKEND_CUDA_V1; }
  uint64_t identity() const noexcept override { return reinterpret_cast<uintptr_t>(context); }
  ocudu_dapp_status_v1 run(ocudu_dapp_status_v1 (*callback)(void*) noexcept, void* argument) noexcept override
  {
    if (!callback || !context || poisoned.load(std::memory_order_acquire)) return OCUDU_DAPP_DEVICE_ERROR_V1;
    CUcontext previous{};
    if (cuCtxGetCurrent(&previous) != CUDA_SUCCESS) return OCUDU_DAPP_DEVICE_ERROR_V1;
    if (cuCtxPushCurrent(context) != CUDA_SUCCESS) {
      poisoned.store(true, std::memory_order_release);
      return OCUDU_DAPP_DEVICE_ERROR_V1;
    }
    const auto result = callback(argument);
    CUcontext current{}, popped{}, restored{};
    if (cuCtxGetCurrent(&current) != CUDA_SUCCESS || current != context ||
        cuCtxPopCurrent(&popped) != CUDA_SUCCESS || popped != context ||
        cuCtxGetCurrent(&restored) != CUDA_SUCCESS || restored != previous) {
      poisoned.store(true, std::memory_order_release);
      return OCUDU_DAPP_DEVICE_ERROR_V1;
    }
    return result;
  }
};
} // namespace
#endif

std::shared_ptr<ocudu::dapp::native_control_context>
ocudu::capture_native_cuda_control_context(uint64_t execution_stream) noexcept
{
#ifdef ENABLE_CUDA
  CUcontext current{};
  CUdevice device{-1};
  unsigned flags{};
  int active{};
  if (cuCtxGetCurrent(&current) != CUDA_SUCCESS || !current || cuCtxGetDevice(&device) != CUDA_SUCCESS ||
      cuDevicePrimaryCtxGetState(device, &flags, &active) != CUDA_SUCCESS || active != 1) return {};
  if (execution_stream) {
    CUcontext stream_context{};
    if (cuStreamGetCtx(reinterpret_cast<CUstream>(static_cast<uintptr_t>(execution_stream)), &stream_context) !=
        CUDA_SUCCESS || stream_context != current) return {};
  }
  try {
    auto owner = std::make_shared<cuda_control_context>();
    owner->device = device;
    if (cuDevicePrimaryCtxRetain(&owner->context, device) != CUDA_SUCCESS) {
      // Even an unexpected output handle on error does not prove that a reference was acquired.
      owner->poisoned.store(true, std::memory_order_release);
      return {};
    }
    if (owner->context != current) return {};
    return owner;
  } catch (...) { return {}; }
#else
  static_cast<void>(execution_stream);
  return {};
#endif
}
