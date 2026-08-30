// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "native_hook_point.h"
#include "ocudu/dapp/management/types.h"
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ocudu::dapp {

class native_instance;

struct native_hook_registry_result {
  ocudu_dapp_status_v1 status{OCUDU_DAPP_INTERNAL_ERROR_V1};
  std::string          detail;
  explicit             operator bool() const noexcept { return status == OCUDU_DAPP_OK_V1; }
};

/// Non-RT observer for host state that must follow native hook ownership changes.
class native_hook_binding_observer
{
public:
  virtual ~native_hook_binding_observer() = default;

  virtual ocudu_dapp_status_v1 activated(const management::identifier&                    instance_id,
                                         const std::vector<management::interface_record>& interfaces) noexcept   = 0;
  virtual ocudu_dapp_status_v1 deactivated(const management::identifier&                    instance_id,
                                           const std::vector<management::interface_record>& interfaces) noexcept = 0;
};

/// Control-plane owner that binds every native interface of one instance as a transaction.
class native_hook_registry
{
public:
  explicit native_hook_registry(std::size_t maximum_hook_points = 64, std::size_t maximum_active_bindings = 1024);

  native_hook_registry_result register_point(std::shared_ptr<native_hook_point> point) noexcept;
  native_hook_registry_result set_binding_observer(std::shared_ptr<native_hook_binding_observer> observer) noexcept;
  native_hook_registry_result validate(const std::vector<management::interface_record>& interfaces) const noexcept;
  native_hook_registry_result stage(const management::identifier&                    instance_id,
                                    native_instance&                                 instance,
                                    const std::vector<management::interface_record>& interfaces,
                                    std::shared_ptr<native_activation_gate>          gate) noexcept;
  native_hook_registry_result commit(const management::identifier& instance_id) noexcept;
  native_hook_registry_result deactivate(const management::identifier&         instance_id,
                                         std::chrono::steady_clock::time_point deadline) noexcept;

  bool bound(const management::identifier& instance_id) const noexcept;
  /// Applies the current host binding state to a manifest-ordered interface inventory.
  bool                               binding_states(const management::identifier&              instance_id,
                                                    std::vector<management::interface_record>& interfaces) const noexcept;
  std::shared_ptr<native_hook_point> find(uint32_t interface_id) const noexcept;

private:
  struct binding_record {
    std::shared_ptr<native_activation_gate>         gate;
    std::vector<std::shared_ptr<native_hook_point>> points;
    std::vector<management::interface_record>       interfaces;
    std::vector<uint32_t>                           unavailable_optional_interfaces;
    bool                                            committed{};
  };

  native_hook_registry_result resolve(const std::vector<management::interface_record>& interfaces,
                                      std::vector<std::shared_ptr<native_hook_point>>& resolved,
                                      std::vector<management::interface_record>&       bound_interfaces,
                                      std::vector<uint32_t>& unavailable_optional_interfaces) const;

  const std::size_t                                                                       maximum_points;
  const std::size_t                                                                       maximum_bindings;
  mutable std::mutex                                                                      mutex;
  std::unordered_map<uint32_t, std::shared_ptr<native_hook_point>>                        points;
  std::unordered_map<management::identifier, binding_record, management::identifier_hash> bindings;
  std::shared_ptr<native_hook_binding_observer>                                           binding_observer;
};

} // namespace ocudu::dapp
