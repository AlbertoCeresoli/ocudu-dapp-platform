// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/management/types.h"
#include "ocudu/dapp/runtime/artifact_verifier.h"
#include "ocudu/dapp/runtime/package_manifest.h"
#include "ocudu/dapp/runtime/package_signature.h"
#include "CLI/CLI11.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace ocudu::dapp;
using namespace ocudu::dapp::management;

namespace {

constexpr std::size_t maximum_manifest_bytes = 1024 * 1024;
constexpr std::size_t maximum_key_bytes      = 64 * 1024;
constexpr const char* signature_algorithm    = "ecdsa-p256-sha256";

struct fd_guard {
  int value{-1};
  ~fd_guard()
  {
    if (value >= 0) {
      ::close(value);
    }
  }
};

struct random_source {
  mbedtls_entropy_context  entropy;
  mbedtls_ctr_drbg_context generator;

  random_source()
  {
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&generator);
    constexpr std::string_view personalization = "ocudu-dapp-package-v1";
    if (mbedtls_ctr_drbg_seed(&generator,
                              mbedtls_entropy_func,
                              &entropy,
                              reinterpret_cast<const uint8_t*>(personalization.data()),
                              personalization.size()) != 0) {
      mbedtls_ctr_drbg_free(&generator);
      mbedtls_entropy_free(&entropy);
      throw std::runtime_error("unable to initialize cryptographic random source");
    }
  }

  ~random_source()
  {
    mbedtls_ctr_drbg_free(&generator);
    mbedtls_entropy_free(&entropy);
  }
};

struct key_guard {
  mbedtls_pk_context value;
  key_guard() { mbedtls_pk_init(&value); }
  ~key_guard() { mbedtls_pk_free(&value); }
  key_guard(const key_guard&)            = delete;
  key_guard& operator=(const key_guard&) = delete;
};

std::filesystem::path absolute_normal_path(const std::string& value, const char* kind)
{
  const std::filesystem::path path(value);
  if (!path.is_absolute() || path.lexically_normal() != path) {
    throw std::runtime_error(std::string(kind) + " must be an absolute normalized path");
  }
  return path;
}

std::vector<uint8_t>
read_controlled_file(const std::filesystem::path& path, std::size_t maximum_bytes, bool private_key = false)
{
  struct stat link_status {};
  if (::lstat(path.c_str(), &link_status) != 0 || S_ISLNK(link_status.st_mode)) {
    throw std::runtime_error("input must be an existing non-symbolic file: " + path.string());
  }
  fd_guard descriptor{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (descriptor.value < 0) {
    throw std::runtime_error("unable to open input: " + path.string() + ": " + std::strerror(errno));
  }
  struct stat  status {};
  const mode_t prohibited = private_key ? (S_IRWXG | S_IRWXO) : (S_IWGRP | S_IWOTH);
  if (::fstat(descriptor.value, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0 ||
      static_cast<uint64_t>(status.st_size) > maximum_bytes || (status.st_uid != 0 && status.st_uid != ::geteuid()) ||
      (status.st_mode & prohibited) != 0) {
    throw std::runtime_error("input ownership, mode, type, or size is not admissible: " + path.string());
  }
  std::vector<uint8_t> bytes(static_cast<std::size_t>(status.st_size));
  std::size_t          offset = 0;
  while (offset != bytes.size()) {
    const ssize_t count =
        ::pread(descriptor.value, bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset));
    if (count <= 0) {
      throw std::runtime_error("unable to read complete input: " + path.string());
    }
    offset += static_cast<std::size_t>(count);
  }
  return bytes;
}

void write_new_file(const std::filesystem::path& path, const uint8_t* bytes, std::size_t size, mode_t mode)
{
  if (bytes == nullptr || size == 0 || !path.is_absolute() || path.lexically_normal() != path) {
    throw std::runtime_error("output path or content is invalid");
  }
  std::error_code error;
  if (!std::filesystem::is_directory(path.parent_path(), error) || error) {
    throw std::runtime_error("output parent directory does not exist: " + path.parent_path().string());
  }
  fd_guard descriptor{::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode)};
  if (descriptor.value < 0) {
    throw std::runtime_error("refusing to replace output: " + path.string() + ": " + std::strerror(errno));
  }
  std::size_t offset = 0;
  while (offset != size) {
    const ssize_t count = ::write(descriptor.value, bytes + offset, size - offset);
    if (count <= 0) {
      ::unlink(path.c_str());
      throw std::runtime_error("unable to write complete output: " + path.string());
    }
    offset += static_cast<std::size_t>(count);
  }
  if (::fsync(descriptor.value) != 0) {
    ::unlink(path.c_str());
    throw std::runtime_error("unable to synchronize output: " + path.string());
  }
}

