// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/module_loader.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include <filesystem>
#include <gtest/gtest.h>
#include <sys/stat.h>

using namespace ocudu::dapp;

namespace {

ocudu_dapp_host_info_v1 host_info()
{
  ocudu_dapp_host_info_v1 host{};
  host.struct_size = sizeof(host);
  host.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  host.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  return host;
}

module_load_result inspect_and_load(const char* path)
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              inspected = verifier.inspect(path);
  EXPECT_TRUE(inspected) << inspected.detail;
  if (!inspected) {
    return {module_load_error::dynamic_load_failed, nullptr, inspected.detail};
  }
  auto artifact = verifier.verify(path, inspected.digest);
  EXPECT_TRUE(artifact) << artifact.detail;
  if (!artifact) {
    return {module_load_error::dynamic_load_failed, nullptr, artifact.detail};
  }
  return load_native_module(std::move(*artifact.artifact), host_info());
}

} // namespace

TEST(dapp_module_loader, discovers_valid_module_through_unmangled_local_query)
{
  auto loaded = inspect_and_load(DAPP_VALID_FIXTURE);
  ASSERT_TRUE(loaded) << loaded.detail;
  EXPECT_STREQ(loaded.library->descriptor().global_name, "org.deepsig.ocudu.test.valid");
}

TEST(dapp_module_loader, loads_frozen_v1_module_from_before_use_case_profiles)
{
  auto loaded = inspect_and_load(DAPP_V1_PRE_PROFILE_FIXTURE);
  ASSERT_TRUE(loaded) << loaded.detail;
  const auto& module = loaded.library->descriptor();
  ASSERT_EQ(module.nof_interfaces, 1U);
  EXPECT_STREQ(module.global_name, "org.deepsig.ocudu.test.v1-pre-profile");
  EXPECT_EQ(module.interfaces[0].use_case_profile, nullptr);

  ocudu_dapp_host_api_v1 host{};
  void*                  instance = nullptr;
  ASSERT_EQ(module.create(&host, nullptr, &instance), OCUDU_DAPP_OK_V1);
  ASSERT_NE(instance, nullptr);
  EXPECT_EQ(module.configure(instance, nullptr), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(module.prepare(instance, nullptr), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(module.warmup(instance, nullptr), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(module.start(instance), OCUDU_DAPP_OK_V1);

  const auto& spectrum = *static_cast<const ocudu_dapp_spectrum_interface_v1*>(module.interfaces[0].interface_vtable);
  void*       worker   = nullptr;
  ASSERT_EQ(spectrum.worker.create_worker_context(instance, 7, &worker), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(spectrum.publish(worker, nullptr), OCUDU_DAPP_INVALID_INPUT_V1);
  spectrum.worker.destroy_worker_context(instance, worker);

  EXPECT_EQ(module.stop(instance), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(module.quiesce(instance, 1), OCUDU_DAPP_OK_V1);
  module.destroy(instance);
}

TEST(dapp_module_loader, rejects_library_without_query_symbol)
{
  auto loaded = inspect_and_load(DAPP_MISSING_QUERY_FIXTURE);
  EXPECT_EQ(loaded.error, module_load_error::missing_query_symbol);
}

TEST(dapp_module_loader, retained_link_map_cannot_alias_a_later_verified_inode)
{
  {
    auto loaded = inspect_and_load(DAPP_VALID_FIXTURE);
    ASSERT_TRUE(loaded) << loaded.detail;
  }
  auto different = inspect_and_load(DAPP_MISSING_QUERY_FIXTURE);
  EXPECT_EQ(different.error, module_load_error::missing_query_symbol);
}

TEST(dapp_module_loader, contains_exception_from_nonconforming_query)
{
  auto loaded = inspect_and_load(DAPP_THROWING_QUERY_FIXTURE);
  EXPECT_EQ(loaded.error, module_load_error::query_exception);
}

TEST(dapp_module_loader, propagates_query_rejection_without_admission)
{
  auto loaded = inspect_and_load(DAPP_REJECTED_QUERY_FIXTURE);
  EXPECT_EQ(loaded.error, module_load_error::query_rejected);
}

TEST(dapp_module_loader, rejects_invalid_descriptor)
{
  auto loaded = inspect_and_load(DAPP_INVALID_DESCRIPTOR_FIXTURE);
  EXPECT_EQ(loaded.error, module_load_error::invalid_descriptor);
}

TEST(dapp_module_loader, held_inode_prevents_path_substitution_after_verification)
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  const auto        live = std::filesystem::path(DAPP_TEST_CATALOG) / "swap-fixture.so";
  const auto        old  = std::filesystem::path(DAPP_TEST_CATALOG) / "swap-fixture.old";
  std::filesystem::copy_file(DAPP_VALID_FIXTURE, live, std::filesystem::copy_options::overwrite_existing);
  ASSERT_EQ(::chmod(live.c_str(), 0555), 0);
  auto inspected = verifier.inspect(live.string());
  ASSERT_TRUE(inspected) << inspected.detail;
  auto approved = verifier.verify(live.string(), inspected.digest);
  ASSERT_TRUE(approved) << approved.detail;

  std::error_code error;
  std::filesystem::remove(old, error);
  std::filesystem::rename(live, old);
  std::filesystem::copy_file(DAPP_MISSING_QUERY_FIXTURE, live);

  auto loaded = load_native_module(std::move(*approved.artifact), host_info());
  EXPECT_TRUE(loaded) << loaded.detail;
  std::filesystem::remove(live);
  std::filesystem::remove(old);
}
