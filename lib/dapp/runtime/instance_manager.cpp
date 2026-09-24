// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/dapp/abi/v1/memory.h"
#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/runtime/package_manifest.h"
#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <random>
#include <stdexcept>

using namespace ocudu::dapp;
using namespace ocudu::dapp::management;

namespace {
#include "instance_manager_helpers.inc"

} // namespace

instance_manager::instance_manager(std::string                                       catalog_root,
                                   ocudu_dapp_host_info_v1                           host_info,
                                   ocudu_dapp_host_api_v1                            host_api,
                                   std::size_t                                       maximum_transactions,
                                   std::shared_ptr<native_hook_registry>             hook_registry,
                                   std::string                                       preflight_helper,
                                   std::chrono::milliseconds                         preflight_timeout,
                                   std::shared_ptr<const package_signature_verifier> signature_verifier) :
  catalog(std::move(catalog_root)),
  host_information(host_info),
  host_callbacks(host_api),
  native_hooks(std::move(hook_registry)),
  native_preflight_helper(std::move(preflight_helper)),
  native_preflight_timeout(preflight_timeout),
  package_signatures(std::move(signature_verifier)),
  maximum_transaction_records(maximum_transactions)
{
  if (maximum_transactions == 0 || host_info.struct_size < sizeof(ocudu_dapp_host_info_v1) ||
      host_info.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || host_api.struct_size < sizeof(ocudu_dapp_host_api_v1) ||
      host_api.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || preflight_timeout.count() <= 0) {
    throw std::invalid_argument("invalid dApp instance manager construction parameters");
  }
  std::random_device random;
  id_nonce = (static_cast<uint64_t>(random()) << 32U) ^ random() ^
             static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  if (id_nonce == 0) {
    id_nonce = 0x4f43554455443345ULL;
  }
}

