// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/management/types.h"
#include "ocudu/dapp/runtime/package_signature.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace ocudu {

class phy_diagnostic_sink;
class scheduler_dapp_hook_provider;
class scheduler_dapp_interference_cache;
class quiet_reservation_manager;
class upper_phy_dapp_hook_provider;

namespace dapp {
class instance_manager;
class native_hook_registry;
namespace e3 {
class ran_control_handler;
}
} // namespace dapp

namespace app_services {

/// Thread-safe consumer of already-generated gNB JSON metrics reports; outlives the
/// service that produced it so late reports never touch freed state.
class dapp_metrics_json_sink
{
public:
  virtual ~dapp_metrics_json_sink()                                 = default;
  virtual bool publish_metrics_json(std::string_view json) noexcept = 0;
};

/// Optional nonblocking, thread-safe consumer for host-normalized SRS CIR preview reports.
class dapp_srs_isac_report_sink
{
public:
  virtual ~dapp_srs_isac_report_sink()                                                   = default;
  virtual ocudu_dapp_status_v1 publish(const ocudu_dapp_srs_isac_cir_preview_v1& report) = 0;
};

struct dapp_service_cell_config {
  uint32_t cell_id{};
  uint32_t nof_uplink_producer_shards{};
  uint16_t nof_ul_prbs{};
  /// Uplink carrier center reported on E3 subscription records; zero when unknown.
  uint64_t rf_center_frequency_hz{};
  int64_t  cell_frequency_offset_hz{};
};

struct dapp_local_e3_config {
  std::string               socket_path;
  std::optional<uid_t>      allowed_uid;
  std::chrono::milliseconds request_timeout{1000};
};

/// E3AP-OCUDU-V1 endpoint over SCTP for out-of-process dApps and observers.
struct dapp_e3ap_sctp_config {
  std::string               bind_address{"127.0.0.1"};
  uint16_t                  port{36423};
  std::vector<std::string>  allowed_peers;
  uint32_t                  maximum_associations{8};
  std::chrono::milliseconds idle_timeout{30000};
  std::chrono::milliseconds setup_timeout{5000};
  uint32_t                  maximum_subscriptions_per_dapp{16};
  /// RAN identifier returned in setup responses; defaults to the gNB name.
  std::string ran_identifier{"ocudu-gnb"};
};

/// Compatibility endpoint for the FlatBuffers E3 data profile (reference SCTP client).
struct dapp_e3dp_sctp_config {
  std::string               bind_address{"127.0.0.1"};
  uint16_t                  port{38472};
  std::vector<std::string>  allowed_peers;
  uint32_t                  maximum_associations{16};
  std::chrono::milliseconds idle_timeout{30000};
  /// Data-profile subscriptions one association may hold concurrently.
  uint32_t maximum_subscriptions_per_association{8};
};

/// Per-RNTI publication floors for the upper-PHY telemetry streams. The
/// minimum interval is the default for subscribers that do not ask for a rate;
/// the hard floor bounds what any subscriber may request (zero = full rate).
struct dapp_telemetry_config {
  std::chrono::microseconds pusch_symbols_minimum_interval{std::chrono::milliseconds(50)};
  std::chrono::microseconds pusch_crc_minimum_interval{0};
  std::chrono::microseconds srs_channel_minimum_interval{std::chrono::milliseconds(20)};
  std::chrono::microseconds pusch_dmrs_minimum_interval{std::chrono::milliseconds(20)};
  std::chrono::microseconds pusch_grid_minimum_interval{std::chrono::milliseconds(100)};
  // Non-zero defaults: a single subscriber requesting interval 0 removes all
  // PHY-side thinning for every subscriber on that stream, so each stream ships
  // with a protective floor. Operators can still set 0 explicitly to allow
  // full-rate capture.
  std::chrono::microseconds pusch_symbols_hard_floor{std::chrono::milliseconds(10)};
  std::chrono::microseconds pusch_crc_hard_floor{std::chrono::milliseconds(1)};
  std::chrono::microseconds srs_channel_hard_floor{std::chrono::milliseconds(5)};
  std::chrono::microseconds pusch_dmrs_hard_floor{std::chrono::milliseconds(5)};
  std::chrono::microseconds pusch_grid_hard_floor{std::chrono::milliseconds(20)};
};

struct dapp_class_c_process_config {
  uint32_t                  ring_slots{4};
  uint32_t                  payload_capacity_bytes{4 * 1024 * 1024};
  std::chrono::milliseconds startup_timeout{1000};
  std::chrono::milliseconds stop_timeout{1000};
  std::chrono::milliseconds restart_backoff{100};
  std::chrono::milliseconds heartbeat_period{1000};
  std::chrono::milliseconds heartbeat_timeout{3000};
  std::chrono::milliseconds management_timeout{1000};
  uint64_t                  maximum_address_space_bytes{};
  uint64_t                  maximum_core_file_bytes{};
  bool                      require_seccomp_filter{};
  uint32_t                  maximum_open_files{512};
  uint32_t                  maximum_restart_attempts{3};
};

/// Optional bounded host-snapshot profile for low-rate portable Class-C E3 payloads.
struct dapp_portable_e3_data_config {
  uint32_t maximum_subscriptions{32};
  uint64_t maximum_retained_bytes{128ULL * 1024 * 1024};
  uint32_t maximum_object_bytes{4 * 1024 * 1024};
  uint32_t maximum_metadata_bytes{64 * 1024};
  uint32_t minimum_chunk_bytes{4 * 1024};
  uint32_t maximum_chunk_bytes{64 * 1024};
  uint64_t minimum_lease_us{100'000};
  uint64_t maximum_lease_us{10'000'000};
};

struct dapp_quiet_reservation_config {
  uint32_t maximum_retained_prb_slots{512};
  uint16_t maximum_prbs_per_slot{16};
  uint16_t duty_cycle_window_slots{100};
  uint32_t maximum_prb_slots_per_window{64};
  uint32_t maximum_source_prb_slots_per_window{64};
};

enum class dapp_initial_instance_target : uint8_t { shadow, active };

/// One fail-closed instance selected for synchronous startup before management ingress opens.
struct dapp_initial_instance_config {
  std::string                  package_manifest;
  ocudu_dapp_placement_v1      placement{OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1};
  dapp::management::backend    backend{dapp::management::backend::cpu};
  dapp_initial_instance_target target{dapp_initial_instance_target::active};
  std::chrono::milliseconds    lifecycle_timeout{1000};
};

/// Static topology and bounded control-plane resources owned by one DU process.
struct dapp_service_config {
  std::string                               catalog_root;
  std::string                               native_preflight_helper;
  std::chrono::milliseconds                 native_preflight_timeout{500};
  std::vector<std::string>                  package_manifests;
  std::vector<dapp_initial_instance_config> initial_instances;
  dapp::package_signature_config            package_signatures;
  std::vector<dapp_service_cell_config>     cells;
  uint32_t                                  nof_pusch_lanes{};
  uint32_t                                  nof_srs_lanes{};
  uint32_t                                  nof_scheduler_lanes{};
  uint32_t                                  max_scheduler_ul_prbs_avoided{};
  std::chrono::nanoseconds                  scheduler_deadline{std::chrono::microseconds(100)};
  bool                                      scheduler_evaluation_only{};
  /// Class-B output authorities (default off except avoid budget above).
  bool scheduler_allow_mcs{false};
  bool scheduler_allow_prb_limits{false};
  bool scheduler_allow_prb_preference{false};
  bool scheduler_allow_prb_assignment{false};
  bool scheduler_allow_tpc{false};
  bool scheduler_allow_layers{false};
  /// Reject dApp MCS farther than this from host LA when host MCS is valid.
  int16_t                                    scheduler_max_mcs_delta_from_host{28};
  uint64_t                                   host_capabilities{};
  uint64_t                                   backend_mask{1};
  std::array<uint64_t, 2>                    host_build_id{};
  std::size_t                                maximum_transactions{4096};
  std::size_t                                maximum_queued_jobs{64};
  std::size_t                                maximum_retained_jobs{1024};
  std::shared_ptr<dapp_srs_isac_report_sink> srs_isac_report_sink;
  /// Optional gNB-wide mutation authority. It must enqueue work onto the
  /// owning component's control executor.
  std::shared_ptr<dapp::e3::ran_control_handler> ran_control;
  dapp_class_c_process_config                    class_c_process;
  std::optional<dapp_portable_e3_data_config>    portable_e3_data;
  dapp_quiet_reservation_config                  quiet_reservation;
  std::optional<dapp_local_e3_config>            local_e3;
  std::optional<dapp_e3ap_sctp_config>           e3ap_sctp;
  std::optional<dapp_e3dp_sctp_config>           e3dp_sctp;
  dapp_telemetry_config                          telemetry;
};

class dapp_service;

struct dapp_service_create_result {
  std::unique_ptr<dapp_service> service;
  std::string                   detail;
  explicit                      operator bool() const noexcept { return service != nullptr; }
};

/// DU-lifetime owner for native hook topology, instance lifecycle and the optional embedded E3 endpoint.
class dapp_service
{
public:
  static dapp_service_create_result create(dapp_service_config config) noexcept;

