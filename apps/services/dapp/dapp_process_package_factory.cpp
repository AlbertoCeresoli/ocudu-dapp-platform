// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "dapp_process_package_factory.h"
#include "ocudu/dapp/runtime/class_c_process_instance.h"
#include "ocudu/dapp/runtime/class_c_process_supervisor.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_process_target.h"
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

using namespace ocudu;
using namespace ocudu::app_services;
using namespace ocudu::dapp;
using namespace ocudu::dapp::management;

namespace {

void append_cuda_core_environment(std::vector<std::string>& environment)
{
  constexpr const char* variables[]{"CUDA_ENABLE_COREDUMP_ON_EXCEPTION",
                                    "CUDA_ENABLE_CPU_COREDUMP_ON_EXCEPTION",
                                    "CUDA_COREDUMP_FILE",
                                    "CUDA_COREDUMP_GENERATION_FLAGS",
                                    "CUDA_COREDUMP_SHOW_PROGRESS"};
  for (const char* name : variables) {
    const char* value = std::getenv(name);
    if (value != nullptr) {
      environment.emplace_back(std::string(name) + '=' + value);
    }
  }
}

class process_route_proxy final : public upper_phy_dapp_publication_target
{
public:
  struct binding_state {
    std::weak_ptr<upper_phy_dapp_process_target> target;
  };

  explicit process_route_proxy(uint32_t interface_id_) : id(interface_id_) {}

  uint32_t interface_id() const noexcept override { return id; }
  bool     active() const noexcept override
  {
    auto target = acquire();
    return target != nullptr && target->active();
  }

  bool uses_accelerator_spectrum() const noexcept override
  {
    auto target = acquire();
    return target != nullptr && target->uses_accelerator_spectrum();
  }

  void publish_accelerator_spectrum(const ocudu_dapp_spectrum_input_v1& input,
                                    const resource_grid_reader&         grid,
                                    const dapp::resource_grid_lease&    lease) noexcept override
  {
    if (auto target = acquire(); target != nullptr) {
      target->publish_accelerator_spectrum(input, grid, lease);
    }
  }

  bool uses_accelerator_srs() const noexcept override
  {
    auto target = acquire();
    return target != nullptr && target->uses_accelerator_srs();
  }

  void publish_accelerator_srs(const ocudu_dapp_srs_isac_input_v1& input,
                               const resource_grid_reader&         grid,
                               const srs_estimator_configuration&  config,
                               const dapp::resource_grid_lease&    lease) noexcept override
  {
    if (auto target = acquire(); target != nullptr) {
      target->publish_accelerator_srs(input, grid, config, lease);
    }
  }

  void publish(const ocudu_dapp_spectrum_input_v1& input) noexcept override
  {
    if (auto target = acquire(); target != nullptr) {
      target->publish(input);
    }
  }

  void publish(const ocudu_dapp_ul_resource_plan_v1& plan) noexcept override
  {
    if (auto target = acquire(); target != nullptr) {
      target->publish(plan);
    }
  }

  void publish(const ocudu_dapp_srs_isac_input_v1& input) noexcept override
  {
    if (auto target = acquire(); target != nullptr) {
      target->publish(input);
    }
  }

  bool bind(const std::shared_ptr<binding_state>& desired) noexcept
  {
    std::shared_ptr<binding_state> expected;
    return std::atomic_compare_exchange_strong_explicit(
        &binding, &expected, desired, std::memory_order_release, std::memory_order_relaxed);
  }

  void unbind(const std::shared_ptr<binding_state>& expected_state) noexcept
  {
    auto expected = expected_state;
    std::atomic_compare_exchange_strong_explicit(
        &binding, &expected, std::shared_ptr<binding_state>{}, std::memory_order_release, std::memory_order_relaxed);
  }

private:
  std::shared_ptr<upper_phy_dapp_process_target> acquire() const noexcept
  {
    auto snapshot = std::atomic_load_explicit(&binding, std::memory_order_acquire);
    return snapshot != nullptr ? snapshot->target.lock() : nullptr;
  }