instance_manager::~instance_manager()
{
  if (native_hooks == nullptr) {
    return;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  for (const auto& entry : instances) {
    if (entry.second.native_runtime != nullptr) {
      static_cast<void>(native_hooks->deactivate(entry.first, deadline));
    }
  }
}

operation_result instance_manager::map_catalog_outcome(package_catalog::outcome outcome,
                                                       const identifier&        transaction) const
{
  using cs = package_catalog::status;
  switch (outcome.code) {
    case cs::ok:
      return make_result(result_code::ok, transaction, outcome.detail.empty() ? nullptr : outcome.detail.c_str());
    case cs::ok_already:
      return make_result(result_code::ok, transaction, outcome.detail.c_str());
    case cs::conflict:
      return make_result(result_code::conflict, transaction, outcome.detail.c_str());
    case cs::not_found:
      return make_result(result_code::not_found, transaction, outcome.detail.c_str());
    case cs::verification_failed:
      return make_result(result_code::verification_failed, transaction, outcome.detail.c_str());
    case cs::invalid_request:
    default:
      return make_result(result_code::invalid_request, transaction, outcome.detail.c_str());
  }
}

operation_result instance_manager::register_package(const package_record& package_record)
{
  std::lock_guard<std::mutex> lock(mutex);
  identifier                  no_transaction{};
  return map_catalog_outcome(catalog.register_package(package_record), no_transaction);
}

operation_result instance_manager::admit_package(const catalog_admit_request& request)
{
  // Take a bounded control-plane snapshot, then release the manager lock for
  // all file IO/hashing. Inventory/health readers and native L1 invocation must
  // not wait behind package verification. Commit rechecks the snapshot below.
  std::unique_lock<std::mutex> lock(mutex);
  const auto                   fingerprint = canonical_fingerprint(request);
  if (request.transaction_id.is_zero()) {
    return make_result(result_code::invalid_request, request.transaction_id, "transaction ID is required");
  }
  if (auto prior = replay(request.transaction_id, fingerprint)) {
    return *prior;
  }
  const auto finish = [&](result_code code, const std::string& detail) {
    if (!lock.owns_lock()) {
      lock.lock();
    }
    if (auto prior = replay(request.transaction_id, fingerprint)) {
      return *prior;
    }
    auto result = make_result(code, request.transaction_id, detail.c_str());
    remember(fingerprint, result);
    return result;
  };
  if (request.expected_inventory_generation &&
      *request.expected_inventory_generation != catalog.inventory_generation()) {
    return finish(result_code::conflict, "inventory generation mismatch");
  }
  if (request.package_id.is_zero()) {
    return finish(result_code::invalid_request, "package ID is required");
  }
  // Fixed UUID namespace: no transport-supplied path, scan, or upload. Bundles
  // are staged by the local operator under <32 lowercase hex>/package.manifest.json.
  constexpr char hex[] = "0123456789abcdef";
  std::string    manifest_path;
  manifest_path.reserve(64);
  for (auto byte : request.package_id.bytes) {
    manifest_path.push_back(hex[byte >> 4U]);
    manifest_path.push_back(hex[byte & 15U]);
  }
  manifest_path += "/package.manifest.json";
  if (!package_signatures) {
    return finish(result_code::verification_failed, "runtime catalog admission requires a signature verifier");
  }
  const auto inventory_generation = catalog.inventory_generation();
  const auto previous_policy      = package_signatures;
  auto       candidate            = catalog;
  lock.unlock();
  auto policy = previous_policy->reload();
  if (!policy) {
    return finish(result_code::verification_failed, policy.detail);
  }
  auto parsed = read_package_manifest(candidate.verifier().catalog_root(), manifest_path, policy.verifier.get());
  if (!parsed) {
    return finish(result_code::verification_failed, parsed.detail);
  }
  const auto& package = *parsed.package;
  if (package.package_id != request.package_id) {
    return finish(result_code::verification_failed, "staged manifest carries a different package identity");
  }
  if (package.signature_status != package_signature_status::verified) {
    return finish(result_code::verification_failed, "runtime admission never accepts unsigned packages");
  }
  // New supervised-process routes need host-owned topology/factory construction;
  // catalog admission must not manufacture one from untrusted package metadata.
  if (package.placement_mask != OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1) {
    return finish(result_code::incompatible, "runtime admission currently supports native in-process packages only");
  }
  for (const auto& entry : candidate.packages()) {
    if (entry.first != package.package_id && entry.second.global_name == package.global_name) {
      return finish(result_code::conflict, "global package name already belongs to another package identifier");
    }
  }
  auto existing = candidate.find_package(package.package_id);
  if (!existing && candidate.packages().size() >= 256) {
    return finish(result_code::resource_exhausted, "runtime catalog package limit reached");
  }
  if (existing && (package.signature_sequence < existing->signature_sequence ||
                   (package.signature_sequence == existing->signature_sequence &&
                    (package.content_digest != existing->content_digest || package.version != existing->version ||
                     package.sbom_digest != existing->sbom_digest)))) {
    return finish(result_code::verification_failed, "package sequence rollback or sequence reuse for changed content");
  }
  // Use a bounded candidate catalog to validate replacement identity and artifact
  // digest even on dry-run, without mutating the admitted inventory or policy.
  const auto outcome = candidate.replace_package(package);
  lock.lock();
  if (auto prior = replay(request.transaction_id, fingerprint)) {
    return *prior;
  }
  if (catalog.inventory_generation() != inventory_generation || package_signatures != previous_policy) {
    return finish(result_code::conflict,
                  "inventory or trust policy changed during verification; retry with a new transaction");
  }
  if (package_has_resident_instance(package.package_id)) {
    return finish(result_code::conflict, "unload all instances before updating an admitted package");
  }
  if (outcome.code != package_catalog::status::ok && outcome.code != package_catalog::status::ok_already) {
    auto result = map_catalog_outcome(outcome, request.transaction_id);
    remember(fingerprint, result);
    return result;
  }
  if (request.dry_run) {
    return finish(result_code::dry_run_ok, "signed package verified; catalog and trust policy unchanged");
  }
  std::shared_ptr<const package_signature_verifier> next_policy(std::move(policy.verifier));
  catalog            = std::move(candidate);
  package_signatures = std::move(next_policy);
  return finish(result_code::ok, "signed package admitted; load and activation remain explicit");
}

bool instance_manager::package_has_resident_instance(const identifier& package_id) const
{
  for (const auto& entry : instances) {
    if (entry.second.record.package_id == package_id && entry.second.record.state != OCUDU_DAPP_STATE_UNLOADED_V1) {
      return true;
    }
  }
  return false;
}

bool instance_manager::try_refresh_package_from_disk(const identifier& package_id, std::string& refusal_detail)
{
  auto found = catalog.find_package(package_id);
  if (!found) {
    refusal_detail = "package is not in the catalog";
    return false;
  }
  const std::filesystem::path artifact(found->canonical_artifact_path);
  const auto                  manifest_abs = artifact.parent_path() / (artifact.stem().string() + ".manifest.json");
  const auto&                 root         = catalog.verifier().catalog_root();
  std::error_code             error;
  const auto                  relative = std::filesystem::relative(manifest_abs, root, error);
  if (error || relative.empty() || *relative.begin() == "..") {
    refusal_detail = "manifest path escapes the catalog root";
    return false;
  }
  // Re-verify the refreshed manifest exactly as startup admission does: pass
  // the signature verifier so a swapped-in artifact/manifest cannot be
  // admitted unsigned in required mode. The verifier is non-null in every
  // service mode (allow_unsigned is a verifier mode); null is test-only.
  auto parsed = read_package_manifest(root, relative.generic_string(), package_signatures.get());
  if (!parsed) {
    refusal_detail = "refreshed manifest unreadable or signature rejected";
    return false;
  }
  if (parsed.package->package_id != package_id) {
    refusal_detail = "refreshed manifest carries a different package identity";
    return false;
  }
  // Runtime refresh must never downgrade: a validly signed but OLDER bundle
  // (floor <= sequence < current) is refused here even though startup would
  // admit it. Restarting the gNB remains the deliberate rollback path.
  // Unsigned packages carry sequence 0 on both sides, so labs are unaffected.
  if (parsed.package->signature_sequence < found->signature_sequence) {
    refusal_detail = "refresh refused: on-disk package sequence is older than the admitted one (anti-rollback)";
    return false;
  }
  if (parsed.package->signature_sequence != 0 && parsed.package->signature_sequence == found->signature_sequence &&
      (parsed.package->content_digest != found->content_digest || parsed.package->version != found->version ||
       parsed.package->sbom_digest != found->sbom_digest)) {
    refusal_detail = "refresh refused: signed sequence was reused for changed content";
    return false;
  }
  const auto outcome = catalog.replace_package(*parsed.package);
  if (outcome.code != package_catalog::status::ok && outcome.code != package_catalog::status::ok_already) {
    refusal_detail = "catalog refused the refreshed package record";
    return false;
  }
  refusal_detail.clear();
  return true;
}

operation_result instance_manager::replace_package(const package_record& package_record)
{
  std::lock_guard<std::mutex> lock(mutex);
  identifier                  no_transaction{};
  if (package_has_resident_instance(package_record.package_id)) {
    return make_result(result_code::conflict, no_transaction, "unload all instances before replacing package content");
  }
  return map_catalog_outcome(catalog.replace_package(package_record), no_transaction);
}

operation_result instance_manager::register_process_factory(const identifier&       package_id,
                                                            process_runtime_factory factory)
{
  std::lock_guard<std::mutex> lock(mutex);
  identifier                  no_transaction{};
  return map_catalog_outcome(catalog.register_process_factory(package_id, std::move(factory)), no_transaction);
}

// Lifecycle: see instance_manager_lifecycle.cpp (load/control).

query_result<std::vector<config_descriptor>> instance_manager::config_schema(const identifier& instance_id)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto                        found = instances.find(instance_id);
  if (found == instances.end()) {
    return {result_code::not_found, {}, "instance does not exist"};
  }
  refresh(found->second);
  if (found->second.record.state == OCUDU_DAPP_STATE_UNLOADED_V1) {
    return {result_code::incompatible, {}, "instance is unloaded"};
  }
  auto result = collect_bounded<ocudu_dapp_config_descriptor_v1, config_descriptor>(
      [&](auto* output, uint32_t capacity) { return found->second.runtime->describe_config(output, capacity); },
      [](const auto& item) -> std::optional<config_descriptor> {
        auto key         = copy_string(item.key);
        auto description = item.description != nullptr ? copy_string(item.description) : std::optional<std::string>{""};
        auto default_value = copy_value(item.default_value);
        auto minimum_value = copy_value(item.minimum_value);
        auto maximum_value = copy_value(item.maximum_value);
        if (item.struct_size < sizeof(item) || !key || key->empty() || !description ||
            item.value_kind < OCUDU_DAPP_VALUE_BOOL_V1 || item.value_kind > OCUDU_DAPP_VALUE_BYTES_V1 ||
            (item.default_value.kind != 0 && !default_value) || (item.minimum_value.kind != 0 && !minimum_value) ||
            (item.maximum_value.kind != 0 && !maximum_value)) {
          return std::nullopt;
        }
        return config_descriptor{std::move(*key),
                                 std::move(*description),
                                 item.flags,
                                 static_cast<ocudu_dapp_value_kind_v1>(item.value_kind),
                                 std::move(default_value),
                                 std::move(minimum_value),
                                 std::move(maximum_value)};
      },
      "module returned malformed config descriptor");
  refresh(found->second);
  return result;
}

