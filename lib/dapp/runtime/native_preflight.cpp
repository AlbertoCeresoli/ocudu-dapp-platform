// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/native_preflight.h"
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace ocudu::dapp;

namespace {

constexpr int child_artifact_descriptor = 3;

std::string error_detail(const char* operation, int error)
{
  return std::string(operation) + ": " + std::strerror(error);
}

std::vector<std::string> preflight_environment()
{
  std::vector<std::string> environment{"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                                       "LANG=C",
                                       "LC_ALL=C",
                                       "CUDA_CACHE_DISABLE=1"};
  // Device selection and lazy/eager loading affect correctness. Loader injection,
  // library paths, HOME, credentials, and arbitrary service state do not cross
  // this boundary; packages must carry normal RPATH/dependency declarations.
  constexpr std::array<const char*, 4> allowed = {
      "CUDA_VISIBLE_DEVICES", "CUDA_DEVICE_ORDER", "CUDA_MODULE_LOADING", "NVIDIA_VISIBLE_DEVICES"};
  for (const char* name : allowed) {
    if (const char* value = std::getenv(name); value != nullptr) {
      environment.emplace_back(std::string(name) + "=" + value);
    }
  }
  return environment;
}

void kill_preflight_group(pid_t leader) noexcept
{
  if (::kill(-leader, SIGKILL) != 0 && errno == ESRCH) {
    static_cast<void>(::kill(leader, SIGKILL));
  }
}

} // namespace

