// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "apps/du/du_appconfig.h"
#include "apps/du/du_appconfig_cli11_schema.h"
#include "apps/du/du_appconfig_yaml_writer.h"
#include "apps/units/flexible_o_du/flexible_o_du_application_unit.h"
#include "yaml_roundtrip_test_helpers.h"
#include "ocudu/support/config_parsers.h"
#include "CLI/CLI11.hpp"

#ifndef CONFIGS_DIR
#error "CONFIGS_DIR must be defined"
#endif

using namespace ocudu;
using namespace ocudu::yaml_roundtrip_test;

namespace {

YAML::Node load_and_emit(const std::string& yaml_text)
{
  temp_yaml_file tmp(yaml_text);

  CLI::App app("du yaml-roundtrip-test");
  app.config_formatter(create_yaml_config_parser());
  app.allow_config_extras(CLI::config_extras_mode::error);
  std::string cfg_path;
  app.set_config("-c,", cfg_path, "Read config from file", false);

  du_appconfig du_cfg;
  configure_cli11_with_du_appconfig_schema(app, du_cfg);

  auto o_du = create_flexible_o_du_application_unit("du");
  o_du->on_parsing_configuration_registration(app);

  app.callback([&]() { o_du->on_configuration_parameters_autoderivation(app); });

  std::vector<const char*> argv = {"du", "-c", tmp.path().c_str()};
  app.parse(static_cast<int>(argv.size()), argv.data());

  YAML::Node node;
  fill_du_appconfig_in_yaml_schema(node, du_cfg);
  o_du->dump_config(node);
  return node;
}

YAML::Node emit_defaults()
{
  du_appconfig du_cfg;
  auto         o_du = create_flexible_o_du_application_unit("du");
  YAML::Node   node;
  fill_du_appconfig_in_yaml_schema(node, du_cfg);
  o_du->dump_config(node);
  return node;
}

const std::string CONFIGS = CONFIGS_DIR;

class du_example_config_test : public ::testing::TestWithParam<std::string>
{};

TEST_P(du_example_config_test, roundtrip)
{
  const std::string& name = GetParam();
  assert_roundtrip(read_file(CONFIGS + "/" + name), &load_and_emit, name);
}

INSTANTIATE_TEST_SUITE_P(,
                         du_example_config_test,
                         ::testing::Values("du_rf_b200_tdd_n78_20mhz.yml", "du_f1u_multiple_sockets.yml"));

TEST(du_default_config_test, roundtrip)
{
  YAML::Node a = emit_defaults();
  assert_roundtrip(YAML::Dump(a), &load_and_emit, "du defaults");
}

#ifdef OCUDU_FLEXIBLE_DU_DAPP
TEST(du_dapp_config_test, roundtrip)
{
  std::string config = read_file(CONFIGS + "/du_rf_b200_tdd_n78_20mhz.yml");
  config += "\ndapp:\n  catalog_root: ";
  config += CONFIGS;
  config += "\n"
            "  native_preflight_helper: /bin/true\n"
            "  max_transactions: 2048\n"
            "  max_queued_jobs: 32\n"
            "  max_retained_jobs: 256\n"
            "  max_scheduler_ul_prbs_avoided: 17\n";
  assert_roundtrip(config, &load_and_emit, "du dApp configuration");
}
#endif

} // namespace
