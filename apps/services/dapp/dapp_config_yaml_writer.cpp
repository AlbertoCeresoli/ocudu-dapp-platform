// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "dapp_config_yaml_writer.h"
#include <chrono>

using namespace ocudu::app_services;

namespace {

const char* placement_name(ocudu_dapp_placement_v1 placement) noexcept
{
  switch (placement) {
    case OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1:
      return "in_process";
    case OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1:
      return "out_of_process";
  }
  return "invalid";
}

const char* backend_name(ocudu::dapp::management::backend backend) noexcept
{
  switch (backend) {
    case ocudu::dapp::management::backend::cpu:
      return "cpu";
    case ocudu::dapp::management::backend::cuda:
      return "cuda";
    case ocudu::dapp::management::backend::other_accelerator:
      return "other_accelerator";
  }
  return "invalid";
}

const char* target_name(dapp_initial_instance_target target) noexcept
{
  switch (target) {
    case dapp_initial_instance_target::shadow:
      return "shadow";
    case dapp_initial_instance_target::active:
      return "active";
  }
  return "invalid";
}

YAML::Node initial_instance_node(const dapp_initial_instance_config& config)
{
  YAML::Node node;
  node["package_manifest"]     = config.package_manifest;
  node["placement"]            = placement_name(config.placement);
  node["backend"]              = backend_name(config.backend);
  node["target"]               = target_name(config.target);
  node["lifecycle_timeout_ms"] = config.lifecycle_timeout.count();
  return node;
}

} // namespace

