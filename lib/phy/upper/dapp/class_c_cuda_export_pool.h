// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/abi/v1/base.h"
#include "ocudu/dapp/abi/v1/memory.h"
#include "ocudu/support/io/unique_fd.h"
#include <cstdint>
#include <memory>
#include <string>

namespace ocudu::dapp {

class class_c_cuda_export_pool;

struct class_c_cuda_export_pool_metrics {
  uint64_t capacity{};
  uint64_t in_use{};
  uint64_t high_watermark{};
  uint64_t reservation_failures{};
};

struct class_c_cuda_export_pool_create_result {
  ocudu_dapp_status_v1                      status{OCUDU_DAPP_INTERNAL_ERROR_V1};
  std::string                               detail;
  std::unique_ptr<class_c_cuda_export_pool> pool;
  explicit operator bool() const noexcept { return status == OCUDU_DAPP_OK_V1 && pool != nullptr; }
};

/// Fixed route-owned CUDA IPC snapshot pool. All methods are called by one
/// admitted producer lane; no internal hot-path lock is required.
class class_c_cuda_export_pool
{
public:
  static class_c_cuda_export_pool_create_result create(uint32_t slot_count, uint64_t slot_capacity) noexcept;

  virtual ~class_c_cuda_export_pool() = default;

  virtual unique_fd                        export_descriptor() const noexcept = 0;
  virtual void*                            stream() const noexcept            = 0;
  virtual uint32_t                         device_ordinal() const noexcept    = 0;
  virtual uint32_t                         slot_count() const noexcept        = 0;
  virtual uint64_t                         slot_capacity() const noexcept     = 0;
  virtual class_c_cuda_export_pool_metrics snapshot_metrics() const noexcept  = 0;

  /// Returns UINT32_MAX when every slot is retained by the worker.
  virtual uint32_t reserve() noexcept = 0;
  /// Enqueues one exact D2D snapshot and records the slot's IPC-ready event.
  virtual bool snapshot(uint32_t slot, const ocudu_dapp_tensor_view_v1& source) noexcept = 0;
  /// Associates a successfully published ring record with the reserved slot.
  virtual bool commit(uint32_t slot) noexcept = 0;
  virtual void cancel(uint32_t slot) noexcept = 0;
  /// Releases the oldest completed publications in ring lease order.
  virtual bool reclaim(uint64_t completed) noexcept = 0;
  /// Requires external proof that the old worker process is dead.
  virtual bool reset_after_consumer_restart() noexcept = 0;
};

} // namespace ocudu::dapp
