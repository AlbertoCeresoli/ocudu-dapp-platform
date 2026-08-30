// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "apps/services/dapp/dapp_config_cli11_schema.h"
#include "apps/services/dapp/dapp_config_validator.h"
#include "apps/services/dapp/dapp_config_yaml_writer.h"
#include "ocudu/support/config_parsers.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace ocudu;
using namespace ocudu::app_services;

namespace {

std::optional<dapp_service_config> parse_config(const std::string& text)
{
  CLI::App app("dApp configuration test");
  app.config_formatter(create_yaml_config_parser());
  app.allow_config_extras(CLI::config_extras_mode::error);

  std::optional<dapp_service_config> config;
  configure_cli11_with_dapp_service_config_schema(app, config);
  std::istringstream input(text);
  app.parse_from_stream(input);
  autoderive_dapp_service_config_after_parsing(app, config);
  return config;
}

dapp_service_config valid_config()
{
  dapp_service_config config;
  config.catalog_root                            = DAPP_TEST_CATALOG;
  config.native_preflight_helper                 = DAPP_PREFLIGHT_HELPER;
  config.package_signatures.mode                 = dapp::package_signature_mode::allow_unsigned;
  config.package_signatures.minimum_policy_epoch = 0;
  config.maximum_transactions                    = 2048;
  config.maximum_queued_jobs                     = 32;
  config.maximum_retained_jobs                   = 256;
  return config;
}

} // namespace

TEST(dapp_config, absent_section_keeps_the_service_disabled)
{
  EXPECT_FALSE(parse_config(""));
}

TEST(dapp_config, enabled_sections_require_their_identity_paths)
{
  EXPECT_THROW(parse_config("dapp:\n  max_queued_jobs: 8\n"), CLI::RequiredError);
  EXPECT_THROW(parse_config("dapp:\n  catalog_root: /tmp\n  local_e3:\n    request_timeout_ms: 50\n"),
               CLI::RequiredError);
}

