// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once
#include "ocudu/dapp/runtime/native_control_context.h"

namespace ocudu {
/// Cold setup on the existing PHY context's thread. Requires an already-active
/// primary context; retains that exact context, never initializes/selects a device.
/// Explicit external reset remains forbidden while host/module users are alive.
/// If provided, the exact PHY stream must belong to the current context. Zero
/// is available to already-context-bound cold consumers, not GPU demodulator installation.
std::shared_ptr<dapp::native_control_context> capture_native_cuda_control_context(uint64_t execution_stream = 0) noexcept;
} // namespace ocudu
