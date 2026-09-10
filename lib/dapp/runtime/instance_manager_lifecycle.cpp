// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Lifecycle orchestration for instance_manager: load() and control().
// Split translation unit only — identical methods, no behavior change.

#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/dapp/abi/v1/memory.h"
#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <stdexcept>

using namespace ocudu::dapp;
using namespace ocudu::dapp::management;

namespace {
#include "instance_manager_helpers.inc"
}

operation_result instance_manager::load(const load_request& request)
{
  std::lock_guard<std::mutex> lock(mutex);
  const uint64_t              request_fingerprint = canonical_fingerprint(request);
  if (request.transaction_id.is_zero()) {
    return make_result(result_code::invalid_request, request.transaction_id, "transaction ID is required");
  }
  if (auto prior = replay(request.transaction_id, request_fingerprint)) {
    return *prior;
  }
  if (request.expected_inventory_generation && *request.expected_inventory_generation != catalog.inventory_generation()) {
    auto response = make_result(result_code::conflict, request.transaction_id, "inventory generation mismatch");
    remember(request_fingerprint, response);
    return response;
  }
  auto package_it = catalog.packages().find(request.package_id);
  if (package_it == catalog.packages().end()) {
    auto response =
        make_result(result_code::not_found, request.transaction_id, "package is not in the approved catalog");
    remember(request_fingerprint, response);
    return response;
  }
  const auto& catalog_package   = package_it->second;
  if (package_signatures) {
    auto permitted = package_signatures->authorize_admitted(catalog_package);
    if (!permitted) {
      auto response = make_result(result_code::verification_failed, request.transaction_id, permitted.detail.c_str());
      remember(request_fingerprint, response);
      return response;
    }
  }
  const bool  process_placement = request.placement == OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1;
  if ((request.placement != OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 && !process_placement) ||
      (process_placement && request.create_args != nullptr)) {
    auto response = make_result(result_code::invalid_request, request.transaction_id, "invalid placement load request");
    remember(request_fingerprint, response);
    return response;
  }
  if ((catalog_package.placement_mask & request.placement) == 0 || !catalog_package.compatible ||
      !supports_backend(catalog_package, request.selected_backend) ||
      !host_supports_backend(host_information.backend_mask, request.selected_backend) ||
      (!process_placement && !abi_backend(request.selected_backend)) ||
      (process_placement && catalog.process_factory(request.package_id) == nullptr)) {
    auto response =
        make_result(result_code::incompatible, request.transaction_id, "requested package/backend is incompatible");
    remember(request_fingerprint, response);
    return response;
  }
  if (const char* l1_detail =
          class_a_l1_path_incompatibility(catalog_package, request.selected_backend, host_information.backend_mask);
      l1_detail != nullptr) {
    auto response = make_result(result_code::incompatible, request.transaction_id, l1_detail);
    remember(request_fingerprint, response);
    return response;
  }
  if (request.dry_run) {
    auto response = make_result(result_code::dry_run_ok, request.transaction_id);
    remember(request_fingerprint, response);
    return response;
  }

  auto        approved = catalog.verifier().verify(catalog_package.canonical_artifact_path, catalog_package.content_digest);
  std::string refresh_refusal;
  if (!approved && !package_has_resident_instance(request.package_id)) {
    if (try_refresh_package_from_disk(request.package_id, refresh_refusal)) {
      const auto refreshed = catalog.find_package(request.package_id);
      if (refreshed) {
        approved = catalog.verifier().verify(refreshed->canonical_artifact_path, refreshed->content_digest);
      }
    }
  }
  if (!approved) {
    // Name the refresh refusal distinctly: "signature rejected" and
    // "anti-rollback refused" demand different operator responses.
    std::string detail = approved.detail;
    if (!refresh_refusal.empty()) {
      detail += "; on-disk refresh: " + refresh_refusal;
    }
    auto response = make_result(result_code::verification_failed, request.transaction_id, detail.c_str());
    remember(request_fingerprint, response);
    return response;
  }
  managed_instance managed;
  if (process_placement) {
    process_runtime_create_result created;
    try {
      created = (*catalog.process_factory(request.package_id))(std::move(approved.artifact), request.selected_backend);
    } catch (const std::bad_alloc&) {
      created.operation = {OCUDU_DAPP_RESOURCE_V1, OCUDU_DAPP_STATE_ERROR_V1, "process factory allocation failed"};
    } catch (...) {
      created.operation = {OCUDU_DAPP_EXCEPTION_V1, OCUDU_DAPP_STATE_ERROR_V1, "process factory threw"};
    }
    if (!created) {
      auto response =
          make_result(result_code::lifecycle_failed, request.transaction_id, created.operation.detail.c_str());
      remember(request_fingerprint, response);
      return response;
    }
    if (created.bindings.empty() &&
        std::all_of(catalog_package.interfaces.begin(), catalog_package.interfaces.end(), [](const auto& interface) {
          return interface.requirement == binding_requirement::required;
        })) {
      created.bindings.assign(catalog_package.interfaces.size(), binding_state::bound);
    }
    if (created.bindings.size() != catalog_package.interfaces.size()) {
      auto response = make_result(
          result_code::lifecycle_failed, request.transaction_id, "process factory returned incomplete binding state");
      remember(request_fingerprint, response);
      return response;
    }
    bool any_bound = false;
    for (std::size_t index = 0; index != created.bindings.size(); ++index) {
      const bool bound       = created.bindings[index] == binding_state::bound;
      const bool unavailable = created.bindings[index] == binding_state::unavailable;
      if ((!bound && !unavailable) ||
          (catalog_package.interfaces[index].requirement == binding_requirement::required && !bound)) {
        auto response = make_result(
            result_code::lifecycle_failed, request.transaction_id, "process factory returned invalid binding state");
        remember(request_fingerprint, response);
        return response;
      }
      any_bound |= bound;
    }
    if (!any_bound) {
      auto response = make_result(
          result_code::lifecycle_failed, request.transaction_id, "process factory resolved no callable interface");
      remember(request_fingerprint, response);
      return response;
    }
    managed.runtime           = std::move(created.runtime);
    managed.record.interfaces = catalog_package.interfaces;
    managed.process_bindings  = std::move(created.bindings);
  } else {
    if (!native_preflight_helper.empty()) {
      auto preflight = preflight_native_module(
          *approved.artifact, host_information, native_preflight_helper, native_preflight_timeout);
      if (!preflight) {
        auto response = make_result(result_code::load_failed, request.transaction_id, preflight.detail.c_str());
        remember(request_fingerprint, response);
        return response;
      }
    }
    try {
      managed.activation_gate = std::make_shared<native_activation_gate>();
    } catch (const std::bad_alloc&) {
      auto response =
          make_result(result_code::resource_exhausted, request.transaction_id, "activation gate allocation failed");
      remember(request_fingerprint, response);
      return response;
    }
    auto loaded = load_native_module(std::move(*approved.artifact), host_information);
    if (!loaded) {
      auto response = make_result(result_code::load_failed, request.transaction_id, loaded.detail.c_str());
      remember(request_fingerprint, response);
      return response;
    }
    const auto& descriptor = loaded.library->descriptor();
    if (catalog_package.global_name != descriptor.global_name || catalog_package.vendor != descriptor.vendor ||
        catalog_package.version != descriptor.version) {
      auto response =
          make_result(result_code::incompatible, request.transaction_id, "module identity differs from manifest");
      remember(request_fingerprint, response);
      return response;
    }
    auto discovered_interfaces = describe_interfaces(descriptor);
    if (!catalog_package.interfaces.empty() &&
        !same_interfaces(catalog_package.interfaces, discovered_interfaces, false)) {
      auto response =
          make_result(result_code::incompatible, request.transaction_id, "module interfaces differ from manifest");
      remember(request_fingerprint, response);
      return response;
    }
    auto created = native_instance::create(
        loaded.library, host_callbacks, request.create_args, *abi_backend(request.selected_backend));
    if (!created) {
      auto response =
          make_result(result_code::lifecycle_failed, request.transaction_id, created.operation.detail.c_str());
      remember(request_fingerprint, response);
      return response;
    }
    if (!catalog_package.interfaces.empty()) {
      for (std::size_t index = 0; index != discovered_interfaces.size(); ++index) {
        discovered_interfaces[index].requirement = catalog_package.interfaces[index].requirement;
      }
    }
    managed.native_runtime    = created.instance.get();
    managed.runtime           = std::move(created.instance);
    managed.record.interfaces = std::move(discovered_interfaces);
  }
  managed.record.instance_id             = next_identifier();
  managed.record.package_id              = catalog_package.package_id;
  managed.record.global_name             = catalog_package.global_name;
  managed.record.vendor                  = catalog_package.vendor;
  managed.record.version                 = catalog_package.version;
  managed.record.placement               = request.placement;
  managed.record.selected_backend        = request.selected_backend;
  managed.record.state                   = managed.runtime->state();
  managed.record.generation              = 1;
  managed.record.management_capabilities = managed.runtime->management_capabilities();
  const auto instance_words              = words(managed.record.instance_id);
  const auto package_words               = words(managed.record.package_id);
  managed.runtime->set_incident_identity(instance_words.data(), package_words.data(), managed.record.generation);
  const identifier instance_id = managed.record.instance_id;
  instances.emplace(instance_id, std::move(managed));
  catalog.bump_inventory_generation();

  auto response                 = make_result(result_code::ok, request.transaction_id);
  response.instance_id          = instance_id;
  response.instance_generation  = 1;
  response.state                = OCUDU_DAPP_STATE_LOADED_V1;
  response.inventory_generation = catalog.inventory_generation();
  remember(request_fingerprint, response);
  return response;
}

