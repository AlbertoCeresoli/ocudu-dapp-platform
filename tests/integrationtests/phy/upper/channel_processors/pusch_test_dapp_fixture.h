// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include "apps/services/dapp/dapp_service.h"
#include "pxsch_bler_test_factories.h"
#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/dapp/use_cases/v1/receiver_completion.h"
#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace ocudu::test_support {

/// Owns only an isolated in-process service. No E3 socket or live gNB connection.
class pusch_test_dapp_fixture
{
public:
  pusch_test_dapp_fixture(const std::string& manifest,
                         unsigned lanes,
                         unsigned prbs,
                         unsigned ports,
                         unsigned layers,
                         bool receiver_completion = false)
  {
    const auto                        path = std::filesystem::canonical(manifest);
    app_services::dapp_service_config config;
    config.catalog_root             = path.parent_path().string();
    config.package_manifests        = {path.filename().string()};
    config.native_preflight_helper  = OCUDU_DAPP_PREFLIGHT_HELPER;
    config.native_preflight_timeout = std::chrono::milliseconds{1000};
    // Explicit local research artifact only; production signature policy is unchanged.
    config.package_signatures.mode                 = dapp::package_signature_mode::allow_unsigned;
    config.package_signatures.minimum_policy_epoch = 0;
    config.cells                                   = {{0, 1, static_cast<uint16_t>(prbs)}};
    config.nof_pusch_lanes                         = lanes;
    config.nof_srs_lanes = config.nof_scheduler_lanes = 1;
    config.backend_mask = (uint64_t{1} << OCUDU_DAPP_BACKEND_HOST_V1) | (uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1);
    // Isolated production-adapter test only. The normal service/gNB default
    // remains zero; this opt-in is not promotion of live completion support.
    config.host_capabilities = receiver_completion ? OCUDU_DAPP_HOST_CAP_RECEIVER_COMPLETION_V1 : 0;
    auto created        = app_services::dapp_service::create(std::move(config));
    require(static_cast<bool>(created), created.detail);
    service = std::move(created.service);
    require(!service->has_local_e3() && !service->has_e3ap_sctp() && !service->has_e3dp_sctp(),
            "unexpected management socket");
    hooks = {service->upper_phy_hooks(),
             {.nof_lanes = lanes, .max_nof_prb = prbs, .max_nof_rx_ports = ports, .max_nof_layers = layers}};
  }

  const pxsch_test_dapp_config& factory_config() const { return hooks; }

  void activate()
  {
    const auto inventory = service->instances().inventory();
    require(inventory.packages.size() == 1, "exactly one explicit receiver package required");
    const auto& package = inventory.packages.front();
    require(std::any_of(package.interfaces.begin(),
                        package.interfaces.end(),
                        [](const auto& i) { return i.interface_id == OCUDU_DAPP_INTERFACE_RECEIVER_V1; }),
            "package lacks receiver interface");
    dapp::management::load_request request;
    request.transaction_id   = transaction_id(next_transaction++);
    request.package_id       = package.package_id;
    request.placement        = OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1;
    request.selected_backend = dapp::management::backend::cuda;
    auto loaded              = service->instances().load(request);
    require(static_cast<bool>(loaded) && loaded.instance_id.has_value(), loaded.detail);
    instance   = *loaded.instance_id;
    generation = loaded.instance_generation;
    for (auto action : {dapp::management::lifecycle_action::prepare,
                        dapp::management::lifecycle_action::arm,
                        dapp::management::lifecycle_action::enter_shadow,
                        dapp::management::lifecycle_action::activate}) {
      control(action);
    }
  }

  uint64_t counter(const char* name) const
  {
    const auto result = service->instances().metrics(instance);
    require(static_cast<bool>(result), "read dApp counters");
    const auto found =
        std::find_if(result.value.begin(), result.value.end(), [&](const auto& sample) { return sample.name == name; });
    require(found != result.value.end(), std::string("missing counter: ") + name);
    const auto* value = std::get_if<uint64_t>(&found->sample_value);
    require(value != nullptr, "counter type mismatch");
    return *value;
  }

  uint64_t fallback_count() const
  {
    // dapp.fallback.count is optional module telemetry. The authoritative host
    // counters cover admission failures too, before the module is invoked.
    const auto result = service->instances().metrics(instance);
    require(static_cast<bool>(result), "read host fallback counters");
    uint64_t total = 0;
    unsigned found = 0;
    for (const auto& sample : result.value) {
      if (sample.name.rfind("class_a.fallback.", 0) != 0)
        continue;
      const auto* value = std::get_if<uint64_t>(&sample.sample_value);
      require(value != nullptr, "host fallback counter type mismatch");
      total += *value;
      ++found;
    }
    require(found == static_cast<unsigned>(dapp::class_a_fallback_reason::count), "incomplete host fallback counters");
    return total;
  }

  /// Failure evidence only, outside the timed processor call. A successful CRC
  /// after a host fallback must not hide which admission/invocation gate rejected
  /// the receiver. These are cumulative counters from this isolated instance.
  std::string fallback_details() const
  {
    const auto result = service->instances().metrics(instance);
    require(static_cast<bool>(result), "read fallback failure details");
    std::string details;
    for (const auto& sample : result.value) {
      if (sample.name.rfind("class_a.fallback.", 0) != 0) {
        continue;
      }
      const auto* value = std::get_if<uint64_t>(&sample.sample_value);
      require(value != nullptr, "fallback detail counter type mismatch");
      if (*value != 0) {
        if (!details.empty()) {
          details += ", ";
        }
        details += sample.name + "=" + std::to_string(*value);
      }
    }
    return details;
  }

  void unload()
  {
    control(dapp::management::lifecycle_action::deactivate);
    control(dapp::management::lifecycle_action::unload);
    service->stop();
  }

private:
  static void require(bool ok, const std::string& why)
  {
    if (!ok)
      throw std::runtime_error(why);
  }
  static dapp::management::identifier transaction_id(uint64_t value)
  {
    dapp::management::identifier id{};
    // Local nonzero transaction identifiers; not package/session identity.
    for (unsigned i = 0; i != 8; ++i)
      id.bytes[15 - i] = static_cast<uint8_t>(value >> (8 * i));
    return id;
  }
  void control(dapp::management::lifecycle_action action)
  {
    dapp::management::lifecycle_request request;
    request.transaction_id      = transaction_id(next_transaction++);
    request.instance_id         = instance;
    request.expected_generation = generation;
    request.action              = action;
    request.timeout_us          = 10'000'000; // Management only; does not relax invocation deadlines.
    const auto result           = service->instances().control(request);
    require(static_cast<bool>(result), result.detail);
    generation = result.instance_generation;
  }

  std::unique_ptr<app_services::dapp_service> service;
  pxsch_test_dapp_config                      hooks;
  dapp::management::identifier                instance{};
  uint64_t                                    generation{};
  uint64_t                                    next_transaction{1};
};
} // namespace ocudu::test_support
