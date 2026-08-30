// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/artifact_digest.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace ocudu::dapp {

enum class artifact_error {
  none,
  invalid_catalog_root,
  path_outside_catalog,
  open_failed,
  not_regular_file,
  insecure_owner,
  insecure_permissions,
  file_too_large,
  read_failed,
  invalid_elf,
  incompatible_architecture,
  executable_stack,
  digest_mismatch
};

class approved_artifact
{
public:
  approved_artifact(approved_artifact&& other) noexcept;
  approved_artifact& operator=(approved_artifact&& other) noexcept;
  ~approved_artifact();

  approved_artifact(const approved_artifact&)            = delete;
  approved_artifact& operator=(const approved_artifact&) = delete;

  const std::string&     canonical_path() const noexcept { return path; }
  const artifact_digest& digest() const noexcept { return content_digest; }
  uint64_t               size_bytes() const noexcept { return size; }
  /// For the loader only. The descriptor remains owned by this admission capability.
  int native_descriptor() const noexcept { return fd; }

private:
  friend class artifact_verifier;
  approved_artifact(int descriptor, std::string canonical_path, artifact_digest digest_, uint64_t size_) noexcept;

  int             fd{-1};
  std::string     path;
  artifact_digest content_digest{};
  uint64_t        size{};
};

struct artifact_verification_result {
  artifact_error                     error{artifact_error::none};
  std::unique_ptr<approved_artifact> artifact;
  std::string                        detail;

  explicit operator bool() const noexcept { return error == artifact_error::none && artifact != nullptr; }
};

struct artifact_inspection_result {
  artifact_error  error{artifact_error::none};
  artifact_digest digest{};
  uint64_t        size_bytes{};
  std::string     canonical_path;
  std::string     detail;

  explicit operator bool() const noexcept { return error == artifact_error::none; }
};

class artifact_verifier
{
public:
  explicit artifact_verifier(std::string catalog_root_, uint64_t maximum_artifact_bytes = 512ULL * 1024 * 1024);

  const std::string& catalog_root() const noexcept { return root; }

  artifact_verification_result verify(const std::string& candidate, const artifact_digest& expected_digest) const;

  /// Packaging/catalog helper. Admission must still call verify, which hashes the held inode again.
  artifact_inspection_result inspect(const std::string& candidate) const;

private:
  artifact_verification_result inspect_impl(const std::string& candidate, const artifact_digest* expected_digest) const;

  std::string root;
  uint64_t    maximum_bytes;
};

const char*     to_string(artifact_error error) noexcept;
std::string     to_hex(const artifact_digest& digest);
artifact_digest sha256_bytes(const uint8_t* data, std::size_t size) noexcept;

} // namespace ocudu::dapp
