// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "artifact_verifier.h"
#include "ocudu/dapp/abi/v1/module.h"
#include <chrono>
#include <cstdint>
#include <string>

namespace ocudu::dapp {

enum class native_preflight_error { none, invalid_request, spawn_failed, timeout, helper_signaled, helper_rejected };

enum class native_preflight_stage : int {
  success                 = 0,
  sandbox_failed          = 10,
  invalid_arguments       = 11,
  dynamic_load_failed     = 12,
  query_symbol_missing    = 13,
  query_threw             = 14,
  query_rejected          = 15,
  descriptor_invalid      = 16,
  create_threw            = 17,
  create_rejected         = 18,
  configure_threw         = 19,
  configure_rejected      = 20,
  prepare_threw           = 21,
  prepare_rejected        = 22,
  warmup_threw            = 23,
  warmup_rejected         = 24,
  start_threw             = 25,
  start_rejected          = 26,
  worker_create_threw     = 27,
  worker_create_rejected  = 28,
  worker_destroy_threw    = 29,
  stop_threw              = 30,
  stop_rejected           = 31,
  quiesce_threw           = 32,
  quiesce_rejected        = 33,
  destroy_threw           = 34,
  invalid_vector_threw    = 35,
  invalid_vector_accepted = 36
};

struct native_preflight_result {
  native_preflight_error error{native_preflight_error::invalid_request};
  int                    helper_status{};
  int                    signal{};
  std::string            detail;

  explicit operator bool() const noexcept { return error == native_preflight_error::none; }
};

/// Runs the untrusted ELF loader/query/descriptor and null-configuration lifecycle boundaries in a disposable process.
native_preflight_result preflight_native_module(const approved_artifact&       artifact,
                                                const ocudu_dapp_host_info_v1& host,
                                                const std::string&             helper_executable,
                                                std::chrono::milliseconds      timeout);

const char* to_string(native_preflight_error error) noexcept;
const char* to_string(native_preflight_stage stage) noexcept;

} // namespace ocudu::dapp
