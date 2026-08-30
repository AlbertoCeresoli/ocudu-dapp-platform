// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/package_manifest.h"
#include "bounded_json.h"
#include "ocudu/dapp/abi/v1/base.h"
#include "ocudu/dapp/runtime/package_provenance.h"
#include "ocudu/dapp/runtime/package_signature.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace ocudu::dapp;
using namespace ocudu::dapp::management;

namespace {

using json = nlohmann::json;

constexpr std::size_t maximum_manifest_bytes      = 256 * 1024;
constexpr std::size_t maximum_manifest_interfaces = 64;
constexpr int         maximum_manifest_depth      = 16;
constexpr std::size_t maximum_manifest_path       = 4096;
constexpr std::size_t maximum_global_name         = 128;
constexpr std::size_t maximum_vendor_name         = 128;
constexpr std::size_t maximum_version             = 32;

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
  auto child_it  = child.begin();
  auto parent_it = parent.begin();
  for (; parent_it != parent.end(); ++parent_it, ++child_it) {
    if (child_it == child.end() || *child_it != *parent_it) {
      return false;
    }
  }
  return child != parent;
}

bool has_only_fields(const json& value, std::initializer_list<const char*> allowed)
{
  if (!value.is_object()) {
    return false;
  }
  for (const auto& entry : value.items()) {
    if (std::none_of(allowed.begin(), allowed.end(), [&](const char* field) { return entry.key() == field; })) {
      return false;
    }
  }
  return true;
}

