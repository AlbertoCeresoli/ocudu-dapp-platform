// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "dapp_config_validator.h"
#include "dapp_service.h"
#include "ocudu/dapp/process/v1/shared_ring.h"
#include "ocudu/ran/resource_block.h"
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <limits>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace ocudu::app_services;

namespace {

bool validate_local_e3(const dapp_local_e3_config& config)
{
#ifndef OCUDU_DAPP_APP_CONFIG_LOCAL_E3
  fmt::print("Local E3 configuration requires ENABLE_E3 and ENABLE_E3_ASN1_CODEC.\n");
  return false;
#else
  const std::filesystem::path socket_path(config.socket_path);
  std::error_code             error;
  sockaddr_un                 address{};
  if (!socket_path.is_absolute() || config.socket_path.size() >= sizeof(address.sun_path)) {
    fmt::print("Local E3 socket path must be absolute and shorter than {} bytes.\n", sizeof(address.sun_path));
    return false;
  }
  if (!std::filesystem::is_directory(socket_path.parent_path(), error) || error) {
    fmt::print("Local E3 socket parent directory does not exist or is not accessible.\n");
    return false;
  }
  if (config.request_timeout.count() <= 0 || config.request_timeout > std::chrono::seconds(60)) {
    fmt::print("Local E3 request timeout must be in the range 1..60000 ms.\n");
    return false;
  }
  return true;
#endif
}

bool validate_package_signatures(const ocudu::dapp::package_signature_config& config)
{
  if (config.trust_store_path.empty()) {
    if (config.mode != ocudu::dapp::package_signature_mode::allow_unsigned || config.minimum_policy_epoch != 0) {
      fmt::print("Required dApp package signatures need an absolute trust store and nonzero minimum epoch.\n");
      return false;
    }
    return true;
  }
  const std::filesystem::path path(config.trust_store_path);
  struct stat                 status {};
  if (!path.is_absolute() || path.lexically_normal() != path || config.minimum_policy_epoch == 0 ||
      ::lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) || S_ISLNK(status.st_mode) ||
      (status.st_uid != 0 && status.st_uid != ::geteuid()) || (status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
      ::access(path.c_str(), R_OK) != 0) {
    fmt::print(
        "dApp package trust store must be a trusted readable absolute file and minimum epoch must be nonzero.\n");
    return false;
  }
  return true;
}

bool validate_sctp_endpoint(const std::string&              name,
                            const std::string&              bind_address,
                            uint16_t                        port,
                            const std::vector<std::string>& allowed_peers,
                            uint32_t                        maximum_associations,
                            std::chrono::milliseconds       idle_timeout)
{
#ifndef OCUDU_DAPP_APP_CONFIG_SCTP_E3
  fmt::print("{} requires ENABLE_E3 and ENABLE_E3_ASN1_CODEC.\n", name);
  return false;
#else
  in6_addr v6{};
  in_addr  v4{};
  if (::inet_pton(AF_INET, bind_address.c_str(), &v4) != 1 && ::inet_pton(AF_INET6, bind_address.c_str(), &v6) != 1) {
    fmt::print("{} bind_address must be a literal IPv4 or IPv6 address.\n", name);
    return false;
  }
  for (const auto& peer : allowed_peers) {
    if (::inet_pton(AF_INET, peer.c_str(), &v4) != 1 && ::inet_pton(AF_INET6, peer.c_str(), &v6) != 1) {
      fmt::print("{} allowed_peers entries must be literal IPv4 or IPv6 addresses.\n", name);
      return false;
    }
  }
  if (port == 0 || maximum_associations == 0 || maximum_associations > 1024 || idle_timeout < std::chrono::seconds(1) ||
      idle_timeout > std::chrono::hours(1)) {
    fmt::print("{} port, association, or idle-timeout bounds are invalid.\n", name);
    return false;
  }
  return true;
#endif
}

bool validate_sctp_carriers(const dapp_service_config& config)
{
  if (config.e3ap_sctp) {
    const auto& e3ap = *config.e3ap_sctp;
    if (!validate_sctp_endpoint("E3AP SCTP",
                                e3ap.bind_address,
                                e3ap.port,
                                e3ap.allowed_peers,
                                e3ap.maximum_associations,
                                e3ap.idle_timeout)) {
      return false;
    }
    if (e3ap.setup_timeout < std::chrono::milliseconds(100) || e3ap.setup_timeout > std::chrono::minutes(10) ||
        e3ap.maximum_subscriptions_per_dapp == 0 || e3ap.maximum_subscriptions_per_dapp > 1024 ||
        e3ap.ran_identifier.empty() || e3ap.ran_identifier.size() > 64) {
      fmt::print("E3AP SCTP setup timeout, subscription bound, or RAN identifier is invalid.\n");
      return false;
    }
    if (!config.portable_e3_data) {
      fmt::print("E3AP SCTP requires the portable_e3_data store.\n");
      return false;
    }
  }
  if (config.e3dp_sctp) {
#ifndef OCUDU_DAPP_APP_CONFIG_E3DP
    fmt::print("The E3 data-profile SCTP endpoint requires FlatBuffers support (flatc) at build time.\n");
    return false;
#else
    const auto& e3dp = *config.e3dp_sctp;
    if (!validate_sctp_endpoint("E3 data-profile SCTP",
                                e3dp.bind_address,
                                e3dp.port,
                                e3dp.allowed_peers,
                                e3dp.maximum_associations,
                                e3dp.idle_timeout)) {
      return false;
    }
    if (!config.portable_e3_data) {
      fmt::print("The E3 data-profile SCTP endpoint requires the portable_e3_data store.\n");
      return false;
    }
#endif
  }
  if (config.e3ap_sctp && config.e3dp_sctp && config.e3ap_sctp->port == config.e3dp_sctp->port &&
      config.e3ap_sctp->bind_address == config.e3dp_sctp->bind_address) {
    fmt::print("E3AP and E3 data-profile endpoints must not share one bind address and port.\n");
    return false;
  }
  for (const auto interval : {config.telemetry.pusch_symbols_minimum_interval,
                              config.telemetry.pusch_crc_minimum_interval,
                              config.telemetry.srs_channel_minimum_interval,
                              config.telemetry.pusch_dmrs_minimum_interval,
                              config.telemetry.pusch_symbols_hard_floor,
                              config.telemetry.pusch_crc_hard_floor,
                              config.telemetry.srs_channel_hard_floor,
                              config.telemetry.pusch_dmrs_hard_floor,
                              config.telemetry.pusch_grid_minimum_interval,
                              config.telemetry.pusch_grid_hard_floor}) {
    if (interval.count() < 0 || interval > std::chrono::seconds(60)) {
      fmt::print("dApp telemetry publication floors must be in the range 0..60000 ms.\n");
      return false;
    }
  }
  return true;
}

bool validate_portable_e3_data(const dapp_service_config& config)
{
  if (!config.portable_e3_data) {
    return true;
  }
  const auto& portable    = *config.portable_e3_data;
  const bool  has_carrier = config.local_e3 || config.e3ap_sctp || config.e3dp_sctp;
  if (!has_carrier || portable.maximum_subscriptions == 0 || portable.maximum_subscriptions > 32 ||
      portable.maximum_retained_bytes < 1024 * 1024 || portable.maximum_retained_bytes > 1024ULL * 1024 * 1024 ||
      portable.maximum_object_bytes == 0 || portable.maximum_object_bytes > 64 * 1024 * 1024 ||
      portable.maximum_metadata_bytes < 1024 || portable.maximum_metadata_bytes > 64 * 1024 ||
      portable.minimum_chunk_bytes < 1024 || portable.minimum_chunk_bytes > portable.maximum_chunk_bytes ||
      portable.maximum_chunk_bytes > 64 * 1024 || portable.minimum_lease_us == 0 ||
      portable.minimum_lease_us > portable.maximum_lease_us || portable.maximum_lease_us > 60'000'000) {
    fmt::print("Portable E3 data needs an E3 carrier (local_e3, e3ap_sctp, or e3dp_sctp) and valid bounded "
               "subscription, storage, chunk, and lease values.\n");
    return false;
  }
  return true;
}

bool validate_initial_instances(const dapp_service_config& config)
{
  if (config.initial_instances.size() > 64) {
    fmt::print("At most 64 initial dApp instances may be selected.\n");
    return false;
  }
  for (std::size_t index = 0; index != config.initial_instances.size(); ++index) {
    const auto&                 instance = config.initial_instances[index];
    const std::filesystem::path manifest(instance.package_manifest);
    const bool                  known_manifest =
        std::find(config.package_manifests.begin(), config.package_manifests.end(), instance.package_manifest) !=
        config.package_manifests.end();
    const bool duplicate =
        std::any_of(config.initial_instances.begin(), config.initial_instances.begin() + index, [&](const auto& prior) {
          return prior.package_manifest == instance.package_manifest;
        });
    const bool placement_valid = instance.placement == OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 ||
                                 instance.placement == OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1;
    const bool backend_valid = instance.backend == ocudu::dapp::management::backend::cpu ||
                               instance.backend == ocudu::dapp::management::backend::cuda ||
                               instance.backend == ocudu::dapp::management::backend::other_accelerator;
    const bool target_valid = instance.target == dapp_initial_instance_target::shadow ||
                              instance.target == dapp_initial_instance_target::active;
    if (manifest.empty() || manifest.is_absolute() || manifest.lexically_normal() != manifest || manifest == "." ||
        (manifest.begin() != manifest.end() && *manifest.begin() == "..") || !known_manifest || duplicate ||
        !placement_valid || !backend_valid || !target_valid || instance.lifecycle_timeout.count() <= 0 ||
        instance.lifecycle_timeout > std::chrono::minutes(1)) {
      fmt::print("Initial dApp instances must uniquely reference an admitted manifest and use valid bounded "
                 "placement, backend, target, and timeout values.\n");
      return false;
    }
  }
  return true;
}

} // namespace

