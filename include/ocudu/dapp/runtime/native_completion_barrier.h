// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <chrono>

namespace ocudu::dapp {

/// Host-owned proof that asynchronous native-module work can no longer execute module code.
class native_completion_barrier
{
public:
  virtual ~native_completion_barrier()                                             = default;
  virtual bool wait_until(std::chrono::steady_clock::time_point deadline) noexcept = 0;
};

} // namespace ocudu::dapp
