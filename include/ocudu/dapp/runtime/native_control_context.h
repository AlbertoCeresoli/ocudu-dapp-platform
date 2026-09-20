// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once
#include "ocudu/dapp/abi/v1/base.h"
#include <cstdint>
#include <memory>
#include <type_traits>

namespace ocudu::dapp {
/// Host-private cold callback context and lifetime. Never transported over E3
/// and never entered for per-grant invoke/inspect/release or metric snapshots.
/// An owner must retain this object through partial creation and quiescence.
class native_control_context
{
public:
  virtual ~native_control_context() = default;
  virtual uint32_t backend_id() const noexcept = 0;
  virtual uint64_t identity() const noexcept = 0;
  virtual ocudu_dapp_status_v1 run(ocudu_dapp_status_v1 (*callback)(void*) noexcept, void*) noexcept = 0;

  template <typename Function> ocudu_dapp_status_v1 call(Function&& function) noexcept
  {
    return run([](void* pointer) noexcept {
      try { return (*static_cast<std::remove_reference_t<Function>*>(pointer))(); }
      catch (...) { return OCUDU_DAPP_EXCEPTION_V1; }
    }, const_cast<void*>(static_cast<const void*>(std::addressof(function))));
  }
};
} // namespace ocudu::dapp