  ~dapp_service();
  dapp_service(const dapp_service&)            = delete;
  dapp_service& operator=(const dapp_service&) = delete;

  std::shared_ptr<upper_phy_dapp_hook_provider>      upper_phy_hooks() const noexcept;
  std::shared_ptr<scheduler_dapp_hook_provider>      scheduler_hooks() const noexcept;
  std::shared_ptr<scheduler_dapp_interference_cache> interference_cache() const noexcept;
  std::shared_ptr<quiet_reservation_manager>         quiet_reservations() const noexcept;
  std::shared_ptr<dapp::native_hook_registry>        native_hooks() const noexcept;
  dapp::instance_manager&                            instances() noexcept;
  bool                                               has_embedded_e3() const noexcept;
  bool                                               has_local_e3() const noexcept;
  bool                                               has_e3ap_sctp() const noexcept;
  bool                                               has_e3dp_sctp() const noexcept;

  /// Upper-PHY diagnostic sink feeding the E3 telemetry streams; null when the
  /// portable E3 data store is not configured.
  std::shared_ptr<phy_diagnostic_sink> phy_diagnostics() const noexcept;

  /// Publishes one gNB JSON metrics report to the E3 telemetry stream. Safe
  /// from metrics threads; returns false when no subscriber is admitted.
  bool publish_metrics_json(std::string_view json) noexcept;

  /// Sink for the same stream that may be retained beyond the service lifetime.
  std::shared_ptr<dapp_metrics_json_sink> metrics_json_sink() const noexcept;

  /// Stops management ingress and its job executor. Call only after DU producers have stopped.
  void stop() noexcept;

private:
  struct implementation;
  explicit dapp_service(std::unique_ptr<implementation> state_);

  std::unique_ptr<implementation> state;
};

} // namespace app_services
} // namespace ocudu
