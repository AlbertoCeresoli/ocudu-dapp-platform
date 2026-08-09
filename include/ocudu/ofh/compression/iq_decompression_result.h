// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

namespace ocudu {
namespace ofh {

/// Result of attempting to decompress IQ samples directly into an accelerator-visible destination.
enum class iq_decompression_result {
  success,
  success_after_retry,
  unsupported,
  invalid_input,
  resource_unavailable,
  acceleration_failure,
  destination_failure
};

/// Returns true when direct IQ decompression completed successfully.
constexpr bool is_iq_decompression_success(iq_decompression_result result)
{
  return (result == iq_decompression_result::success) ||
         (result == iq_decompression_result::success_after_retry);
}

} // namespace ofh
} // namespace ocudu