TEST(dapp_config, yaml_roundtrip_preserves_only_operator_controlled_fields)
{
  auto config                                                  = valid_config();
  config.package_manifests                                     = {"packages/spectrum.json", "packages/srs-isac.json"};
  config.initial_instances                                     = {{"packages/spectrum.json",
                                                                   OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1,
                                                                   dapp::management::backend::cpu,
                                                                   dapp_initial_instance_target::active,
                                                                   std::chrono::milliseconds(400)},
                                                                  {"packages/srs-isac.json",
                                                                   OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1,
                                                                   dapp::management::backend::cuda,
                                                                   dapp_initial_instance_target::shadow,
                                                                   std::chrono::milliseconds(900)}};
  config.max_scheduler_ul_prbs_avoided                         = 17;
  config.scheduler_evaluation_only                             = true;
  config.scheduler_allow_mcs                                   = true;
  config.scheduler_allow_prb_limits                            = true;
  config.scheduler_allow_prb_preference                        = true;
  config.scheduler_allow_prb_assignment                        = true;
  config.scheduler_allow_tpc                                   = true;
  config.scheduler_allow_layers                                = true;
  config.scheduler_max_mcs_delta_from_host                     = 4;
  config.class_c_process.ring_slots                            = 8;
  config.class_c_process.payload_capacity_bytes                = 2 * 1024 * 1024;
  config.class_c_process.startup_timeout                       = std::chrono::milliseconds(321);
  config.class_c_process.stop_timeout                          = std::chrono::milliseconds(654);
  config.class_c_process.restart_backoff                       = std::chrono::milliseconds(77);
  config.class_c_process.heartbeat_period                      = std::chrono::milliseconds(88);
  config.class_c_process.heartbeat_timeout                     = std::chrono::milliseconds(99);
  config.class_c_process.management_timeout                    = std::chrono::milliseconds(77);
  config.class_c_process.maximum_address_space_bytes           = 768 * 1024 * 1024;
  config.class_c_process.maximum_core_file_bytes               = 256 * 1024 * 1024;
  config.class_c_process.require_seccomp_filter                = true;
  config.class_c_process.maximum_open_files                    = 128;
  config.class_c_process.maximum_restart_attempts              = 9;
  config.quiet_reservation.maximum_retained_prb_slots          = 700;
  config.quiet_reservation.maximum_prbs_per_slot               = 24;
  config.quiet_reservation.duty_cycle_window_slots             = 80;
  config.quiet_reservation.maximum_prb_slots_per_window        = 96;
  config.quiet_reservation.maximum_source_prb_slots_per_window = 48;
  config.cells                                                 = {{4, 2, 52}};
  config.nof_pusch_lanes                                       = 8;
  config.backend_mask                                          = 7;
  config.host_build_id                                         = {11, 12};
  config.local_e3 = dapp_local_e3_config{"/tmp/ocudu-dapp-management.sock", 1234, std::chrono::milliseconds(250)};
  config.portable_e3_data                         = dapp_portable_e3_data_config{};
  config.portable_e3_data->maximum_subscriptions  = 7;
  config.portable_e3_data->maximum_retained_bytes = 16 * 1024 * 1024;
  config.portable_e3_data->maximum_object_bytes   = 2 * 1024 * 1024;
  config.portable_e3_data->maximum_metadata_bytes = 32 * 1024;
  config.portable_e3_data->minimum_chunk_bytes    = 2 * 1024;
  config.portable_e3_data->maximum_chunk_bytes    = 32 * 1024;
  config.portable_e3_data->minimum_lease_us       = 20'000;
  config.portable_e3_data->maximum_lease_us       = 2'000'000;

  YAML::Node output;
  fill_dapp_service_config_in_yaml_schema(output, config);
  const YAML::Node dapp = output["dapp"];
  ASSERT_TRUE(dapp);
  EXPECT_FALSE(dapp["cells"]);
  EXPECT_FALSE(dapp["nof_pusch_lanes"]);
  EXPECT_FALSE(dapp["backend_mask"]);
  EXPECT_FALSE(dapp["host_build_id"]);

  auto reparsed = parse_config(YAML::Dump(output));
  ASSERT_TRUE(reparsed);
  EXPECT_EQ(reparsed->catalog_root, config.catalog_root);
  EXPECT_EQ(reparsed->native_preflight_helper, config.native_preflight_helper);
  EXPECT_EQ(reparsed->native_preflight_timeout, config.native_preflight_timeout);
  EXPECT_EQ(reparsed->package_signatures.mode, dapp::package_signature_mode::allow_unsigned);
  EXPECT_TRUE(reparsed->package_signatures.trust_store_path.empty());
  EXPECT_EQ(reparsed->package_signatures.minimum_policy_epoch, 0U);
  EXPECT_EQ(reparsed->package_manifests, config.package_manifests);
  ASSERT_EQ(reparsed->initial_instances.size(), config.initial_instances.size());
  for (std::size_t index = 0; index != config.initial_instances.size(); ++index) {
    EXPECT_EQ(reparsed->initial_instances[index].package_manifest, config.initial_instances[index].package_manifest);
    EXPECT_EQ(reparsed->initial_instances[index].placement, config.initial_instances[index].placement);
    EXPECT_EQ(reparsed->initial_instances[index].backend, config.initial_instances[index].backend);
    EXPECT_EQ(reparsed->initial_instances[index].target, config.initial_instances[index].target);
    EXPECT_EQ(reparsed->initial_instances[index].lifecycle_timeout, config.initial_instances[index].lifecycle_timeout);
  }
  EXPECT_EQ(reparsed->maximum_transactions, config.maximum_transactions);
  EXPECT_EQ(reparsed->maximum_queued_jobs, config.maximum_queued_jobs);
  EXPECT_EQ(reparsed->maximum_retained_jobs, config.maximum_retained_jobs);
  EXPECT_EQ(reparsed->max_scheduler_ul_prbs_avoided, config.max_scheduler_ul_prbs_avoided);
  EXPECT_EQ(reparsed->scheduler_evaluation_only, config.scheduler_evaluation_only);
  EXPECT_EQ(reparsed->scheduler_allow_mcs, config.scheduler_allow_mcs);
  EXPECT_EQ(reparsed->scheduler_allow_prb_limits, config.scheduler_allow_prb_limits);
  EXPECT_EQ(reparsed->scheduler_allow_prb_preference, config.scheduler_allow_prb_preference);
  EXPECT_EQ(reparsed->scheduler_allow_prb_assignment, config.scheduler_allow_prb_assignment);
  EXPECT_EQ(reparsed->scheduler_allow_tpc, config.scheduler_allow_tpc);
  EXPECT_EQ(reparsed->scheduler_allow_layers, config.scheduler_allow_layers);
  EXPECT_EQ(reparsed->scheduler_max_mcs_delta_from_host, config.scheduler_max_mcs_delta_from_host);
  EXPECT_EQ(reparsed->class_c_process.ring_slots, config.class_c_process.ring_slots);
  EXPECT_EQ(reparsed->class_c_process.payload_capacity_bytes, config.class_c_process.payload_capacity_bytes);
  EXPECT_EQ(reparsed->class_c_process.startup_timeout, config.class_c_process.startup_timeout);
  EXPECT_EQ(reparsed->class_c_process.stop_timeout, config.class_c_process.stop_timeout);
  EXPECT_EQ(reparsed->class_c_process.restart_backoff, config.class_c_process.restart_backoff);
  EXPECT_EQ(reparsed->class_c_process.heartbeat_period, config.class_c_process.heartbeat_period);
  EXPECT_EQ(reparsed->class_c_process.heartbeat_timeout, config.class_c_process.heartbeat_timeout);
  EXPECT_EQ(reparsed->class_c_process.management_timeout, config.class_c_process.management_timeout);
  EXPECT_EQ(reparsed->class_c_process.maximum_address_space_bytes, config.class_c_process.maximum_address_space_bytes);
  EXPECT_EQ(reparsed->class_c_process.maximum_core_file_bytes, config.class_c_process.maximum_core_file_bytes);
  EXPECT_EQ(reparsed->class_c_process.require_seccomp_filter, config.class_c_process.require_seccomp_filter);
  EXPECT_EQ(reparsed->class_c_process.maximum_open_files, config.class_c_process.maximum_open_files);
  EXPECT_EQ(reparsed->class_c_process.maximum_restart_attempts, config.class_c_process.maximum_restart_attempts);
  EXPECT_EQ(reparsed->quiet_reservation.maximum_retained_prb_slots,
            config.quiet_reservation.maximum_retained_prb_slots);
  EXPECT_EQ(reparsed->quiet_reservation.maximum_prbs_per_slot, config.quiet_reservation.maximum_prbs_per_slot);
  EXPECT_EQ(reparsed->quiet_reservation.duty_cycle_window_slots, config.quiet_reservation.duty_cycle_window_slots);
  EXPECT_EQ(reparsed->quiet_reservation.maximum_prb_slots_per_window,
            config.quiet_reservation.maximum_prb_slots_per_window);
  EXPECT_EQ(reparsed->quiet_reservation.maximum_source_prb_slots_per_window,
            config.quiet_reservation.maximum_source_prb_slots_per_window);
  ASSERT_TRUE(reparsed->local_e3);
  EXPECT_EQ(reparsed->local_e3->socket_path, config.local_e3->socket_path);
  EXPECT_EQ(reparsed->local_e3->allowed_uid, config.local_e3->allowed_uid);
  EXPECT_EQ(reparsed->local_e3->request_timeout, config.local_e3->request_timeout);
  ASSERT_TRUE(reparsed->portable_e3_data);
  EXPECT_EQ(reparsed->portable_e3_data->maximum_subscriptions, config.portable_e3_data->maximum_subscriptions);
  EXPECT_EQ(reparsed->portable_e3_data->maximum_retained_bytes, config.portable_e3_data->maximum_retained_bytes);
  EXPECT_EQ(reparsed->portable_e3_data->maximum_object_bytes, config.portable_e3_data->maximum_object_bytes);
  EXPECT_EQ(reparsed->portable_e3_data->maximum_metadata_bytes, config.portable_e3_data->maximum_metadata_bytes);
  EXPECT_EQ(reparsed->portable_e3_data->minimum_chunk_bytes, config.portable_e3_data->minimum_chunk_bytes);
  EXPECT_EQ(reparsed->portable_e3_data->maximum_chunk_bytes, config.portable_e3_data->maximum_chunk_bytes);
  EXPECT_EQ(reparsed->portable_e3_data->minimum_lease_us, config.portable_e3_data->minimum_lease_us);
  EXPECT_EQ(reparsed->portable_e3_data->maximum_lease_us, config.portable_e3_data->maximum_lease_us);
}

