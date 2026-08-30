// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include "ocudu/ran/slot_point.h"
#include <chrono>
#include <cstdint>
#include <memory>

namespace ocudu {

enum class scheduler_dapp_context_result : uint8_t { accepted, invalid, busy };

/// Host-normalized, full-cell PRB confidence map. Producer storage is never retained.
struct scheduler_dapp_interference_update {
  ocudu_dapp_context_header_v1 context;
  span<const uint8_t>          prb_confidence;
  uint64_t                     publish_monotonic_ns;
};

class scheduler_dapp_interference_cache_impl;

/// Pins one immutable cache snapshot for the duration of a scheduler callback.
class scheduler_dapp_interference_lease
{
public:
  scheduler_dapp_interference_lease() noexcept                                           = default;
  scheduler_dapp_interference_lease(const scheduler_dapp_interference_lease&)            = delete;
  scheduler_dapp_interference_lease& operator=(const scheduler_dapp_interference_lease&) = delete;
  scheduler_dapp_interference_lease(scheduler_dapp_interference_lease&& other) noexcept;
  scheduler_dapp_interference_lease& operator=(scheduler_dapp_interference_lease&& other) noexcept;
  ~scheduler_dapp_interference_lease();

  explicit                            operator bool() const noexcept { return context_value != nullptr; }
  const ocudu_dapp_context_header_v1& context() const noexcept { return *context_value; }
  const ocudu_dapp_tensor_view_v1&    mask() const noexcept { return *mask_value; }

private:
  friend class scheduler_dapp_interference_cache_impl;

  scheduler_dapp_interference_lease(const ocudu_dapp_context_header_v1* context_,
                                    const ocudu_dapp_tensor_view_v1*    mask_,
                                    void*                               release_token_,
                                    void (*release_)(void*) noexcept) noexcept;
  void reset() noexcept;

  const ocudu_dapp_context_header_v1* context_value{};
  const ocudu_dapp_tensor_view_v1*    mask_value{};
  void*                               release_token{};
  void (*release)(void*) noexcept {};
};

/// Bounded multi-cell latest-value cache. Publication is serialized and never waits for readers.
class scheduler_dapp_interference_cache
{
public:
  virtual ~scheduler_dapp_interference_cache() = default;

  virtual scheduler_dapp_context_result publish(const scheduler_dapp_interference_update& update) noexcept = 0;
  virtual scheduler_dapp_context_result invalidate(uint32_t cell_id) noexcept                              = 0;

  /// The cache owner must outlive every returned lease; scheduler decorators satisfy this by shared ownership.
  virtual scheduler_dapp_interference_lease acquire(uint32_t   cell_id,
                                                    uint32_t   cell_nof_prbs,
                                                    slot_point transmission_time,
                                                    uint64_t   now_monotonic_ns) noexcept = 0;
};

std::shared_ptr<scheduler_dapp_interference_cache> create_scheduler_dapp_interference_cache(
    std::chrono::nanoseconds max_residence = std::chrono::milliseconds(250)) noexcept;

} // namespace ocudu
