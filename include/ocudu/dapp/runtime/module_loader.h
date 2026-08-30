// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "artifact_verifier.h"
#include "ocudu/dapp/abi/v1/module.h"
#include <memory>
#include <string>

namespace ocudu::dapp {

enum class module_load_error {
  none,
  dynamic_load_failed,
  missing_query_symbol,
  query_exception,
  query_rejected,
  invalid_descriptor
};

struct module_load_result;

class native_module_library
{
public:
  ~native_module_library();
  native_module_library(const native_module_library&)            = delete;
  native_module_library& operator=(const native_module_library&) = delete;

  const ocudu_dapp_module_v1& descriptor() const noexcept { return module; }
  const artifact_digest&      content_digest() const noexcept { return digest; }
  const std::string&          source_path() const noexcept { return path; }

private:
  friend struct module_load_result;
  friend module_load_result load_native_module(approved_artifact&&, const ocudu_dapp_host_info_v1&);
  native_module_library(void*                handle_,
                        ocudu_dapp_module_v1 module_,
                        artifact_digest      digest_,
                        std::string          path_) noexcept;

  void*                handle{};
  ocudu_dapp_module_v1 module{};
  artifact_digest      digest{};
  std::string          path;
};

struct module_load_result {
  module_load_error                      error{module_load_error::none};
  std::shared_ptr<native_module_library> library;
  std::string                            detail;

  explicit operator bool() const noexcept { return error == module_load_error::none && library != nullptr; }
};

module_load_result load_native_module(approved_artifact&& artifact, const ocudu_dapp_host_info_v1& host);
const char*        to_string(module_load_error error) noexcept;

} // namespace ocudu::dapp