void write_new_file(const std::filesystem::path& path, const std::string& text, mode_t mode)
{
  write_new_file(path, reinterpret_cast<const uint8_t*>(text.data()), text.size(), mode);
}

void require_absent(const std::filesystem::path& path)
{
  struct stat status {};
  errno = 0;
  if (::lstat(path.c_str(), &status) == 0) {
    throw std::runtime_error("refusing to replace output: " + path.string());
  }
  if (errno != ENOENT) {
    throw std::runtime_error("unable to examine output: " + path.string() + ": " + std::strerror(errno));
  }
}

template <typename Range>
std::string hex(const Range& bytes)
{
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (uint8_t byte : bytes) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
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
std::array<uint8_t, N> parse_fixed_hex(std::string_view text, const char* kind)
{
  if (text.size() != N * 2) {
    throw std::runtime_error(std::string(kind) + " must contain exactly " + std::to_string(N * 2) + " hex digits");
  }
  std::array<uint8_t, N> result{};
  for (std::size_t index = 0; index != N; ++index) {
    const int high = hex_value(text[index * 2]);
    const int low  = hex_value(text[index * 2 + 1]);
    if (high < 0 || low < 0) {
      throw std::runtime_error(std::string(kind) + " contains a non-hexadecimal character");
    }
    result[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return result;
}

identifier parse_identifier(std::string_view text)
{
  std::string compact;
  compact.reserve(32);
  for (char character : text) {
    if (character != '-') {
      compact.push_back(character);
    }
  }
  identifier result;
  result.bytes = parse_fixed_hex<16>(compact, "package identifier");
  if (result.is_zero()) {
    throw std::runtime_error("package identifier must be nonzero");
  }
  return result;
}

std::string identifier_text(const identifier& value)
{
  const std::string compact = hex(value.bytes);
  return compact.substr(0, 8) + '-' + compact.substr(8, 4) + '-' + compact.substr(12, 4) + '-' + compact.substr(16, 4) +
         '-' + compact.substr(20);
}

bool valid_p256_key(const mbedtls_pk_context& key)
{
  return mbedtls_pk_can_do(&key, MBEDTLS_PK_ECDSA) != 0 && mbedtls_pk_ec(key)->grp.id == MBEDTLS_ECP_DP_SECP256R1;
}

std::vector<uint8_t> canonical_public_der(const mbedtls_pk_context& key)
{
  std::array<uint8_t, 1024> buffer{};
  const int size = mbedtls_pk_write_pubkey_der(const_cast<mbedtls_pk_context*>(&key), buffer.data(), buffer.size());
  if (size <= 0) {
    throw std::runtime_error("unable to encode P-256 public key");
  }
  return {buffer.end() - size, buffer.end()};
}

void read_private_key(const std::filesystem::path& path, key_guard& key)
{
  auto bytes = read_controlled_file(path, maximum_key_bytes, true);
  bytes.push_back(0);
  const int status = mbedtls_pk_parse_key(&key.value, bytes.data(), bytes.size(), nullptr, 0);
  mbedtls_platform_zeroize(bytes.data(), bytes.size());
  if (status != 0 || !valid_p256_key(key.value)) {
    throw std::runtime_error("private key must be an unencrypted P-256 PEM or DER key");
  }
}

std::vector<uint8_t> read_public_key(const std::filesystem::path& path)
{
  auto bytes = read_controlled_file(path, maximum_key_bytes);
  bytes.push_back(0);
  key_guard key;
  if (mbedtls_pk_parse_public_key(&key.value, bytes.data(), bytes.size()) != 0 || !valid_p256_key(key.value)) {
    throw std::runtime_error("public key must be a P-256 PEM or DER SubjectPublicKeyInfo");
  }
  return canonical_public_der(key.value);
}

std::filesystem::path catalog_manifest_path(const std::filesystem::path& root, const std::string& relative)
{
  const std::filesystem::path path(relative);
  if (path.empty() || path.is_absolute() || path.lexically_normal() != path || path == "." || *path.begin() == "..") {
    throw std::runtime_error("manifest path must be normalized and catalog-relative");
  }
  const auto candidate = (root / path).lexically_normal();
  auto       child     = candidate.begin();
  for (auto parent = root.begin(); parent != root.end(); ++parent, ++child) {
    if (child == candidate.end() || *child != *parent) {
      throw std::runtime_error("manifest path escapes the catalog root");
    }
  }
  return candidate;
}

void keygen(const std::string& private_path_text, const std::string& public_path_text)
{
  const auto private_path = absolute_normal_path(private_path_text, "private-key output");
  const auto public_path  = absolute_normal_path(public_path_text, "public-key output");
  if (private_path == public_path) {
    throw std::runtime_error("private and public key outputs must differ");
  }
  require_absent(private_path);
  require_absent(public_path);
  random_source random;
  key_guard     key;
  if (mbedtls_pk_setup(&key.value, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
      mbedtls_ecp_gen_key(
          MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key.value), mbedtls_ctr_drbg_random, &random.generator) != 0) {
    throw std::runtime_error("unable to generate P-256 signing key");
  }
  std::array<uint8_t, 4096> private_pem{};
  std::array<uint8_t, 4096> public_pem{};
  if (mbedtls_pk_write_key_pem(&key.value, private_pem.data(), private_pem.size()) != 0 ||
      mbedtls_pk_write_pubkey_pem(&key.value, public_pem.data(), public_pem.size()) != 0) {
    throw std::runtime_error("unable to encode generated signing key");
  }
  try {
    write_new_file(public_path, public_pem.data(), std::strlen(reinterpret_cast<const char*>(public_pem.data())), 0444);
    write_new_file(
        private_path, private_pem.data(), std::strlen(reinterpret_cast<const char*>(private_pem.data())), 0600);
  } catch (...) {
    ::unlink(public_path.c_str());
    ::unlink(private_path.c_str());
    mbedtls_platform_zeroize(private_pem.data(), private_pem.size());
    throw;
  }
  mbedtls_platform_zeroize(private_pem.data(), private_pem.size());
  const auto der    = canonical_public_der(key.value);
  const auto key_id = sha256_bytes(der.data(), der.size());
  std::cout << "key-id=" << hex(key_id) << '\n';
}

void sign_manifest(const std::string& catalog_root_text,
                   const std::string& relative_manifest,
                   const std::string& private_key_text,
                   uint64_t           package_sequence)
{
  if (package_sequence == 0) {
    throw std::runtime_error("package sequence must be nonzero");
  }
  const auto root = std::filesystem::canonical(absolute_normal_path(catalog_root_text, "catalog root"));
  if (!std::filesystem::is_directory(root)) {
    throw std::runtime_error("catalog root is not a directory");
  }
  const auto manifest_path   = catalog_manifest_path(root, relative_manifest);
  const auto manifest_before = read_controlled_file(manifest_path, maximum_manifest_bytes);
  auto       parsed          = read_package_manifest(root.string(), relative_manifest);
  if (!parsed) {
    throw std::runtime_error("manifest is not admissible: " + parsed.detail);
  }
  auto manifest = read_controlled_file(manifest_path, maximum_manifest_bytes);
  if (manifest != manifest_before) {
    throw std::runtime_error("manifest changed while it was being validated");
  }
  key_guard key;
  read_private_key(absolute_normal_path(private_key_text, "private key"), key);
  const auto public_der    = canonical_public_der(key.value);
  const auto key_id        = sha256_bytes(public_der.data(), public_der.size());
  const auto manifest_hash = sha256_bytes(manifest.data(), manifest.size());
  const auto signed_hash   = package_signature_digest(manifest_hash, package_sequence);

  random_source                                      random;
  std::array<uint8_t, MBEDTLS_PK_SIGNATURE_MAX_SIZE> signature{};
  std::size_t                                        signature_size = 0;
  if (mbedtls_pk_sign(&key.value,
                      MBEDTLS_MD_SHA256,
                      signed_hash.data(),
                      signed_hash.size(),
                      signature.data(),
                      &signature_size,
                      mbedtls_ctr_drbg_random,
                      &random.generator) != 0) {
    throw std::runtime_error("unable to sign manifest");
  }
  const nlohmann::json sidecar = {
      {"schema_version", 1},
      {"algorithm", signature_algorithm},
      {"key_id", hex(key_id)},
      {"package_sequence", package_sequence},
      {"manifest_sha256", hex(manifest_hash)},
      {"signature", hex(std::vector<uint8_t>(signature.begin(), signature.begin() + signature_size))}};
  const auto output_path = std::filesystem::path(manifest_path.string() + ".sig.json");
  write_new_file(output_path, sidecar.dump(2) + "\n", 0444);
  std::cout << "package-id=" << identifier_text(parsed.package->package_id) << "\nkey-id=" << hex(key_id)
            << "\nsequence=" << package_sequence << "\nsidecar=" << output_path.string() << '\n';
}

struct package_floor {
  identifier package_id;
  uint64_t   minimum_sequence{};
};

package_floor parse_package_floor(std::string_view value)
{
  const auto separator = value.find(':');
  if (separator == std::string_view::npos || separator == 0 || separator + 1 == value.size()) {
    throw std::runtime_error("package floor must use PACKAGE-ID:MINIMUM-SEQUENCE form");
  }
  uint64_t   minimum = 0;
  const auto text    = value.substr(separator + 1);
  const auto parsed  = std::from_chars(text.data(), text.data() + text.size(), minimum);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || minimum == 0) {
    throw std::runtime_error("package minimum sequence must be a nonzero integer");
  }
  return {parse_identifier(value.substr(0, separator)), minimum};
}

void build_trust(const std::string&              output_text,
                 uint64_t                        policy_epoch,
                 const std::vector<std::string>& public_key_paths,
                 const std::vector<std::string>& package_floors,
                 const std::vector<std::string>& revoked_ids)
{
  if (policy_epoch == 0 || public_key_paths.empty() || package_floors.empty()) {
    throw std::runtime_error("trust policy needs a nonzero epoch, at least one key, and at least one package floor");
  }
  nlohmann::json            keys = nlohmann::json::array();
  std::set<artifact_digest> unique_keys;
  for (const auto& path_text : public_key_paths) {
    const auto der    = read_public_key(absolute_normal_path(path_text, "public key"));
    const auto key_id = sha256_bytes(der.data(), der.size());
    if (!unique_keys.insert(key_id).second) {
      throw std::runtime_error("trust policy contains a duplicate public key");
    }
    keys.push_back({{"key_id", hex(key_id)}, {"algorithm", signature_algorithm}, {"public_key_spki", hex(der)}});
  }

  nlohmann::json            revocations = nlohmann::json::array();
  std::set<artifact_digest> unique_revocations;
  for (const auto& text : revoked_ids) {
    const auto key_id = parse_fixed_hex<32>(text, "revoked key identifier");
    if (!unique_revocations.insert(key_id).second) {
      throw std::runtime_error("trust policy contains a duplicate revocation");
    }
    revocations.push_back(hex(key_id));
  }

  nlohmann::json        packages = nlohmann::json::array();
  std::set<std::string> unique_packages;
  for (const auto& text : package_floors) {
    const auto floor      = parse_package_floor(text);
    const auto package_id = identifier_text(floor.package_id);
    if (!unique_packages.insert(package_id).second) {
      throw std::runtime_error("trust policy contains a duplicate package floor");
    }
    packages.push_back({{"package_id", package_id}, {"minimum_sequence", floor.minimum_sequence}});
  }
  const nlohmann::json policy = {{"schema_version", 1},
                                 {"policy_epoch", policy_epoch},
                                 {"keys", std::move(keys)},
                                 {"revoked_key_ids", std::move(revocations)},
                                 {"packages", std::move(packages)}};
  write_new_file(absolute_normal_path(output_text, "trust-policy output"), policy.dump(2) + "\n", 0444);
  std::cout << "policy-epoch=" << policy_epoch << "\nactive-keys=" << public_key_paths.size()
            << "\nrevoked-keys=" << revoked_ids.size() << "\npackages=" << package_floors.size() << '\n';
}

void inspect_package(const std::string& catalog_root_text,
                     const std::string& relative_manifest,
                     const std::string& trust_store_text,
                     uint64_t           minimum_policy_epoch,
                     bool               allow_unsigned)
{
  const auto root     = std::filesystem::canonical(absolute_normal_path(catalog_root_text, "catalog root"));
  const auto trust    = absolute_normal_path(trust_store_text, "trust store");
  auto       verifier = package_signature_verifier::create(
      {allow_unsigned ? package_signature_mode::allow_unsigned : package_signature_mode::required,
       trust.string(),
       minimum_policy_epoch});
  if (!verifier) {
    throw std::runtime_error("trust policy rejected: " + verifier.detail);
  }
  auto manifest = read_package_manifest(root.string(), relative_manifest, verifier.verifier.get());
  if (!manifest) {
    throw std::runtime_error("package rejected: " + manifest.detail);
  }
  artifact_verifier artifacts(root.string());
  auto              verified_artifact =
      artifacts.verify(manifest.package->canonical_artifact_path, manifest.package->content_digest);
  if (!verified_artifact) {
    throw std::runtime_error("artifact rejected: " + verified_artifact.detail);
  }
  const auto& package = *manifest.package;
  std::cout << "package-id=" << identifier_text(package.package_id) << "\nname=" << package.global_name
            << "\nversion=" << package.version << "\nartifact-sha256=" << hex(package.content_digest) << "\nsignature="
            << (package.signature_status == package_signature_status::verified ? "verified" : "unsigned") << '\n';
  std::cout << "manifest-schema-version=" << package.manifest_schema_version << '\n';
  if (package.sbom_verified) {
    std::cout << "sbom=verified\nsbom-sha256=" << hex(package.sbom_digest) << '\n';
  }
  if (package.signature_status == package_signature_status::verified) {
    std::cout << "key-id=" << hex(package.signing_key_id) << "\nsequence=" << package.signature_sequence
              << "\npolicy-epoch=" << package.trust_policy_epoch << '\n';
  }
}

} // namespace

int main(int argc, char** argv)
{
  CLI::App app("Offline OCUDU dApp package signing and inspection");
  app.require_subcommand(1, 1);

  std::string private_key_output;
  std::string public_key_output;
  auto*       keygen_command = app.add_subcommand("keygen", "Generate a file-backed P-256 signing key pair");
  keygen_command->add_option("--private-key", private_key_output)->required();
  keygen_command->add_option("--public-key", public_key_output)->required();

  std::string catalog_root;
  std::string manifest;
  std::string private_key;
  uint64_t    package_sequence = 0;
  auto*       sign_command     = app.add_subcommand("sign", "Create the detached signature sidecar for one manifest");
  sign_command->add_option("--catalog-root", catalog_root)->required();
  sign_command->add_option("--manifest", manifest)->required();
  sign_command->add_option("--private-key", private_key)->required();
  sign_command->add_option("--package-sequence", package_sequence)->required();

  std::string              trust_output;
  uint64_t                 policy_epoch = 0;
  std::vector<std::string> public_keys;
  std::vector<std::string> package_floors;
  std::vector<std::string> revoked_key_ids;
  auto* trust_command = app.add_subcommand("trust", "Build one bounded offline operator trust policy");
  trust_command->add_option("--output", trust_output)->required();
  trust_command->add_option("--policy-epoch", policy_epoch)->required();
  trust_command->add_option("--public-key", public_keys)->required()->take_all();
  trust_command->add_option("--package", package_floors)->required()->take_all();
  trust_command->add_option("--revoke-key-id", revoked_key_ids)->take_all();

  std::string inspect_root;
  std::string inspect_manifest;
  std::string trust_store;
  uint64_t    minimum_policy_epoch = 0;
  bool        allow_unsigned       = false;
  auto* inspect_command = app.add_subcommand("inspect", "Verify package signature, policy, manifest, and artifact");
  inspect_command->add_option("--catalog-root", inspect_root)->required();
  inspect_command->add_option("--manifest", inspect_manifest)->required();
  inspect_command->add_option("--trust-store", trust_store)->required();
  inspect_command->add_option("--minimum-policy-epoch", minimum_policy_epoch)->required();
  inspect_command->add_flag("--allow-unsigned", allow_unsigned);

  CLI11_PARSE(app, argc, argv);
  try {
    if (*keygen_command) {
      keygen(private_key_output, public_key_output);
    } else if (*sign_command) {
      sign_manifest(catalog_root, manifest, private_key, package_sequence);
    } else if (*trust_command) {
      build_trust(trust_output, policy_epoch, public_keys, package_floors, revoked_key_ids);
    } else if (*inspect_command) {
      inspect_package(inspect_root, inspect_manifest, trust_store, minimum_policy_epoch, allow_unsigned);
    }
  } catch (const std::exception& error) {
    std::cerr << "ocudu-dapp-package: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