int hex_value(char value) noexcept
{
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

template <std::size_t N>
bool parse_hex(const std::string& text, std::array<uint8_t, N>& output)
{
  std::size_t nibble = 0;
  output.fill(0);
  for (char character : text) {
    const int value = hex_value(character);
    if (value < 0 || nibble == output.size() * 2) {
      return false;
    }
    output[nibble / 2] |= static_cast<uint8_t>(value << ((nibble % 2 == 0) ? 4 : 0));
    ++nibble;
  }
  return nibble == output.size() * 2;
}

bool parse_identifier(const std::string& text, identifier& output)
{
  std::string compact;
  if (text.size() == output.bytes.size() * 2) {
    compact = text;
  } else if (text.size() == 36 && text[8] == '-' && text[13] == '-' && text[18] == '-' && text[23] == '-') {
    compact.reserve(output.bytes.size() * 2);
    for (char character : text) {
      if (character != '-') {
        compact.push_back(character);
      }
    }
  } else {
    return false;
  }
  return parse_hex(compact, output.bytes);
}

constexpr uint32_t placement_bit(ocudu_dapp_placement_v1 placement) noexcept
{
  return 1U << (static_cast<uint32_t>(placement) - 1U);
}

std::optional<uint32_t> parse_placement(const std::string& value)
{
  if (value == "in_process") {
    return placement_bit(OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1);
  }
  if (value == "out_of_process") {
    return placement_bit(OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1);
  }
  return std::nullopt;
}

std::optional<uint32_t> parse_placement_mask(const json& value)
{
  if (!value.is_array() || value.empty() || value.size() > 2) {
    return std::nullopt;
  }
  uint32_t result = 0;
  for (const auto& item : value) {
    if (!item.is_string()) {
      return std::nullopt;
    }
    auto placement = parse_placement(item.get<std::string>());
    if (!placement || (result & *placement) != 0) {
      return std::nullopt;
    }
    result |= *placement;
  }
  return result;
}

std::optional<backend> parse_backend(const std::string& value)
{
  if (value == "cpu") {
    return backend::cpu;
  }
  if (value == "cuda") {
    return backend::cuda;
  }
  if (value == "other_accelerator") {
    return backend::other_accelerator;
  }
  return std::nullopt;
}

std::optional<ocudu_dapp_class_v1> parse_class(const std::string& value)
{
  if (value == "A") {
    return OCUDU_DAPP_CLASS_A_INLINE_V1;
  }
  if (value == "B") {
    return OCUDU_DAPP_CLASS_B_DEADLINE_V1;
  }
  if (value == "C") {
    return OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1;
  }
  return std::nullopt;
}

std::optional<ocudu_dapp_threading_v1> parse_threading(const std::string& value)
{
  if (value == "per_worker") {
    return OCUDU_DAPP_THREADING_PER_WORKER_V1;
  }
  if (value == "reentrant_shared") {
    return OCUDU_DAPP_THREADING_REENTRANT_SHARED_V1;
  }
  if (value == "serialized") {
    return OCUDU_DAPP_THREADING_SERIALIZED_V1;
  }
  return std::nullopt;
}

std::optional<interface_record> parse_interface(const json& value)
{
  static constexpr std::initializer_list<const char*> fields = {
      "interface_id", "major", "minor", "class", "placements", "threading", "binding", "capabilities", "pusch_profile"};
  if (!has_only_fields(value, fields) || !value.contains("interface_id") || !value.contains("major") ||
      !value.contains("minor") || !value.contains("class") || !value.contains("placements") ||
      !value.contains("threading") || !value.at("interface_id").is_number_unsigned() ||
      !value.at("major").is_number_unsigned() || !value.at("minor").is_number_unsigned() ||
      !value.at("class").is_string() || !value.at("threading").is_string()) {
    return std::nullopt;
  }

  const uint64_t interface_id = value.at("interface_id").get<uint64_t>();
  const uint64_t major        = value.at("major").get<uint64_t>();
  const uint64_t minor        = value.at("minor").get<uint64_t>();
  auto           dapp_class   = parse_class(value.at("class").get<std::string>());
  auto           placements   = parse_placement_mask(value.at("placements"));
  auto           threading    = parse_threading(value.at("threading").get<std::string>());
  if (interface_id == 0 || interface_id > UINT32_MAX || major > UINT16_MAX || minor > UINT16_MAX || !dapp_class ||
      !placements || !threading) {
    return std::nullopt;
  }
  if ((*dapp_class == OCUDU_DAPP_CLASS_A_INLINE_V1 || *dapp_class == OCUDU_DAPP_CLASS_B_DEADLINE_V1) &&
      (*placements != placement_bit(OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1) ||
       *threading == OCUDU_DAPP_THREADING_SERIALIZED_V1)) {
    return std::nullopt;
  }
  binding_requirement requirement = binding_requirement::required;
  if (value.contains("binding")) {
    if (!value.at("binding").is_string()) {
      return std::nullopt;
    }
    const auto binding = value.at("binding").get<std::string>();
    if (binding == "optional") {
      requirement = binding_requirement::optional;
    } else if (binding != "required") {
      return std::nullopt;
    }
  }
  uint64_t capabilities = 0;
  if (value.contains("capabilities")) {
    if (!value.at("capabilities").is_number_unsigned()) {
      return std::nullopt;
    }
    capabilities = value.at("capabilities").get<uint64_t>();
  }
  std::optional<interface_record::pusch_profile> pusch;
  if (value.contains("pusch_profile")) {
    static constexpr std::initializer_list<const char*> profile_fields = {
        "max_prb", "max_rx_ports", "max_layers", "modulation_mask", "dmrs_type_mask", "flags"};
    const auto& profile = value.at("pusch_profile");
    if (!has_only_fields(profile, profile_fields) || !profile.is_object() || !profile.contains("max_prb") ||
        !profile.contains("max_rx_ports") || !profile.contains("max_layers") || !profile.contains("modulation_mask") ||
        !profile.contains("dmrs_type_mask")) {
      return std::nullopt;
    }
    for (const char* field : {"max_prb", "max_rx_ports", "max_layers", "modulation_mask", "dmrs_type_mask"}) {
      if (!profile.at(field).is_number_unsigned() || profile.at(field).get<uint64_t>() > UINT32_MAX) {
        return std::nullopt;
      }
    }
    uint64_t flags = 0;
    if (profile.contains("flags")) {
      if (!profile.at("flags").is_number_unsigned() || profile.at("flags").get<uint64_t>() > UINT32_MAX) {
        return std::nullopt;
      }
      flags = profile.at("flags").get<uint64_t>();
    }
    pusch                              = interface_record::pusch_profile{profile.at("max_prb").get<uint32_t>(),
                                            profile.at("max_rx_ports").get<uint32_t>(),
                                            profile.at("max_layers").get<uint32_t>(),
                                            profile.at("modulation_mask").get<uint32_t>(),
                                            profile.at("dmrs_type_mask").get<uint32_t>(),
                                            static_cast<uint32_t>(flags)};
    constexpr uint32_t all_modulations = OCUDU_DAPP_PUSCH_MODULATION_QPSK_V1 | OCUDU_DAPP_PUSCH_MODULATION_QAM16_V1 |
                                         OCUDU_DAPP_PUSCH_MODULATION_QAM64_V1 | OCUDU_DAPP_PUSCH_MODULATION_QAM256_V1;
    constexpr uint32_t all_dmrs = OCUDU_DAPP_PUSCH_DMRS_TYPE1_V1 | OCUDU_DAPP_PUSCH_DMRS_TYPE2_V1;
    if (pusch->max_nof_prb == 0 || pusch->max_nof_prb > 275 || pusch->max_nof_rx_ports == 0 ||
        pusch->max_nof_rx_ports > 64 || pusch->max_nof_layers == 0 || pusch->max_nof_layers > 4 ||
        pusch->modulation_mask == 0 || (pusch->modulation_mask & ~all_modulations) != 0 || pusch->dmrs_type_mask == 0 ||
        (pusch->dmrs_type_mask & ~all_dmrs) != 0 ||
        (pusch->flags & ~(OCUDU_DAPP_PUSCH_PROFILE_TRANSFORM_PRECODING_V1 | OCUDU_DAPP_PUSCH_PROFILE_DC_OVERLAP_V1)) !=
            0) {
      return std::nullopt;
    }
  }
  const bool class_a = *dapp_class == OCUDU_DAPP_CLASS_A_INLINE_V1;
  if (class_a != pusch.has_value()) {
    return std::nullopt;
  }
  return interface_record{static_cast<uint32_t>(interface_id),
                          static_cast<uint16_t>(major),
                          static_cast<uint16_t>(minor),
                          *dapp_class,
                          *placements,
                          *threading,
                          capabilities,
                          pusch,
                          requirement,
                          binding_state::unbound};
}

package_manifest_result
parse_manifest(const json& document, const std::filesystem::path& root, const std::filesystem::path& manifest_path)
{
  static constexpr std::initializer_list<const char*> fields = {"schema_version",
                                                                "package_id",
                                                                "global_name",
                                                                "vendor",
                                                                "version",
                                                                "artifact",
                                                                "sha256",
                                                                "placements",
                                                                "backends",
                                                                "interfaces",
                                                                "provenance"};
  if (!document.is_object() || !document.contains("schema_version") ||
      !document.at("schema_version").is_number_unsigned()) {
    return {{}, "manifest is missing a required field or uses an unsupported schema"};
  }
  const uint64_t schema_version = document.at("schema_version").get<uint64_t>();
  if (schema_version != 1 || !has_only_fields(document, fields) || !document.contains("package_id") ||
      !document.at("package_id").is_string() || !document.contains("global_name") ||
      !document.at("global_name").is_string() || !document.contains("vendor") || !document.at("vendor").is_string() ||
      !document.contains("version") || !document.at("version").is_string() || !document.contains("artifact") ||
      !document.at("artifact").is_string() || !document.contains("sha256") || !document.at("sha256").is_string() ||
      !document.contains("placements") || !document.contains("backends") || !document.at("backends").is_array() ||
      document.at("backends").empty() || !document.contains("interfaces") || !document.at("interfaces").is_array() ||
      document.at("interfaces").empty() || document.at("interfaces").size() > maximum_manifest_interfaces) {
    return {{}, "manifest is missing a required field or uses an unsupported schema"};
  }

  package_record package;
  package.manifest_schema_version = 1;
  if (!parse_identifier(document.at("package_id").get<std::string>(), package.package_id) ||
      package.package_id.is_zero() || !parse_hex(document.at("sha256").get<std::string>(), package.content_digest)) {
    return {{}, "manifest package_id or sha256 is invalid"};
  }
  package.global_name = document.at("global_name").get<std::string>();
  package.vendor      = document.at("vendor").get<std::string>();
  package.version     = document.at("version").get<std::string>();
  if (package.global_name.empty() || package.global_name.size() > maximum_global_name || package.vendor.empty() ||
      package.vendor.size() > maximum_vendor_name || package.version.empty() ||
      package.version.size() > maximum_version) {
    return {{}, "manifest package identity is empty or too long"};
  }
  auto placements = parse_placement_mask(document.at("placements"));
  if (!placements || (*placements & (*placements - 1U)) != 0) {
    return {{}, "manifest must bind one artifact to exactly one placement"};
  }
  package.placement_mask = *placements;

  const std::filesystem::path artifact(document.at("artifact").get<std::string>());
  if (artifact.empty() || artifact.is_absolute() || artifact.native().size() > maximum_manifest_path) {
    return {{}, "manifest artifact path must be relative to the manifest"};
  }
  const auto artifact_path = (manifest_path.parent_path() / artifact).lexically_normal();
  if (!path_is_below(artifact_path, root)) {
    return {{}, "manifest artifact path escapes the catalog"};
  }
  package.canonical_artifact_path = artifact_path.string();

  if (document.contains("provenance")) {
    static constexpr std::initializer_list<const char*> provenance_fields = {"sbom", "format", "sha256"};
    if (!has_only_fields(document.at("provenance"), provenance_fields)) {
      return {{}, "manifest-v1 provenance must be a closed declaration"};
    }
    const auto& provenance = document.at("provenance");
    if (!provenance.contains("sbom") || !provenance.at("sbom").is_string() || !provenance.contains("format") ||
        !provenance.at("format").is_string() || provenance.at("format").get<std::string>() != "spdx-2.3-json" ||
        !provenance.contains("sha256") || !provenance.at("sha256").is_string() ||
        !parse_hex(provenance.at("sha256").get<std::string>(), package.sbom_digest)) {
      return {{}, "manifest-v1 SPDX provenance declaration is invalid"};
    }
    const std::filesystem::path sbom(provenance.at("sbom").get<std::string>());
    if (sbom.empty() || sbom.is_absolute() || sbom.lexically_normal() != sbom || sbom == "." ||
        sbom.native().size() > maximum_manifest_path) {
      return {{}, "manifest-v1 SBOM path must be normalized and relative"};
    }
    const auto sbom_path = (manifest_path.parent_path() / sbom).lexically_normal();
    if (!path_is_below(sbom_path, root) || sbom_path == artifact_path) {
      return {{}, "manifest-v1 SBOM path escapes the catalog or aliases the artifact"};
    }
    package.canonical_sbom_path = sbom_path.string();
  }

  std::set<backend> unique_backends;
  for (const auto& item : document.at("backends")) {
    if (!item.is_string()) {
      return {{}, "manifest backend list is invalid"};
    }
    auto parsed = parse_backend(item.get<std::string>());
    if (!parsed || !unique_backends.insert(*parsed).second) {
      return {{}, "manifest backend list is invalid"};
    }
    package.backends.push_back(*parsed);
  }

  std::set<uint32_t> interface_ids;
  for (const auto& item : document.at("interfaces")) {
    auto parsed = parse_interface(item);
    if (!parsed || !interface_ids.insert(parsed->interface_id).second) {
      return {{}, "manifest interface list is invalid"};
    }
    package.interfaces.push_back(*parsed);
  }
  if ((package.placement_mask & placement_bit(OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1)) != 0 &&
      std::any_of(package.interfaces.begin(), package.interfaces.end(), [](const interface_record& interface) {
        return interface.dapp_class != OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1 ||
               (interface.placement_mask & placement_bit(OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1)) == 0;
      })) {
    return {{}, "out-of-process packages may contain only process-capable Class-C interfaces"};
  }
  return {std::move(package), {}};
}

} // namespace

