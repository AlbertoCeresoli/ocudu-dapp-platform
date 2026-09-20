// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once
#include <utility>

namespace ocudu {
/// Move-only, allocation-free host processing-lifetime loan. Not an accelerator fence or a dApp ABI payload.
/// Acquire before publishing a terminal result or scheduling a consumer which can outlive its caller. The
/// last owner returns its pool lease only after every processing/diagnostic consumer releases its activity.
class pusch_processing_activity
{
public:
  pusch_processing_activity() = default;
  pusch_processing_activity(void* context_, void (*release_)(void*) noexcept) noexcept :
    context(context_), release(release_)
  {
  }
  ~pusch_processing_activity() { reset(); }
  pusch_processing_activity(const pusch_processing_activity&)            = delete;
  pusch_processing_activity& operator=(const pusch_processing_activity&) = delete;
  pusch_processing_activity(pusch_processing_activity&& other) noexcept :
    context(std::exchange(other.context, nullptr)), release(std::exchange(other.release, nullptr))
  {
  }
  pusch_processing_activity& operator=(pusch_processing_activity&& other) noexcept
  {
    if (this != &other) {
      reset();
      context = std::exchange(other.context, nullptr);
      release = std::exchange(other.release, nullptr);
    }
    return *this;
  }
  explicit operator bool() const noexcept { return release != nullptr; }
  void     reset() noexcept
  {
    auto* owner    = std::exchange(context, nullptr);
    auto  callback = std::exchange(release, nullptr);
    if (callback) {
      callback(owner);
    }
  }

private:
  void* context{};
  void (*release)(void*) noexcept {};
};
} // namespace ocudu
