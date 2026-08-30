// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Package admission catalog — factored out of instance_manager so registration
// and lookup are not mixed with lifecycle/control paths.

#pragma once

#include "artifact_verifier.h"
#include "native_instance.h"
#include "ocudu/dapp/management/types.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace ocudu::dapp {

struct process_runtime_create_result {
  instance_operation_result              operation;
  std::unique_ptr<instance_runtime>      runtime;
  std::vector<management::binding_state> bindings;
  explicit                               operator bool() const noexcept { return operation && runtime != nullptr; }
};

using process_runtime_factory =
    std::function<process_runtime_create_result(std::unique_ptr<approved_artifact>, management::backend)>;

/// Thread-unsafe package registry. Callers (instance_manager) hold the mutex.
class package_catalog
{
public:
  enum class status {
    ok,
    ok_already,
    conflict,
    invalid_request,
    not_found,
    verification_failed,
  };

  struct outcome {
    status      code{status::invalid_request};
    std::string detail;
  };

  explicit package_catalog(std::string catalog_root);

  outcome register_package(const management::package_record& package);
  /// Replace an already-admitted package's artifact digest/path. Identity
  /// (global_name, interfaces, placement, backends) must stay the same.
  /// Callers must ensure no resident instance of this package_id remains.
  outcome replace_package(const management::package_record& package);
  outcome register_process_factory(const management::identifier& package_id, process_runtime_factory factory);

  std::optional<management::package_record> find_package(const management::identifier& id) const;
  bool                                      has_process_factory(const management::identifier& id) const;
  const process_runtime_factory*            process_factory(const management::identifier& id) const;

  uint64_t inventory_generation() const noexcept { return inventory_generation_; }
  /// Shared inventory generation (packages + instances); instance_manager bumps on lifecycle.
  void bump_inventory_generation() noexcept { ++inventory_generation_; }

  const std::unordered_map<management::identifier, management::package_record, management::identifier_hash>&
  packages() const noexcept
  {
    return packages_;
  }

  artifact_verifier& verifier() noexcept { return verifier_; }

private:
  artifact_verifier                                                                                   verifier_;
  uint64_t                                                                                            inventory_generation_{1};
  std::unordered_map<management::identifier, management::package_record, management::identifier_hash> packages_;
  std::unordered_map<management::identifier, process_runtime_factory, management::identifier_hash>    process_factories_;
};

} // namespace ocudu::dapp