operation_result instance_manager::control(const lifecycle_request& request)
{
  std::lock_guard<std::mutex> lock(mutex);
  const uint64_t              request_fingerprint = canonical_fingerprint(request);
  if (request.transaction_id.is_zero() || request.timeout_us > 3'600'000'000ULL) {
    return make_result(result_code::invalid_request, request.transaction_id, "invalid transaction or timeout");
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
  if (request.expected_generation && *request.expected_generation != managed.record.generation) {
    auto response        = make_result(result_code::conflict, request.transaction_id, "instance generation mismatch");
    response.instance_id = managed.record.instance_id;
    response.instance_generation = managed.record.generation;
    response.state               = managed.record.state;
    remember(request_fingerprint, response);
    return response;
  }
  if (!action_valid_for_state(request.action, managed.record.state)) {
    auto response =
        make_result(result_code::lifecycle_failed, request.transaction_id, "action is illegal in current state");
    response.instance_id         = managed.record.instance_id;
    response.instance_generation = managed.record.generation;
    response.state               = managed.record.state;
    remember(request_fingerprint, response);
    return response;
  }
  if (request.action == lifecycle_action::activate && package_signatures) {
    const auto package   = catalog.find_package(managed.record.package_id);
    auto       permitted = package ? package_signatures->authorize_admitted(*package)
                                   : package_signature_result{false, "instance package is absent from the catalog"};
    if (!permitted) {
      auto response = make_result(result_code::verification_failed, request.transaction_id, permitted.detail.c_str());
      response.instance_id         = managed.record.instance_id;
      response.instance_generation = managed.record.generation;
      response.state               = managed.record.state;
      remember(request_fingerprint, response);
      return response;
    }
  }
  if (request.dry_run) {
    if (request.action == lifecycle_action::activate && native_hooks != nullptr && managed.native_runtime != nullptr) {
      auto validation = native_hooks->validate(managed.record.interfaces);
      if (!validation) {
        auto response = make_result(map_status(validation.status), request.transaction_id, validation.detail.c_str());
        response.instance_id         = managed.record.instance_id;
        response.instance_generation = managed.record.generation;
        response.state               = managed.record.state;
        remember(request_fingerprint, response);
        return response;
      }
    }
    auto response                = make_result(result_code::dry_run_ok, request.transaction_id);
    response.instance_id         = managed.record.instance_id;
    response.instance_generation = managed.record.generation;
    response.state               = managed.record.state;
    remember(request_fingerprint, response);
    return response;
  }

  const auto                generation_before_operation = managed.record.generation;
  const auto                deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(request.timeout_us);
  instance_operation_result operation;
  const auto                hook_failure = [&](const native_hook_registry_result& failure) {
    return instance_operation_result{failure.status, managed.runtime->state(), failure.detail};
  };
  switch (request.action) {
    case lifecycle_action::prepare:
      operation = managed.runtime->prepare(request.primary_payload, request.secondary_payload);
      break;
    case lifecycle_action::arm:
      operation = managed.runtime->arm(request.primary_payload);
      break;
    case lifecycle_action::enter_shadow:
      operation = managed.runtime->enter_shadow();
      break;
    case lifecycle_action::activate: {
      if (native_hooks == nullptr || managed.native_runtime == nullptr) {
        operation = managed.runtime->activate();
        break;
      }
      auto staged = native_hooks->stage(
          managed.record.instance_id, *managed.native_runtime, managed.record.interfaces, managed.activation_gate);
      if (!staged) {
        operation = hook_failure(staged);
        break;
      }
      operation = managed.runtime->activate();
      if (!operation) {
        static_cast<void>(native_hooks->deactivate(managed.record.instance_id, deadline));
        break;
      }
      auto committed = native_hooks->commit(managed.record.instance_id);
      if (!committed) {
        const auto hooks = native_hooks->deactivate(managed.record.instance_id, deadline);
        if (hooks) {
          static_cast<void>(managed.runtime->drain(deadline));
        }
        operation = hook_failure(committed);
      } else if (!native_hooks->binding_states(managed.record.instance_id, managed.record.interfaces)) {
        const auto hooks = native_hooks->deactivate(managed.record.instance_id, deadline);
        if (hooks) {
          static_cast<void>(managed.runtime->drain(deadline));
        }
        operation = {OCUDU_DAPP_INTERNAL_ERROR_V1,
                     managed.runtime->state(),
                     "native hook binding inventory is unavailable after commit"};
      }
      break;
    }
    case lifecycle_action::deactivate: {
      if (native_hooks != nullptr && managed.native_runtime != nullptr) {
        auto hooks = native_hooks->deactivate(managed.record.instance_id, deadline);
        if (!hooks) {
          operation = hook_failure(hooks);
          break;
        }
      }
      operation = managed.runtime->drain(deadline);
      break;
    }
    case lifecycle_action::retry_quiescence: {
      if (native_hooks != nullptr && managed.native_runtime != nullptr) {
        auto hooks = native_hooks->deactivate(managed.record.instance_id, deadline);
        if (!hooks) {
          operation = hook_failure(hooks);
          break;
        }
      }
      operation = managed.runtime->retry_quiescence(deadline);
      break;
    }
    case lifecycle_action::unload:
      if (native_hooks != nullptr && managed.native_runtime != nullptr &&
          native_hooks->bound(managed.record.instance_id)) {
        operation = {OCUDU_DAPP_CONFLICT_V1, managed.runtime->state(), "native hook binding has not quiesced"};
      } else {
        operation = managed.runtime->unload();
      }
      break;
    case lifecycle_action::breaker_probe:
      operation = managed.runtime->reset_breakers();
      break;
  }
  if (operation && request.action == lifecycle_action::activate && managed.native_runtime == nullptr) {
    for (std::size_t index = 0; index != managed.record.interfaces.size(); ++index) {
      managed.record.interfaces[index].state = managed.process_bindings[index];
    }
  }
  if (operation &&
      (request.action == lifecycle_action::deactivate || request.action == lifecycle_action::retry_quiescence ||
       request.action == lifecycle_action::unload)) {
    for (auto& interface : managed.record.interfaces) {
      interface.state = binding_state::unbound;
    }
  }
  refresh(managed);
  const std::string last_error    = operation ? "" : operation.detail;
  const bool        error_changed = managed.record.last_error != last_error;
  managed.record.last_error       = last_error;
  if ((operation || error_changed) && managed.record.generation == generation_before_operation) {
    advance_generation(managed);
  }

  auto response = make_result(operation ? result_code::ok : result_code::lifecycle_failed,
                              request.transaction_id,
                              operation.detail.empty() ? nullptr : operation.detail.c_str());
  response.instance_id          = managed.record.instance_id;
  response.instance_generation  = managed.record.generation;
  response.state                = managed.record.state;
  response.inventory_generation = catalog.inventory_generation();
  remember(request_fingerprint, response);
  return response;
}
