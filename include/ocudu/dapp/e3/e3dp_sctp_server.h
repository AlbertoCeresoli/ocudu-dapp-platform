// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/e3/portable_data_store.h"
#include "ocudu/dapp/e3/sctp_association_server.h"
#include <memory>
#include <string>
#include <vector>

namespace ocudu::dapp::e3 {

class management_service;

/// RF placement reported on data-profile subscription records for one cell.
struct e3dp_cell_placement {
  uint16_t cell_id{};
  uint64_t rf_center_frequency_hz{};
  int64_t  cell_frequency_offset_hz{};
};

struct e3dp_sctp_server_config {
  /// Transport policy. The reference client expects port 38472.
  sctp_server_config transport;
  /// Store queue depth assigned to new subscriptions (64 KiB chunks); it
  /// must hold one maximum-size store object.
  uint32_t queue_depth_chunks{64};
  /// Cells and their RF placement; index 0 is used when a subscription has no
  /// cell filter.
  std::vector<e3dp_cell_placement> cells;
  /// Store subscriptions one association may hold at once. Bounds a single
  /// peer's share of the global store slots and retained-bytes budget.
  uint32_t maximum_subscriptions_per_association{8};
};

struct e3dp_sctp_server_metrics {
  sctp_server_metrics transport;
  uint64_t            requests{};
  uint64_t            malformed_requests{};
  uint64_t            rejected_operations{};
  uint64_t            subscription_controls{};
  uint64_t            polls{};
};

class e3dp_sctp_server;

struct e3dp_sctp_server_create_result {
  std::unique_ptr<e3dp_sctp_server> server;
  std::string                       detail;
  explicit                          operator bool() const noexcept { return server != nullptr; }
};

/// Optional compatibility endpoint implementing the E3 data profile (size-prefixed
/// FlatBuffers "E3DP" over SCTP): persistent associations, one request per SCTP
/// message, in-order responses echoing the transaction id, and only the
/// dataSubscriptions / dataPoll operations. Everything else is answered with
/// invalidRequest and the association stays open.
class e3dp_sctp_server final : private sctp_carrier
{
public:
  static e3dp_sctp_server_create_result create(management_service&                  management,
                                               std::shared_ptr<portable_data_store> store,
                                               e3dp_sctp_server_config              config) noexcept;

  ~e3dp_sctp_server() override;
  e3dp_sctp_server(const e3dp_sctp_server&)            = delete;
  e3dp_sctp_server& operator=(const e3dp_sctp_server&) = delete;

  void                     stop() noexcept;
  uint16_t                 port() const noexcept;
  e3dp_sctp_server_metrics metrics() const noexcept;

private:
  struct implementation;
  explicit e3dp_sctp_server(std::unique_ptr<implementation> state_);

  void on_message(sctp_association& association, ocudu::span<const uint8_t> message) override;
  void on_closed(sctp_association& association) override;

  std::unique_ptr<implementation> state;
};

} // namespace ocudu::dapp::e3
