// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/management/types.h"
#include "ocudu/dapp/runtime/artifact_verifier.h"
#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/dapp/runtime/package_manifest.h"
#include "ocudu/dapp/runtime/package_provenance.h"
#include "ocudu/dapp/runtime/package_signature.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef OCUDU_DAPP_TEST_SIGNATURE_CRYPTO
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#endif

using namespace ocudu::dapp;
using namespace ocudu::dapp::management;

namespace {

class temporary_directory
{
public:
  temporary_directory()
  {
    std::array<char, 64> pattern{};
    std::snprintf(
        pattern.data(), pattern.size(), "/tmp/ocudu-dapp-signature-%ld-XXXXXX", static_cast<long>(::getpid()));
    char* created = ::mkdtemp(pattern.data());
    if (created == nullptr) {
      throw std::runtime_error("unable to create temporary signature catalog");
    }
    path = created;
  }

  ~temporary_directory() { std::filesystem::remove_all(path); }

  std::filesystem::path path;
};

void write_file(const std::filesystem::path& path, const std::string& bytes, mode_t mode = 0444)
{
  if (std::filesystem::exists(path) && ::chmod(path.c_str(), 0644) != 0) {
    throw std::runtime_error("unable to make signature test file writable");
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  if (!output) {
    throw std::runtime_error("unable to write signature test file");
  }
  if (::chmod(path.c_str(), mode) != 0) {
    throw std::runtime_error("unable to set signature test file mode");
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

std::string identifier_text(const identifier& value)
{
  const std::string compact = hex(value.bytes);
  return compact.substr(0, 8) + '-' + compact.substr(8, 4) + '-' + compact.substr(12, 4) + '-' + compact.substr(16, 4) +
         '-' + compact.substr(20);
}

package_record test_package()
{
  package_record package;
  package.package_id.bytes.back() = 7;
  package.global_name             = "org.deepsig.signature-test";
  return package;
}

#ifdef OCUDU_DAPP_TEST_SIGNATURE_CRYPTO
constexpr char private_key[] = R"(-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIFlQ5uo44ycYaw9FMXj9nFyK41bzbpqHt879CXc6Asn8oAoGCCqGSM49
AwEHoUQDQgAEqT5N0ufjoSjcT0mI58MuuvPPOOuBxs40kB5BeWVEMeSxvp+LDXwl
9kJ3iOxqLntiDOM1BTWwxJ8p5KFE/hYDiw==
-----END EC PRIVATE KEY-----
)";

struct signed_fixture {
  artifact_digest      key_id{};
  std::vector<uint8_t> public_key;
  std::vector<uint8_t> signature;
};

signed_fixture sign(const artifact_digest& digest)
{
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  mbedtls_entropy_context entropy;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_context random;
  mbedtls_ctr_drbg_init(&random);
  const char                                         personalization[] = "ocudu-dapp-package-signature-test";
  signed_fixture                                     result;
  std::array<uint8_t, 512>                           public_buffer{};
  std::array<uint8_t, MBEDTLS_PK_SIGNATURE_MAX_SIZE> signature_buffer{};
  std::size_t                                        signature_size = 0;
  const int                                          seeded         = mbedtls_ctr_drbg_seed(&random,
                                           mbedtls_entropy_func,
                                           &entropy,
                                           reinterpret_cast<const uint8_t*>(personalization),
                                           sizeof(personalization) - 1);
  const int                                          parsed =
      seeded == 0
                                                   ? mbedtls_pk_parse_key(&key, reinterpret_cast<const uint8_t*>(private_key), sizeof(private_key), nullptr, 0)
                                                   : seeded;
  const int public_size =
      parsed == 0 ? mbedtls_pk_write_pubkey_der(&key, public_buffer.data(), public_buffer.size()) : parsed;
  const int signed_status = public_size > 0 ? mbedtls_pk_sign(&key,
                                                              MBEDTLS_MD_SHA256,
                                                              digest.data(),
                                                              digest.size(),
                                                              signature_buffer.data(),
                                                              &signature_size,
                                                              mbedtls_ctr_drbg_random,
                                                              &random)
                                            : public_size;
  if (signed_status == 0) {
    result.public_key.assign(public_buffer.end() - public_size, public_buffer.end());
    result.key_id = sha256_bytes(result.public_key.data(), result.public_key.size());
    result.signature.assign(signature_buffer.begin(), signature_buffer.begin() + signature_size);
  }
  mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&random);
  mbedtls_entropy_free(&entropy);
  if (signed_status != 0) {
    throw std::runtime_error("unable to construct package signature test fixture");
  }
  return result;
}

nlohmann::json trust_document(const package_record& package,
                              const signed_fixture& signature,
                              uint64_t              policy_epoch     = 4,
                              uint64_t              minimum_sequence = 11)
{
  return {
      {"schema_version", 1},
      {"policy_epoch", policy_epoch},
      {"keys",
       {{{"key_id", hex(signature.key_id)},
         {"algorithm", "ecdsa-p256-sha256"},
         {"public_key_spki", hex(signature.public_key)}}}},
      {"revoked_key_ids", nlohmann::json::array()},
      {"packages", {{{"package_id", identifier_text(package.package_id)}, {"minimum_sequence", minimum_sequence}}}}};
}

nlohmann::json
signature_document(const signed_fixture& signature, const artifact_digest& manifest_digest, uint64_t sequence = 11)
{
  return {{"schema_version", 1},
          {"algorithm", "ecdsa-p256-sha256"},
          {"key_id", hex(signature.key_id)},
          {"package_sequence", sequence},
          {"manifest_sha256", hex(manifest_digest)},
          {"signature", hex(signature.signature)}};
}
#endif

} // namespace

TEST(dapp_package_signature, unsigned_development_policy_is_explicit_and_never_satisfies_required_mode)
{
  auto rejected = package_signature_verifier::create({package_signature_mode::required, {}, 1});
  EXPECT_FALSE(rejected);

  auto allowed = package_signature_verifier::create({package_signature_mode::allow_unsigned, {}, 0});
  ASSERT_TRUE(allowed) << allowed.detail;
  temporary_directory directory;
  const std::string   manifest = "manifest bytes";
  auto                package  = test_package();
  auto                verified = allowed.verifier->verify(directory.path.string(),
                                           "package.json",
                                           reinterpret_cast<const uint8_t*>(manifest.data()),
                                           manifest.size(),
                                           package);
  EXPECT_TRUE(verified) << verified.detail;
  EXPECT_EQ(package.signature_status, package_signature_status::unsigned_package);
}

TEST(dapp_package_signature, special_files_never_wait_for_a_fifo_writer)
{
  temporary_directory directory;
  const auto          fifo = directory.path / "pipe";
  ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
  ASSERT_EXIT(
      {
        ::alarm(2);
        artifact_verifier artifact(directory.path.string());
        if (artifact.inspect(fifo.string())) {
          ::_exit(1);
        }
        if (read_package_manifest(directory.path.string(), "pipe")) {
          ::_exit(2);
        }
        package_record package;
        package.canonical_sbom_path = fifo.string();
        if (verify_package_provenance(directory.path.string(), package)) {
          ::_exit(3);
        }
        if (package_signature_verifier::create({package_signature_mode::required, fifo.string(), 1})) {
          ::_exit(4);
        }
        ::_exit(0);
      },
      ::testing::ExitedWithCode(0),
      "");
}

#ifdef OCUDU_DAPP_TEST_SIGNATURE_CRYPTO
TEST(dapp_package_signature, runtime_policy_reload_is_monotonic_and_keeps_original_immutable)
{
  temporary_directory directory;
  auto                package     = test_package();
  auto                signed_data = sign({});
  auto                trust       = trust_document(package, signed_data);
  const auto          path        = directory.path / "trust.json";
  write_file(path, trust.dump());
  auto original = package_signature_verifier::create({package_signature_mode::required, path.string(), 4});
  ASSERT_TRUE(original);
  package.signature_status   = package_signature_status::verified;
  package.signing_key_id     = signed_data.key_id;
  package.signature_sequence = 11;
  EXPECT_TRUE(original.verifier->authorize_admitted(package));
  EXPECT_TRUE(original.verifier->reload());
  auto next = trust;
  next["packages"].push_back({{"package_id", "00112233-4455-6677-8899-aabbccddeeff"}, {"minimum_sequence", 1}});
  write_file(path, next.dump());
  EXPECT_FALSE(original.verifier->reload()); // Same epoch cannot grant new trust.
  next["policy_epoch"] = 5;
  write_file(path, next.dump());
  auto extended = original.verifier->reload();
  ASSERT_TRUE(extended) << extended.detail;
  EXPECT_EQ(extended.verifier->policy_epoch(), 5U);
  EXPECT_EQ(original.verifier->policy_epoch(), 4U);
  write_file(path, trust.dump());
  EXPECT_FALSE(extended.verifier->reload()); // Epoch rollback.
  next["policy_epoch"]                    = 6;
  next["packages"][0]["minimum_sequence"] = 10;
  write_file(path, next.dump());
  EXPECT_FALSE(extended.verifier->reload()); // Floor rollback despite new epoch.
  next["packages"][0]["minimum_sequence"] = 11;
  next["packages"].erase(next["packages"].begin() + 1);
  write_file(path, next.dump());
  EXPECT_FALSE(extended.verifier->reload()); // Forgotten package floor.
  next                 = trust;
  next["policy_epoch"] = 7;
  next["revoked_key_ids"].push_back(hex(signed_data.key_id));
  write_file(path, next.dump());
  auto revoked = original.verifier->reload();
  ASSERT_TRUE(revoked);
  EXPECT_FALSE(revoked.verifier->authorize_admitted(package));
  next["policy_epoch"]    = 8;
  next["revoked_key_ids"] = nlohmann::json::array();
  write_file(path, next.dump());
  EXPECT_FALSE(revoked.verifier->reload());
  write_file(path, next.dump(), 0666);
  EXPECT_FALSE(original.verifier->reload()); // Existing secure-file rules still apply.
}

TEST(dapp_package_signature, runtime_catalog_verifies_then_admits_without_loading_and_replays_transactions)
{
  temporary_directory directory;
  auto                package     = test_package();
  const auto          bundle_path = directory.path / hex(package.package_id.bytes);
  std::filesystem::create_directory(bundle_path);
  const auto manifest_path = bundle_path / "package.manifest.json";
  const auto artifact_path = bundle_path / "receiver.so";
  std::filesystem::copy_file(DAPP_VALID_FIXTURE, artifact_path);
  ASSERT_EQ(::chmod(artifact_path.c_str(), 0444), 0);
  artifact_verifier artifact_check(directory.path.string());
  auto              artifact = artifact_check.inspect(artifact_path.string());
  ASSERT_TRUE(artifact);
  nlohmann::json manifest = {{"schema_version", 1},
                             {"package_id", identifier_text(package.package_id)},
                             {"global_name", "org.deepsig.ocudu.test.valid"},
                             {"vendor", "DeepSig"},
                             {"version", "1.0.0"},
                             {"artifact", "receiver.so"},
                             {"sha256", to_hex(artifact.digest)},
                             {"placements", {"in_process"}},
                             {"backends", {"cpu"}},
                             {"interfaces",
                              {{{"interface_id", 0x1234},
                                {"major", 1},
                                {"minor", 0},
                                {"class", "C"},
                                {"placements", {"in_process"}},
                                {"threading", "reentrant_shared"},
                                {"capabilities", 0}}}}};
  const auto     stage    = [&](uint64_t sequence) {
    const auto bytes     = manifest.dump();
    const auto digest    = sha256_bytes(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    auto       signature = sign(package_signature_digest(digest, sequence));
    write_file(manifest_path, bytes);
    write_file(manifest_path.string() + ".sig.json", signature_document(signature, digest, sequence).dump());
    return signature;
  };
  auto       signature  = stage(11);
  auto       trust      = trust_document(package, signature);
  const auto trust_path = directory.path / "trust.json";
  write_file(trust_path, trust.dump());
  auto policy = package_signature_verifier::create({package_signature_mode::required, trust_path.string(), 4});
  ASSERT_TRUE(policy);
  ocudu_dapp_host_info_v1 host{};
  host.struct_size  = sizeof(host);
  host.abi_major    = OCUDU_DAPP_ABI_MAJOR_V1;
  host.backend_mask = 1;
  ocudu_dapp_host_api_v1 api{};
  api.struct_size = sizeof(api);
  api.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  instance_manager manager(
      directory.path.string(), host, api, 128, {}, {}, std::chrono::milliseconds(500), std::move(policy.verifier));
  uint8_t    transaction = 1;
  const auto request     = [&](bool dry_run = false) {
    catalog_admit_request value;
    value.transaction_id.bytes.back() = transaction++;
    value.package_id                  = package.package_id;
    value.dry_run                     = dry_run;
    return value;
  };
  const auto initial = manager.inventory().generation;
  auto       dry     = manager.admit_package(request(true));
  ASSERT_EQ(dry.code, result_code::dry_run_ok) << dry.detail;
  EXPECT_TRUE(manager.inventory().packages.empty());
  EXPECT_EQ(manager.inventory().generation, initial);
  auto first_request                          = request();
  first_request.expected_inventory_generation = initial;
  auto admitted                               = manager.admit_package(first_request);
  ASSERT_TRUE(admitted) << admitted.detail;
  EXPECT_EQ(manager.inventory().packages.size(), 1U);
  EXPECT_TRUE(manager.inventory().instances.empty());
  EXPECT_EQ(manager.inventory().generation, initial + 1);
  EXPECT_TRUE(manager.admit_package(first_request)); // Replay before stale-generation check.
  ASSERT_EQ(::chmod(artifact_path.c_str(), 0644), 0);
  std::ofstream(artifact_path, std::ios::app) << 'x';
  EXPECT_EQ(manager.admit_package(request(true)).code, result_code::verification_failed);
  EXPECT_EQ(manager.inventory().generation, initial + 1);
  std::filesystem::copy_file(DAPP_VALID_FIXTURE, artifact_path, std::filesystem::copy_options::overwrite_existing);
  ASSERT_EQ(::chmod(artifact_path.c_str(), 0444), 0);
  auto stale                          = request();
  stale.expected_inventory_generation = initial;
  EXPECT_EQ(manager.admit_package(stale).code, result_code::conflict);
  auto mismatch                     = first_request;
  mismatch.package_id.bytes.front() = 1;
  EXPECT_EQ(manager.admit_package(mismatch).code, result_code::transaction_mismatch);
  auto bad       = request();
  bad.package_id = {};
  EXPECT_EQ(manager.admit_package(bad).code, result_code::invalid_request);
  std::filesystem::rename(manifest_path, bundle_path / "target.json");
  std::filesystem::create_symlink(bundle_path / "target.json", manifest_path);
  EXPECT_EQ(manager.admit_package(request()).code, result_code::verification_failed);
  std::filesystem::remove(manifest_path);
  std::filesystem::rename(bundle_path / "target.json", manifest_path);
  stage(10);
  EXPECT_EQ(manager.admit_package(request()).code, result_code::verification_failed);
  stage(12);
  auto updated = manager.admit_package(request());
  ASSERT_TRUE(updated) << updated.detail;
  EXPECT_EQ(manager.package(package.package_id)->signature_sequence, 12U);
  stage(11); // Above trust floor, but below the live high-water mark.
  EXPECT_EQ(manager.admit_package(request()).code, result_code::verification_failed);
  stage(12);
  manifest["version"] = "1.0.1";
  stage(12);
  EXPECT_EQ(manager.admit_package(request()).code, result_code::verification_failed);
  manifest["version"]                       = "1.0.0";
  manifest["interfaces"][0]["capabilities"] = 1;
  stage(13);
  EXPECT_EQ(manager.admit_package(request()).code, result_code::conflict);
  manifest["interfaces"][0]["capabilities"] = 0;
  stage(13);
  ASSERT_TRUE(manager.admit_package(request()));
  load_request load;
  load.transaction_id.bytes.back() = transaction++;
  load.package_id                  = package.package_id;
  auto loaded                      = manager.load(load);
  ASSERT_TRUE(loaded) << loaded.detail;
  EXPECT_EQ(manager.admit_package(request()).code, result_code::conflict);
  EXPECT_EQ(manager.inventory().instances.size(), 1U);
}

TEST(dapp_package_signature, verifies_rotation_metadata_and_fails_closed_for_security_negative_vectors)
{
  temporary_directory directory;
  const std::string   manifest = "{\"schema_version\":1,\"package\":\"exact bytes\"}\n";
  const auto manifest_digest   = sha256_bytes(reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size());
  const auto signed_data       = sign(package_signature_digest(manifest_digest, 11));
  auto       package           = test_package();
  const auto trust_path        = directory.path / "trust.json";
  const auto signature_path    = directory.path / "package.json.sig.json";
  auto       trust             = trust_document(package, signed_data);
  auto       sidecar           = signature_document(signed_data, manifest_digest);
  write_file(trust_path, trust.dump());
  write_file(signature_path, sidecar.dump());

  auto verifier = package_signature_verifier::create({package_signature_mode::required, trust_path.string(), 4});
  ASSERT_TRUE(verifier) << verifier.detail;
  auto accepted = verifier.verifier->verify(directory.path.string(),
                                            "package.json",
                                            reinterpret_cast<const uint8_t*>(manifest.data()),
                                            manifest.size(),
                                            package);
  ASSERT_TRUE(accepted) << accepted.detail;
  EXPECT_EQ(package.signature_status, package_signature_status::verified);
  EXPECT_EQ(package.signing_key_id, signed_data.key_id);
  EXPECT_EQ(package.signature_sequence, 11U);
  EXPECT_EQ(package.trust_policy_epoch, 4U);

  auto              tampered_package  = test_package();
  const std::string tampered_manifest = manifest + " ";
  EXPECT_FALSE(verifier.verifier->verify(directory.path.string(),
                                         "package.json",
                                         reinterpret_cast<const uint8_t*>(tampered_manifest.data()),
                                         tampered_manifest.size(),
                                         tampered_package));

  sidecar["key_id"] = std::string(64, '1');
  write_file(signature_path, sidecar.dump());
  EXPECT_FALSE(verifier.verifier->verify(directory.path.string(),
                                         "package.json",
                                         reinterpret_cast<const uint8_t*>(manifest.data()),
                                         manifest.size(),
                                         tampered_package));

  write_file(signature_path, "{\"schema_version\":1,\"schema_version\":1}");
  EXPECT_FALSE(verifier.verifier->verify(directory.path.string(),
                                         "package.json",
                                         reinterpret_cast<const uint8_t*>(manifest.data()),
                                         manifest.size(),
                                         tampered_package));

  std::filesystem::remove(signature_path);
  EXPECT_FALSE(verifier.verifier->verify(directory.path.string(),
                                         "package.json",
                                         reinterpret_cast<const uint8_t*>(manifest.data()),
                                         manifest.size(),
                                         tampered_package));
  auto development =
      package_signature_verifier::create({package_signature_mode::allow_unsigned, trust_path.string(), 4});
  ASSERT_TRUE(development) << development.detail;
  EXPECT_TRUE(development.verifier->verify(directory.path.string(),
                                           "package.json",
                                           reinterpret_cast<const uint8_t*>(manifest.data()),
                                           manifest.size(),
                                           tampered_package));
  write_file(signature_path, "not JSON");
  EXPECT_FALSE(development.verifier->verify(directory.path.string(),
                                            "package.json",
                                            reinterpret_cast<const uint8_t*>(manifest.data()),
                                            manifest.size(),
                                            tampered_package));

  trust = trust_document(package, signed_data);
  trust["revoked_key_ids"].push_back(hex(signed_data.key_id));
  write_file(trust_path, trust.dump());
  auto revoked = package_signature_verifier::create({package_signature_mode::required, trust_path.string(), 4});
  ASSERT_TRUE(revoked) << revoked.detail;
  write_file(signature_path, signature_document(signed_data, manifest_digest).dump());
  EXPECT_FALSE(revoked.verifier->verify(directory.path.string(),
                                        "package.json",
                                        reinterpret_cast<const uint8_t*>(manifest.data()),
                                        manifest.size(),
                                        tampered_package));

  trust = trust_document(package, signed_data, 4, 12);
  write_file(trust_path, trust.dump());
  auto replay = package_signature_verifier::create({package_signature_mode::required, trust_path.string(), 4});
  ASSERT_TRUE(replay) << replay.detail;
  EXPECT_FALSE(replay.verifier->verify(directory.path.string(),
                                       "package.json",
                                       reinterpret_cast<const uint8_t*>(manifest.data()),
                                       manifest.size(),
                                       tampered_package));

  trust = trust_document(package, signed_data, 3);
  write_file(trust_path, trust.dump());
  EXPECT_FALSE(package_signature_verifier::create({package_signature_mode::required, trust_path.string(), 4}));

  std::filesystem::remove(signature_path);
  const auto outside = directory.path.parent_path() / (directory.path.filename().string() + "-outside");
  write_file(outside, signature_document(signed_data, manifest_digest).dump());
  ASSERT_EQ(::symlink(outside.c_str(), signature_path.c_str()), 0);
  trust = trust_document(package, signed_data);
  write_file(trust_path, trust.dump());
  auto symlink_verifier =
      package_signature_verifier::create({package_signature_mode::required, trust_path.string(), 4});
  ASSERT_TRUE(symlink_verifier) << symlink_verifier.detail;
  EXPECT_FALSE(symlink_verifier.verifier->verify(directory.path.string(),
                                                 "package.json",
                                                 reinterpret_cast<const uint8_t*>(manifest.data()),
                                                 manifest.size(),
                                                 tampered_package));
  std::filesystem::remove(outside);
}
#endif
