// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/artifact_digest.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace ocudu::dapp {

namespace management {
struct package_record;
}

enum class package_signature_mode : uint8_t { required, allow_unsigned };

struct package_signature_config {
  package_signature_mode mode{package_signature_mode::required};
  std::string            trust_store_path;
  uint64_t               minimum_policy_epoch{1};
};

class package_signature_verifier;

struct package_signature_verifier_result {
  std::unique_ptr<package_signature_verifier> verifier;
  std::string                                 detail;

  explicit operator bool() const noexcept { return verifier != nullptr; }
};

struct package_signature_result {
  bool        accepted{};
  std::string detail;

  explicit operator bool() const noexcept { return accepted; }
};

/// Immutable startup verifier for detached dApp package signatures.
class package_signature_verifier
{
public:
  static package_signature_verifier_result create(package_signature_config config) noexcept;

  ~package_signature_verifier();
  package_signature_verifier(package_signature_verifier&&) noexcept;
  package_signature_verifier& operator=(package_signature_verifier&&) noexcept;

  package_signature_verifier(const package_signature_verifier&)            = delete;
  package_signature_verifier& operator=(const package_signature_verifier&) = delete;

  package_signature_result verify(const std::string&          catalog_root,
                                  const std::string&          relative_manifest_path,
                                  const uint8_t*              manifest_bytes,
                                  std::size_t                 manifest_size,
                                  management::package_record& package) const noexcept;

  uint64_t policy_epoch() const noexcept;

private:
  struct implementation;
  explicit package_signature_verifier(std::unique_ptr<implementation> state_) noexcept;

  std::unique_ptr<implementation> state;
};

/// Domain-separated bytes hashed and signed by the v1 package profile.
artifact_digest package_signature_digest(const artifact_digest& manifest_digest, uint64_t package_sequence) noexcept;

} // namespace ocudu::dapp
