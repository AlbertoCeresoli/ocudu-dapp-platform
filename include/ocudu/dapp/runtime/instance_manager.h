// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "incident_store.h"
#include "native_hook_registry.h"
#include "native_instance.h"
#include "native_preflight.h"
#include "ocudu/dapp/management/types.h"
#include "ocudu/dapp/runtime/package_catalog.h"
#include "ocudu/dapp/runtime/package_signature.h"
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace ocudu::dapp {

// process_runtime_create_result / process_runtime_factory: package_catalog.h

class instance_manager
{
public:
  instance_manager(std::string                           catalog_root,
                   ocudu_dapp_host_info_v1               host_info,
                   ocudu_dapp_host_api_v1                host_api,
                   std::size_t                           maximum_transactions = 4096,
                   std::shared_ptr<native_hook_registry> hook_registry        = {},
                   std::string                           preflight_helper     = {},
                   std::chrono::milliseconds             preflight_timeout    = std::chrono::milliseconds(500),
                   // Signature verifier reused for on-disk package refresh so a
                   // refreshed manifest is verified exactly as it was at startup.
                   // The service always passes one (allow_unsigned is a verifier
                   // MODE, not a null pointer); null is a test-only convenience
                   // that skips signature checks on refresh entirely.
                   std::shared_ptr<const package_signature_verifier> signature_verifier = {});
  ~instance_manager();

  /// Local catalog/bootstrap API; never exposed as an E3 arbitrary-path operation.
  management::operation_result register_package(const management::package_record& package);
  /// Re-admit a new artifact for an existing package_id from a trusted
  /// catalog-root manifest. Fails if any non-unloaded instance still holds the
  /// old image. Used by load() when the on-disk SHA no longer matches the
  /// start-of-day digest (unload → restage → load, no gNB restart).
  management::operation_result replace_package(const management::package_record& package);
  /// Trusted DU bootstrap binds an approved process package to concrete host route construction.
  management::operation_result register_process_factory(const management::identifier& package_id,
                                                        process_runtime_factory       factory);

  management::operation_result load(const management::load_request& request);
  management::operation_result control(const management::lifecycle_request& request);
  management::query_result<std::vector<management::config_descriptor>>
                                                        config_schema(const management::identifier& instance_id);
  management::query_result<management::config_snapshot> config(const management::identifier& instance_id);
  management::operation_result                          set_config(const management::config_set_request& request);
  management::query_result<std::vector<management::metric_descriptor>>
  metric_schema(const management::identifier& instance_id);
  management::query_result<std::vector<management::metric_sample>> metrics(const management::identifier& instance_id);
  management::operation_result manage_model(const management::model_request& request);

  management::inventory_snapshot             inventory();
  std::optional<management::package_record>  package(const management::identifier& id) const;
  std::optional<management::instance_record> instance(const management::identifier& id);

  std::size_t drain_incidents();
  /// Registers one host-owned nonblocking incident source before management ingress starts.
  bool                  set_host_incident_drain(std::function<std::size_t(incident_store&)> drain);
  incident_store&       incidents() noexcept { return incident_history; }
  const incident_store& incidents() const noexcept { return incident_history; }

private:
  struct managed_instance {
    management::instance_record             record;
    std::unique_ptr<instance_runtime>       runtime;
    native_instance*                        native_runtime{};
    std::shared_ptr<native_activation_gate> activation_gate;
    std::vector<management::binding_state>  process_bindings;
  };

  struct transaction_record {
    uint64_t                     fingerprint{};
    management::operation_result result;
  };

  management::identifier                      next_identifier() noexcept;
  std::optional<management::operation_result> replay(const management::identifier& transaction,
                                                     uint64_t                      fingerprint) const;
  void                         remember(uint64_t fingerprint, const management::operation_result& result);
  management::operation_result make_result(management::result_code       code,
                                           const management::identifier& transaction,
                                           const char*                   detail = nullptr) const;
  void                         advance_generation(managed_instance& instance);
  std::size_t                  refresh(managed_instance& instance);
  static std::vector<management::interface_record> describe_interfaces(const ocudu_dapp_module_v1& module);
  management::operation_result                     map_catalog_outcome(package_catalog::outcome      outcome,
                                                                       const management::identifier& transaction) const;
  bool package_has_resident_instance(const management::identifier& package_id) const;
  /// Re-reads and re-verifies a package manifest from the catalog root.
  /// On failure \p refusal_detail names the specific cause (path escape,
  /// unreadable manifest, identity mismatch, signature rejection, or
  /// anti-rollback refusal) so callers can report it distinctly.
  bool try_refresh_package_from_disk(const management::identifier& package_id, std::string& refusal_detail);

  mutable std::mutex                                                                          mutex;
  package_catalog                                                                             catalog;
  ocudu_dapp_host_info_v1                                                                     host_information{};
  ocudu_dapp_host_api_v1                                                                      host_callbacks{};
  std::shared_ptr<native_hook_registry>                                                       native_hooks;
  std::string                                                                                 native_preflight_helper;
  std::chrono::milliseconds                                                                   native_preflight_timeout;
  std::shared_ptr<const package_signature_verifier>                                           package_signatures;
  uint64_t                                                                                    id_nonce{};
  uint64_t                                                                                    id_sequence{1};
  std::unordered_map<management::identifier, managed_instance, management::identifier_hash>   instances;
  std::unordered_map<management::identifier, transaction_record, management::identifier_hash> transactions;
  std::deque<management::identifier>                                                          transaction_order;
  const std::size_t                           maximum_transaction_records;
  incident_store                              incident_history;
  std::function<std::size_t(incident_store&)> host_incident_drain;
};

} // namespace ocudu::dapp
