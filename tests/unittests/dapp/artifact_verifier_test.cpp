// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/artifact_verifier.h"
#include <filesystem>
#include <gtest/gtest.h>
#include <sys/stat.h>

using namespace ocudu::dapp;

TEST(dapp_artifact_verifier, sha256_matches_standard_known_vector)
{
  static constexpr uint8_t input[] = {'a', 'b', 'c'};
  EXPECT_EQ(to_hex(sha256_bytes(input, sizeof(input))),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(dapp_artifact_verifier, admits_catalog_elf_with_matching_digest)
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              inspected = verifier.inspect(DAPP_VALID_FIXTURE);
  ASSERT_TRUE(inspected) << inspected.detail;
  EXPECT_EQ(to_hex(inspected.digest).size(), 64U);
  auto verified = verifier.verify(DAPP_VALID_FIXTURE, inspected.digest);
  ASSERT_TRUE(verified) << verified.detail;
  EXPECT_EQ(verified.artifact->canonical_path(), std::filesystem::canonical(DAPP_VALID_FIXTURE).string());
}

TEST(dapp_artifact_verifier, rejects_digest_mismatch)
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  artifact_digest   wrong{};
  auto              result = verifier.verify(DAPP_VALID_FIXTURE, wrong);
  EXPECT_EQ(result.error, artifact_error::digest_mismatch);
}

TEST(dapp_artifact_verifier, rejects_path_outside_catalog)
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              result = verifier.inspect("/bin/ls");
  EXPECT_EQ(result.error, artifact_error::path_outside_catalog);
}

TEST(dapp_artifact_verifier, rejects_symbolic_link_even_when_target_is_inside_catalog)
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  const auto        link = std::filesystem::path(DAPP_TEST_CATALOG) / "fixture-link.so";
  std::error_code   error;
  std::filesystem::remove(link, error);
  std::filesystem::create_symlink(DAPP_VALID_FIXTURE, link);
  auto result = verifier.inspect(link.string());
  EXPECT_EQ(result.error, artifact_error::open_failed);
  std::filesystem::remove(link);
}

TEST(dapp_artifact_verifier, rejects_group_or_other_writable_artifact)
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  const auto        copy = std::filesystem::path(DAPP_TEST_CATALOG) / "writable-fixture.so";
  std::filesystem::copy_file(DAPP_VALID_FIXTURE, copy, std::filesystem::copy_options::overwrite_existing);
  ASSERT_EQ(::chmod(copy.c_str(), 0666), 0);
  auto result = verifier.inspect(copy.string());
  EXPECT_EQ(result.error, artifact_error::insecure_permissions);
  std::filesystem::remove(copy);
}

TEST(dapp_artifact_verifier, rejects_executable_stack)
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              result = verifier.inspect(DAPP_EXECUTABLE_STACK_FIXTURE);
  EXPECT_EQ(result.error, artifact_error::executable_stack);
}
