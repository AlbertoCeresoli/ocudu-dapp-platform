// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/native_preflight.h"
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>
#include <unistd.h>

using namespace ocudu::dapp;
using namespace std::chrono_literals;

namespace {

native_preflight_result
preflight(const char* path, std::chrono::milliseconds timeout = 500ms, const char* helper = DAPP_PREFLIGHT_HELPER)
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              inspected = verifier.inspect(path);
  if (!inspected) {
    return {native_preflight_error::invalid_request, 0, 0, inspected.detail};
  }
  auto approved = verifier.verify(path, inspected.digest);
  if (!approved) {
    return {native_preflight_error::invalid_request, 0, 0, approved.detail};
  }
  ocudu_dapp_host_info_v1 host{};
  host.struct_size       = sizeof(host);
  host.abi_major         = OCUDU_DAPP_ABI_MAJOR_V1;
  host.abi_minor         = OCUDU_DAPP_ABI_MINOR_V1;
  host.host_capabilities = 7;
  host.backend_mask      = 3;
  host.host_build_id[0]  = 11;
  host.host_build_id[1]  = 13;
  return preflight_native_module(*approved.artifact, host, helper, timeout);
}

} // namespace

TEST(dapp_native_preflight, admits_a_valid_descriptor_in_a_disposable_process)
{
  EXPECT_TRUE(preflight(DAPP_VALID_FIXTURE));
  EXPECT_TRUE(preflight(DAPP_V1_PRE_PROFILE_FIXTURE));
}

TEST(dapp_native_preflight, passes_only_the_artifact_descriptor_and_an_allowlisted_environment)
{
  ASSERT_EQ(::setenv("OCUDU_PREFLIGHT_TEST_SECRET", "must-not-cross", 1), 0);
  const int source = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  ASSERT_GE(source, 0);
  const int inherited = ::fcntl(source, F_DUPFD_CLOEXEC, 64);
  ASSERT_GE(inherited, 64);
  ASSERT_EQ(::close(source), 0);
  EXPECT_TRUE(preflight(DAPP_SANDBOX_PROBE_FIXTURE));
  EXPECT_EQ(::close(inherited), 0);
  EXPECT_EQ(::unsetenv("OCUDU_PREFLIGHT_TEST_SECRET"), 0);
}

TEST(dapp_native_preflight, timeout_terminates_the_entire_isolated_process_group)
{
  static_cast<void>(::unlink(DAPP_PREFLIGHT_PROCESS_TREE_PID_FILE));
  const auto result = preflight(DAPP_VALID_FIXTURE, 200ms, DAPP_PREFLIGHT_PROCESS_TREE_HELPER);
  ASSERT_EQ(result.error, native_preflight_error::timeout);

  std::ifstream pid_stream(DAPP_PREFLIGHT_PROCESS_TREE_PID_FILE);
  pid_t         descendant{};
  ASSERT_TRUE(pid_stream >> descendant);
  ASSERT_GT(descendant, 1);
  bool gone = false;
  for (unsigned attempt = 0; attempt != 100; ++attempt) {
    errno = 0;
    if (::kill(descendant, 0) != 0 && errno == ESRCH) {
      gone = true;
      break;
    }
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_TRUE(gone) << "preflight descendant survived group timeout: " << descendant;
  static_cast<void>(::unlink(DAPP_PREFLIGHT_PROCESS_TREE_PID_FILE));
}

TEST(dapp_native_preflight, maps_query_and_descriptor_rejections_without_loading_them_in_the_host)
{
  const std::array cases = {std::pair{DAPP_MISSING_QUERY_FIXTURE, native_preflight_stage::query_symbol_missing},
                            std::pair{DAPP_THROWING_QUERY_FIXTURE, native_preflight_stage::query_threw},
                            std::pair{DAPP_REJECTED_QUERY_FIXTURE, native_preflight_stage::query_rejected},
                            std::pair{DAPP_INVALID_DESCRIPTOR_FIXTURE, native_preflight_stage::descriptor_invalid}};
  for (const auto& [path, expected_stage] : cases) {
    const auto result = preflight(path);
    EXPECT_EQ(result.error, native_preflight_error::helper_rejected) << path;
    EXPECT_EQ(result.helper_status, static_cast<int>(expected_stage)) << path;
  }
}

TEST(dapp_native_preflight, contains_a_query_crash_and_bounds_a_query_hang)
{
  auto result = preflight(DAPP_CRASHING_QUERY_FIXTURE);
#if defined(__SANITIZE_ADDRESS__)
  // ASan converts the helper's fatal signal into a nonzero process exit after reporting it.
  EXPECT_TRUE((result.error == native_preflight_error::helper_signaled && result.signal == SIGSEGV) ||
              (result.error == native_preflight_error::helper_rejected && result.helper_status != 0));
#else
  EXPECT_EQ(result.error, native_preflight_error::helper_signaled);
  EXPECT_EQ(result.signal, SIGSEGV);
#endif

  result = preflight(DAPP_HANGING_QUERY_FIXTURE, 20ms);
  EXPECT_EQ(result.error, native_preflight_error::timeout);
  EXPECT_EQ(result.signal, SIGKILL);
}

TEST(dapp_native_preflight, executes_lifecycle_and_worker_construction_before_host_admission)
{
  auto result = preflight(DAPP_THROWING_CREATE_FIXTURE);
  EXPECT_EQ(result.error, native_preflight_error::helper_rejected);
  EXPECT_EQ(result.helper_status, static_cast<int>(native_preflight_stage::create_threw));

  result = preflight(DAPP_THROWING_WORKER_FIXTURE);
  EXPECT_EQ(result.error, native_preflight_error::helper_rejected);
  EXPECT_EQ(result.helper_status, static_cast<int>(native_preflight_stage::worker_create_threw));
  EXPECT_NE(result.detail.find("worker create threw"), std::string::npos);

  result = preflight(DAPP_THROWING_INVOKE_FIXTURE);
  EXPECT_EQ(result.error, native_preflight_error::helper_rejected);
  EXPECT_EQ(result.helper_status, static_cast<int>(native_preflight_stage::invalid_vector_threw));
}

TEST(dapp_native_preflight, contains_a_lifecycle_crash_and_bounds_a_lifecycle_hang)
{
  auto result = preflight(DAPP_CRASHING_PREPARE_FIXTURE);
#if defined(__SANITIZE_ADDRESS__)
  EXPECT_TRUE((result.error == native_preflight_error::helper_signaled && result.signal == SIGSEGV) ||
              (result.error == native_preflight_error::helper_rejected && result.helper_status != 0));
#else
  EXPECT_EQ(result.error, native_preflight_error::helper_signaled);
  EXPECT_EQ(result.signal, SIGSEGV);
#endif

  result = preflight(DAPP_HANGING_START_FIXTURE, 20ms);
  EXPECT_EQ(result.error, native_preflight_error::timeout);
  EXPECT_EQ(result.signal, SIGKILL);
}
