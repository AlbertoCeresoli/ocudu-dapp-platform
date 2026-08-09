// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/ofh/compression/iq_decompression_result.h"

namespace ocudu {
namespace ofh {

/// Result of attempting to write compressed Open Fronthaul IQ directly into an accelerator-visible destination.
enum class direct_write_result {
  success,
  success_after_retry,
  fallback_unsupported_destination,
  fallback_unsupported_compression,
  failure_missing_context,
  failure_invalid_input,
  failure_no_relevant_data,
  failure_resource_unavailable,
  failure_acceleration,
  failure_destination
};

/// Returns true when the direct write completed successfully.
constexpr bool is_direct_write_success(direct_write_result result)
{
  return (result == direct_write_result::success) || (result == direct_write_result::success_after_retry);
}

/// Returns true when no destination write was attempted and host decompression remains safe.
constexpr bool allows_host_decompression_fallback(direct_write_result result)
{
  return (result == direct_write_result::fallback_unsupported_destination) ||
         (result == direct_write_result::fallback_unsupported_compression);
}

/// Converts an IQ decompressor result into the corresponding direct-write result.
constexpr direct_write_result to_direct_write_result(iq_decompression_result result)
{
  switch (result) {
    case iq_decompression_result::success:
      return direct_write_result::success;
    case iq_decompression_result::success_after_retry:
      return direct_write_result::success_after_retry;
    case iq_decompression_result::unsupported:
      return direct_write_result::fallback_unsupported_compression;
    case iq_decompression_result::invalid_input:
      return direct_write_result::failure_invalid_input;
    case iq_decompression_result::resource_unavailable:
      return direct_write_result::failure_resource_unavailable;
    case iq_decompression_result::acceleration_failure:
      return direct_write_result::failure_acceleration;
    case iq_decompression_result::destination_failure:
      return direct_write_result::failure_destination;
  }

  return direct_write_result::failure_acceleration;
}

} // namespace ofh
} // namespace ocudu
