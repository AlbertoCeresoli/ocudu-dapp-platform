// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "dapp_service.h"
#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_data_router.h"
#include <memory>
#include <string>
#include <vector>

namespace ocudu::dapp {
class class_c_process_result_sink;
}

namespace ocudu::app_services {

class dapp_process_package_factory;

struct dapp_process_package_factory_create_result {
  std::shared_ptr<dapp_process_package_factory> factory;
  std::vector<upper_phy_dapp_route_binding>     routes;
  std::string                                   detail;
  explicit                                      operator bool() const noexcept { return factory != nullptr; }
};

/// Returns why the current host cannot bind a process package, or an empty string when supported.
std::string process_package_incompatibility(const dapp::management::package_record& package,
                                            const dapp_service_config&              config);

/// Builds stable router proxies. Shared rings and encoders are allocated only when E3 loads the package.
dapp_process_package_factory_create_result
create_dapp_process_package_factory(const dapp::management::package_record&            package,
                                    const dapp_service_config&                         config,
                                    std::shared_ptr<dapp::class_c_process_result_sink> result_sink) noexcept;

class dapp_process_package_factory
{
public:
  ~dapp_process_package_factory();
  dapp_process_package_factory(const dapp_process_package_factory&)            = delete;
  dapp_process_package_factory& operator=(const dapp_process_package_factory&) = delete;

  const dapp::management::identifier& package_id() const noexcept;
  dapp::process_runtime_create_result create_runtime(std::unique_ptr<dapp::approved_artifact> executable,
                                                     dapp::management::backend selected_backend) noexcept;
  void                                publish(const ocudu_dapp_ul_resource_plan_v1& plan) noexcept;

private:
  struct implementation;
  friend dapp_process_package_factory_create_result
  create_dapp_process_package_factory(const dapp::management::package_record&,
                                      const dapp_service_config&,
                                      std::shared_ptr<dapp::class_c_process_result_sink>) noexcept;
  explicit dapp_process_package_factory(std::unique_ptr<implementation> state_) noexcept;

  std::unique_ptr<implementation> state;
};

} // namespace ocudu::app_services