  const uint32_t                         id;
  mutable std::shared_ptr<binding_state> binding;
};

class leased_process_endpoint final : public class_c_process_endpoint
{
public:
  leased_process_endpoint(std::shared_ptr<upper_phy_dapp_process_target>      target_,
                          std::shared_ptr<process_route_proxy>                proxy_,
                          std::shared_ptr<process_route_proxy::binding_state> binding_) :
    target(std::move(target_)), proxy(std::move(proxy_)), binding(std::move(binding_))
  {
  }

  ~leased_process_endpoint() override { proxy->unbind(binding); }

  uint32_t interface_id() const noexcept override
  {
    return static_cast<const class_c_process_endpoint&>(*target).interface_id();
  }
  ipc::route_capabilities export_capabilities() const noexcept override { return target->export_capabilities(); }
  void                    set_active(bool active) noexcept override { target->set_active(active); }
  bool reset_after_consumer_restart() noexcept override { return target->reset_after_consumer_restart(); }
  class_c_process_endpoint_metrics snapshot_metrics() const noexcept override { return target->snapshot_metrics(); }
  bool                             consume_result_source(uint64_t                        source_sequence,
                                                         uint64_t                        source_plan_generation,
                                                         uint32_t                        cell_id,
                                                         const ocudu_dapp_radio_time_v1& created) noexcept override
  {
    return target->consume_result_source(source_sequence, source_plan_generation, cell_id, created);
  }
  bool consume_quiet_intent_source(uint64_t                        source_sequence,
                                   uint64_t                        source_plan_generation,
                                   uint32_t                        cell_id,
                                   const ocudu_dapp_radio_time_v1& created) noexcept override
  {
    return target->consume_quiet_intent_source(source_sequence, source_plan_generation, cell_id, created);
  }
  bool consume_srs_result_source(uint64_t                        source_sequence,
                                 uint32_t                        srs_resource_id,
                                 uint32_t                        srs_config_id,
                                 uint32_t                        cell_id,
                                 const ocudu_dapp_radio_time_v1& created) noexcept override
  {
    return target->consume_srs_result_source(source_sequence, srs_resource_id, srs_config_id, cell_id, created);
  }

private:
  std::shared_ptr<upper_phy_dapp_process_target>      target;
  std::shared_ptr<process_route_proxy>                proxy;
  std::shared_ptr<process_route_proxy::binding_state> binding;
};

struct route_plan {
  std::shared_ptr<process_route_proxy> proxy;
  uint32_t                             interface_id{};
  uint32_t                             cell_id{};
  uint32_t                             flags{};
  uint32_t                             result_capabilities{};
};

struct interface_plan {
  interface_record contract;
  std::size_t      first_route{};
  std::size_t      route_count{};
};

ipc::class_c_identity producer_identity(const identifier& package_id, uint64_t route_id) noexcept
{
  ipc::class_c_identity result;
  std::memcpy(&result.low, package_id.bytes.data(), sizeof(result.low));
  std::memcpy(&result.high, package_id.bytes.data() + sizeof(result.low), sizeof(result.high));
  result.low ^= route_id * UINT64_C(0x9e3779b97f4a7c15);
  if (!result) {
    result.low = route_id;
  }
  return result;
}

uint32_t route_count(uint32_t interface_id, const dapp_service_config& config) noexcept
{
  if (interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1) {
    uint64_t count = 0;
    for (const auto& cell : config.cells) {
      count += cell.nof_uplink_producer_shards;
    }
    return count <= UINT32_MAX ? static_cast<uint32_t>(count) : 0;
  }
  return interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1 ? config.nof_srs_lanes : 0;
}

uint32_t total_route_count(uint32_t interface_id, uint64_t capabilities, const dapp_service_config& config) noexcept
{
  const uint32_t producers = route_count(interface_id, config);
  if (interface_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 ||
      (capabilities & OCUDU_DAPP_SPECTRUM_CAP_FUTURE_UL_PLAN_V1) == 0) {
    return producers;
  }
  const uint64_t total = static_cast<uint64_t>(producers) + config.cells.size();
  return total <= UINT32_MAX ? static_cast<uint32_t>(total) : 0;
}

uint64_t allowed_interface_capabilities(uint32_t interface_id) noexcept
{
  return interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1
             ? OCUDU_DAPP_SPECTRUM_CAP_INTERFERENCE_MAP_V1 | OCUDU_DAPP_SPECTRUM_CAP_QUIET_PERIOD_INTENT_V1 |
                   OCUDU_DAPP_SPECTRUM_CAP_FUTURE_UL_PLAN_V1
             : 0;
}

uint32_t result_capabilities(const interface_record& interface) noexcept
{
  if (interface.interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) {
    return static_cast<uint32_t>(class_c_process_result_capability::srs_preview);
  }
  if (interface.interface_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1) {
    return 0;
  }
  uint32_t result = 0;
  if ((interface.capability_bits & OCUDU_DAPP_SPECTRUM_CAP_INTERFERENCE_MAP_V1) != 0) {
    result |= static_cast<uint32_t>(class_c_process_result_capability::spectrum_map);
  }
  if ((interface.capability_bits & OCUDU_DAPP_SPECTRUM_CAP_QUIET_PERIOD_INTENT_V1) != 0) {
    result |= static_cast<uint32_t>(class_c_process_result_capability::quiet_intent);
  }
  return result;
}

std::string interface_incompatibility(const interface_record&    interface,
                                      backend                    selected_backend,
                                      const dapp_service_config& config)
{
  if (interface.interface_major != 1 || interface.interface_minor != 0 ||
      interface.dapp_class != OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1 ||
      (interface.placement_mask & OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1) == 0 ||
      interface.threading != OCUDU_DAPP_THREADING_PER_WORKER_V1 ||
      (interface.capability_bits & ~allowed_interface_capabilities(interface.interface_id)) != 0) {
    return "process interface contract is not supported by the v1 host";
  }
  if (selected_backend == backend::cuda && interface.interface_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 &&
      interface.interface_id != OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) {
    return "v1 CUDA process export supports only spectrum and SRS--ISAC";
  }
  const uint32_t count = total_route_count(interface.interface_id, interface.capability_bits, config);
  if (count == 0 || count > OCUDU_DAPP_MAX_PRODUCER_SHARDS_V1) {
    return interface.interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 ||
                   interface.interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1
               ? "process producer shard count exceeds the v1 route-set bound"
               : "process interface has no host route factory";
  }
  return {};
}

} // namespace

