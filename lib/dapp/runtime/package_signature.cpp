// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/package_signature.h"
#include "bounded_json.h"
#include "ocudu/dapp/management/types.h"
#include "ocudu/dapp/runtime/artifact_verifier.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#ifdef OCUDU_DAPP_PACKAGE_SIGNATURE_CRYPTO
#include <mbedtls/ecp.h>
#include <mbedtls/pk.h>
#endif

using namespace ocudu::dapp;
using namespace ocudu::dapp::management;

namespace {

using json = nlohmann::json;

constexpr std::size_t maximum_trust_store_bytes = 256 * 1024;
constexpr std::size_t maximum_signature_bytes   = 16 * 1024;
constexpr std::size_t maximum_keys              = 64;
constexpr std::size_t maximum_packages          = 256;
constexpr std::size_t maximum_path              = 4096;
constexpr int         maximum_json_depth        = 12;
constexpr const char* signature_algorithm       = "ecdsa-p256-sha256";

struct fd_guard {
  int value{-1};
  ~fd_guard()
  {
    if (value >= 0) {
      ::close(value);
    }
  }
};

enum class read_status { ok, missing, error };

struct read_result {
  read_status status{read_status::error};
  std::string bytes;
  std::string detail;
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

read_result read_secure_file(const std::filesystem::path&                path,
                             std::size_t                                 maximum_bytes,
                             bool                                        missing_allowed,
                             const std::optional<std::filesystem::path>& required_root = std::nullopt)
{
  if (path.empty() || path.native().size() > maximum_path || path.lexically_normal() != path) {
    return {read_status::error, {}, "path is empty, too long, or not normalized"};
  }
  struct stat link_status {};
  if (::lstat(path.c_str(), &link_status) != 0) {
    if (missing_allowed && errno == ENOENT) {
      return {read_status::missing, {}, {}};
    }
    return {read_status::error, {}, std::string("unable to inspect file: ") + std::strerror(errno)};
  }
  if (S_ISLNK(link_status.st_mode)) {
    return {read_status::error, {}, "file must not be a symbolic link"};
  }
  fd_guard descriptor{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (descriptor.value < 0) {
    return {read_status::error, {}, std::string("unable to open file: ") + std::strerror(errno)};
  }
  struct stat status {};
  if (::fstat(descriptor.value, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0 ||
      static_cast<uint64_t>(status.st_size) > maximum_bytes || (status.st_uid != ::geteuid() && status.st_uid != 0) ||
      (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    return {read_status::error, {}, "file ownership, mode, type, or size is not admissible"};
  }
  if (required_root.has_value()) {
    std::error_code error;
    const auto      held_path = std::filesystem::canonical("/proc/self/fd/" + std::to_string(descriptor.value), error);
    if (error || !path_is_below(held_path, *required_root)) {
      return {read_status::error, {}, "opened file escapes the catalog root"};
    }
  }
  std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
  std::size_t offset = 0;
  while (offset != bytes.size()) {
    const ssize_t count =
        ::pread(descriptor.value, bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset));
    if (count <= 0) {
      return {read_status::error, {}, "unable to read complete file"};
    }
    offset += static_cast<std::size_t>(count);
  }
  return {read_status::ok, std::move(bytes), {}};
}

bool object_has_only_fields(const json& value, std::initializer_list<const char*> fields)
{
  if (!value.is_object()) {
    return false;
  }
  for (const auto& item : value.items()) {
    if (std::find(fields.begin(), fields.end(), item.key()) == fields.end()) {
      return false;
    }
  }
  return true;
}

std::optional<json> parse_bounded_json(const std::string& bytes, std::string& detail)
{
  auto parsed = ocudu::dapp::detail::parse_bounded_json(bytes.begin(), bytes.end(), maximum_json_depth);
  if (!parsed.document) {
    detail = parsed.error == ocudu::dapp::detail::bounded_json_error::duplicate_key
                 ? "JSON contains a duplicate object key"
             : parsed.error == ocudu::dapp::detail::bounded_json_error::excessive_depth ? "JSON nesting is too deep"
             : parsed.error == ocudu::dapp::detail::bounded_json_error::invalid_structure
                 ? "JSON parser reported an invalid object structure"
                 : "file is not valid JSON";
    return std::nullopt;
  }
  return std::move(parsed.document);
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

bool parse_hex(const std::string& text, std::vector<uint8_t>& output, std::size_t maximum)
{
  if (text.empty() || (text.size() % 2) != 0 || text.size() / 2 > maximum) {
    return false;
  }
  output.resize(text.size() / 2);
  for (std::size_t index = 0; index != output.size(); ++index) {
    const int high = hex_value(text[index * 2]);
    const int low  = hex_value(text[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    output[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

template <std::size_t N>
bool parse_fixed_hex(const std::string& text, std::array<uint8_t, N>& output)
{
  std::vector<uint8_t> decoded;
  if (!parse_hex(text, decoded, N) || decoded.size() != N) {
    return false;
  }
  std::copy(decoded.begin(), decoded.end(), output.begin());
  return true;
}

bool parse_identifier(const std::string& text, identifier& output)
{
  std::string compact;
  compact.reserve(32);
  for (char character : text) {
    if (character != '-') {
      compact.push_back(character);
    }
  }
  return compact.size() == 32 && parse_fixed_hex(compact, output.bytes) && !output.is_zero();
}

struct trusted_key {
  artifact_digest      key_id{};
  std::vector<uint8_t> public_key_spki;
};

struct package_signature_policy {
  package_signature_mode                                    mode{package_signature_mode::required};
  uint64_t                                                  policy_epoch{};
  std::vector<trusted_key>                                  keys;
  std::set<artifact_digest>                                 revoked_keys;
  std::unordered_map<identifier, uint64_t, identifier_hash> minimum_sequences;
};

} // namespace

struct package_signature_verifier::implementation : package_signature_policy {};

namespace {

#ifdef OCUDU_DAPP_PACKAGE_SIGNATURE_CRYPTO
bool valid_p256_public_key(const std::vector<uint8_t>& bytes)
{
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  const int  parsed = mbedtls_pk_parse_public_key(&key, bytes.data(), bytes.size());
  const bool valid  = parsed == 0 && mbedtls_pk_can_do(&key, MBEDTLS_PK_ECDSA) != 0 &&
                     mbedtls_pk_ec(key)->grp.id == MBEDTLS_ECP_DP_SECP256R1;
  mbedtls_pk_free(&key);
  return valid;
}

bool verify_signature(const std::vector<uint8_t>& key_bytes,
                      const artifact_digest&      digest,
                      const std::vector<uint8_t>& signature)
{
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  const int parsed = mbedtls_pk_parse_public_key(&key, key_bytes.data(), key_bytes.size());
  const int status =
      parsed == 0
          ? mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest.data(), digest.size(), signature.data(), signature.size())
          : parsed;
  mbedtls_pk_free(&key);
  return status == 0;
}
#endif

struct verifier_state_result {
  std::unique_ptr<package_signature_policy> state;
  std::string                               detail;
};

verifier_state_result create_verifier_state(package_signature_config config)
{
  if (config.trust_store_path.empty()) {
    if (config.mode == package_signature_mode::required || config.minimum_policy_epoch != 0) {
      return {{}, "required package signatures need a trust store and nonzero minimum policy epoch"};
    }
    auto state  = std::make_unique<package_signature_policy>();
    state->mode = config.mode;
    return {std::move(state), {}};
  }
#ifndef OCUDU_DAPP_PACKAGE_SIGNATURE_CRYPTO
  return {{}, "package signature verification is unavailable because mbedTLS was disabled"};
#else
  const std::filesystem::path path(config.trust_store_path);
  if (!path.is_absolute() || path.lexically_normal() != path || config.minimum_policy_epoch == 0) {
    return {{}, "trust store path or minimum policy epoch is invalid"};
  }
  auto file = read_secure_file(path, maximum_trust_store_bytes, false);
  if (file.status != read_status::ok) {
    return {{}, "unable to read package trust store: " + file.detail};
  }
  std::string detail;
  auto        parsed = parse_bounded_json(file.bytes, detail);
  if (!parsed) {
    return {{}, "unable to parse package trust store: " + detail};
  }
  const auto& document = *parsed;
  if (!object_has_only_fields(document, {"schema_version", "policy_epoch", "keys", "revoked_key_ids", "packages"}) ||
      !document.contains("schema_version") || !document.at("schema_version").is_number_unsigned() ||
      document.at("schema_version").get<uint64_t>() != 1 || !document.contains("policy_epoch") ||
      !document.at("policy_epoch").is_number_unsigned() || !document.contains("keys") ||
      !document.at("keys").is_array() || document.at("keys").empty() || document.at("keys").size() > maximum_keys ||
      !document.contains("revoked_key_ids") || !document.at("revoked_key_ids").is_array() ||
      document.at("revoked_key_ids").size() > maximum_keys || !document.contains("packages") ||
      !document.at("packages").is_array() || document.at("packages").empty() ||
      document.at("packages").size() > maximum_packages) {
    return {{}, "package trust store is missing a required field or uses an unsupported schema"};
  }
  auto state          = std::make_unique<package_signature_policy>();
  state->mode         = config.mode;
  state->policy_epoch = document.at("policy_epoch").get<uint64_t>();
  if (state->policy_epoch == 0 || state->policy_epoch < config.minimum_policy_epoch) {
    return {{}, "package trust policy epoch is below the configured minimum"};
  }
  for (const auto& item : document.at("keys")) {
    if (!object_has_only_fields(item, {"key_id", "algorithm", "public_key_spki"}) || !item.contains("key_id") ||
        !item.at("key_id").is_string() || !item.contains("algorithm") || !item.at("algorithm").is_string() ||
        item.at("algorithm").get<std::string>() != signature_algorithm || !item.contains("public_key_spki") ||
        !item.at("public_key_spki").is_string()) {
      return {{}, "package trust key is malformed or uses an unsupported algorithm"};
    }
    trusted_key key;
    if (!parse_fixed_hex(item.at("key_id").get<std::string>(), key.key_id) ||
        !parse_hex(item.at("public_key_spki").get<std::string>(), key.public_key_spki, 1024) ||
        !valid_p256_public_key(key.public_key_spki) ||
        sha256_bytes(key.public_key_spki.data(), key.public_key_spki.size()) != key.key_id ||
        std::any_of(state->keys.begin(), state->keys.end(), [&](const trusted_key& existing) {
          return existing.key_id == key.key_id;
        })) {
      return {{}, "package trust key identity, encoding, curve, or uniqueness is invalid"};
    }
    state->keys.push_back(std::move(key));
  }
  for (const auto& item : document.at("revoked_key_ids")) {
    artifact_digest key_id{};
    if (!item.is_string() || !parse_fixed_hex(item.get<std::string>(), key_id) ||
        !state->revoked_keys.insert(key_id).second) {
      return {{}, "package trust revocation list is invalid"};
    }
  }
  for (const auto& item : document.at("packages")) {
    if (!object_has_only_fields(item, {"package_id", "minimum_sequence"}) || !item.contains("package_id") ||
        !item.at("package_id").is_string() || !item.contains("minimum_sequence") ||
        !item.at("minimum_sequence").is_number_unsigned()) {
      return {{}, "package trust sequence constraint is malformed"};
    }
    identifier package_id;
    const auto minimum = item.at("minimum_sequence").get<uint64_t>();
    if (!parse_identifier(item.at("package_id").get<std::string>(), package_id) || minimum == 0 ||
        !state->minimum_sequences.emplace(package_id, minimum).second) {
      return {{}, "package trust sequence constraint is invalid or duplicated"};
    }
  }
  return {std::move(state), {}};
#endif
}

} // namespace

package_signature_verifier::package_signature_verifier(std::unique_ptr<implementation> state_) noexcept :
  state(std::move(state_))
{
}

package_signature_verifier::~package_signature_verifier()                                                = default;
package_signature_verifier::package_signature_verifier(package_signature_verifier&&) noexcept            = default;
package_signature_verifier& package_signature_verifier::operator=(package_signature_verifier&&) noexcept = default;

package_signature_verifier_result package_signature_verifier::create(package_signature_config config) noexcept
{
  try {
    auto result = create_verifier_state(std::move(config));
    if (!result.state) {
      return {{}, std::move(result.detail)};
    }
    auto state                                     = std::make_unique<implementation>();
    static_cast<package_signature_policy&>(*state) = std::move(*result.state);
    return {std::unique_ptr<package_signature_verifier>(new package_signature_verifier(std::move(state))), {}};
  } catch (const std::bad_alloc&) {
    return {{}, "package trust policy exhausted memory"};
  } catch (const std::exception& error) {
    return {{}, std::string("package trust policy failed: ") + error.what()};
  } catch (...) {
    return {{}, "unknown package trust policy failure"};
  }
}

package_signature_result package_signature_verifier::verify(const std::string&          catalog_root,
                                                            const std::string&          relative_manifest_path,
                                                            const uint8_t*              manifest_bytes,
                                                            std::size_t                 manifest_size,
                                                            management::package_record& package) const noexcept
{
  try {
    if (state == nullptr || manifest_bytes == nullptr || manifest_size == 0) {
      return {false, "package signature verifier input is invalid"};
    }
    std::error_code error;
    const auto      root               = std::filesystem::canonical(catalog_root, error);
    const auto      relative_signature = std::filesystem::path(relative_manifest_path + ".sig.json");
    if (error || !std::filesystem::is_directory(root) || relative_signature.is_absolute() ||
        relative_signature.lexically_normal() != relative_signature) {
      return {false, "package signature path or catalog root is invalid"};
    }
    const auto signature_path = (root / relative_signature).lexically_normal();
    if (!path_is_below(signature_path, root)) {
      return {false, "package signature path escapes the catalog"};
    }
    auto file = read_secure_file(signature_path, maximum_signature_bytes, true, root);
    if (file.status == read_status::missing) {
      if (state->mode == package_signature_mode::required) {
        return {false, "required package signature is missing"};
      }
      package.signature_status   = package_signature_status::unsigned_package;
      package.signing_key_id     = {};
      package.signature_sequence = 0;
      package.trust_policy_epoch = state->policy_epoch;
      return {true, {}};
    }
    if (file.status != read_status::ok) {
      return {false, "unable to read package signature: " + file.detail};
    }
#ifndef OCUDU_DAPP_PACKAGE_SIGNATURE_CRYPTO
    return {false, "a signature sidecar exists but signature crypto is unavailable"};
#else
    std::string detail;
    auto        parsed = parse_bounded_json(file.bytes, detail);
    if (!parsed) {
      return {false, "unable to parse package signature: " + detail};
    }
    const auto& document = *parsed;
    if (!object_has_only_fields(
            document, {"schema_version", "algorithm", "key_id", "package_sequence", "manifest_sha256", "signature"}) ||
        !document.contains("schema_version") || !document.at("schema_version").is_number_unsigned() ||
        document.at("schema_version").get<uint64_t>() != 1 || !document.contains("algorithm") ||
        !document.at("algorithm").is_string() || document.at("algorithm").get<std::string>() != signature_algorithm ||
        !document.contains("key_id") || !document.at("key_id").is_string() || !document.contains("package_sequence") ||
        !document.at("package_sequence").is_number_unsigned() || !document.contains("manifest_sha256") ||
        !document.at("manifest_sha256").is_string() || !document.contains("signature") ||
        !document.at("signature").is_string()) {
      return {false, "package signature is missing a required field or uses an unsupported schema"};
    }
    artifact_digest      key_id{};
    artifact_digest      claimed_manifest_digest{};
    std::vector<uint8_t> signature;
    const uint64_t       sequence        = document.at("package_sequence").get<uint64_t>();
    const auto           manifest_digest = sha256_bytes(manifest_bytes, manifest_size);
    if (sequence == 0 || !parse_fixed_hex(document.at("key_id").get<std::string>(), key_id) ||
        !parse_fixed_hex(document.at("manifest_sha256").get<std::string>(), claimed_manifest_digest) ||
        claimed_manifest_digest != manifest_digest ||
        !parse_hex(document.at("signature").get<std::string>(), signature, MBEDTLS_PK_SIGNATURE_MAX_SIZE)) {
      return {false, "package signature identity, sequence, manifest digest, or encoding is invalid"};
    }
    const auto key = std::find_if(state->keys.begin(), state->keys.end(), [&](const trusted_key& candidate) {
      return candidate.key_id == key_id;
    });
    if (key == state->keys.end()) {
      return {false, "package signature key is not trusted"};
    }
    if (state->revoked_keys.count(key_id) != 0) {
      return {false, "package signature key is revoked"};
    }
    const auto floor = state->minimum_sequences.find(package.package_id);
    if (floor == state->minimum_sequences.end()) {
      return {false, "package has no trusted minimum sequence"};
    }
    if (sequence < floor->second) {
      return {false, "package signature sequence is below the trusted minimum"};
    }
    if (!verify_signature(key->public_key_spki, package_signature_digest(manifest_digest, sequence), signature)) {
      return {false, "package signature verification failed"};
    }
    package.signature_status   = package_signature_status::verified;
    package.signing_key_id     = key_id;
    package.signature_sequence = sequence;
    package.trust_policy_epoch = state->policy_epoch;
    return {true, {}};
#endif
  } catch (const std::bad_alloc&) {
    return {false, "package signature verification exhausted memory"};
  } catch (const std::exception& error) {
    return {false, std::string("package signature verification failed: ") + error.what()};
  } catch (...) {
    return {false, "unknown package signature verification failure"};
  }
}

uint64_t package_signature_verifier::policy_epoch() const noexcept
{
  return state != nullptr ? state->policy_epoch : 0;
}

artifact_digest ocudu::dapp::package_signature_digest(const artifact_digest& manifest_digest,
                                                      uint64_t               package_sequence) noexcept
{
  constexpr std::string_view domain = "OCUDU-DAPP-MANIFEST-SIGNATURE-V1\n";
  std::array<uint8_t, domain.size() + artifact_digest{}.size() + sizeof(uint64_t)> payload{};
  std::copy(domain.begin(), domain.end(), payload.begin());
  std::copy(manifest_digest.begin(), manifest_digest.end(), payload.begin() + domain.size());
  const std::size_t offset = domain.size() + manifest_digest.size();
  for (unsigned index = 0; index != sizeof(package_sequence); ++index) {
    payload[offset + index] = static_cast<uint8_t>(package_sequence >> ((7U - index) * 8U));
  }
  return sha256_bytes(payload.data(), offset + sizeof(package_sequence));
}
