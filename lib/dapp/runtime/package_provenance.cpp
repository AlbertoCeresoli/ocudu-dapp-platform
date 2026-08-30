// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/package_provenance.h"
#include "bounded_json.h"
#include "ocudu/dapp/runtime/artifact_verifier.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace ocudu::dapp;
using namespace ocudu::dapp::management;

namespace {

using json = nlohmann::json;

constexpr std::size_t maximum_sbom_bytes = 4 * 1024 * 1024;
constexpr int         maximum_sbom_depth = 24;
constexpr std::size_t maximum_spdx_items = 4096;

struct fd_guard {
  int value{-1};
  ~fd_guard()
  {
    if (value >= 0) {
      ::close(value);
    }
  }
};

bool path_is_below(const std::filesystem::path& child, const std::filesystem::path& parent)
{
  auto child_it = child.begin();
  for (auto parent_it = parent.begin(); parent_it != parent.end(); ++parent_it, ++child_it) {
    if (child_it == child.end() || *child_it != *parent_it) {
      return false;
    }
  }
  return child != parent;
}

bool bounded_string(const json& object, const char* field, std::size_t maximum)
{
  return object.contains(field) && object.at(field).is_string() &&
         !object.at(field).get_ref<const std::string&>().empty() &&
         object.at(field).get_ref<const std::string&>().size() <= maximum;
}

bool valid_document_header(const json& document)
{
  if (!document.is_object() || !bounded_string(document, "name", 256) ||
      !bounded_string(document, "documentNamespace", 512) || document.value("spdxVersion", "") != "SPDX-2.3" ||
      document.value("dataLicense", "") != "CC0-1.0" || document.value("SPDXID", "") != "SPDXRef-DOCUMENT" ||
      !document.contains("creationInfo") || !document.at("creationInfo").is_object()) {
    return false;
  }
  const auto& creation = document.at("creationInfo");
  if (!bounded_string(creation, "created", 64) || !creation.contains("creators") ||
      !creation.at("creators").is_array() || creation.at("creators").empty() || creation.at("creators").size() > 16) {
    return false;
  }
  return std::all_of(creation.at("creators").begin(), creation.at("creators").end(), [](const json& creator) {
    return creator.is_string() && !creator.get_ref<const std::string&>().empty() &&
           creator.get_ref<const std::string&>().size() <= 256;
  });
}

bool valid_bound_package(const json& document, const package_record& package)
{
  if (!document.contains("packages") || !document.at("packages").is_array() || document.at("packages").empty() ||
      document.at("packages").size() > maximum_spdx_items) {
    return false;
  }
  std::set<std::string> ids;
  std::size_t           matches = 0;
  for (const auto& item : document.at("packages")) {
    if (!item.is_object() || !bounded_string(item, "SPDXID", 128) ||
        !ids.insert(item.at("SPDXID").get<std::string>()).second) {
      return false;
    }
    if (item.at("SPDXID") != "SPDXRef-Package") {
      continue;
    }
    if (item.value("name", "") != package.global_name || item.value("versionInfo", "") != package.version ||
        item.value("supplier", "") != "Organization: " + package.vendor || !item.value("filesAnalyzed", false) ||
        !item.contains("hasFiles") || !item.at("hasFiles").is_array() ||
        std::count(item.at("hasFiles").begin(), item.at("hasFiles").end(), "SPDXRef-Artifact") != 1) {
      return false;
    }
    ++matches;
  }
  return matches == 1;
}

bool valid_bound_file(const json& document, const package_record& package, const std::filesystem::path& root)
{
  if (!document.contains("files") || !document.at("files").is_array() || document.at("files").empty() ||
      document.at("files").size() > maximum_spdx_items) {
    return false;
  }
  std::error_code error;
  const auto      relative_artifact = std::filesystem::relative(package.canonical_artifact_path, root, error);
  if (error || relative_artifact.empty() || *relative_artifact.begin() == "..") {
    return false;
  }
  const std::string     expected_name   = "./" + relative_artifact.generic_string();
  const std::string     expected_digest = to_hex(package.content_digest);
  std::set<std::string> ids;
  std::size_t           matches = 0;
  for (const auto& item : document.at("files")) {
    if (!item.is_object() || !bounded_string(item, "SPDXID", 128) ||
        !ids.insert(item.at("SPDXID").get<std::string>()).second) {
      return false;
    }
    if (item.at("SPDXID") != "SPDXRef-Artifact") {
      continue;
    }
    if (item.value("fileName", "") != expected_name || !item.contains("checksums") ||
        !item.at("checksums").is_array() || item.at("checksums").empty() || item.at("checksums").size() > 8) {
      return false;
    }
    const auto& checksums  = item.at("checksums");
    const bool  has_digest = std::any_of(checksums.begin(), checksums.end(), [&](const json& checksum) {
      return checksum.is_object() && checksum.value("algorithm", "") == "SHA256" &&
             checksum.value("checksumValue", "") == expected_digest;
    });
    if (!has_digest) {
      return false;
    }
    ++matches;
  }
  return matches == 1;
}

bool valid_describes_relationship(const json& document)
{
  if (!document.contains("relationships") || !document.at("relationships").is_array() ||
      document.at("relationships").size() > maximum_spdx_items) {
    return false;
  }
  return std::any_of(document.at("relationships").begin(), document.at("relationships").end(), [](const json& item) {
    return item.is_object() && item.value("spdxElementId", "") == "SPDXRef-DOCUMENT" &&
           item.value("relationshipType", "") == "DESCRIBES" &&
           item.value("relatedSpdxElement", "") == "SPDXRef-Package";
  });
}

} // namespace