struct dapp_process_package_factory::implementation {
  identifier                                   id;
  backend                                      selected_backend{backend::cpu};
  dapp_class_c_process_config                  config;
  std::vector<interface_plan>                  interfaces;
  std::vector<route_plan>                      plans;
  std::shared_ptr<class_c_process_result_sink> result_sink;
};

std::string ocudu::app_services::process_package_incompatibility(const package_record&      package,
                                                                 const dapp_service_config& config)
{
  if (package.placement_mask != OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1) {
    return "process artifact must have an out-of-process-only package placement";
  }
  if (package.interfaces.empty() || package.interfaces.size() > OCUDU_DAPP_PROCESS_MAX_INTERFACES_V1) {
    return "v1 process host requires a bounded Class-C interface vector";
  }
  if (package.backends.size() != 1 ||
      (config.backend_mask & (uint64_t{1} << static_cast<uint32_t>(package.backends.front()))) == 0) {
    return "process artifact backend is unavailable in the host configuration";
  }
  if (package.backends.front() != backend::cpu && package.backends.front() != backend::cuda) {
    return "process artifact backend is not supported by the v1 host";
  }
  uint64_t total_routes = 0;
  bool     resolved     = false;
  for (const auto& interface : package.interfaces) {
    const auto incompatible = interface_incompatibility(interface, package.backends.front(), config);
    if (!incompatible.empty()) {
      if (interface.requirement == binding_requirement::required) {
        return incompatible;
      }
      continue;
    }
    resolved = true;
    total_routes += total_route_count(interface.interface_id, interface.capability_bits, config);
  }
  if (!resolved) {
    return "process package resolves no callable Class-C interface";
  }
  if (total_routes > OCUDU_DAPP_MAX_PRODUCER_SHARDS_V1) {
    return "process multi-interface route bundle exceeds the v1 bound";
  }
  return {};
}