query_result<config_snapshot> instance_manager::config(const identifier& instance_id)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto                        found = instances.find(instance_id);
  if (found == instances.end()) {
    return {result_code::not_found, {}, "instance does not exist"};
  }
  refresh(found->second);
  if (found->second.record.state == OCUDU_DAPP_STATE_UNLOADED_V1) {
    return {result_code::incompatible, {}, "instance is unloaded"};
  }
  auto sized = found->second.runtime->get_config(nullptr, 0);
  if (!sized) {
    refresh(found->second);
    return {map_status(sized.operation.status), {}, sized.operation.detail};
  }
  if (sized.count > maximum_management_items) {
    refresh(found->second);
    return {result_code::resource_exhausted, {}, "module result exceeds management item limit"};
  }
  std::vector<ocudu_dapp_config_entry_v1> native(sized.count);
  auto                                    read = found->second.runtime->get_config(native.data(), native.size());
  refresh(found->second);
  if (!read || read.count != native.size() || read.generation != sized.generation) {
    return {read ? result_code::conflict : map_status(read.operation.status), {}, "config changed during snapshot"};
  }
  config_snapshot result;
  result.generation = read.generation;
  result.entries.reserve(native.size());
  for (const auto& item : native) {
    auto key   = copy_string(item.key);
    auto value = copy_value(item.value);
    if (!key || key->empty() || !value) {
      return {result_code::invalid_request, {}, "module returned malformed config entry"};
    }
    result.entries.push_back({std::move(*key), std::move(*value)});
  }
  return {result_code::ok, std::move(result), {}};
}

