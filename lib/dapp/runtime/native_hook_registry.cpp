// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/native_hook_registry.h"
#include "ocudu/dapp/runtime/native_instance.h"
#include <algorithm>
#include <stdexcept>

using namespace ocudu::dapp;

namespace {

constexpr uint32_t in_process_mask = 1U << (OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 - 1);

native_hook_registry_result result(ocudu_dapp_status_v1 status, const char* detail = nullptr)
{
  return {status, detail != nullptr ? detail : ""};
}

} // namespace

native_hook_registry::native_hook_registry(std::size_t maximum_hook_points, std::size_t maximum_active_bindings) :
  maximum_points(maximum_hook_points), maximum_bindings(maximum_active_bindings)
{
  if (maximum_points == 0 || maximum_bindings == 0) {
    throw std::invalid_argument("native hook registry bounds must be nonzero");
  }
}

native_hook_registry_result native_hook_registry::register_point(std::shared_ptr<native_hook_point> point) noexcept
{
  if (point == nullptr || point->interface_id() == 0 || point->bound()) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "native hook point is null, invalid, or already bound");
  }
  try {
    std::lock_guard<std::mutex> lock(mutex);
    if (points.size() >= maximum_points) {
      return result(OCUDU_DAPP_RESOURCE_V1, "native hook point limit reached");
    }
    if (!points.emplace(point->interface_id(), std::move(point)).second) {
      return result(OCUDU_DAPP_CONFLICT_V1, "native hook interface is already registered");
    }
    return result(OCUDU_DAPP_OK_V1);
  } catch (const std::bad_alloc&) {
    return result(OCUDU_DAPP_RESOURCE_V1, "unable to register native hook point");
  } catch (...) {
    return result(OCUDU_DAPP_EXCEPTION_V1, "native hook point registration threw");
  }
}

native_hook_registry_result
native_hook_registry::set_binding_observer(std::shared_ptr<native_hook_binding_observer> observer) noexcept
{
  try {
    std::lock_guard<std::mutex> lock(mutex);
    if (!bindings.empty()) {
      return result(OCUDU_DAPP_CONFLICT_V1, "cannot change the native hook observer while bindings exist");
    }
    binding_observer = std::move(observer);
    return result(OCUDU_DAPP_OK_V1);
  } catch (...) {
    return result(OCUDU_DAPP_EXCEPTION_V1, "native hook observer registration threw");
  }
}

native_hook_registry_result native_hook_registry::resolve(const std::vector<management::interface_record>& interfaces,
                                                          std::vector<std::shared_ptr<native_hook_point>>& resolved,
                                                          std::vector<management::interface_record>& bound_interfaces,
                                                          std::vector<uint32_t>& unavailable_optional_interfaces) const
{
  if (interfaces.empty() || interfaces.size() > 64) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "native module interface set is empty or exceeds its bound");
  }
  resolved.clear();
  bound_interfaces.clear();
  unavailable_optional_interfaces.clear();
  resolved.reserve(interfaces.size());
  bound_interfaces.reserve(interfaces.size());
  unavailable_optional_interfaces.reserve(interfaces.size());
  for (std::size_t index = 0; index != interfaces.size(); ++index) {
    const auto& interface = interfaces[index];
    if (interface.interface_id == 0 || (interface.requirement != management::binding_requirement::required &&
                                        interface.requirement != management::binding_requirement::optional)) {
      return result(OCUDU_DAPP_INVALID_INPUT_V1, "native interface binding policy is invalid");
    }
    for (std::size_t prior = 0; prior != index; ++prior) {
      if (interfaces[prior].interface_id == interface.interface_id) {
        return result(OCUDU_DAPP_INVALID_INPUT_V1, "native module repeats an interface hook");
      }
    }
    const bool optional = interface.requirement == management::binding_requirement::optional;
    if (interface.interface_major != 1 || (interface.placement_mask & in_process_mask) == 0) {
      if (optional) {
        unavailable_optional_interfaces.push_back(interface.interface_id);
        continue;
      }
      return result(OCUDU_DAPP_UNSUPPORTED_V1, "required native interface version or placement is not supported");
    }
    auto point = points.find(interface.interface_id);
    if (point == points.end()) {
      if (optional) {
        unavailable_optional_interfaces.push_back(interface.interface_id);
        continue;
      }
      return result(OCUDU_DAPP_UNSUPPORTED_V1, "native module requires an unavailable hook point");
    }
    if (point->second->bound()) {
      if (optional) {
        unavailable_optional_interfaces.push_back(interface.interface_id);
        continue;
      }
      return result(OCUDU_DAPP_CONFLICT_V1, "required native hook point is already owned by another instance");
    }
    resolved.push_back(point->second);
    bound_interfaces.push_back(interface);
  }
  if (resolved.empty()) {
    // Control-plane packages (e.g. channel-emul 0x00010008) advertise only
    // optional interfaces the RU/PHY never registers. Activate still runs
    // module start/stop + E3 config; apply stays in the RU hook.
    if (unavailable_optional_interfaces.size() == interfaces.size()) {
      return result(OCUDU_DAPP_OK_V1);
    }
    return result(OCUDU_DAPP_UNSUPPORTED_V1, "native module has no available interface hook");
  }
  return result(OCUDU_DAPP_OK_V1);
}