package_manifest_result
ocudu::dapp::read_package_manifest(const std::string&                catalog_root,
                                   const std::string&                relative_manifest_path,
                                   const package_signature_verifier* signature_verifier) noexcept
{
  try {
    std::error_code             error;
    const auto                  root = std::filesystem::canonical(catalog_root, error);
    const std::filesystem::path relative(relative_manifest_path);
    if (error || !std::filesystem::is_directory(root) || relative.empty() || relative.is_absolute() ||
        relative.lexically_normal() != relative || relative == "." ||
        relative.native().size() > maximum_manifest_path) {
      return {{}, "manifest path or catalog root is invalid"};
    }
    const auto candidate = (root / relative).lexically_normal();
    if (!path_is_below(candidate, root)) {
      return {{}, "manifest path escapes the catalog"};
    }
    struct stat link_status {};
    if (::lstat(candidate.c_str(), &link_status) != 0 || S_ISLNK(link_status.st_mode)) {
      return {{}, "manifest must be an existing non-symbolic file"};
    }

    fd_guard descriptor{::open(candidate.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (descriptor.value < 0) {
      return {{}, std::string("unable to open manifest: ") + std::strerror(errno)};
    }
    struct stat status {};
    if (::fstat(descriptor.value, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0 ||
        static_cast<uint64_t>(status.st_size) > maximum_manifest_bytes ||
        (status.st_uid != ::geteuid() && status.st_uid != 0) || (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      return {{}, "manifest ownership, mode, type, or size is not admissible"};
    }

    const auto held_path = std::filesystem::canonical("/proc/self/fd/" + std::to_string(descriptor.value), error);
    if (error || !path_is_below(held_path, root)) {
      return {{}, "opened manifest does not resolve below the catalog"};
    }
    std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t offset = 0;
    while (offset != bytes.size()) {
      const ssize_t count =
          ::pread(descriptor.value, bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset));
      if (count <= 0) {
        return {{}, "unable to read complete manifest"};
      }
      offset += static_cast<std::size_t>(count);
    }
    auto parsed = ocudu::dapp::detail::parse_bounded_json(bytes.begin(), bytes.end(), maximum_manifest_depth);
    if (!parsed.document) {
      return {{},
              parsed.error == ocudu::dapp::detail::bounded_json_error::duplicate_key
                  ? "manifest contains a duplicate object key"
              : parsed.error == ocudu::dapp::detail::bounded_json_error::excessive_depth
                  ? "manifest nesting is too deep"
              : parsed.error == ocudu::dapp::detail::bounded_json_error::invalid_structure
                  ? "manifest parser reported an invalid object structure"
                  : "manifest is not valid JSON"};
    }
    auto& document = *parsed.document;
    auto  result   = parse_manifest(document, root, held_path);
    if (!result) {
      return result;
    }
    if (signature_verifier != nullptr) {
      auto signature = signature_verifier->verify(root.string(),
                                                  relative_manifest_path,
                                                  reinterpret_cast<const uint8_t*>(bytes.data()),
                                                  bytes.size(),
                                                  *result.package);
      if (!signature) {
        return {{}, std::move(signature.detail)};
      }
    }
    if (!result.package->canonical_sbom_path.empty()) {
      auto provenance = verify_package_provenance(root.string(), *result.package);
      if (!provenance) {
        return {{}, std::move(provenance.detail)};
      }
      result.package->sbom_verified = true;
    }
    return result;
  } catch (const std::bad_alloc&) {
    return {{}, "manifest parsing exhausted memory"};
  } catch (const std::exception& error) {
    return {{}, std::string("manifest parsing failed: ") + error.what()};
  } catch (...) {
    return {{}, "unknown manifest parsing failure"};
  }
}
