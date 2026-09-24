// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "dapp_config_cli11_schema.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/support/cli11_utils.h"
#include "ocudu/support/config_parsers.h"
#include <sstream>

using namespace ocudu;
using namespace ocudu::app_services;

namespace {

void configure_local_e3(CLI::App& app, std::optional<dapp_local_e3_config>& config)
{
  config.emplace();
  add_option(app, "--socket_path", config->socket_path, "Absolute path of the credentialed local E3 socket")
      ->required();
  app.add_option_function<uint32_t>(
      "--allowed_uid",
      [&config](uint32_t value) { config->allowed_uid = static_cast<uid_t>(value); },
      "UID permitted to use the local E3 socket; defaults to the DU effective UID");
  app.add_option_function<uint32_t>(
         "--request_timeout_ms",
         [&config](uint32_t value) { config->request_timeout = std::chrono::milliseconds(value); },
         "Maximum time for one local E3 request")
      ->default_str(std::to_string(config->request_timeout.count()))
      ->check(CLI::Range(1U, 60000U));
}

void configure_portable_e3_data(CLI::App& app, std::optional<dapp_portable_e3_data_config>& config)
{
  config.emplace();
  add_option(app, "--maximum_subscriptions", config->maximum_subscriptions, "Maximum live portable data subscriptions")
      ->capture_default_str()
      ->check(CLI::Range(1U, 32U));
  add_option(app, "--maximum_retained_bytes", config->maximum_retained_bytes, "Total preallocated payload queue bytes")
      ->capture_default_str()
      ->check(CLI::Range(UINT64_C(1048576), UINT64_C(1073741824)));
  add_option(app, "--maximum_object_bytes", config->maximum_object_bytes, "Largest tensor admitted to E3 chunking")
      ->capture_default_str()
      ->check(CLI::Range(1U, 64U * 1024U * 1024U));
  add_option(app, "--maximum_metadata_bytes", config->maximum_metadata_bytes, "Largest FlatBuffers metadata envelope")
      ->capture_default_str()
      ->check(CLI::Range(1024U, 64U * 1024U));
  add_option(app, "--minimum_chunk_bytes", config->minimum_chunk_bytes, "Smallest negotiable APER tensor chunk")
      ->capture_default_str()
      ->check(CLI::Range(1024U, 64U * 1024U));
  add_option(app, "--maximum_chunk_bytes", config->maximum_chunk_bytes, "Largest negotiable APER tensor chunk")
      ->capture_default_str()
      ->check(CLI::Range(1024U, 64U * 1024U));
  add_option(app, "--minimum_lease_us", config->minimum_lease_us, "Shortest subscription liveness lease")
      ->capture_default_str()
      ->check(CLI::Range(UINT64_C(1), UINT64_C(60000000)));
  add_option(app, "--maximum_lease_us", config->maximum_lease_us, "Longest subscription liveness lease")
      ->capture_default_str()
      ->check(CLI::Range(UINT64_C(1), UINT64_C(60000000)));
}

void configure_e3ap_sctp(CLI::App& app, std::optional<dapp_e3ap_sctp_config>& config)
{
  config.emplace();
  add_option(app, "--bind_address", config->bind_address, "Literal IPv4/IPv6 address for the E3AP SCTP listener")
      ->capture_default_str();
  add_option(app, "--port", config->port, "SCTP port for E3AP (E3AP-OCUDU-V1)")
      ->capture_default_str()
      ->check(CLI::Range(1U, 65535U));
  add_option(app, "--allowed_peers", config->allowed_peers, "Peer addresses admitted after accept; empty admits all");
  add_option(app, "--maximum_associations", config->maximum_associations, "Concurrent dApp associations")
      ->capture_default_str()
      ->check(CLI::Range(1U, 1024U));
  app.add_option_function<uint32_t>(
         "--idle_timeout_ms",
         [&config](uint32_t value) { config->idle_timeout = std::chrono::milliseconds(value); },
         "Close associations quiet for longer than this")
      ->default_str(std::to_string(config->idle_timeout.count()))
      ->check(CLI::Range(1000U, 3600000U));
  app.add_option_function<uint32_t>(
         "--setup_timeout_ms",
         [&config](uint32_t value) { config->setup_timeout = std::chrono::milliseconds(value); },
         "Close associations that do not complete E3 setup within this time")
      ->default_str(std::to_string(config->setup_timeout.count()))
      ->check(CLI::Range(100U, 600000U));
  add_option(app,
             "--maximum_subscriptions_per_dapp",
             config->maximum_subscriptions_per_dapp,
             "Subscriptions admitted per dApp association")
      ->capture_default_str()
      ->check(CLI::Range(1U, 1024U));
  add_option(app, "--ran_identifier", config->ran_identifier, "RAN identifier returned in E3 setup responses")
      ->capture_default_str();
}

void configure_e3dp_sctp(CLI::App& app, std::optional<dapp_e3dp_sctp_config>& config)
{
  config.emplace();
  add_option(app, "--bind_address", config->bind_address, "Literal IPv4/IPv6 address for the data-profile listener")
      ->capture_default_str();
  add_option(app, "--port", config->port, "SCTP port for the FlatBuffers E3 data profile")
      ->capture_default_str()
      ->check(CLI::Range(1U, 65535U));
  add_option(app, "--allowed_peers", config->allowed_peers, "Peer addresses admitted after accept; empty admits all");
  add_option(app, "--maximum_associations", config->maximum_associations, "Concurrent client associations")
      ->capture_default_str()
      ->check(CLI::Range(1U, 1024U));
  add_option(app,
             "--maximum_subscriptions_per_association",
             config->maximum_subscriptions_per_association,
             "Data-profile subscriptions one association may hold concurrently")
      ->capture_default_str()
      ->check(CLI::Range(1U, 1024U));
  app.add_option_function<uint32_t>(
         "--idle_timeout_ms",
         [&config](uint32_t value) { config->idle_timeout = std::chrono::milliseconds(value); },
         "Close associations quiet for longer than this")
      ->default_str(std::to_string(config->idle_timeout.count()))
      ->check(CLI::Range(1000U, 3600000U));
}

void configure_telemetry(CLI::App& app, dapp_telemetry_config& config)
{
  app.add_option_function<uint32_t>(
         "--telemetry_pusch_symbols_min_interval_ms",
         [&config](uint32_t value) { config.pusch_symbols_minimum_interval = std::chrono::milliseconds(value); },
         "Per-RNTI floor between published PUSCH symbol captures; zero publishes every grant")
      ->default_str("50")
      ->check(CLI::Range(0U, 60000U));
  app.add_option_function<uint32_t>(
         "--telemetry_pusch_crc_min_interval_ms",
         [&config](uint32_t value) { config.pusch_crc_minimum_interval = std::chrono::milliseconds(value); },
         "Per-RNTI floor between published PUSCH CRC outcomes; zero publishes every transport block")
      ->default_str("0")
      ->check(CLI::Range(0U, 60000U));
  app.add_option_function<uint32_t>(
         "--telemetry_srs_channel_min_interval_ms",
         [&config](uint32_t value) { config.srs_channel_minimum_interval = std::chrono::milliseconds(value); },
         "Per-RNTI floor between published SRS channel estimates")
      ->default_str("20")
      ->check(CLI::Range(0U, 60000U));
  app.add_option_function<uint32_t>(
         "--telemetry_pusch_dmrs_min_interval_ms",
         [&config](uint32_t value) { config.pusch_dmrs_minimum_interval = std::chrono::milliseconds(value); },
         "Per-RNTI floor between published PUSCH DM-RS channel estimates")
      ->default_str("20")
      ->check(CLI::Range(0U, 60000U));
  app.add_option_function<uint32_t>(
         "--telemetry_pusch_grid_min_interval_ms",
         [&config](uint32_t value) { config.pusch_grid_minimum_interval = std::chrono::milliseconds(value); },
         "Per-RNTI floor between published raw PUSCH resource-grid captures")
      ->default_str("100")
      ->check(CLI::Range(0U, 60000U));
  // default_ms must match the corresponding dapp_telemetry_config member default.
  const auto hard_floor =
      [&app](const char* name, std::chrono::microseconds& target, const char* stream, const char* default_ms) {
        app.add_option_function<uint32_t>(
               name,
               [&target](uint32_t value) { target = std::chrono::milliseconds(value); },
               std::string("Hard per-stream floor (aggregate across RNTIs) for ") + stream +
                   " that no subscriber may request below; zero allows full-rate subscriptions")
            ->default_str(default_ms)
            ->check(CLI::Range(0U, 60000U));
      };
  hard_floor("--telemetry_pusch_symbols_hard_floor_ms", config.pusch_symbols_hard_floor, "PUSCH symbol captures", "10");
  hard_floor("--telemetry_pusch_crc_hard_floor_ms", config.pusch_crc_hard_floor, "PUSCH CRC outcomes", "1");
  hard_floor("--telemetry_srs_channel_hard_floor_ms", config.srs_channel_hard_floor, "SRS channel estimates", "5");
  hard_floor(
      "--telemetry_pusch_dmrs_hard_floor_ms", config.pusch_dmrs_hard_floor, "PUSCH DM-RS channel estimates", "5");
  // Raw-grid captures read the resource grid on the PUSCH thread (a device->host
  // staging on accelerated grids), so the default hard floor bounds a full-rate
  // subscriber to 50 captures per second per RNTI.
  app.add_option_function<uint32_t>(
         "--telemetry_pusch_grid_hard_floor_ms",
         [&config](uint32_t value) { config.pusch_grid_hard_floor = std::chrono::milliseconds(value); },
         "Hard per-stream floor (aggregate across RNTIs) for raw PUSCH resource-grid captures that no subscriber may "
         "request below")
      ->default_str("20")
      ->check(CLI::Range(0U, 60000U));
}

void configure_initial_instance(CLI::App& app, dapp_initial_instance_config& config)
{
  add_option(app,
             "--package_manifest",
             config.package_manifest,
             "Catalog-relative manifest already present in package_manifests")
      ->required();
  app.add_option_function<std::string>(
         "--placement",
         [&config](const std::string& value) {
           if (value == "in_process") {
             config.placement = OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1;
           } else if (value == "out_of_process") {
             config.placement = OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1;
           } else {
             throw std::runtime_error("initial instance placement must be in_process or out_of_process");
           }
         },
         "Execution placement")
      ->default_str("in_process");
  app.add_option_function<std::string>(
         "--backend",
         [&config](const std::string& value) {
           if (value == "cpu") {
             config.backend = dapp::management::backend::cpu;
           } else if (value == "cuda") {
             config.backend = dapp::management::backend::cuda;
           } else if (value == "other_accelerator") {
             config.backend = dapp::management::backend::other_accelerator;
           } else {
             throw std::runtime_error("initial instance backend must be cpu, cuda, or other_accelerator");
           }
         },
         "Selected package backend")
      ->default_str("cpu");
  app.add_option_function<std::string>(
         "--target",
         [&config](const std::string& value) {
           if (value == "shadow") {
             config.target = dapp_initial_instance_target::shadow;
           } else if (value == "active") {
             config.target = dapp_initial_instance_target::active;
           } else {
             throw std::runtime_error("initial instance target must be shadow or active");
           }
         },
         "Lifecycle target reached before management ingress opens")
      ->default_str("active");
  app.add_option_function<uint32_t>(
         "--lifecycle_timeout_ms",
         [&config](uint32_t value) { config.lifecycle_timeout = std::chrono::milliseconds(value); },
         "Maximum time for each startup lifecycle transition")
      ->default_str(std::to_string(config.lifecycle_timeout.count()))
      ->check(CLI::Range(1U, 60000U));
}

} // namespace