operation_result instance_manager::set_config(const config_set_request& request)
{
  std::lock_guard<std::mutex> lock(mutex);
  const uint64_t              request_fingerprint = canonical_fingerprint(request);
  if (request.transaction_id.is_zero() || request.expected_config_generation == 0 ||
      request.entries.size() > maximum_management_items || request.boundary < OCUDU_DAPP_CONFIG_BOUNDARY_IMMEDIATE_V1 ||
      request.boundary > OCUDU_DAPP_CONFIG_BOUNDARY_RESTART_V1) {
    return make_result(result_code::invalid_request, request.transaction_id, "invalid config request");
  }
  if (auto prior = replay(request.transaction_id, request_fingerprint)) {
    return *prior;
  }
  auto found = instances.find(request.instance_id);
  if (found == instances.end()) {
    auto response = make_result(result_code::not_found, request.transaction_id, "instance does not exist");
    remember(request_fingerprint, response);
    return response;
  }
  auto& managed = found->second;
  refresh(managed);
  if (managed.record.state == OCUDU_DAPP_STATE_UNLOADED_V1 ||
      (request.expected_instance_generation && *request.expected_instance_generation != managed.record.generation)) {
    auto response = make_result(managed.record.state == OCUDU_DAPP_STATE_UNLOADED_V1 ? result_code::incompatible
                                                                                     : result_code::conflict,
                                request.transaction_id,
                                "instance is unloaded or generation changed");
    response.instance_id         = request.instance_id;
    response.instance_generation = managed.record.generation;
    remember(request_fingerprint, response);
    return response;
  }
  std::vector<ocudu_dapp_config_entry_v1> native;
  native.reserve(request.entries.size());
  for (const auto& entry : request.entries) {
    if (entry.key.empty() || entry.key.size() > 128 ||
        (std::holds_alternative<std::string>(entry.entry_value) &&
         std::get<std::string>(entry.entry_value).size() > maximum_management_string) ||
        (std::holds_alternative<std::vector<uint8_t>>(entry.entry_value) &&
         std::get<std::vector<uint8_t>>(entry.entry_value).size() > maximum_management_blob)) {
      auto response = make_result(result_code::invalid_request, request.transaction_id, "invalid config entry");
      remember(request_fingerprint, response);
      return response;
    }
    native.push_back({entry.key.c_str(), borrow_value(entry.entry_value), {}});
  }
  ocudu_dapp_config_update_v1 update{};
  update.struct_size                     = sizeof(update);
  update.nof_entries                     = native.size();
  update.entries                         = native.data();
  update.expected_generation             = request.expected_config_generation;
  update.boundary                        = request.boundary;
  update.dry_run                         = request.dry_run;
  const auto generation_before_operation = managed.record.generation;
  auto       applied                     = managed.runtime->set_config(update);
  refresh(managed);
  if (applied && !request.dry_run && managed.record.generation == generation_before_operation) {
    advance_generation(managed);
  }
  auto response = make_result(applied ? (request.dry_run ? result_code::dry_run_ok : result_code::ok)
                                      : map_status(applied.operation.status),
                              request.transaction_id,
                              applied.operation.detail.empty() ? nullptr : applied.operation.detail.c_str());
  response.instance_id          = request.instance_id;
  response.instance_generation  = managed.record.generation;
  response.resource_generation  = applied.generation;
  response.state                = managed.record.state;
  response.inventory_generation = catalog.inventory_generation();
  remember(request_fingerprint, response);
  return response;
}