void ocudu::app_services::fill_dapp_service_config_in_yaml_schema(YAML::Node&                               node,
                                                                  const std::optional<dapp_service_config>& config)
{
  if (!config) {
    return;
  }

  YAML::Node dapp                     = node["dapp"];
  dapp["catalog_root"]                = config->catalog_root;
  dapp["native_preflight_helper"]     = config->native_preflight_helper;
  dapp["native_preflight_timeout_ms"] = config->native_preflight_timeout.count();
  dapp["package_signature_mode"] =
      config->package_signatures.mode == ocudu::dapp::package_signature_mode::required ? "required" : "allow_unsigned";
  if (!config->package_signatures.trust_store_path.empty()) {
    dapp["package_trust_store"] = config->package_signatures.trust_store_path;
  }
  dapp["minimum_trust_policy_epoch"]              = config->package_signatures.minimum_policy_epoch;
  dapp["max_transactions"]                        = config->maximum_transactions;
  dapp["max_queued_jobs"]                         = config->maximum_queued_jobs;
  dapp["max_retained_jobs"]                       = config->maximum_retained_jobs;
  dapp["max_scheduler_ul_prbs_avoided"]           = config->max_scheduler_ul_prbs_avoided;
  dapp["scheduler_evaluation_only"]               = config->scheduler_evaluation_only;
  dapp["scheduler_allow_mcs"]                     = config->scheduler_allow_mcs;
  dapp["scheduler_allow_prb_limits"]              = config->scheduler_allow_prb_limits;
  dapp["scheduler_allow_prb_preference"]          = config->scheduler_allow_prb_preference;
  dapp["scheduler_allow_prb_assignment"]          = config->scheduler_allow_prb_assignment;
  dapp["scheduler_allow_tpc"]                     = config->scheduler_allow_tpc;
  dapp["scheduler_allow_layers"]                  = config->scheduler_allow_layers;
  dapp["scheduler_max_mcs_delta_from_host"]       = config->scheduler_max_mcs_delta_from_host;
  dapp["class_c_process_ring_slots"]              = config->class_c_process.ring_slots;
  dapp["class_c_process_payload_capacity_bytes"]  = config->class_c_process.payload_capacity_bytes;
  dapp["class_c_process_startup_timeout_ms"]      = config->class_c_process.startup_timeout.count();
  dapp["class_c_process_stop_timeout_ms"]         = config->class_c_process.stop_timeout.count();
  dapp["class_c_process_restart_backoff_ms"]      = config->class_c_process.restart_backoff.count();
  dapp["class_c_process_heartbeat_period_ms"]     = config->class_c_process.heartbeat_period.count();
  dapp["class_c_process_heartbeat_timeout_ms"]    = config->class_c_process.heartbeat_timeout.count();
  dapp["class_c_process_management_timeout_ms"]   = config->class_c_process.management_timeout.count();
  dapp["class_c_process_max_address_space_bytes"] = config->class_c_process.maximum_address_space_bytes;
  dapp["class_c_process_max_core_file_bytes"]     = config->class_c_process.maximum_core_file_bytes;
  dapp["class_c_process_require_seccomp_filter"]  = config->class_c_process.require_seccomp_filter;
  dapp["class_c_process_max_open_files"]          = config->class_c_process.maximum_open_files;
  dapp["class_c_process_max_restart_attempts"]    = config->class_c_process.maximum_restart_attempts;
  dapp["quiet_max_retained_prb_slots"]            = config->quiet_reservation.maximum_retained_prb_slots;
  dapp["quiet_max_prbs_per_slot"]                 = config->quiet_reservation.maximum_prbs_per_slot;
  dapp["quiet_duty_window_slots"]                 = config->quiet_reservation.duty_cycle_window_slots;
  dapp["quiet_max_prb_slots_per_window"]          = config->quiet_reservation.maximum_prb_slots_per_window;
  dapp["quiet_max_source_prb_slots_per_window"]   = config->quiet_reservation.maximum_source_prb_slots_per_window;
  if (!config->package_manifests.empty()) {
    YAML::Node manifests = dapp["package_manifests"];
    for (const auto& manifest : config->package_manifests) {
      manifests.push_back(manifest);
    }
  }
  if (!config->initial_instances.empty()) {
    YAML::Node instances = dapp["initial_instances"];
    for (const auto& instance : config->initial_instances) {
      instances.push_back(initial_instance_node(instance));
    }
  }
  if (config->portable_e3_data) {
    YAML::Node portable                = dapp["portable_e3_data"];
    portable["maximum_subscriptions"]  = config->portable_e3_data->maximum_subscriptions;
    portable["maximum_retained_bytes"] = config->portable_e3_data->maximum_retained_bytes;
    portable["maximum_object_bytes"]   = config->portable_e3_data->maximum_object_bytes;
    portable["maximum_metadata_bytes"] = config->portable_e3_data->maximum_metadata_bytes;
    portable["minimum_chunk_bytes"]    = config->portable_e3_data->minimum_chunk_bytes;
    portable["maximum_chunk_bytes"]    = config->portable_e3_data->maximum_chunk_bytes;
    portable["minimum_lease_us"]       = config->portable_e3_data->minimum_lease_us;
    portable["maximum_lease_us"]       = config->portable_e3_data->maximum_lease_us;
  }
  dapp["telemetry_pusch_symbols_min_interval_ms"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(config->telemetry.pusch_symbols_minimum_interval).count();
  dapp["telemetry_pusch_crc_min_interval_ms"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(config->telemetry.pusch_crc_minimum_interval).count();
  dapp["telemetry_srs_channel_min_interval_ms"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(config->telemetry.srs_channel_minimum_interval).count();
  dapp["telemetry_pusch_dmrs_min_interval_ms"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(config->telemetry.pusch_dmrs_minimum_interval).count();
  dapp["telemetry_pusch_symbols_hard_floor_ms"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(config->telemetry.pusch_symbols_hard_floor).count();
  dapp["telemetry_pusch_crc_hard_floor_ms"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(config->telemetry.pusch_crc_hard_floor).count();
  dapp["telemetry_srs_channel_hard_floor_ms"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(config->telemetry.srs_channel_hard_floor).count();
  dapp["telemetry_pusch_dmrs_hard_floor_ms"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(config->telemetry.pusch_dmrs_hard_floor).count();
  dapp["telemetry_pusch_grid_min_interval_ms"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(config->telemetry.pusch_grid_minimum_interval).count();
  dapp["telemetry_pusch_grid_hard_floor_ms"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(config->telemetry.pusch_grid_hard_floor).count();
  if (config->e3ap_sctp) {
    YAML::Node e3ap                        = dapp["e3ap_sctp"];
    e3ap["bind_address"]                   = config->e3ap_sctp->bind_address;
    e3ap["port"]                           = config->e3ap_sctp->port;
    e3ap["maximum_associations"]           = config->e3ap_sctp->maximum_associations;
    e3ap["idle_timeout_ms"]                = config->e3ap_sctp->idle_timeout.count();
    e3ap["setup_timeout_ms"]               = config->e3ap_sctp->setup_timeout.count();
    e3ap["maximum_subscriptions_per_dapp"] = config->e3ap_sctp->maximum_subscriptions_per_dapp;
    e3ap["ran_identifier"]                 = config->e3ap_sctp->ran_identifier;
    if (!config->e3ap_sctp->allowed_peers.empty()) {
      YAML::Node peers = e3ap["allowed_peers"];
      for (const auto& peer : config->e3ap_sctp->allowed_peers) {
        peers.push_back(peer);
      }
    }
  }
  if (config->e3dp_sctp) {
    YAML::Node e3dp              = dapp["e3dp_sctp"];
    e3dp["bind_address"]         = config->e3dp_sctp->bind_address;
    e3dp["port"]                 = config->e3dp_sctp->port;
    e3dp["maximum_associations"] = config->e3dp_sctp->maximum_associations;
    e3dp["idle_timeout_ms"]      = config->e3dp_sctp->idle_timeout.count();
    if (!config->e3dp_sctp->allowed_peers.empty()) {
      YAML::Node peers = e3dp["allowed_peers"];
      for (const auto& peer : config->e3dp_sctp->allowed_peers) {
        peers.push_back(peer);
      }
    }
  }
  if (!config->local_e3) {
    return;
  }

  YAML::Node local            = dapp["local_e3"];
  local["socket_path"]        = config->local_e3->socket_path;
  local["request_timeout_ms"] = config->local_e3->request_timeout.count();
  if (config->local_e3->allowed_uid) {
    local["allowed_uid"] = static_cast<uint64_t>(*config->local_e3->allowed_uid);
  }
}