dapp_process_package_factory_create_result ocudu::app_services::create_dapp_process_package_factory(
    const package_record&                        package,
    const dapp_service_config&                   config,
    std::shared_ptr<class_c_process_result_sink> result_sink) noexcept
{
  try {
    auto incompatibility = process_package_incompatibility(package, config);
    if (!incompatibility.empty()) {
      return {{}, {}, std::move(incompatibility)};
    }
    auto state              = std::make_unique<dapp_process_package_factory::implementation>();
    state->id               = package.package_id;
    state->selected_backend = package.backends.front();
    state->config           = config.class_c_process;

    std::vector<upper_phy_dapp_route_binding> routes;
    state->interfaces.reserve(package.interfaces.size());
    for (const auto& interface : package.interfaces) {
      interface_plan planned{interface, state->plans.size(), 0};
      if (!interface_incompatibility(interface, state->selected_backend, config).empty()) {
        state->interfaces.push_back(std::move(planned));
        continue;
      }
      const uint32_t outputs = result_capabilities(interface);
      const auto append = [&](upper_phy_dapp_producer_context producer, bool pooled_srs, uint32_t flags, bool routed) {
        auto proxy = std::make_shared<process_route_proxy>(interface.interface_id);
        state->plans.push_back({proxy, interface.interface_id, producer.cell_id, flags, outputs});
        if (routed) {
          routes.push_back({producer, std::move(proxy), pooled_srs});
        }
      };
      if (interface.interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1) {
        for (const auto& cell : config.cells) {
          for (uint32_t shard = 0; shard != cell.nof_uplink_producer_shards; ++shard) {
            append({cell.cell_id, shard, 0}, false, 0, true);
          }
        }
        if ((interface.capability_bits & OCUDU_DAPP_SPECTRUM_CAP_FUTURE_UL_PLAN_V1) != 0) {
          for (const auto& cell : config.cells) {
            append({cell.cell_id, 0, 0}, false, OCUDU_DAPP_ROUTE_FLAG_UL_RESOURCE_PLAN_V1, false);
          }
        }
      } else {
        for (uint32_t lane = 0; lane != config.nof_srs_lanes; ++lane) {
          append({0, 0, lane}, true, 0, true);
        }
      }
      planned.route_count = state->plans.size() - planned.first_route;
      state->interfaces.push_back(std::move(planned));
      if (outputs != 0) {
        state->result_sink = result_sink;
      }
    }
    auto factory = std::shared_ptr<dapp_process_package_factory>(new dapp_process_package_factory(std::move(state)));
    return {std::move(factory), std::move(routes), {}};
  } catch (const std::bad_alloc&) {
    return {{}, {}, "unable to allocate Class-C process route proxies"};
  } catch (...) {
    return {{}, {}, "unknown Class-C process factory construction failure"};
  }
}

dapp_process_package_factory::dapp_process_package_factory(std::unique_ptr<implementation> state_) noexcept :
  state(std::move(state_))
{
}

dapp_process_package_factory::~dapp_process_package_factory() = default;

const identifier& dapp_process_package_factory::package_id() const noexcept
{
  return state->id;
}

void dapp_process_package_factory::publish(const ocudu_dapp_ul_resource_plan_v1& plan) noexcept
{
  for (const auto& route : state->plans) {
    if (route.interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 &&
        route.flags == OCUDU_DAPP_ROUTE_FLAG_UL_RESOURCE_PLAN_V1 && route.cell_id == plan.context.cell_id) {
      route.proxy->publish(plan);
      return;
    }
  }
}

