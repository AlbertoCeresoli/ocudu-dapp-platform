// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

namespace ocudu {
namespace ofh {

/// Defines whether the Open Fronthaul receive path may decompress IQ samples into host memory.
enum class host_decompression_policy {
  /// Host decompression may be used when direct decompression into the destination is unavailable.
  allow,
  /// Direct decompression is required. The message is dropped when the direct path cannot be used.
  forbid
};

} // namespace ofh
} // namespace ocudu