void ocudu::app_services::configure_cli11_with_dapp_service_config_schema(CLI::App&                           app,
                                                                          std::optional<dapp_service_config>& config)
{
  config.emplace();
  CLI::App* dapp = add_subcommand(app, "dapp", "dApp runtime and embedded E3 configuration")->configurable();
  add_option(*dapp, "--catalog_root", config->catalog_root, "Root directory containing approved dApp artifacts")
      ->required();
  add_option(*dapp,
             "--native_preflight_helper",
             config->native_preflight_helper,
             "Absolute path of the trusted native dApp preflight helper")
      ->required();
  dapp->add_option_function<uint32_t>(
          "--native_preflight_timeout_ms",
          [&config](uint32_t value) { config->native_preflight_timeout = std::chrono::milliseconds(value); },
          "Maximum time for native loader/query/descriptor/lifecycle preflight")
      ->default_str(std::to_string(config->native_preflight_timeout.count()))
      ->check(CLI::Range(1U, 60000U));
  add_option(*dapp,
             "--package_manifests",
             config->package_manifests,
             "Relative catalog manifest paths admitted during DU startup");
  dapp->add_option_function<std::vector<std::string>>(
      "--initial_instances",
      [&config](const std::vector<std::string>& values) {
        config->initial_instances.resize(values.size());
        for (std::size_t index = 0; index != values.size(); ++index) {
          CLI::App item("Initial dApp instance");
          item.config_formatter(create_yaml_config_parser());
          item.allow_config_extras(CLI::config_extras_mode::error);
          configure_initial_instance(item, config->initial_instances[index]);
          std::istringstream input(values[index]);
          item.parse_from_stream(input);
        }
      },
      "Fail-closed dApp instances loaded through the ordinary lifecycle before E3 ingress");
  dapp->add_option_function<std::string>(
          "--package_signature_mode",
          [&config](const std::string& value) {
            if (value == "required") {
              config->package_signatures.mode = ocudu::dapp::package_signature_mode::required;
            } else if (value == "allow_unsigned") {
              config->package_signatures.mode = ocudu::dapp::package_signature_mode::allow_unsigned;
            } else {
              throw std::runtime_error("package_signature_mode must be required or allow_unsigned");
            }
          },
          "Package signature admission policy")
      ->default_str("required");
  add_option(*dapp,
             "--package_trust_store",
             config->package_signatures.trust_store_path,
             "Absolute path of the offline package trust policy");
  add_option(*dapp,
             "--minimum_trust_policy_epoch",
             config->package_signatures.minimum_policy_epoch,
             "Minimum accepted offline package trust-policy epoch")
      ->capture_default_str();
  add_option(
      *dapp, "--max_transactions", config->maximum_transactions, "Maximum retained idempotent management transactions")
      ->capture_default_str()
      ->check(CLI::Range(1U, 65536U));
  add_option(*dapp, "--max_queued_jobs", config->maximum_queued_jobs, "Maximum queued dApp management jobs")
      ->capture_default_str()
      ->check(CLI::Range(1U, 4096U));
  add_option(*dapp, "--max_retained_jobs", config->maximum_retained_jobs, "Maximum retained dApp management jobs")
      ->capture_default_str()
      ->check(CLI::Range(1U, 65536U));
  add_option(*dapp,
             "--max_scheduler_ul_prbs_avoided",
             config->max_scheduler_ul_prbs_avoided,
             "Maximum UL PRBs a scheduler dApp may avoid per invocation; zero disables this authority")
      ->capture_default_str()
      ->check(CLI::Range(0U, static_cast<unsigned>(MAX_NOF_PRBS - 1)));
  add_option(*dapp,
             "--pusch_route_slots",
             config->pusch_route_slots,
             "Instances each Class-A PUSCH hook (channel estimator, equalizer, receiver) can hold at once for "
             "per-UE routing over E3; 1 keeps one instance per hook")
      ->capture_default_str()
      ->check(CLI::Range(1U, static_cast<unsigned>(dapp::pusch_route_table::max_slots_per_hook)));
  dapp->add_flag("--scheduler_evaluation_only",
                 config->scheduler_evaluation_only,
                 "Execute and validate Class-B scheduler output without applying it");
  dapp->add_flag("--scheduler_allow_mcs",
                 config->scheduler_allow_mcs,
                 "Allow Class-B MCS intents (table-clamped) to drive newTx MCS");
  dapp->add_flag("--scheduler_allow_prb_limits",
                 config->scheduler_allow_prb_limits,
                 "Allow Class-B min/max PRB limits on newTx grants");
  dapp->add_flag("--scheduler_allow_prb_preference",
                 config->scheduler_allow_prb_preference,
                 "Allow Class-B soft PRB start/length preferences");
  dapp->add_flag("--scheduler_allow_prb_assignment",
                 config->scheduler_allow_prb_assignment,
                 "Allow Class-B hard multi-UE PRB assignments on free-VRB snapshot");
  dapp->add_flag("--scheduler_allow_tpc", config->scheduler_allow_tpc, "Allow Class-B UL TPC command overrides");
  dapp->add_flag("--scheduler_allow_layers", config->scheduler_allow_layers, "Allow Class-B layer/RI overrides");
  add_option(*dapp,
             "--scheduler_max_mcs_delta_from_host",
             config->scheduler_max_mcs_delta_from_host,
             "Maximum |dApp MCS − host LA MCS| accepted when host MCS is valid")
      ->capture_default_str()
      ->check(CLI::Range(0, 28));
  add_option(*dapp,
             "--class_c_process_ring_slots",
             config->class_c_process.ring_slots,
             "Shared-memory slots allocated per isolated Class-C producer route")
      ->capture_default_str()
      ->check(CLI::Range(2U, 64U));
  add_option(*dapp,
             "--class_c_process_payload_capacity_bytes",
             config->class_c_process.payload_capacity_bytes,
             "Maximum metadata and tensor bytes copied into one isolated Class-C route slot")
      ->capture_default_str()
      ->check(CLI::Range(64U * 1024U, 64U * 1024U * 1024U));
  dapp->add_option_function<uint32_t>(
          "--class_c_process_startup_timeout_ms",
          [&config](uint32_t value) { config->class_c_process.startup_timeout = std::chrono::milliseconds(value); },
          "Maximum isolated Class-C worker readiness time")
      ->default_str(std::to_string(config->class_c_process.startup_timeout.count()))
      ->check(CLI::Range(1U, 60000U));
  dapp->add_option_function<uint32_t>(
          "--class_c_process_stop_timeout_ms",
          [&config](uint32_t value) { config->class_c_process.stop_timeout = std::chrono::milliseconds(value); },
          "Grace period before an isolated Class-C worker is killed")
      ->default_str(std::to_string(config->class_c_process.stop_timeout.count()))
      ->check(CLI::Range(1U, 60000U));
  dapp->add_option_function<uint32_t>(
          "--class_c_process_restart_backoff_ms",
          [&config](uint32_t value) { config->class_c_process.restart_backoff = std::chrono::milliseconds(value); },
          "Delay between isolated Class-C worker restart attempts")
      ->default_str(std::to_string(config->class_c_process.restart_backoff.count()))
      ->check(CLI::Range(1U, 60000U));
  dapp->add_option_function<uint32_t>(
          "--class_c_process_heartbeat_period_ms",
          [&config](uint32_t value) { config->class_c_process.heartbeat_period = std::chrono::milliseconds(value); },
          "Interval between authenticated isolated Class-C heartbeat challenges")
      ->default_str(std::to_string(config->class_c_process.heartbeat_period.count()))
      ->check(CLI::Range(1U, 60000U));
  dapp->add_option_function<uint32_t>(
          "--class_c_process_heartbeat_timeout_ms",
          [&config](uint32_t value) { config->class_c_process.heartbeat_timeout = std::chrono::milliseconds(value); },
          "Maximum time for an isolated Class-C heartbeat response")
      ->default_str(std::to_string(config->class_c_process.heartbeat_timeout.count()))
      ->check(CLI::Range(1U, 60000U));
  dapp->add_option_function<uint32_t>(
          "--class_c_process_management_timeout_ms",
          [&config](uint32_t value) { config->class_c_process.management_timeout = std::chrono::milliseconds(value); },
          "Maximum time for one isolated Class-C configuration or model operation")
      ->default_str(std::to_string(config->class_c_process.management_timeout.count()))
      ->check(CLI::Range(1U, 60000U));
  add_option(*dapp,
             "--class_c_process_max_address_space_bytes",
             config->class_c_process.maximum_address_space_bytes,
             "Maximum isolated Class-C virtual address space; zero inherits the host limit")
      ->capture_default_str();
  add_option(*dapp,
             "--class_c_process_max_core_file_bytes",
             config->class_c_process.maximum_core_file_bytes,
             "Maximum isolated Class-C CPU core bytes; zero disables worker core dumps")
      ->capture_default_str();
  dapp->add_flag("--class_c_process_require_seccomp_filter",
                 config->class_c_process.require_seccomp_filter,
                 "Require isolated Class-C workers to install a seccomp filter before route admission");
  add_option(*dapp,
             "--class_c_process_max_open_files",
             config->class_c_process.maximum_open_files,
             "Maximum open descriptors for each isolated Class-C worker")
      ->capture_default_str()
      ->check(CLI::Range(16U, 1048576U));
  add_option(*dapp,
             "--class_c_process_max_restart_attempts",
             config->class_c_process.maximum_restart_attempts,
             "Maximum supervised restart attempts per isolated Class-C worker")
      ->capture_default_str()
      ->check(CLI::Range(0U, 100U));
  add_option(*dapp,
             "--quiet_max_retained_prb_slots",
             config->quiet_reservation.maximum_retained_prb_slots,
             "Maximum retained quiet PRB-slot volume per cell")
      ->capture_default_str()
      ->check(CLI::Range(1U, 1048576U));
  add_option(*dapp,
             "--quiet_max_prbs_per_slot",
             config->quiet_reservation.maximum_prbs_per_slot,
             "Maximum PRBs suppressed by quiet reservations in one slot")
      ->capture_default_str()
      ->check(CLI::Range(1U, static_cast<unsigned>(MAX_NOF_PRBS)));
  add_option(*dapp,
             "--quiet_duty_window_slots",
             config->quiet_reservation.duty_cycle_window_slots,
             "Rolling slot window for quiet-reservation duty accounting")
      ->capture_default_str()
      ->check(CLI::Range(1U, 4096U));
  add_option(*dapp,
             "--quiet_max_prb_slots_per_window",
             config->quiet_reservation.maximum_prb_slots_per_window,
             "Maximum quiet PRB-slot volume in any rolling duty window")
      ->capture_default_str()
      ->check(CLI::Range(1U, 1048576U));
  add_option(*dapp,
             "--quiet_max_source_prb_slots_per_window",
             config->quiet_reservation.maximum_source_prb_slots_per_window,
             "Maximum quiet PRB-slot volume attributed to one dApp instance in any rolling duty window")
      ->capture_default_str()
      ->check(CLI::Range(1U, 1048576U));

  configure_telemetry(*dapp, config->telemetry);

  CLI::App* local = add_subcommand(*dapp, "local_e3", "Credentialed same-host E3 management endpoint")->configurable();
  configure_local_e3(*local, config->local_e3);
  CLI::App* portable =
      add_subcommand(*dapp, "portable_e3_data", "Bounded low-rate Class-C E3 payload fallback")->configurable();
  configure_portable_e3_data(*portable, config->portable_e3_data);
  CLI::App* e3ap = add_subcommand(*dapp, "e3ap_sctp", "E3AP-OCUDU-V1 endpoint over SCTP")->configurable();
  configure_e3ap_sctp(*e3ap, config->e3ap_sctp);
  CLI::App* e3dp = add_subcommand(*dapp, "e3dp_sctp", "FlatBuffers E3 data-profile compatibility endpoint over SCTP")
                       ->configurable();
  configure_e3dp_sctp(*e3dp, config->e3dp_sctp);
}