query_result<std::vector<metric_descriptor>> instance_manager::metric_schema(const identifier& instance_id)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto                        found = instances.find(instance_id);
  if (found == instances.end()) {
    return {result_code::not_found, {}, "instance does not exist"};
  }
  refresh(found->second);
  if (found->second.record.state == OCUDU_DAPP_STATE_UNLOADED_V1) {
    return {result_code::incompatible, {}, "instance is unloaded"};
  }
  auto standard =
      host_metric_schema(has_class_a_interface(found->second.record), has_class_b_scheduler(found->second.record));
  if ((found->second.record.management_capabilities & OCUDU_DAPP_MANAGEMENT_METRICS_V1) == 0) {
    return {result_code::ok, std::move(standard), {}};
  }
  auto result = collect_bounded<ocudu_dapp_metric_descriptor_v1, metric_descriptor>(
      [&](auto* output, uint32_t capacity) { return found->second.runtime->describe_metrics(output, capacity); },
      [](const auto& item) -> std::optional<metric_descriptor> {
        auto name        = copy_string(item.name);
        auto description = item.description != nullptr ? copy_string(item.description) : std::optional<std::string>{""};
        auto unit        = item.unit != nullptr ? copy_string(item.unit) : std::optional<std::string>{""};
        const uint32_t metric_flags = OCUDU_DAPP_METRIC_COUNTER_V1 | OCUDU_DAPP_METRIC_GAUGE_V1;
        if (item.struct_size < sizeof(item) || !name || name->empty() || !description || !unit ||
            item.value_kind < OCUDU_DAPP_VALUE_BOOL_V1 || item.value_kind > OCUDU_DAPP_VALUE_BYTES_V1 ||
            (item.flags & ~metric_flags) != 0 || item.flags == metric_flags) {
          return std::nullopt;
        }
        return metric_descriptor{std::move(*name),
                                 std::move(*description),
                                 std::move(*unit),
                                 static_cast<ocudu_dapp_value_kind_v1>(item.value_kind),
                                 item.flags};
      },
      "module returned malformed metric descriptor");
  refresh(found->second);
  if (result) {
    standard.insert(
        standard.end(), std::make_move_iterator(result.value.begin()), std::make_move_iterator(result.value.end()));
    result.value = std::move(standard);
  }
  return result;
}

