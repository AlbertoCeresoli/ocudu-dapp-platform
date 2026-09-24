// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/abi/v1/management.h"
#include "ocudu/dapp/artifact_digest.h"
#include "ocudu/dapp/use_cases/v1/pusch_metadata.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ocudu::dapp::management {

struct identifier {
  std::array<uint8_t, 16> bytes{};
  bool                    operator==(const identifier& other) const noexcept { return bytes == other.bytes; }
  bool                    operator!=(const identifier& other) const noexcept { return !(*this == other); }
  bool                    is_zero() const noexcept;
};

struct identifier_hash {
  std::size_t operator()(const identifier& value) const noexcept;
};

enum class backend : uint8_t { cpu, cuda, other_accelerator };

/// Signed package policy for one native interface binding.
enum class binding_requirement : uint8_t { required, optional };

/// Host-observed binding state. Package records are unbound; active instance records are bound or unavailable.
enum class binding_state : uint8_t { unbound, bound, unavailable };

enum class package_signature_status : uint8_t { unsigned_package, verified };

/// Management outcome. Maps one-to-one onto the ASN.1 ResultCode of
/// E3SM-OCUDU-DAPP-MGMT-V1 in declaration order: success(0), dryRunSuccess(1),
/// accepted(2), notFound(3), conflict(4), invalidRequest(5), incompatible(6),
/// verificationFailed(7), loadFailed(8), lifecycleFailed(9),
/// transactionMismatch(10), resourceExhausted(11). \c accepted means an
/// asynchronous job was queued; the job record carries the final outcome.
enum class result_code : uint8_t {
  ok,
  dry_run_ok,
  accepted,
  not_found,
  conflict,
  invalid_request,
  incompatible,
  verification_failed,
  load_failed,
  lifecycle_failed,
  transaction_mismatch,
  resource_exhausted
};

/// Instance lifecycle transitions (loading itself is a separate load request):
/// prepare (module create), arm (worker readiness), enter_shadow (warm, no
/// hook authority), activate (bind hooks), deactivate (remove authority),
/// retry_quiescence (retry an uncertain native unload), unload (destroy after
/// quiescence), breaker_probe (policy-controlled circuit-breaker probe).
enum class lifecycle_action : uint8_t {
  prepare,
  arm,
  enter_shadow,
  activate,
  deactivate,
  retry_quiescence,
  unload,
  breaker_probe
};

enum class job_state : uint8_t { accepted, running, waiting_boundary, succeeded, failed, cancelled };

struct interface_record {
  uint32_t                interface_id{};
  uint16_t                interface_major{};
  uint16_t                interface_minor{};
  ocudu_dapp_class_v1     dapp_class{OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1};
  uint32_t                placement_mask{};
  ocudu_dapp_threading_v1 threading{OCUDU_DAPP_THREADING_REENTRANT_SHARED_V1};
  uint64_t                capability_bits{};
  struct pusch_profile {
    uint32_t max_nof_prb{};
    uint32_t max_nof_rx_ports{};
    uint32_t max_nof_layers{};
    uint32_t modulation_mask{};
    uint32_t dmrs_type_mask{};
    uint32_t flags{};
    bool     operator==(const pusch_profile& other) const noexcept
    {
      return max_nof_prb == other.max_nof_prb && max_nof_rx_ports == other.max_nof_rx_ports &&
             max_nof_layers == other.max_nof_layers && modulation_mask == other.modulation_mask &&
             dmrs_type_mask == other.dmrs_type_mask && flags == other.flags;
    }
    bool operator!=(const pusch_profile& other) const noexcept { return !(*this == other); }
  };
  /// Admission envelope of the SRS channel estimator (ocudu_dapp_srs_profile_v1).
  struct srs_profile {
    uint32_t max_nof_rx_ports{};
    uint32_t max_nof_tx_ports{};
    uint32_t max_sequence_length{};
    uint32_t comb_mask{};
    bool     operator==(const srs_profile& other) const noexcept
    {
      return max_nof_rx_ports == other.max_nof_rx_ports && max_nof_tx_ports == other.max_nof_tx_ports &&
             max_sequence_length == other.max_sequence_length && comb_mask == other.comb_mask;
    }
    bool operator!=(const srs_profile& other) const noexcept { return !(*this == other); }
  };
  std::optional<pusch_profile> pusch;
  binding_requirement          requirement{binding_requirement::required};
  binding_state                state{binding_state::unbound};
  /// Set for the SRS channel estimator, whose Class-A profile replaces the PUSCH one.
  std::optional<srs_profile> srs;
};

struct package_record {
  identifier                    package_id;
  std::string                   global_name;
  std::string                   vendor;
  std::string                   version;
  std::string                   canonical_artifact_path;
  artifact_digest               content_digest{};
  uint32_t                      placement_mask{OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1};
  std::vector<interface_record> interfaces;
  std::vector<backend>          backends;
  package_signature_status      signature_status{package_signature_status::unsigned_package};
  artifact_digest               signing_key_id{};
  uint64_t                      signature_sequence{};
  uint64_t                      trust_policy_epoch{};
  uint32_t                      manifest_schema_version{1};
  std::string                   canonical_sbom_path;
  artifact_digest               sbom_digest{};
  bool                          sbom_verified{};
  bool                          compatible{true};
  std::string                   incompatibility;
};