bool ocudu::app_services::validate_dapp_service_config(const dapp_service_config& config)
{
  std::error_code error;
  if (config.catalog_root.empty() || !std::filesystem::is_directory(config.catalog_root, error) || error) {
    fmt::print("dApp catalog_root must name an existing accessible directory.\n");
    return false;
  }
  const std::filesystem::path preflight_helper(config.native_preflight_helper);
  struct stat                 helper_status {};
  if (!preflight_helper.is_absolute() || preflight_helper.lexically_normal() != preflight_helper ||
      ::lstat(preflight_helper.c_str(), &helper_status) != 0 || !S_ISREG(helper_status.st_mode) ||
      (helper_status.st_mode & (S_IWGRP | S_IWOTH)) != 0 || ::access(preflight_helper.c_str(), R_OK | X_OK) != 0 ||
      config.native_preflight_timeout.count() <= 0 || config.native_preflight_timeout > std::chrono::minutes(1)) {
    fmt::print("dApp native_preflight_helper must be a trusted absolute executable and timeout must be 1..60000 ms.\n");
    return false;
  }
  // Owner policy: root-owned and euid-owned helpers are always admitted. When the
  // gNB itself runs as root (euid 0), also admit a non-root-owned helper so lab
  // launches under sudo can use a developer-built preflight binary (e.g. owned by
  // deepsig) without an extra chown/stage step.
  const uid_t euid = ::geteuid();
  if (euid != 0 && helper_status.st_uid != 0 && helper_status.st_uid != euid) {
    fmt::print("dApp native_preflight_helper must be owned by root or the gNB euid "
               "(root may use a developer-owned helper).\n");
    return false;
  }
  if (config.package_manifests.size() > 256) {
    fmt::print("At most 256 dApp package manifests may be bootstrapped.\n");
    return false;
  }
  if (!validate_package_signatures(config.package_signatures)) {
    return false;
  }
  for (std::size_t index = 0; index != config.package_manifests.size(); ++index) {
    const std::filesystem::path manifest(config.package_manifests[index]);
    if (manifest.empty() || manifest.is_absolute() || manifest.lexically_normal() != manifest || manifest == "." ||
        (manifest.begin() != manifest.end() && *manifest.begin() == "..") ||
        std::find(config.package_manifests.begin(),
                  config.package_manifests.begin() + index,
                  config.package_manifests[index]) != config.package_manifests.begin() + index) {
      fmt::print("dApp package manifest paths must be normalized, nonempty, unique, and catalog-relative.\n");
      return false;
    }
  }
  if (!validate_initial_instances(config)) {
    return false;
  }
  if (config.maximum_transactions == 0 || config.maximum_transactions > 65536 || config.maximum_queued_jobs == 0 ||
      config.maximum_queued_jobs > 4096 || config.maximum_retained_jobs < config.maximum_queued_jobs ||
      config.maximum_retained_jobs > 65536) {
    fmt::print("dApp transaction/job bounds are invalid or retained jobs are fewer than queued jobs.\n");
    return false;
  }
  if (config.max_scheduler_ul_prbs_avoided >= MAX_NOF_PRBS || config.backend_mask == 0) {
    fmt::print("dApp scheduler guardrail or backend mask is invalid.\n");
    return false;
  }
  if (config.pusch_route_slots == 0 || config.pusch_route_slots > dapp::pusch_route_table::max_slots_per_hook) {
    fmt::print("dApp pusch_route_slots must be between 1 and {}.\n", dapp::pusch_route_table::max_slots_per_hook);
    return false;
  }
  const auto& process     = config.class_c_process;
  const auto  ring_stride = (sizeof(ocudu_dapp_shared_ring_slot_v1) + uint64_t{process.payload_capacity_bytes} +
                            OCUDU_DAPP_SHARED_RING_ALIGNMENT_V1 - 1U) &
                           ~uint64_t{OCUDU_DAPP_SHARED_RING_ALIGNMENT_V1 - 1U};
  const auto ring_bytes = uint64_t{OCUDU_DAPP_SHARED_RING_HEADER_BYTES_V1} + process.ring_slots * ring_stride;
  if (process.ring_slots < 2 || process.ring_slots > 64 || process.payload_capacity_bytes < 64 * 1024 ||
      process.payload_capacity_bytes > 64 * 1024 * 1024 || ring_bytes > OCUDU_DAPP_SHARED_RING_MAX_BYTES_V1 ||
      process.startup_timeout.count() <= 0 || process.startup_timeout > std::chrono::minutes(1) ||
      process.stop_timeout.count() <= 0 || process.stop_timeout > std::chrono::minutes(1) ||
      process.restart_backoff.count() <= 0 || process.restart_backoff > std::chrono::minutes(1) ||
      process.heartbeat_period.count() <= 0 || process.heartbeat_period > std::chrono::minutes(1) ||
      process.heartbeat_timeout.count() <= 0 || process.heartbeat_timeout > std::chrono::minutes(1) ||
      process.management_timeout.count() <= 0 || process.management_timeout > std::chrono::minutes(1) ||
      (process.maximum_address_space_bytes != 0 && process.maximum_address_space_bytes < 64 * 1024 * 1024) ||
      process.maximum_core_file_bytes > std::numeric_limits<rlim_t>::max() || process.maximum_open_files < 16 ||
      process.maximum_open_files > 1048576 || process.maximum_restart_attempts > 100) {
    fmt::print("dApp isolated Class-C resource or supervision bounds are invalid.\n");
    return false;
  }
  const auto& quiet = config.quiet_reservation;
  if (quiet.maximum_retained_prb_slots == 0 || quiet.maximum_retained_prb_slots > 1048576 ||
      quiet.maximum_prbs_per_slot == 0 || quiet.maximum_prbs_per_slot > MAX_NOF_PRBS ||
      quiet.duty_cycle_window_slots == 0 || quiet.duty_cycle_window_slots > 4096 ||
      quiet.maximum_prb_slots_per_window == 0 || quiet.maximum_prb_slots_per_window > 1048576 ||
      quiet.maximum_prb_slots_per_window >
          static_cast<uint32_t>(quiet.maximum_prbs_per_slot) * quiet.duty_cycle_window_slots ||
      quiet.maximum_retained_prb_slots < quiet.maximum_prb_slots_per_window ||
      quiet.maximum_source_prb_slots_per_window == 0 ||
      quiet.maximum_source_prb_slots_per_window > quiet.maximum_prb_slots_per_window) {
    fmt::print("dApp quiet-reservation duty bounds are invalid.\n");
    return false;
  }
  return validate_portable_e3_data(config) && validate_sctp_carriers(config) &&
         (!config.local_e3 || validate_local_e3(*config.local_e3));
}