query_result<std::vector<metric_sample>> instance_manager::metrics(const identifier& instance_id)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto                        found = instances.find(instance_id);
  if (found == instances.end()) {
    return {result_code::not_found, {}, "instance does not exist"};
  }
  refresh(found->second);
  if (found->second.record.state == OCUDU_DAPP_STATE_UNLOADED_V1) {
    return {result_code::incompatible, {}, "instance is unloaded"};
  }
  const auto health = found->second.runtime->health();
  refresh(found->second);
  auto standard = host_metric_samples(
      health, has_class_a_interface(found->second.record), has_class_b_scheduler(found->second.record));
  if ((found->second.record.management_capabilities & OCUDU_DAPP_MANAGEMENT_METRICS_V1) == 0) {
    return {result_code::ok, std::move(standard), {}};
  }
  auto result = collect_bounded<ocudu_dapp_metric_sample_v1, metric_sample>(
      [&](auto* output, uint32_t capacity) { return found->second.runtime->read_metrics(output, capacity); },
      [](const auto& item) -> std::optional<metric_sample> {
        auto name  = copy_string(item.name);
        auto value = copy_value(item.value);
        if (!name || name->empty() || !value) {
          return std::nullopt;
        }
        return metric_sample{std::move(*name), std::move(*value), item.monotonic_timestamp_ns};
      },
      "module returned malformed metric sample");
  refresh(found->second);
  if (result) {
    standard.insert(
        standard.end(), std::make_move_iterator(result.value.begin()), std::make_move_iterator(result.value.end()));
    result.value = std::move(standard);
  }
  return result;
}

