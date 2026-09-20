// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "pusch_test_receiver_retirement_trace.h"
#include "ocudu/dapp/runtime/native_worker.h"
#include <atomic>
#include <cstdio>
#include <cuda_runtime_api.h>
#include <dlfcn.h>

namespace {
std::atomic<bool>     trace_enabled{};
std::atomic<unsigned> traced{};
void report(const char* phase, unsigned status, void* caller, unsigned ready = 0, unsigned valid = 0) noexcept
{
  if (!trace_enabled.load(std::memory_order_relaxed) || traced.fetch_add(1, std::memory_order_relaxed) >= 128) {
    return;
  }
  Dl_info    info{};
  const bool located = dladdr(caller, &info) != 0;
  const auto offset  = located ? reinterpret_cast<uintptr_t>(caller) - reinterpret_cast<uintptr_t>(info.dli_fbase) : 0;
  std::fprintf(stderr,
               "RECEIVER_RETIREMENT phase=%s status=%u ready=%u valid=%u caller_offset=0x%zx located=%u\n",
               phase,
               status,
               ready,
               valid,
               size_t(offset),
               unsigned(located));
  std::fflush(stderr);
}
} // namespace

void ocudu::test_support::enable_receiver_retirement_trace(bool enabled) noexcept
{
  trace_enabled.store(enabled, std::memory_order_relaxed);
}

extern "C" cudaError_t __real_cudaStreamQuery(cudaStream_t);
extern "C" cudaError_t __wrap_cudaStreamQuery(cudaStream_t stream)
{
  const auto result = __real_cudaStreamQuery(stream);
  report("stream_query", unsigned(result), __builtin_return_address(0));
  return result;
}

ocudu_dapp_status_v1
real_receiver_inspect(ocudu::dapp::native_worker*,
                      const ocudu::dapp::native_receiver_ticket&,
                      ocudu_dapp_receiver_completion_result_v1&) asm("__real_" PUSCH_TEST_INSPECT_SYMBOL);
ocudu_dapp_status_v1
                     wrap_receiver_inspect(ocudu::dapp::native_worker*,
                                           const ocudu::dapp::native_receiver_ticket&,
                                           ocudu_dapp_receiver_completion_result_v1&) asm("__wrap_" PUSCH_TEST_INSPECT_SYMBOL);
ocudu_dapp_status_v1 wrap_receiver_inspect(ocudu::dapp::native_worker*                worker,
                                           const ocudu::dapp::native_receiver_ticket& ticket,
                                           ocudu_dapp_receiver_completion_result_v1&  output)
{
  const auto result = real_receiver_inspect(worker, ticket, output);
  report("inspect", unsigned(result), __builtin_return_address(0), output.ready, output.valid);
  return result;
}

ocudu_dapp_status_v1
real_receiver_release(ocudu::dapp::native_worker*,
                      const ocudu::dapp::native_receiver_ticket&) asm("__real_" PUSCH_TEST_RELEASE_SYMBOL);
ocudu_dapp_status_v1
                     wrap_receiver_release(ocudu::dapp::native_worker*,
                                           const ocudu::dapp::native_receiver_ticket&) asm("__wrap_" PUSCH_TEST_RELEASE_SYMBOL);
ocudu_dapp_status_v1 wrap_receiver_release(ocudu::dapp::native_worker*                worker,
                                           const ocudu::dapp::native_receiver_ticket& ticket)
{
  const auto result = real_receiver_release(worker, ticket);
  report("release", unsigned(result), __builtin_return_address(0));
  return result;
}