TEST(dapp_config, structured_initial_instances_have_explicit_and_bounded_defaults)
{
  auto parsed = parse_config("dapp:\n"
                             "  catalog_root: /tmp\n"
                             "  native_preflight_helper: /bin/true\n"
                             "  package_signature_mode: allow_unsigned\n"
                             "  minimum_trust_policy_epoch: 0\n"
                             "  package_manifests: [native.json, process.json]\n"
                             "  initial_instances:\n"
                             "    - package_manifest: native.json\n"
                             "    - package_manifest: process.json\n"
                             "      placement: out_of_process\n"
                             "      backend: cuda\n"
                             "      target: shadow\n"
                             "      lifecycle_timeout_ms: 2500\n");
  ASSERT_TRUE(parsed);
  ASSERT_EQ(parsed->initial_instances.size(), 2U);
  EXPECT_EQ(parsed->initial_instances[0].package_manifest, "native.json");
  EXPECT_EQ(parsed->initial_instances[0].placement, OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1);
  EXPECT_EQ(parsed->initial_instances[0].backend, dapp::management::backend::cpu);
  EXPECT_EQ(parsed->initial_instances[0].target, dapp_initial_instance_target::active);
  EXPECT_EQ(parsed->initial_instances[0].lifecycle_timeout, std::chrono::seconds(1));
  EXPECT_EQ(parsed->initial_instances[1].placement, OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1);
  EXPECT_EQ(parsed->initial_instances[1].backend, dapp::management::backend::cuda);
  EXPECT_EQ(parsed->initial_instances[1].target, dapp_initial_instance_target::shadow);
  EXPECT_EQ(parsed->initial_instances[1].lifecycle_timeout, std::chrono::milliseconds(2500));

  EXPECT_THROW(parse_config("dapp:\n"
                            "  catalog_root: /tmp\n"
                            "  native_preflight_helper: /bin/true\n"
                            "  initial_instances:\n"
                            "    - package_manifest: native.json\n"
                            "      unexpected: true\n"),
               CLI::Error);
  EXPECT_THROW(parse_config("dapp:\n"
                            "  catalog_root: /tmp\n"
                            "  native_preflight_helper: /bin/true\n"
                            "  initial_instances:\n"
                            "    - package_manifest: native.json\n"
                            "      placement: remote\n"),
               std::runtime_error);
}

