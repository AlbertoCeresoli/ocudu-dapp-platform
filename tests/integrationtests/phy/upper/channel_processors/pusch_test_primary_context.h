// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

namespace ocudu::test_support {

/// Isolated-process test ownership, not production recovery. The caller must
/// destroy every processor/grid/module consumer before close(). Initialization
/// or stack failures retain uncertain resources until this test process exits;
/// destruction never resets a device or speculatively unwinds a CUDA stack.
/// Operations are injectable so every state transition can be tested without CUDA.
template <typename Operations>
class pusch_test_primary_context
{
public:
  explicit pusch_test_primary_context(Operations& operations) : ops(operations) {}
  pusch_test_primary_context(const pusch_test_primary_context&)            = delete;
  pusch_test_primary_context& operator=(const pusch_test_primary_context&) = delete;

  bool initialize()
  {
    if (attempted) {
      return false;
    }
    attempted = true;
    if (!ops.driver_init() || !ops.current(previous) || !ops.device(device) || !ops.retain(context, device) ||
        !context || !ops.push(context)) {
      return false;
    }
    typename Operations::context_type current{};
    int                               runtime_device = -1;
    if (!ops.current(current) || current != context || !ops.runtime_device(runtime_device) ||
        runtime_device != device || !ops.current(current) || current != context) {
      return false;
    }
    ready = true;
    return true;
  }

  bool close()
  {
    if (closed) {
      return true;
    }
    if (!ready) {
      return false;
    }
    // No retry after an uncertain failure: a failed pop/release may have changed
    // backend state. Never pop somebody else's context or release its storage.
    ready = false;
    typename Operations::context_type current{};
    typename Operations::context_type popped{};
    if (!ops.current(current) || current != context || !ops.pop(popped) || popped != context || !ops.current(current) ||
        current != previous || !ops.release(device)) {
      return false;
    }
    closed = true;
    return true;
  }

private:
  Operations&                       ops;
  typename Operations::context_type previous{};
  typename Operations::context_type context{};
  int                               device    = -1;
  bool                              attempted = false;
  bool                              ready     = false;
  bool                              closed    = false;
};
} // namespace ocudu::test_support