package_provenance_result ocudu::dapp::verify_package_provenance(const std::string&    catalog_root,
                                                                 const package_record& package) noexcept
{
  try {
    if (package.manifest_schema_version != 1 || package.canonical_sbom_path.empty()) {
      return {false, "manifest-v1 package is missing its SPDX SBOM declaration"};
    }
    std::error_code error;
    const auto      root      = std::filesystem::canonical(catalog_root, error);
    const auto      candidate = std::filesystem::path(package.canonical_sbom_path).lexically_normal();
    if (error || !std::filesystem::is_directory(root) || !candidate.is_absolute() ||
        candidate != package.canonical_sbom_path || !path_is_below(candidate, root)) {
      return {false, "SBOM path or catalog root is invalid"};
    }
    const auto canonical = std::filesystem::canonical(candidate, error);
    if (error || canonical != candidate || !path_is_below(canonical, root)) {
      return {false, "SBOM path contains a symbolic link or escapes the catalog"};
    }
    struct stat link_status {};
    if (::lstat(candidate.c_str(), &link_status) != 0 || S_ISLNK(link_status.st_mode)) {
      return {false, "SBOM must be an existing non-symbolic file"};
    }
    fd_guard descriptor{::open(candidate.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (descriptor.value < 0) {
      return {false, std::string("unable to open SBOM: ") + std::strerror(errno)};
    }
    struct stat status {};
    if (::fstat(descriptor.value, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0 ||
        static_cast<uint64_t>(status.st_size) > maximum_sbom_bytes ||
        (status.st_uid != ::geteuid() && status.st_uid != 0) || (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      return {false, "SBOM ownership, mode, type, or size is not admissible"};
    }
    const auto held = std::filesystem::canonical("/proc/self/fd/" + std::to_string(descriptor.value), error);
    if (error || held != candidate) {
      return {false, "opened SBOM identity changed or left the catalog"};
    }
    std::vector<uint8_t> bytes(static_cast<std::size_t>(status.st_size));
    std::size_t          offset = 0;
    while (offset != bytes.size()) {
      const auto count = ::pread(descriptor.value, bytes.data() + offset, bytes.size() - offset, offset);
      if (count <= 0) {
        return {false, "unable to read complete SBOM"};
      }
      offset += static_cast<std::size_t>(count);
    }
    if (sha256_bytes(bytes.data(), bytes.size()) != package.sbom_digest) {
      return {false, "SBOM SHA-256 differs from the signed manifest"};
    }

    auto parsed = ocudu::dapp::detail::parse_bounded_json(bytes.begin(), bytes.end(), maximum_sbom_depth);
    if (!parsed.document) {
      return {false, "SBOM is malformed, duplicated, or too deeply nested"};
    }
    const auto& document = *parsed.document;
    if (!valid_document_header(document) || !valid_bound_package(document, package) ||
        !valid_bound_file(document, package, root) || !valid_describes_relationship(document)) {
      return {false, "SPDX SBOM does not bind the admitted package and artifact"};
    }
    return {true, {}};
  } catch (const std::bad_alloc&) {
    return {false, "SBOM verification exhausted memory"};
  } catch (const std::exception& error) {
    return {false, std::string("SBOM verification failed: ") + error.what()};
  } catch (...) {
    return {false, "unknown SBOM verification failure"};
  }
}