TEST(dapp_config, validator_rejects_invalid_catalog_and_resource_bounds)
{
  auto config         = valid_config();
  config.catalog_root = "/directory/that/cannot/be/a/dapp/catalog";
  EXPECT_FALSE(validate_dapp_service_config(config));

  config                       = valid_config();
  config.maximum_retained_jobs = config.maximum_queued_jobs - 1;
  EXPECT_FALSE(validate_dapp_service_config(config));

  config              = valid_config();
  config.backend_mask = 0;
  EXPECT_FALSE(validate_dapp_service_config(config));

  config                         = valid_config();
  config.native_preflight_helper = "relative/helper";
  EXPECT_FALSE(validate_dapp_service_config(config));

  config                          = valid_config();
  config.native_preflight_timeout = std::chrono::milliseconds::zero();
  EXPECT_FALSE(validate_dapp_service_config(config));

  config                   = valid_config();
  config.package_manifests = {"../outside.json"};
  EXPECT_FALSE(validate_dapp_service_config(config));

  config.package_manifests = {"package.json", "package.json"};
  EXPECT_FALSE(validate_dapp_service_config(config));

  config                   = valid_config();
  config.package_manifests = {"package.json"};
  config.initial_instances = {{"missing.json"}};
  EXPECT_FALSE(validate_dapp_service_config(config));

  config.initial_instances = {{"package.json"}, {"package.json"}};
  EXPECT_FALSE(validate_dapp_service_config(config));

  config.initial_instances                      = {{"package.json"}};
  config.initial_instances[0].lifecycle_timeout = std::chrono::milliseconds::zero();
  EXPECT_FALSE(validate_dapp_service_config(config));

  config                                        = valid_config();
  config.class_c_process.ring_slots             = 64;
  config.class_c_process.payload_capacity_bytes = 64 * 1024 * 1024;
  EXPECT_FALSE(validate_dapp_service_config(config));

  config                                    = valid_config();
  config.class_c_process.maximum_open_files = 8;
  EXPECT_FALSE(validate_dapp_service_config(config));

  config                                             = valid_config();
  config.class_c_process.maximum_address_space_bytes = 32 * 1024 * 1024;
  EXPECT_FALSE(validate_dapp_service_config(config));

  config                                                = valid_config();
  config.quiet_reservation.maximum_prbs_per_slot        = 4;
  config.quiet_reservation.duty_cycle_window_slots      = 8;
  config.quiet_reservation.maximum_prb_slots_per_window = 33;
  EXPECT_FALSE(validate_dapp_service_config(config));

  config                                                = valid_config();
  config.quiet_reservation.maximum_retained_prb_slots   = 32;
  config.quiet_reservation.maximum_prb_slots_per_window = 64;
  EXPECT_FALSE(validate_dapp_service_config(config));

  config = valid_config();
  config.quiet_reservation.maximum_source_prb_slots_per_window =
      config.quiet_reservation.maximum_prb_slots_per_window + 1;
  EXPECT_FALSE(validate_dapp_service_config(config));
}

