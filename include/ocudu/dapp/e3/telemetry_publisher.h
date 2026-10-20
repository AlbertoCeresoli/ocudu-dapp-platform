// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/e3/portable_data_store.h"
#include "ocudu/dapp/e3/telemetry_types.h"
#include "ocudu/phy/upper/diagnostics/phy_diagnostics.h"
#include <chrono>
#include <memory>
#include <string_view>

namespace ocudu::dapp::e3 {

struct telemetry_publisher_config {
  std::shared_ptr<portable_data_store> store;
  /// Default per-RNTI publication floor for each stream, used for subscribers
  /// that do not request a capture interval; zero publishes every event.
  std::chrono::microseconds pusch_symbols_minimum_interval{std::chrono::milliseconds(50)};
  std::chrono::microseconds pusch_crc_minimum_interval{0};
  std::chrono::microseconds srs_channel_minimum_interval{std::chrono::milliseconds(20)};
  std::chrono::microseconds pusch_dmrs_minimum_interval{std::chrono::milliseconds(20)};
  std::chrono::microseconds pusch_grid_minimum_interval{std::chrono::milliseconds(100)};
  /// Hard floor no subscriber can go below, enforced as an aggregate
  /// per-stream bound: the stream as a whole never publishes faster than it,
  /// across all RNTIs. The per-RNTI effective floor is max(hard floor,
  /// fastest interval among active subscribers, where an unspecified request
  /// counts as the default). Zero allows full rate.
  std::chrono::microseconds pusch_symbols_hard_floor{0};
  std::chrono::microseconds pusch_crc_hard_floor{0};
  std::chrono::microseconds srs_channel_hard_floor{0};
  std::chrono::microseconds pusch_dmrs_hard_floor{0};
  std::chrono::microseconds pusch_grid_hard_floor{std::chrono::milliseconds(20)};
  /// Cell attributed to PHY events that carry no cell identity of their own.
  uint16_t default_cell_id{0};
};

struct telemetry_publisher_metrics {
  uint64_t published{};
  uint64_t rate_limited{};
  uint64_t no_subscription{};
  uint64_t dropped{};
  uint64_t oversized{};
  uint64_t invalid{};
};

/// Bridges upper-PHY diagnostics and gNB JSON metrics into the bounded portable
/// data store as telemetry publications.
///
/// Radio producers pay one metadata memcpy plus the store's bounded copy and
/// never allocate, block, or run a codec. A stream with no admitted subscriber
/// is gated out in \c should_collect before any expensive capture work.
class telemetry_publisher final : public phy_diagnostic_sink
{
public:
  static std::pair<std::shared_ptr<telemetry_publisher>, std::string>
  create(telemetry_publisher_config config) noexcept;

  ~telemetry_publisher() override;
  telemetry_publisher(const telemetry_publisher&)            = delete;
  telemetry_publisher& operator=(const telemetry_publisher&) = delete;

  bool should_collect(phy_diagnostic_stream stream, uint16_t rnti) override;
  bool is_collect_due(phy_diagnostic_stream stream, uint16_t rnti) override;
  void on_pusch_crc(const pusch_crc_diagnostic& event) override;
  void on_pusch_symbols(const pusch_symbols_diagnostic& event) override;
  void on_srs_channel(const srs_channel_diagnostic& event) override;
  void on_pusch_dmrs_channel(const pusch_dmrs_channel_diagnostic& event) override;
  void on_pusch_grid(const pusch_grid_diagnostic& event) override;

  /// Publishes one already-generated gNB JSON metrics report. Safe from any
  /// non-real-time thread; returns false when the report was not queued.
  bool publish_metrics_json(std::string_view json) noexcept;

  /// Constant-time gate used by producers that hold a JSON report.
  bool metrics_json_active() const noexcept;

  /// Publishes the SRS configuration of one UE context. Control path: may allocate and lock. The publisher assigns
  /// \c config_epoch (it advances when a context is created or removed, or its configuration changes) and keeps the
  /// latest record per cell and RNTI, which \ref poll_srs_config_snapshot replays to new subscribers.
  void publish_srs_config(uint16_t cell_id, srs_config_metadata_v1 record) noexcept;

  /// Replays every cached SRS configuration, as snapshot events, once the srs.config stream has gained a subscriber.
  /// Real-time safe: constant time when nothing changed, and skipped while a configuration update holds the cache.
  void poll_srs_config_snapshot() noexcept;

  /// Publishes one timing advance command. Real-time safe.
  void publish_ta_command(uint16_t cell_id, const ta_command_metadata_v1& record) noexcept;

  /// Constant-time gate for the timing advance producer.
  bool ta_command_active() const noexcept;

  telemetry_publisher_metrics metrics() const noexcept;

private:
  struct implementation;
  explicit telemetry_publisher(std::unique_ptr<implementation> state_);
  std::unique_ptr<implementation> state;
};

} // namespace ocudu::dapp::e3
