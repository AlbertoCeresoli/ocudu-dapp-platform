// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/e3/e3ap_protocol.h"
#include "ocudu/dapp/e3/portable_data_store.h"
#include "ocudu/dapp/e3/sctp_association_server.h"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

namespace ocudu::dapp::e3 {

class management_service;
class e3_connection_registry;

struct e3ap_sctp_server_config {
  /// Transport policy. Port zero binds an ephemeral port (tests); production
  /// deployments set \ref e3ap_default_port or an operator-chosen port.
  sctp_server_config transport;
  /// RAN identifier returned in every setup response.
  std::string ran_identifier{"ocudu-gnb"};
  /// Time a new association may stay connected without completing setup.
  std::chrono::milliseconds setup_timeout{5000};
  /// Store lease requested for each subscription; renewed on every drain.
  uint64_t lease_us{2'000'000};
  /// Maximum reassembled service-model payload accepted or produced.
  uint32_t maximum_payload_bytes{4U * 1024U * 1024U};
  /// Fragment size for outbound indications and control outcomes.
  uint32_t fragment_bytes{e3ap_maximum_fragment_bytes};
  /// Store queue depth (64 KiB chunks) for streams whose objects may reach the
  /// store's maximum object size (PUSCH symbols, Class-C snapshots). Small
  /// record streams use \ref telemetry_queue_depth so each subscription
  /// reserves only what it needs.
  uint32_t queue_depth_chunks{64};
  /// Subscriptions admitted per dApp (association).
  uint32_t maximum_subscriptions_per_dapp{16};
  /// Advertised telemetry streams. Defaults to \ref default_telemetry_streams.
  std::vector<telemetry_stream_record> streams;
  /// Cells advertised for every stream (informational).
  std::vector<uint16_t> cell_ids;
  /// By-reference delivery: rows per shared region (each row holds one object
  /// of up to \c maximum_payload_bytes), the region file mode, and the name
  /// prefix; regions are named "<prefix>-<pid>-<dApp>-<subscription>".
  uint16_t    shared_region_rows{4};
  mode_t      shared_region_mode{0640};
  std::string shared_region_name_prefix{"/ocudu-e3"};
  /// Aggregate mapping admission budget, separate from portable queue storage.
  uint64_t maximum_shared_region_bytes{128ULL * 1024 * 1024};
  /// Objects smaller than this stay in band unless the subscriber asks otherwise.
  uint32_t default_minimum_by_reference_bytes{e3ap_maximum_fragment_bytes};
  /// Accept peers that speak the upstream libe3 E3AP 1.0.0 module on the same
  /// port. The dialect is fixed per association by its setup request: a PDU
  /// that decodes under E3AP-OCUDU-V1 selects the native module, one that
  /// decodes only under the libe3 module selects libe3. libe3 peers get dApp
  /// identifiers 1..100, message identifiers 1..1000, whole (unfragmented)
  /// payloads of at most 32 KiB, and E3-MessageAck in place of
  /// E3-ControlResponse; indications that do not fit are counted in
  /// \c libe3_indications_dropped.
  bool libe3_profile{true};
};

/// Default store queue depth per telemetry stream; large-object streams use the
/// server's \c queue_depth_chunks.
uint32_t telemetry_queue_depth(uint16_t telemetry_id, uint32_t large_object_depth) noexcept;

struct e3ap_sctp_server_metrics {
  sctp_server_metrics transport;
  uint64_t            setups_completed{};
  uint64_t            setups_rejected{};
  uint64_t            subscriptions_created{};
  uint64_t            subscriptions_removed{};
  uint64_t            control_requests{};
  uint64_t            control_failures{};
  uint64_t            indications_sent{};
  uint64_t            indications_filtered{};
  uint64_t            indications_dropped{};
  uint64_t            protocol_errors{};
  uint32_t            live_subscriptions{};
  uint64_t            indications_by_reference{};
  uint64_t            reports_received{};
  uint64_t            xapp_controls_sent{};
  uint64_t            xapp_controls_dropped{};
  /// Associations that completed setup with the libe3 module.
  uint64_t libe3_setups_completed{};
  /// Indications to libe3 peers dropped because the libe3 module cannot carry them.
  uint64_t libe3_indications_dropped{};
};

/// One E3-DAppReport received from a dApp, handed to the report handler on the
/// server thread (the handler must not block) and kept in a short ring.
struct e3ap_dapp_report_event {
  uint16_t               dapp_identifier{};
  std::string            dapp_name;
  uint16_t               ran_function_identifier{};
  std::vector<uint8_t>   report_data;
  std::optional<int64_t> timestamp_ns;
  uint64_t               received_ns{};
  /// Host session identity; pass this when replying to avoid ID reuse races.
  uint64_t association_id{};
};

class e3ap_sctp_server;

struct e3ap_sctp_server_create_result {
  std::unique_ptr<e3ap_sctp_server> server;
  std::string                       detail;
  explicit                          operator bool() const noexcept { return server != nullptr; }
};

/// E3AP-OCUDU-V1 endpoint over SCTP for out-of-process dApps and observers.
///
/// The transport core accepts associations and frames PDUs; this class runs
/// the E3AP procedures (setup, subscription, control, release), forwards
/// management control actions to the shared \ref management_service, and
/// drains admitted portable publications into fragmented indications. The
/// radio path is never entered: producers only see the bounded store.
class e3ap_sctp_server final : private sctp_carrier
{
public:
  static e3ap_sctp_server_create_result create(management_service*                     management,
                                               std::shared_ptr<portable_data_store>    store,
                                               e3ap_sctp_server_config                 config,
                                               std::shared_ptr<e3_connection_registry> connections = {}) noexcept;

  ~e3ap_sctp_server() override;
  e3ap_sctp_server(const e3ap_sctp_server&)            = delete;
  e3ap_sctp_server& operator=(const e3ap_sctp_server&) = delete;

  void                     stop() noexcept;
  uint16_t                 port() const noexcept;
  e3ap_sctp_server_metrics metrics() const noexcept;

  /// Consumer of E3-DAppReport messages (for example an E2SM-DAPP bridge).
  /// Invoked on the server thread; replaces any previous handler.
  void set_dapp_report_handler(std::function<void(const e3ap_dapp_report_event&)> handler) noexcept;
  /// The most recent reports (newest last), bounded to sixteen.
  std::vector<e3ap_dapp_report_event> recent_reports() const noexcept;

  /// Queues an E3-XAppControlAction for the dApp with the given identifier;
  /// it is sent on the procedure stream at the next timer tick. Returns false
  /// when the queue is full. A dApp that is not (or no longer) connected is
  /// counted in \c xapp_controls_dropped.
  /// Pass a nonzero expected association from discovery/report context to
  /// reject a stale caller before enqueue. Zero resolves the current session.
  bool send_xapp_control_action(uint16_t             dapp_identifier,
                                uint16_t             ran_function_identifier,
                                std::vector<uint8_t> data) noexcept;
  bool send_xapp_control_action(uint16_t             dapp_identifier,
                                uint16_t             ran_function_identifier,
                                std::vector<uint8_t> data,
                                uint64_t             expected_association_id) noexcept;

private:
  struct implementation;
  explicit e3ap_sctp_server(std::unique_ptr<implementation> state_);

  void on_opened(sctp_association& association) override;
  void on_message(sctp_association& association, ocudu::span<const uint8_t> message) override;
  void on_closed(sctp_association& association) override;
  void on_timer(std::vector<sctp_association*>& associations) override;

  std::unique_ptr<implementation> state;
};

} // namespace ocudu::dapp::e3