struct instance_record {
  identifier                    instance_id;
  identifier                    package_id;
  std::string                   global_name;
  std::string                   vendor;
  std::string                   version;
  ocudu_dapp_placement_v1       placement{OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1};
  backend                       selected_backend{backend::cpu};
  ocudu_dapp_lifecycle_state_v1 state{OCUDU_DAPP_STATE_DISCOVERED_V1};
  uint64_t                      generation{};
  std::vector<interface_record> interfaces;
  uint64_t                      last_incident_id{};
  uint64_t                      management_capabilities{};
  std::string                   last_error;
};

struct inventory_snapshot {
  uint64_t                     generation{};
  std::vector<package_record>  packages;
  std::vector<instance_record> instances;
};

struct operation_result {
  result_code                   code{result_code::invalid_request};
  identifier                    transaction_id;
  std::optional<identifier>     instance_id;
  uint64_t                      inventory_generation{};
  uint64_t                      instance_generation{};
  uint64_t                      resource_generation{};
  ocudu_dapp_lifecycle_state_v1 state{OCUDU_DAPP_STATE_DISCOVERED_V1};
  std::string                   detail;

  explicit operator bool() const noexcept { return code == result_code::ok || code == result_code::dry_run_ok; }
};

using value = std::variant<bool, int64_t, uint64_t, double, std::string, std::vector<uint8_t>>;

struct config_descriptor {
  std::string              key;
  std::string              description;
  uint32_t                 flags{};
  ocudu_dapp_value_kind_v1 value_kind{OCUDU_DAPP_VALUE_BOOL_V1};
  std::optional<value>     default_value;
  std::optional<value>     minimum_value;
  std::optional<value>     maximum_value;
};

struct config_entry {
  std::string key;
  value       entry_value;
};

struct config_snapshot {
  uint64_t                  generation{};
  std::vector<config_entry> entries;
};

struct metric_descriptor {
  std::string              name;
  std::string              description;
  std::string              unit;
  ocudu_dapp_value_kind_v1 value_kind{OCUDU_DAPP_VALUE_UINT64_V1};
  uint32_t                 flags{};
};

struct metric_sample {
  std::string name;
  value       sample_value;
  uint64_t    monotonic_timestamp_ns{};
};

template <typename T>
struct query_result {
  result_code code{result_code::invalid_request};
  T           value{};
  std::string detail;
  explicit    operator bool() const noexcept { return code == result_code::ok; }
};

struct config_set_request {
  identifier                    transaction_id;
  identifier                    instance_id;
  std::optional<uint64_t>       expected_instance_generation;
  uint64_t                      expected_config_generation{};
  ocudu_dapp_config_boundary_v1 boundary{OCUDU_DAPP_CONFIG_BOUNDARY_INVOCATION_V1};
  bool                          dry_run{};
  uint64_t                      payload_fingerprint{};
  std::vector<config_entry>     entries;
};

struct model_request {
  identifier                 transaction_id;
  identifier                 instance_id;
  std::optional<uint64_t>    expected_instance_generation;
  uint64_t                   expected_model_generation{};
  ocudu_dapp_model_action_v1 action{OCUDU_DAPP_MODEL_VALIDATE_V1};
  std::string                model_id;
  std::string                model_version;
  std::vector<uint8_t>       artifact;
  bool                       dry_run{};
  uint64_t                   payload_fingerprint{};
};

struct job_record {
  identifier                      job_id;
  identifier                      transaction_id;
  job_state                       state{job_state::accepted};
  uint8_t                         progress_percent{};
  uint64_t                        submitted_monotonic_ns{};
  uint64_t                        started_monotonic_ns{};
  uint64_t                        completed_monotonic_ns{};
  std::optional<operation_result> result;
  std::string                     detail;
};

struct load_request {
  identifier              transaction_id;
  identifier              package_id;
  std::optional<uint64_t> expected_inventory_generation;
  ocudu_dapp_placement_v1 placement{OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1};
  backend                 selected_backend{backend::cpu};
  bool                    dry_run{};
  uint64_t                payload_fingerprint{};
  const void*             create_args{};
};

/// Explicit admission of one locally staged, signed native package. No upload,
/// loader invocation, lifecycle change, or caller-selected trust path.
struct catalog_admit_request {
  identifier              transaction_id;
  identifier              package_id;
  std::optional<uint64_t> expected_inventory_generation;
  bool                    dry_run{};
};

struct lifecycle_request {
  identifier              transaction_id;
  identifier              instance_id;
  lifecycle_action        action{lifecycle_action::prepare};
  std::optional<uint64_t> expected_generation;
  bool                    dry_run{};
  uint64_t                timeout_us{1'000'000};
  uint64_t                payload_fingerprint{};
  const void*             primary_payload{};
  const void*             secondary_payload{};
};

uint64_t canonical_fingerprint(const load_request& request) noexcept;
uint64_t canonical_fingerprint(const catalog_admit_request& request) noexcept;
uint64_t canonical_fingerprint(const lifecycle_request& request) noexcept;
uint64_t canonical_fingerprint(const config_set_request& request) noexcept;
uint64_t canonical_fingerprint(const model_request& request) noexcept;

const char* to_string(result_code code) noexcept;

} // namespace ocudu::dapp::management
