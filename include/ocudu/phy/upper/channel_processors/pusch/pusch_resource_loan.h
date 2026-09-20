// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include "ocudu/adt/unique_function.h"
#include <type_traits>
#include <utility>

namespace ocudu {

/// Move-only host resource retention with fixed inline storage. Not a public dApp ABI or an accelerator fence.
/// The supplied owner must retain the actual allocations/leases, not merely an unrelated lifetime sentinel.
/// Construct owners during cold provisioning; wrapping, moving and resetting this holder never allocates.
/// Reset only after every consumer is fenced, on a path suitable for the supplied owner's destructors.
class pusch_resource_loan
{
public:
  static constexpr size_t owner_capacity                         = 64;
  pusch_resource_loan()                                          = default;
  pusch_resource_loan(const pusch_resource_loan&)                = delete;
  pusch_resource_loan& operator=(const pusch_resource_loan&)     = delete;
  pusch_resource_loan(pusch_resource_loan&&) noexcept            = default;
  pusch_resource_loan& operator=(pusch_resource_loan&&) noexcept = default;

  template <typename Owner, std::enable_if_t<!std::is_same_v<Owner, pusch_resource_loan>, int> = 0>
  explicit pusch_resource_loan(Owner owner) noexcept
  {
    static_assert(std::is_nothrow_move_constructible_v<Owner> && std::is_nothrow_destructible_v<Owner>,
                  "Resource ownership must move and release without throwing.");
    if (owner) {
      retention = [held = std::move(owner)]() noexcept { static_cast<void>(held); };
    }
  }

  explicit operator bool() const noexcept { return !retention.is_empty(); }
  void     reset() noexcept { retention = {}; }

private:
  unique_function<void(), owner_capacity, true> retention;
};

/// Borrowed only for the duration of demodulate(). Acquire before submitting work; retain the returned loan,
/// never this provider pointer, in deferred continuations. Empty means authentic ownership is unavailable.
class pusch_resource_loan_provider
{
public:
  virtual ~pusch_resource_loan_provider()        = default;
  virtual pusch_resource_loan acquire() noexcept = 0;
};
} // namespace ocudu
