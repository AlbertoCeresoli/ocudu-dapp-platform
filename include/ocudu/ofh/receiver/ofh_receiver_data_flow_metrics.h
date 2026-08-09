// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

namespace ocudu {
namespace ofh {

/// Open Fronthaul direct IQ decompression result counters.
struct rx_direct_decompression_metrics {
  unsigned nof_acceleration_retries;
  unsigned nof_unsupported_destinations;
  unsigned nof_unsupported_compressions;
  unsigned nof_missing_contexts;
  unsigned nof_invalid_inputs;
  unsigned nof_messages_without_relevant_data;
  unsigned nof_resource_unavailable_failures;
  unsigned nof_acceleration_failures;
  unsigned nof_destination_failures;
};

/// Open Fronthaul receiver message processing metrics.
struct rx_data_flow_perf_metrics {
  /// Received message unpacking statistics.
  float message_unpacking_min_latency_us;
  float message_unpacking_max_latency_us;
  float message_unpacking_avg_latency_us;

  /// Number of dropped messages by the data flow.
  unsigned nof_dropped_messages;

  /// Direct IQ decompression result counters.
  rx_direct_decompression_metrics direct_decompression;

  /// CPU usage in microseconds of the message processing in the receiver.
  float cpu_usage_us;
};

} // namespace ofh
} // namespace ocudu
