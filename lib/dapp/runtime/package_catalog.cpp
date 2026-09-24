// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/package_catalog.h"
#include "ocudu/dapp/abi/v1/module.h"
#include <algorithm>

namespace ocudu::dapp {
namespace {

using management::identifier;
using management::interface_record;
using management::package_record;
using management::binding_requirement;

bool valid_process_interfaces(const std::vector<interface_record>& interfaces) noexcept
{
  if (interfaces.empty() || interfaces.size() > 64) {
    return false;
  }
  for (std::size_t index = 0; index != interfaces.size(); ++index) {
    const auto& interface = interfaces[index];
    if (interface.interface_id == 0 || interface.dapp_class != OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1 ||
        (interface.placement_mask & OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1) == 0 ||
        (interface.placement_mask & ~(OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 | OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1)) !=
            0 ||
        (interface.requirement != binding_requirement::required &&
         interface.requirement != binding_requirement::optional) ||
        interface.threading < OCUDU_DAPP_THREADING_PER_WORKER_V1 ||
        interface.threading > OCUDU_DAPP_THREADING_SERIALIZED_V1) {
      return false;
    }
    for (std::size_t previous = 0; previous != index; ++previous) {
      if (interfaces[previous].interface_id == interface.interface_id) {
        return false;
      }
    }
  }
  return true;
}

bool same_interfaces(const std::vector<interface_record>& left, const std::vector<interface_record>& right) noexcept
{
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index != left.size(); ++index) {
    if (left[index].interface_id != right[index].interface_id ||
        left[index].interface_major != right[index].interface_major ||
        left[index].interface_minor != right[index].interface_minor ||
        left[index].dapp_class != right[index].dapp_class ||
        left[index].placement_mask != right[index].placement_mask ||
        left[index].requirement != right[index].requirement || left[index].threading != right[index].threading ||
        left[index].capability_bits != right[index].capability_bits || left[index].pusch != right[index].pusch) {
      return false;
    }
  }
  return true;
}

} // namespace

package_catalog::package_catalog(std::string catalog_root) : verifier_(std::move(catalog_root)) {}

package_catalog::outcome package_catalog::register_package(const package_record& package_record)
{
  if (package_record.package_id.is_zero() || package_record.global_name.empty() ||
      package_record.global_name.size() > 128 || package_record.vendor.empty() || package_record.vendor.size() > 128 ||
      package_record.version.empty() || package_record.version.size() > 32 ||
      package_record.canonical_artifact_path.empty() || package_record.backends.empty() ||
      (package_record.placement_mask &
       ~(OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 | OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1)) != 0 ||
      package_record.placement_mask == 0 ||
      ((package_record.placement_mask & OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1) != 0 &&
       !valid_process_interfaces(package_record.interfaces))) {
    return {status::invalid_request, "invalid or incomplete package manifest"};
  }
  if (auto found = packages_.find(package_record.package_id); found != packages_.end()) {
    if (found->second.content_digest == package_record.content_digest &&
        found->second.global_name == package_record.global_name && found->second.vendor == package_record.vendor &&
        found->second.version == package_record.version &&
        found->second.placement_mask == package_record.placement_mask &&
        same_interfaces(found->second.interfaces, package_record.interfaces) &&
        found->second.backends == package_record.backends) {
      return {status::ok_already, "package already registered"};
    }
    return {status::conflict, "package identifier already names different content"};
  }

  auto verified = verifier_.verify(package_record.canonical_artifact_path, package_record.content_digest);
  if (!verified) {
    return {status::verification_failed, verified.detail};
  }
  auto canonical                    = package_record;
  canonical.canonical_artifact_path = verified.artifact->canonical_path();
  packages_.emplace(canonical.package_id, std::move(canonical));
  ++inventory_generation_;
  return {status::ok, {}};
}

package_catalog::outcome package_catalog::replace_package(const package_record& package_record)
{
  if (package_record.package_id.is_zero() || package_record.global_name.empty() ||
      package_record.global_name.size() > 128 || package_record.vendor.empty() || package_record.vendor.size() > 128 ||
      package_record.version.empty() || package_record.version.size() > 32 ||
      package_record.canonical_artifact_path.empty() || package_record.backends.empty() ||
      (package_record.placement_mask &
       ~(OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 | OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1)) != 0 ||
      package_record.placement_mask == 0 ||
      ((package_record.placement_mask & OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1) != 0 &&
       !valid_process_interfaces(package_record.interfaces))) {
    return {status::invalid_request, "invalid or incomplete package manifest"};
  }
  auto found = packages_.find(package_record.package_id);
  if (found == packages_.end()) {
    return register_package(package_record);
  }
  const bool same_identity = found->second.global_name == package_record.global_name &&
                             found->second.vendor == package_record.vendor &&
                             found->second.placement_mask == package_record.placement_mask &&
                             same_interfaces(found->second.interfaces, package_record.interfaces) &&
                             found->second.backends == package_record.backends;
  if (!same_identity) {
    return {status::conflict, "package identifier already names different content"};
  }
  // Even idempotent admission must verify current disk bytes. Otherwise a
  // valid manifest with a now-tampered artifact could pass a catalog dry-run.
  auto verified = verifier_.verify(package_record.canonical_artifact_path, package_record.content_digest);
  if (!verified) {
    return {status::verification_failed, verified.detail};
  }
  if (found->second.content_digest == package_record.content_digest &&
      found->second.canonical_artifact_path == package_record.canonical_artifact_path &&
      found->second.version == package_record.version &&
      found->second.signature_sequence == package_record.signature_sequence &&
      found->second.signing_key_id == package_record.signing_key_id &&
      found->second.trust_policy_epoch == package_record.trust_policy_epoch &&
      found->second.signature_status == package_record.signature_status &&
      found->second.manifest_schema_version == package_record.manifest_schema_version &&
      found->second.canonical_sbom_path == package_record.canonical_sbom_path &&
      found->second.sbom_verified == package_record.sbom_verified &&
      found->second.sbom_digest == package_record.sbom_digest) {
    return {status::ok_already, "package already registered"};
  }
  auto canonical                    = package_record;
  canonical.canonical_artifact_path = verified.artifact->canonical_path();
  found->second                     = std::move(canonical);
  ++inventory_generation_;
  return {status::ok, {}};
}

package_catalog::outcome package_catalog::register_process_factory(const management::identifier& package_id,
                                                                   process_runtime_factory       factory)
{
  auto package = packages_.find(package_id);
  if (package == packages_.end()) {
    return {status::not_found, "process package is not registered"};
  }
  if (!factory || (package->second.placement_mask & OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1) == 0 ||
      !valid_process_interfaces(package->second.interfaces)) {
    return {status::invalid_request, "invalid Class-C process factory"};
  }
  if (process_factories_.find(package_id) != process_factories_.end()) {
    return {status::conflict, "process factory is already registered"};
  }
  process_factories_.emplace(package_id, std::move(factory));
  ++inventory_generation_;
  return {status::ok, {}};
}

std::optional<package_record> package_catalog::find_package(const identifier& id) const
{
  auto found = packages_.find(id);
  return found != packages_.end() ? std::optional<package_record>(found->second) : std::nullopt;
}

bool package_catalog::has_process_factory(const identifier& id) const
{
  return process_factories_.find(id) != process_factories_.end();
}

const process_runtime_factory* package_catalog::process_factory(const identifier& id) const
{
  auto found = process_factories_.find(id);
  return found != process_factories_.end() ? &found->second : nullptr;
}

} // namespace ocudu::dapp