native_hook_registry_result
native_hook_registry::validate(const std::vector<management::interface_record>& interfaces) const noexcept
{
  try {
    std::lock_guard<std::mutex>                     lock(mutex);
    std::vector<std::shared_ptr<native_hook_point>> resolved;
    std::vector<management::interface_record>       bound_interfaces;
    std::vector<uint32_t>                           unavailable_optional_interfaces;
    return resolve(interfaces, resolved, bound_interfaces, unavailable_optional_interfaces);
  } catch (const std::bad_alloc&) {
    return result(OCUDU_DAPP_RESOURCE_V1, "unable to validate native hook set");
  } catch (...) {
    return result(OCUDU_DAPP_EXCEPTION_V1, "native hook validation threw");
  }
}

native_hook_registry_result native_hook_registry::stage(const management::identifier&                    instance_id,
                                                        native_instance&                                 instance,
                                                        const std::vector<management::interface_record>& interfaces,
                                                        std::shared_ptr<native_activation_gate>          gate) noexcept
{
  if (instance_id.is_zero() || gate == nullptr || gate->enabled.load(std::memory_order_acquire)) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "native hook stage requires an instance and disabled unique gate");
  }
  try {
    std::lock_guard<std::mutex> lock(mutex);
    if (bindings.size() >= maximum_bindings) {
      return result(OCUDU_DAPP_RESOURCE_V1, "native hook binding limit reached");
    }
    if (bindings.find(instance_id) != bindings.end()) {
      return result(OCUDU_DAPP_CONFLICT_V1, "native instance already has a hook binding");
    }
    std::vector<std::shared_ptr<native_hook_point>> resolved;
    std::vector<management::interface_record>       bound_interfaces;
    std::vector<uint32_t>                           unavailable_optional_interfaces;
    auto validated = resolve(interfaces, resolved, bound_interfaces, unavailable_optional_interfaces);
    if (!validated) {
      return validated;
    }

    binding_record binding;
    binding.gate = std::move(gate);
    binding.points.reserve(resolved.size());
    binding.interfaces                      = std::move(bound_interfaces);
    binding.unavailable_optional_interfaces = std::move(unavailable_optional_interfaces);
    auto [position, inserted]               = bindings.emplace(instance_id, std::move(binding));
    if (!inserted) {
      return result(OCUDU_DAPP_CONFLICT_V1, "native instance already has a hook binding");
    }

    for (const auto& point : resolved) {
      position->second.points.push_back(point);
      const auto status = point->stage(instance, position->second.gate);
      if (status != OCUDU_DAPP_OK_V1) {
        position->second.gate->enabled.store(false, std::memory_order_release);
        const auto rollback_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        bool       rollback_complete = true;
        for (const auto& staged : position->second.points) {
          rollback_complete &= staged->clear(position->second.gate, rollback_deadline) == OCUDU_DAPP_OK_V1;
        }
        if (rollback_complete) {
          bindings.erase(position);
        }
        const char* detail = status == OCUDU_DAPP_UNSUPPORTED_V1
                                 ? "package backend is not valid for this hook's L1 data path "
                                   "(host vs accelerated Class-A, or host-record Class-B)"
                                 : "native hook worker staging failed";
        return result(status, detail);
      }
    }
    return result(OCUDU_DAPP_OK_V1);
  } catch (const std::bad_alloc&) {
    static_cast<void>(deactivate(instance_id, std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    return result(OCUDU_DAPP_RESOURCE_V1, "unable to allocate native hook binding");
  } catch (...) {
    static_cast<void>(deactivate(instance_id, std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    return result(OCUDU_DAPP_EXCEPTION_V1, "native hook staging threw");
  }
}

native_hook_registry_result native_hook_registry::commit(const management::identifier& instance_id) noexcept
{
  try {
    std::lock_guard<std::mutex> lock(mutex);
    auto                        binding = bindings.find(instance_id);
    if (binding == bindings.end() || binding->second.committed) {
      return result(OCUDU_DAPP_CONFLICT_V1, "native hook binding is absent or already committed");
    }
    if (binding_observer != nullptr) {
      const auto status = binding_observer->activated(instance_id, binding->second.interfaces);
      if (status != OCUDU_DAPP_OK_V1) {
        return result(status, "native hook activation observer rejected the binding");
      }
    }
    binding->second.committed = true;
    binding->second.gate->enabled.store(true, std::memory_order_release);
    return result(OCUDU_DAPP_OK_V1);
  } catch (...) {
    return result(OCUDU_DAPP_EXCEPTION_V1, "native hook commit threw");
  }
}

native_hook_registry_result native_hook_registry::deactivate(const management::identifier&         instance_id,
                                                             std::chrono::steady_clock::time_point deadline) noexcept
{
  try {
    std::lock_guard<std::mutex> lock(mutex);
    auto                        binding = bindings.find(instance_id);
    if (binding == bindings.end()) {
      return result(OCUDU_DAPP_OK_V1);
    }
    binding->second.gate->enabled.store(false, std::memory_order_release);
    ocudu_dapp_status_v1 status = OCUDU_DAPP_OK_V1;
    for (const auto& point : binding->second.points) {
      const auto point_status = point->clear(binding->second.gate, deadline);
      if (point_status != OCUDU_DAPP_OK_V1) {
        status = point_status;
      }
    }
    if (status == OCUDU_DAPP_OK_V1) {
      if (binding->second.committed && binding_observer != nullptr) {
        status = binding_observer->deactivated(instance_id, binding->second.interfaces);
        if (status != OCUDU_DAPP_OK_V1) {
          return result(status, "native hook deactivation observer rejected the binding");
        }
      }
      bindings.erase(binding);
      return result(OCUDU_DAPP_OK_V1);
    }
    return result(status, "native hook readers did not quiesce before the deadline");
  } catch (...) {
    return result(OCUDU_DAPP_EXCEPTION_V1, "native hook deactivation threw");
  }
}

bool native_hook_registry::bound(const management::identifier& instance_id) const noexcept
{
  try {
    std::lock_guard<std::mutex> lock(mutex);
    return bindings.find(instance_id) != bindings.end();
  } catch (...) {
    return true;
  }
}

bool native_hook_registry::binding_states(const management::identifier&              instance_id,
                                          std::vector<management::interface_record>& interfaces) const noexcept
{
  try {
    std::lock_guard<std::mutex> lock(mutex);
    auto                        binding = bindings.find(instance_id);
    if (binding == bindings.end() || !binding->second.committed) {
      return false;
    }
    for (auto& interface : interfaces) {
      interface.state = management::binding_state::unbound;
      if (std::any_of(binding->second.interfaces.begin(), binding->second.interfaces.end(), [&](const auto& bound) {
            return bound.interface_id == interface.interface_id;
          })) {
        interface.state = management::binding_state::bound;
      } else if (std::find(binding->second.unavailable_optional_interfaces.begin(),
                           binding->second.unavailable_optional_interfaces.end(),
                           interface.interface_id) != binding->second.unavailable_optional_interfaces.end()) {
        interface.state = management::binding_state::unavailable;
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

std::shared_ptr<native_hook_point> native_hook_registry::find(uint32_t interface_id) const noexcept
{
  try {
    std::lock_guard<std::mutex> lock(mutex);
    auto                        point = points.find(interface_id);
    return point != points.end() ? point->second : nullptr;
  } catch (...) {
    return {};
  }
}