operation_result instance_manager::manage_model(const model_request& request)
{
  std::lock_guard<std::mutex> lock(mutex);
  const uint64_t              request_fingerprint = canonical_fingerprint(request);
  if (request.transaction_id.is_zero() || request.expected_model_generation == 0 || request.model_id.empty() ||
      request.model_id.size() > 128 || request.model_version.size() > 64 ||
      request.artifact.size() > maximum_management_blob || request.action < OCUDU_DAPP_MODEL_STAGE_V1 ||
      request.action > OCUDU_DAPP_MODEL_RETIRE_V1) {
    return make_result(result_code::invalid_request, request.transaction_id, "invalid model request");
  }
  if (auto prior = replay(request.transaction_id, request_fingerprint)) {
    return *prior;
  }
  auto found = instances.find(request.instance_id);
  if (found == instances.end()) {
    auto response = make_result(result_code::not_found, request.transaction_id, "instance does not exist");
    remember(request_fingerprint, response);
    return response;
  }
  auto& managed = found->second;
  refresh(managed);
  if (managed.record.state == OCUDU_DAPP_STATE_UNLOADED_V1 ||
      (request.expected_instance_generation && *request.expected_instance_generation != managed.record.generation)) {
    auto response = make_result(managed.record.state == OCUDU_DAPP_STATE_UNLOADED_V1 ? result_code::incompatible
                                                                                     : result_code::conflict,
                                request.transaction_id,
                                "instance is unloaded or generation changed");
    remember(request_fingerprint, response);
    return response;
  }
  ocudu_dapp_model_request_v1 native{};
  native.struct_size                     = sizeof(native);
  native.action                          = request.action;
  native.model_id                        = request.model_id.c_str();
  native.model_version                   = request.model_version.c_str();
  native.artifact.data                   = request.artifact.data();
  native.artifact.size                   = request.artifact.size();
  native.expected_generation             = request.expected_model_generation;
  native.dry_run                         = request.dry_run;
  const auto generation_before_operation = managed.record.generation;
  auto       applied                     = managed.runtime->manage_model(native);
  refresh(managed);
  if (applied && !request.dry_run && managed.record.generation == generation_before_operation) {
    advance_generation(managed);
  }
  auto response = make_result(applied ? (request.dry_run ? result_code::dry_run_ok : result_code::ok)
                                      : map_status(applied.operation.status),
                              request.transaction_id,
                              applied.operation.detail.empty() ? nullptr : applied.operation.detail.c_str());
  response.instance_id          = request.instance_id;
  response.instance_generation  = managed.record.generation;
  response.resource_generation  = applied.generation;
  response.state                = managed.record.state;
  response.inventory_generation = catalog.inventory_generation();
  remember(request_fingerprint, response);
  return response;
}

inventory_snapshot instance_manager::inventory()
{
  std::lock_guard<std::mutex> lock(mutex);
  inventory_snapshot          snapshot;
  snapshot.packages.reserve(catalog.packages().size());
  snapshot.instances.reserve(instances.size());
  for (const auto& entry : catalog.packages()) {
    snapshot.packages.push_back(entry.second);
    if (package_signatures) {
      auto permitted = package_signatures->authorize_admitted(entry.second);
      if (!permitted) {
        snapshot.packages.back().compatible      = false;
        snapshot.packages.back().incompatibility = permitted.detail;
      }
    }
  }
  for (auto& entry : instances) {
    refresh(entry.second);
    snapshot.instances.push_back(entry.second.record);
  }
  snapshot.generation = catalog.inventory_generation();
  std::sort(snapshot.packages.begin(), snapshot.packages.end(), [](const auto& left, const auto& right) {
    return identifier_less(left.package_id, right.package_id);
  });
  std::sort(snapshot.instances.begin(), snapshot.instances.end(), [](const auto& left, const auto& right) {
    return identifier_less(left.instance_id, right.instance_id);
  });
  return snapshot;
}

std::optional<package_record> instance_manager::package(const identifier& id) const
{
  std::lock_guard<std::mutex> lock(mutex);
  return catalog.find_package(id);
}

std::optional<instance_record> instance_manager::instance(const identifier& id)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto                        found = instances.find(id);
  if (found != instances.end()) {
    refresh(found->second);
  }
  return found != instances.end() ? std::optional<instance_record>(found->second.record) : std::nullopt;
}

std::size_t instance_manager::drain_incidents()
{
  std::lock_guard<std::mutex> lock(mutex);
  std::size_t                 count = 0;
  if (host_incident_drain) {
    try {
      count += host_incident_drain(incident_history);
    } catch (...) {
    }
  }
  for (auto& entry : instances) {
    count += refresh(entry.second);
  }
  return count;
}

bool instance_manager::set_host_incident_drain(std::function<std::size_t(incident_store&)> drain)
{
  std::lock_guard<std::mutex> lock(mutex);
  if (!drain || host_incident_drain) {
    return false;
  }
  host_incident_drain = std::move(drain);
  return true;
}

