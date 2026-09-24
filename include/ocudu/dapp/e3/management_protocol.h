// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/e3/portable_data_types.h"
#include "ocudu/dapp/management/incidents.h"
#include "ocudu/dapp/management/pusch_routes.h"
#include "ocudu/dapp/management/types.h"
#include "ocudu/dapp/use_cases/v1/coordination.h"
#include <optional>
#include <variant>

namespace ocudu::dapp::e3 {

/// Transport-neutral C++ representation of E3SM-OCUDU-DAPP-MGMT-V1.
/// Native pointers and bulk Class-C tensors are intentionally absent.
struct request_header {
  management::identifier                transaction_id;
  std::optional<management::identifier> instance_id;
  std::optional<uint64_t>               expected_instance_generation;
  std::optional<uint64_t>               expected_inventory_generation;
  bool                                  dry_run{};
};

struct inventory_query {
  request_header header;
  bool           include_packages{true};
  bool           include_instances{true};
};

struct catalog_admit {
  request_header         header;
  management::identifier package_id;
};

struct e3_subscription_record {
  uint16_t              subscription_id{};
  uint16_t              ran_function_id{};
  std::vector<uint16_t> telemetry_identifiers;
};

struct e3_connection_record {
  uint64_t                            association_id{};
  uint64_t                            opened_ns{};
  std::string                         peer_address;
  bool                                setup_complete{};
  std::optional<uint16_t>             dapp_id;
  std::string                         dapp_name;
  std::vector<e3_subscription_record> subscriptions;
  uint64_t                            control_requests{};
};

struct inventory_result {
  management::inventory_snapshot    inventory;
  std::vector<e3_connection_record> connections;
};

enum class lifecycle_action : uint8_t {
  load,
  prepare,
  arm,
  enter_shadow,
  activate,
  deactivate,
  retry_quiescence,
  unload,
  breaker_probe
};

struct lifecycle_control {
  request_header                         header;
  lifecycle_action                       action{lifecycle_action::prepare};
  std::optional<management::identifier>  package_id;
  std::optional<ocudu_dapp_placement_v1> placement;
  std::optional<management::backend>     selected_backend;
  uint64_t                               timeout_us{1'000'000};
};

enum class query_kind : uint8_t { schema, effective };

struct config_query {
  request_header header;
  query_kind     kind{query_kind::effective};
};

struct config_set {
  request_header                        header;
  uint64_t                              expected_config_generation{};
  ocudu_dapp_config_boundary_v1         boundary{OCUDU_DAPP_CONFIG_BOUNDARY_INVOCATION_V1};
  std::vector<management::config_entry> entries;
};

struct model_control {
  request_header             header;
  uint64_t                   expected_model_generation{};
  ocudu_dapp_model_action_v1 action{OCUDU_DAPP_MODEL_VALIDATE_V1};
  std::string                model_id;
  std::string                model_version;
  std::vector<uint8_t>       artifact;
};

struct metric_query {
  request_header header;
  query_kind     kind{query_kind::effective};
};

enum class incident_query_action : uint8_t { list, clear_through };

struct incident_query {
  request_header                header;
  incident_query_action         action{incident_query_action::list};
  std::optional<incident_cause> cause;
  std::optional<uint64_t>       after_incident_id;
  std::size_t                   maximum_records{100};
};

struct health_query {
  request_header header;
};

enum class job_query_action : uint8_t { get, list, cancel };

struct job_query {
  request_header                        header;
  job_query_action                      action{job_query_action::list};
  std::optional<management::identifier> job_id;
};

struct quiet_reservation_query {
  request_header          header;
  std::optional<uint32_t> cell_id;
  std::size_t             maximum_records{100};
};

enum class data_subscription_action : uint8_t { create, remove, list };

struct data_subscription_control {
  request_header                        header;
  data_subscription_action              action{data_subscription_action::list};
  std::optional<management::identifier> subscription_id;
  uint32_t                              interface_id{};
  std::optional<uint32_t>               cell_id;
  uint32_t                              sample_every_n{1};
  uint32_t                              queue_depth_chunks{64};
  uint32_t                              maximum_chunk_bytes{64 * 1024};
  uint64_t                              lease_us{1'000'000};
  portable_drop_policy                  drop_policy{portable_drop_policy::drop_newest};
};

struct data_poll_query {
  request_header         header;
  management::identifier subscription_id;
  uint32_t               maximum_chunks{8};
};

/// ASN.1 RanControlAction: releaseUeToIdle(0), handoverUe(1), setTxGain(2), setRxGain(3).
/// release/handover are deferred onto the CU-CP control executor; the gain
/// actions apply immediately through the RU gain controller.
enum class ran_control_action : uint8_t { release_ue_to_idle = 0, handover_ue = 1, set_tx_gain = 2, set_rx_gain = 3 };

/// RAN-level mutation. release/handover identify a UE by serving PCI and RNTI
/// (handover adds the target PCI); the gain actions address an RU port and
/// carry the gain in dB (rnti is 0 for them).
struct ran_control {
  request_header header;
  ran_control_action action{ran_control_action::release_ue_to_idle};
  /// Physical cell identity (0..1007) of the serving cell.
  uint16_t serving_pci{};
  /// C-RNTI of the addressed UE; 0 for the gain actions.
  uint16_t rnti{};
  /// Handover destination PCI (handover_ue only).
  std::optional<uint16_t> target_pci;
  /// Radio-unit port index (set_tx_gain / set_rx_gain only).
  std::optional<uint8_t> port_id;
  /// Requested gain in dB (set_tx_gain / set_rx_gain only).
  std::optional<double> gain_db;
};

/// Read-only query for the served cells and the RAN-control capabilities.
struct cell_info_query {
  request_header header;
};

/// TDD UL/DL pattern (TS 38.213 tdd-UL-DL-ConfigurationCommon pattern1):
/// \c period_slots is the transmission period in slots at the cell numerology,
/// followed by the number of leading full DL slots, extra DL symbols in the
/// transition slot, trailing full UL slots, and extra UL symbols.
struct cell_tdd_pattern {
  uint16_t period_slots{};
  uint8_t  dl_slots{};
  uint8_t  dl_symbols{};
  uint8_t  ul_slots{};
  uint8_t  ul_symbols{};
};

/// PRACH configuration of the initial UL BWP: TS 38.211 configuration index,
/// logical root sequence index (0..837), zero-correlation-zone config
/// (0..15), the total number of RA preambles, and the msg1 frequency start as
/// a PRB offset from PRB 0 of the UL BWP.
struct cell_prach_config {
  uint8_t  config_index{};
  uint16_t root_sequence_index{};
  uint16_t zero_correlation_zone{};
  uint8_t  total_preambles{};
  uint16_t prb_start{};
};

/// One served cell as reported by the cellInfo management operation.
struct cell_descriptor {
  /// DU-local cell index (0-based), not a 3GPP identity.
  uint16_t cell_id{};
  /// Physical cell identity (0..1007).
  uint16_t pci{};
  /// PLMN as 5 or 6 ASCII decimal digits (MCC followed by MNC), e.g. "00101".
  std::string plmn;
  /// Tracking area code (0..16777215).
  uint32_t tac{};
  /// 36-bit NR Cell Identity (NCI).
  uint64_t nr_cell_identity{};
  /// NR operating band number (e.g. 78 for n78).
  uint16_t band{};
  /// NR-ARFCN of the DL/UL reference frequency (point F_REF).
  uint32_t dl_arfcn{};
  uint32_t ul_arfcn{};
  /// Carrier center frequencies in Hz derived from the ARFCNs.
  uint64_t dl_center_hz{};
  uint64_t ul_center_hz{};
  /// DL channel bandwidth in MHz (wire bound 0..400).
  uint16_t bandwidth_mhz{};
  /// UL carrier size in PRBs (initial UL BWP CRBs).
  uint16_t nof_prbs{};
  /// Common subcarrier spacing in kHz (15/30/60/...).
  uint16_t scs_khz{};
  uint8_t  nof_tx_antennas{};
  uint8_t  nof_rx_antennas{};
  /// True when the cell is barred for access.
  bool barred{};
  /// Present only for TDD cells.
  std::optional<cell_tdd_pattern> tdd;
  /// Present when the initial UL BWP configures common RACH.
  std::optional<cell_prach_config> prach;
  /// Radio-unit gains in dB; absent when the RU does not report them.
  std::optional<double> tx_gain_db;
  std::optional<double> rx_gain_db;
  /// Radio sampling rate in Hz; absent when the radio does not expose one
  /// (for example split-7.2 fronthaul, where the RU owns the sampling).
  std::optional<uint64_t> sample_rate_hz;
};

/// RAN-control actions the host can honour; a false flag means the matching
/// ran_control_action would be rejected.
struct ran_control_capabilities {
  bool ue_release{};
  bool handover{};
  bool tx_gain{};
  bool rx_gain{};
};

/// Payload of the cellInfo operation: the served-cell table and the
/// RAN-control capability flags.
struct cell_info_result {
  std::vector<cell_descriptor> cells;
  ran_control_capabilities     capabilities;
};

/// Per-UE routing of Class-A PUSCH grants (see pusch_route_table). query returns the table; replace
/// sets the default (primary when absent) and every entry; update inserts, overwrites or (with a
/// primary target) removes entries; clear resets the table.
enum class pusch_routing_action : uint8_t { query, replace, update, clear };

struct pusch_routing {
  request_header                    header;
  pusch_routing_action              action{pusch_routing_action::query};
  std::optional<pusch_route_target> default_target;
  std::vector<pusch_route_entry>    entries;
};

/// The table after the request (every action answers with it).
using pusch_routing_result = pusch_route_state;

using management_request = std::variant<inventory_query,
                                        lifecycle_control,
                                        config_query,
                                        config_set,
                                        model_control,
                                        metric_query,
                                        incident_query,
                                        health_query,
                                        job_query,
                                        quiet_reservation_query,
                                        data_subscription_control,
                                        data_poll_query,
                                        ran_control,
                                        cell_info_query,
                                        catalog_admit,
                                        pusch_routing>;

struct config_result {
  std::optional<uint64_t>                    generation;
  std::vector<management::config_descriptor> descriptors;
  std::vector<management::config_entry>      entries;
};

struct metric_result {
  std::vector<management::metric_descriptor> descriptors;
  std::vector<management::metric_sample>     samples;
};

struct incident_result {
  std::vector<incident_record> incidents;
  std::optional<uint64_t>      cleared_count;
};

struct job_result {
  std::vector<management::job_record> jobs;
};

struct quiet_reservation_record {
  uint32_t                               cell_id{};
  ocudu_dapp_quiet_period_reservation_v1 reservation{};
};

struct quiet_reservation_result {
  std::vector<quiet_reservation_record> reservations;
  bool                                  truncated{};
};

struct data_subscription_result {
  std::vector<portable_subscription_record> subscriptions;
};

struct data_publication_result {
  std::vector<portable_publication_chunk>     chunks;
  std::optional<portable_subscription_record> subscription;
};

using management_payload = std::variant<std::monostate,
                                        inventory_result,
                                        config_result,
                                        metric_result,
                                        incident_result,
                                        incident_health_summary,
                                        job_result,
                                        quiet_reservation_result,
                                        data_subscription_result,
                                        data_publication_result,
                                        cell_info_result,
                                        pusch_routing_result>;

struct management_response {
  management::identifier                      transaction_id;
  management::result_code                     code{management::result_code::invalid_request};
  std::optional<management::identifier>       job_id;
  std::optional<management::operation_result> result;
  management_payload                          payload;
  std::string                                 error_text;
};

} // namespace ocudu::dapp::e3
