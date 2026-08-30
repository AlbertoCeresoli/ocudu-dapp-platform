// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/management/types.h"
#include <optional>
#include <string>

namespace ocudu::dapp {

class package_signature_verifier;

struct package_manifest_result {
  std::optional<management::package_record> package;
  std::string                               detail;

  explicit operator bool() const noexcept { return package.has_value(); }
};

/// Reads one bounded, non-writable JSON manifest below the configured catalog root.
package_manifest_result read_package_manifest(const std::string&                catalog_root,
                                              const std::string&                relative_manifest_path,
                                              const package_signature_verifier* signature_verifier = nullptr) noexcept;

} // namespace ocudu::dapp