TEST(dapp_config, signature_policy_defaults_fail_closed_and_unsigned_development_is_explicit)
{
  auto parsed = parse_config("dapp:\n"
                             "  catalog_root: /tmp\n"
                             "  native_preflight_helper: /bin/true\n"
                             "  package_signature_mode: allow_unsigned\n"
                             "  minimum_trust_policy_epoch: 0\n");
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->package_signatures.mode, dapp::package_signature_mode::allow_unsigned);
  EXPECT_EQ(parsed->package_signatures.minimum_policy_epoch, 0U);

  auto config               = valid_config();
  config.package_signatures = {};
  EXPECT_FALSE(validate_dapp_service_config(config));
  config.package_signatures.mode = dapp::package_signature_mode::allow_unsigned;
  EXPECT_FALSE(validate_dapp_service_config(config));
  config.package_signatures.minimum_policy_epoch = 0;
  EXPECT_TRUE(validate_dapp_service_config(config));
  config.package_signatures.trust_store_path     = "relative/trust.json";
  config.package_signatures.minimum_policy_epoch = 1;
  EXPECT_FALSE(validate_dapp_service_config(config));
}

TEST(dapp_config, validator_accepts_local_e3_only_when_its_codec_is_built)
{
  auto config     = valid_config();
  config.local_e3 = dapp_local_e3_config{"/tmp/ocudu-dapp-management.sock", {}, std::chrono::milliseconds(500)};
#ifdef OCUDU_DAPP_TEST_LOCAL_E3
  EXPECT_TRUE(validate_dapp_service_config(config));
  config.portable_e3_data = dapp_portable_e3_data_config{};
  EXPECT_TRUE(validate_dapp_service_config(config));
  config.portable_e3_data->maximum_chunk_bytes = 512;
  EXPECT_FALSE(validate_dapp_service_config(config));
#else
  EXPECT_FALSE(validate_dapp_service_config(config));
  config.portable_e3_data = dapp_portable_e3_data_config{};
  EXPECT_FALSE(validate_dapp_service_config(config));
#endif
}