process_runtime_create_result
dapp_process_package_factory::create_runtime(std::unique_ptr<approved_artifact> executable,
                                             backend                            selected_backend) noexcept
{
  if (selected_backend != state->selected_backend || executable == nullptr) {
    return {{OCUDU_DAPP_UNSUPPORTED_V1, OCUDU_DAPP_STATE_ERROR_V1, "process package backend selection mismatch"}, {}};
  }
  try {
    class_c_process_supervisor_config supervisor;
    supervisor.executable                  = std::move(executable);
    supervisor.startup_timeout             = state->config.startup_timeout;
    supervisor.stop_timeout                = state->config.stop_timeout;
    supervisor.restart_backoff             = state->config.restart_backoff;
    supervisor.heartbeat_period            = state->config.heartbeat_period;
    supervisor.heartbeat_timeout           = state->config.heartbeat_timeout;
    supervisor.management_timeout          = state->config.management_timeout;
    supervisor.maximum_address_space_bytes = state->config.maximum_address_space_bytes;
    supervisor.maximum_core_file_bytes     = state->config.maximum_core_file_bytes;
    supervisor.require_seccomp_filter      = state->config.require_seccomp_filter;
    supervisor.maximum_open_files          = state->config.maximum_open_files;
    supervisor.maximum_restart_attempts    = state->config.maximum_restart_attempts;
    supervisor.environment                 = {"PATH=/usr/bin:/bin", "LANG=C"};
    if (selected_backend == backend::cuda && supervisor.maximum_core_file_bytes != 0) {
      append_cuda_core_environment(supervisor.environment);
    }
    supervisor.routes.reserve(state->plans.size());
    supervisor.interfaces.reserve(state->interfaces.size());
    std::vector<binding_state> bindings;
    bindings.reserve(state->interfaces.size());
    bool any_bound   = false;
    bool any_outputs = false;
    for (const auto& interface : state->interfaces) {
      if (interface.route_count == 0) {
        if (interface.contract.requirement != binding_requirement::optional) {
          return {{OCUDU_DAPP_UNSUPPORTED_V1,
                   OCUDU_DAPP_STATE_ERROR_V1,
                   "required process interface has no host route factory"},
                  {}};
        }
        supervisor.interfaces.push_back({interface.contract.interface_id,
                                         0,
                                         OCUDU_DAPP_PROCESS_BINDING_OPTIONAL_V1,
                                         OCUDU_DAPP_PROCESS_BINDING_UNAVAILABLE_V1});
        bindings.push_back(binding_state::unavailable);
        continue;
      }

      std::vector<class_c_process_route_binding> prepared;
      prepared.reserve(interface.route_count);
      bool binding_conflict = false;
      for (std::size_t local = 0; local != interface.route_count; ++local) {
        const std::size_t                    index = interface.first_route + local;
        const auto&                          plan  = state->plans[index];
        upper_phy_dapp_process_target_config target_config;
        target_config.interface_id              = plan.interface_id;
        target_config.backend_id                = state->selected_backend == backend::cuda && plan.flags == 0
                                                      ? OCUDU_DAPP_BACKEND_CUDA_V1
                                                      : OCUDU_DAPP_BACKEND_HOST_V1;
        target_config.route_flags               = plan.flags;
        target_config.slot_count                = state->config.ring_slots;
        target_config.payload_capacity          = state->config.payload_capacity_bytes;
        target_config.encoder.producer_instance = producer_identity(state->id, index + 1);
        auto target                             = create_upper_phy_dapp_process_target(std::move(target_config));
        if (!target) {
          return {{target.status, OCUDU_DAPP_STATE_ERROR_V1, std::move(target.detail)}, {}};
        }

        auto binding    = std::make_shared<process_route_proxy::binding_state>();
        binding->target = target.target;
        auto endpoint   = std::make_shared<leased_process_endpoint>(target.target, plan.proxy, binding);
        if (!plan.proxy->bind(binding)) {
          binding_conflict = true;
          break;
        }

        ocudu_dapp_route_offer_v1 offer{};
        offer.struct_size             = sizeof(offer);
        offer.interface_id            = plan.interface_id;
        offer.metadata_schema_id      = OCUDU_DAPP_CLASS_C_PUBLICATION_SCHEMA_V1;
        offer.metadata_schema_version = 1;
        offer.route_id                = index + 1;
        offer.flags                   = plan.flags;
        offer.producer_shard_id       = static_cast<uint16_t>(local);
        offer.producer_shard_count    = static_cast<uint16_t>(interface.route_count);
        if (target_config.backend_id == OCUDU_DAPP_BACKEND_CUDA_V1) {
          offer.transport_kind       = OCUDU_DAPP_ROUTE_TRANSPORT_CUDA_IPC_EXPORT_V1;
          offer.backend_id           = OCUDU_DAPP_BACKEND_CUDA_V1;
          offer.export_slot_count    = state->config.ring_slots;
          offer.export_slot_capacity = state->config.payload_capacity_bytes;
        }
        prepared.push_back({offer, std::move(endpoint), plan.result_capabilities});
      }
      if (binding_conflict) {
        prepared.clear();
        if (interface.contract.requirement == binding_requirement::required) {
          return {{OCUDU_DAPP_CONFLICT_V1,
                   OCUDU_DAPP_STATE_ERROR_V1,
                   "required process interface already has a resident instance"},
                  {}};
        }
        supervisor.interfaces.push_back({interface.contract.interface_id,
                                         0,
                                         OCUDU_DAPP_PROCESS_BINDING_OPTIONAL_V1,
                                         OCUDU_DAPP_PROCESS_BINDING_UNAVAILABLE_V1});
        bindings.push_back(binding_state::unavailable);
        continue;
      }

      supervisor.interfaces.push_back(
          {interface.contract.interface_id,
           static_cast<uint16_t>(interface.route_count),
           static_cast<uint8_t>(interface.contract.requirement == binding_requirement::optional
                                    ? OCUDU_DAPP_PROCESS_BINDING_OPTIONAL_V1
                                    : OCUDU_DAPP_PROCESS_BINDING_REQUIRED_V1),
           static_cast<uint8_t>(OCUDU_DAPP_PROCESS_BINDING_BOUND_V1)});
      bindings.push_back(binding_state::bound);
      any_bound = true;
      any_outputs |= std::any_of(
          prepared.begin(), prepared.end(), [](const auto& route) { return route.result_capabilities != 0; });
      supervisor.routes.insert(
          supervisor.routes.end(), std::make_move_iterator(prepared.begin()), std::make_move_iterator(prepared.end()));
    }
    if (!any_bound) {
      return {{OCUDU_DAPP_UNSUPPORTED_V1, OCUDU_DAPP_STATE_ERROR_V1, "process package resolved no callable interface"},
              {}};
    }
    supervisor.result_sink        = any_outputs ? state->result_sink : nullptr;
    supervisor.results_prohibited = !any_outputs;

    auto supervised = class_c_process_supervisor::create(std::move(supervisor));
    if (!supervised) {
      return {{supervised.operation.status, OCUDU_DAPP_STATE_ERROR_V1, std::move(supervised.operation.detail)}, {}};
    }
    auto                          instance = class_c_process_instance::create(std::move(supervised.supervisor));
    process_runtime_create_result result{std::move(instance.operation), {}};
    result.runtime  = std::move(instance.instance);
    result.bindings = std::move(bindings);
    return result;
  } catch (const std::bad_alloc&) {
    return {{OCUDU_DAPP_RESOURCE_V1, OCUDU_DAPP_STATE_ERROR_V1, "unable to allocate Class-C process routes"}, {}};
  } catch (...) {
    return {{OCUDU_DAPP_EXCEPTION_V1, OCUDU_DAPP_STATE_ERROR_V1, "Class-C process factory threw"}, {}};
  }
}