void ocudu::app_services::autoderive_dapp_service_config_after_parsing(CLI::App&                           app,
                                                                       std::optional<dapp_service_config>& config)
{
  CLI::App* dapp = app.get_subcommand("dapp");
  if (dapp->count_all() == 0) {
    config.reset();
    dapp->disabled();
    return;
  }

  CLI::App* local = dapp->get_subcommand("local_e3");
  if (local->count_all() == 0) {
    config->local_e3.reset();
    local->disabled();
  }
  CLI::App* portable = dapp->get_subcommand("portable_e3_data");
  if (portable->count_all() == 0) {
    config->portable_e3_data.reset();
    portable->disabled();
  }
  CLI::App* e3ap = dapp->get_subcommand("e3ap_sctp");
  if (e3ap->count_all() == 0) {
    config->e3ap_sctp.reset();
    e3ap->disabled();
  }
  CLI::App* e3dp = dapp->get_subcommand("e3dp_sctp");
  if (e3dp->count_all() == 0) {
    config->e3dp_sctp.reset();
    e3dp->disabled();
  }
  // The SCTP carriers publish through the portable data store; default it when
  // the operator enabled a carrier without tuning the store explicitly.
  if ((config->e3ap_sctp || config->e3dp_sctp) && !config->portable_e3_data) {
    config->portable_e3_data.emplace();
  }
}
