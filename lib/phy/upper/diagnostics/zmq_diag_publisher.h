// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief ZMQ PUB transport adapter for real-time upper-PHY diagnostics.

#pragma once

#include "zmq_diag_wire_format.h"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ocudu {

/// ZMQ PUB diagnostic adapter.
///
/// The adapter owns all ZMQ details and keeps the PHY-facing interface
/// transport-neutral through \ref phy_diagnostic_sink. Sends are non-blocking
/// and diagnostic loss is acceptable by design.
class zmq_diag_publisher : public phy_diagnostic_sink
{
public:
  /// Construct from a bind endpoint (e.g. "tcp://127.0.0.1:5555").
  /// Empty string leaves the publisher disabled.
  explicit zmq_diag_publisher(const std::string& endpoint);

  ~zmq_diag_publisher() override;

  zmq_diag_publisher(const zmq_diag_publisher&)            = delete;
  zmq_diag_publisher& operator=(const zmq_diag_publisher&) = delete;

  bool is_enabled() const { return enabled_.load(std::memory_order_acquire); }

  zmq_diag_publisher_stats get_stats() const;

  bool should_collect(phy_diagnostic_stream stream, uint16_t rnti) override;

  bool is_collect_due(phy_diagnostic_stream stream, uint16_t rnti) override;

  void on_pusch_crc(const pusch_crc_diagnostic& event) override;

  void on_pusch_symbols(const pusch_symbols_diagnostic& event) override;

  void on_srs_channel(const srs_channel_diagnostic& event) override;

  void on_pusch_dmrs_channel(const pusch_dmrs_channel_diagnostic& event) override;

  /// Cheap rate-limit check that consumes the publish token for this topic/RNTI.
  bool should_emit(zmq_diag_topic topic, uint16_t rnti);

  /// Non-consuming rate-limit check.
  bool is_emit_due(zmq_diag_topic topic, uint16_t rnti);

private:
  void send_payload(zmq_diag_topic topic, const uint8_t* payload, std::size_t payload_len);

  static uint64_t now_ns()
  {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
  }

  void*                                  ctx_     = nullptr;
  void*                                  sock_    = nullptr;
  std::atomic<bool>                      enabled_ = {false};
  std::mutex                             send_mtx_;
  std::mutex                             rate_mtx_;
  std::unordered_map<uint32_t, uint64_t> last_emit_ns_;

  std::atomic<uint64_t> messages_attempted_{0};
  std::atomic<uint64_t> messages_sent_{0};
  std::atomic<uint64_t> transport_drops_{0};
  std::atomic<uint64_t> oversized_drops_{0};
  std::atomic<uint64_t> rate_policy_skips_{0};
};

} // namespace ocudu