std::size_t instance_manager::refresh(managed_instance& managed)
{
  const auto      runtime_state = managed.runtime->state();
  const auto      count         = managed.runtime->drain_incidents(incident_history);
  incident_filter filter;
  filter.instance_id              = words(managed.record.instance_id);
  const auto     related          = incident_history.query(filter);
  const uint64_t latest_incident  = related.empty() ? managed.record.last_incident_id : related.back().incident_id;
  const bool     state_changed    = runtime_state != managed.record.state;
  const bool     incident_changed = latest_incident != managed.record.last_incident_id;
  const bool     incident_advances_generation =
      incident_changed && !related.empty() && related.back().action != incident_action::observed;
  managed.record.state            = runtime_state;
  managed.record.last_incident_id = latest_incident;
  if (state_changed || incident_advances_generation) {
    advance_generation(managed);
  } else if (incident_changed) {
    catalog.bump_inventory_generation();
  }
  return count;
}

void instance_manager::advance_generation(managed_instance& managed)
{
  ++managed.record.generation;
  catalog.bump_inventory_generation();
  const auto instance_words = words(managed.record.instance_id);
  const auto package_words  = words(managed.record.package_id);
  managed.runtime->set_incident_identity(instance_words.data(), package_words.data(), managed.record.generation);
}

identifier instance_manager::next_identifier() noexcept
{
  identifier     result;
  const uint64_t sequence = id_sequence++;
  std::memcpy(result.bytes.data(), &id_nonce, sizeof(id_nonce));
  std::memcpy(result.bytes.data() + sizeof(id_nonce), &sequence, sizeof(sequence));
  return result;
}

std::optional<operation_result> instance_manager::replay(const identifier& transaction, uint64_t fingerprint_) const
{
  auto found = transactions.find(transaction);
  if (found == transactions.end()) {
    return std::nullopt;
  }
  if (found->second.fingerprint == fingerprint_) {
    return found->second.result;
  }
  return make_result(
      result_code::transaction_mismatch, transaction, "transaction ID was reused for a different request");
}

void instance_manager::remember(uint64_t fingerprint_, const operation_result& result)
{
  if (transactions.find(result.transaction_id) != transactions.end()) {
    return;
  }
  if (transactions.size() == maximum_transaction_records) {
    transactions.erase(transaction_order.front());
    transaction_order.pop_front();
  }
  transaction_order.push_back(result.transaction_id);
  transactions.emplace(result.transaction_id, transaction_record{fingerprint_, result});
}

operation_result
instance_manager::make_result(result_code code, const identifier& transaction, const char* detail) const
{
  operation_result result;
  result.code                 = code;
  result.transaction_id       = transaction;
  result.inventory_generation = catalog.inventory_generation();
  result.detail               = detail != nullptr ? detail : "";
  return result;
}

std::vector<interface_record> instance_manager::describe_interfaces(const ocudu_dapp_module_v1& module)
{
  std::vector<interface_record> result;
  result.reserve(module.nof_interfaces);
  for (uint32_t i = 0; i != module.nof_interfaces; ++i) {
    const auto&                                    source = module.interfaces[i];
    std::optional<interface_record::pusch_profile> pusch;
    if (source.use_case_profile != nullptr) {
      const auto& profile = *static_cast<const ocudu_dapp_pusch_profile_v1*>(source.use_case_profile);
      pusch               = interface_record::pusch_profile{profile.max_nof_prb,
                                              profile.max_nof_rx_ports,
                                              profile.max_nof_layers,
                                              profile.modulation_mask,
                                              profile.dmrs_type_mask,
                                              profile.flags};
    }
    result.push_back({source.interface_id,
                      source.interface_major,
                      source.interface_minor,
                      static_cast<ocudu_dapp_class_v1>(source.dapp_class),
                      source.placement_mask,
                      static_cast<ocudu_dapp_threading_v1>(source.threading),
                      source.capability_bits,
                      pusch});
  }
  return result;
}
