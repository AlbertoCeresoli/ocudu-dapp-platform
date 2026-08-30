// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/management/types.h"
#include <string>

namespace ocudu::dapp {

struct package_provenance_result {
  bool        verified{};
  std::string detail;

  explicit operator bool() const noexcept { return verified; }
};

/// Verifies the digest, file policy and package/artifact identity of a manifest-v1 SPDX SBOM.
package_provenance_result verify_package_provenance(const std::string&                catalog_root,
                                                    const management::package_record& package) noexcept;

} // namespace ocudu::dapp