native_preflight_result ocudu::dapp::preflight_native_module(const approved_artifact&       artifact,
                                                             const ocudu_dapp_host_info_v1& host,
                                                             const std::string&             helper_executable,
                                                             std::chrono::milliseconds      timeout)
{
  if (artifact.native_descriptor() < 0 || helper_executable.empty() || timeout.count() <= 0 ||
      host.struct_size < offsetof(ocudu_dapp_host_info_v1, reserved) || host.abi_major != OCUDU_DAPP_ABI_MAJOR_V1) {
    return {native_preflight_error::invalid_request, 0, 0, "invalid native preflight request"};
  }

  posix_spawn_file_actions_t actions;
  int                        error = ::posix_spawn_file_actions_init(&actions);
  if (error != 0) {
    return {native_preflight_error::spawn_failed, error, 0, error_detail("preflight file actions", error)};
  }
  struct actions_guard {
    posix_spawn_file_actions_t* actions;
    ~actions_guard() { ::posix_spawn_file_actions_destroy(actions); }
  } guard{&actions};
  error = ::posix_spawn_file_actions_adddup2(&actions, artifact.native_descriptor(), child_artifact_descriptor);
  if (error != 0) {
    return {native_preflight_error::spawn_failed, error, 0, error_detail("preflight artifact handoff", error)};
  }
  error = ::posix_spawn_file_actions_addclosefrom_np(&actions, child_artifact_descriptor + 1);
  if (error != 0) {
    return {native_preflight_error::spawn_failed, error, 0, error_detail("preflight descriptor isolation", error)};
  }

  posix_spawnattr_t attributes;
  error = ::posix_spawnattr_init(&attributes);
  if (error != 0) {
    return {native_preflight_error::spawn_failed, error, 0, error_detail("preflight spawn attributes", error)};
  }
  struct attributes_guard {
    posix_spawnattr_t* attributes;
    ~attributes_guard() { ::posix_spawnattr_destroy(attributes); }
  } attributes_cleanup{&attributes};
  sigset_t empty_signals;
  ::sigemptyset(&empty_signals);
  constexpr short spawn_flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK;
  if ((error = ::posix_spawnattr_setflags(&attributes, spawn_flags)) != 0 ||
      (error = ::posix_spawnattr_setpgroup(&attributes, 0)) != 0 ||
      (error = ::posix_spawnattr_setsigmask(&attributes, &empty_signals)) != 0) {
    return {native_preflight_error::spawn_failed, error, 0, error_detail("preflight process isolation", error)};
  }

  std::array<std::string, 5> arguments = {helper_executable,
                                          std::to_string(child_artifact_descriptor),
                                          std::to_string(host.host_capabilities),
                                          std::to_string(host.backend_mask),
                                          std::to_string(host.host_build_id[0]) + ":" +
                                              std::to_string(host.host_build_id[1])};
  std::array<char*, 6>       argv{};
  for (std::size_t index = 0; index != arguments.size(); ++index) {
    argv[index] = arguments[index].data();
  }

  auto               environment = preflight_environment();
  std::vector<char*> envp;
  envp.reserve(environment.size() + 1);
  for (auto& entry : environment) {
    envp.push_back(entry.data());
  }
  envp.push_back(nullptr);

  pid_t child{};
  error = ::posix_spawn(&child, helper_executable.c_str(), &actions, &attributes, argv.data(), envp.data());
  if (error != 0) {
    return {native_preflight_error::spawn_failed, error, 0, error_detail("preflight spawn", error)};
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int        status{};
  while (true) {
    siginfo_t child_state{};
    errno                = 0;
    const int wait_error = ::waitid(P_PID, static_cast<id_t>(child), &child_state, WEXITED | WNOHANG | WNOWAIT);
    if (wait_error == 0 && child_state.si_pid == child) {
      // The leader remains waitable (and its PID cannot be reused) while every
      // background process left in the isolated group is terminated.
      kill_preflight_group(child);
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      break;
    }
    if (wait_error != 0 && errno != EINTR) {
      const int saved_error = errno;
      kill_preflight_group(child);
      static_cast<void>(::waitpid(child, &status, 0));
      return {native_preflight_error::spawn_failed, saved_error, 0, error_detail("preflight wait", saved_error)};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      kill_preflight_group(child);
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      return {native_preflight_error::timeout, 0, SIGKILL, "native preflight helper timed out"};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (WIFSIGNALED(status)) {
    return {native_preflight_error::helper_signaled,
            0,
            WTERMSIG(status),
            "native preflight helper terminated by signal " + std::to_string(WTERMSIG(status))};
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    const int helper_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {native_preflight_error::helper_rejected,
            helper_status,
            0,
            "native preflight helper rejected the module at " +
                std::string(to_string(static_cast<native_preflight_stage>(helper_status))) + " (exit " +
                std::to_string(helper_status) + ")"};
  }
  return {native_preflight_error::none, 0, 0, {}};
}

const char* ocudu::dapp::to_string(native_preflight_error error) noexcept
{
  static constexpr const char* names[] = {
      "none", "invalid request", "spawn failed", "timeout", "helper signaled", "helper rejected"};
  const auto index = static_cast<unsigned>(error);
  return index < std::size(names) ? names[index] : "unknown native preflight error";
}

const char* ocudu::dapp::to_string(native_preflight_stage stage) noexcept
{
  static constexpr const char* names[] = {"success",
                                          nullptr,
                                          nullptr,
                                          nullptr,
                                          nullptr,
                                          nullptr,
                                          nullptr,
                                          nullptr,
                                          nullptr,
                                          nullptr,
                                          "sandbox setup failed",
                                          "invalid helper arguments",
                                          "dynamic load failed",
                                          "query symbol missing",
                                          "query threw",
                                          "query rejected",
                                          "descriptor invalid",
                                          "create threw",
                                          "create rejected",
                                          "configure threw",
                                          "configure rejected",
                                          "prepare threw",
                                          "prepare rejected",
                                          "warmup threw",
                                          "warmup rejected",
                                          "start threw",
                                          "start rejected",
                                          "worker create threw",
                                          "worker create rejected",
                                          "worker destroy threw",
                                          "stop threw",
                                          "stop rejected",
                                          "quiesce threw",
                                          "quiesce rejected",
                                          "destroy threw",
                                          "invalid-vector callback threw",
                                          "invalid vector was accepted"};
  const auto                   index   = static_cast<unsigned>(stage);
  return index < std::size(names) && names[index] != nullptr ? names[index] : "unknown preflight stage";
}
