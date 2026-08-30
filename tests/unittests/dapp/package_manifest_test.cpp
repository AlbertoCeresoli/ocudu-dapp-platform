// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "../phy/support/resource_grid_test_doubles.h"
#include "apps/services/dapp/dapp_service.h"
#include "fixtures/fixture_common.h"
#include "ocudu/adt/complex.h"
#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/runtime/artifact_verifier.h"
#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/dapp/runtime/package_manifest.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_hook_provider.h"
#include "ocudu/phy/upper/phy_tap/phy_tap.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_configuration.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_factory.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include "ocudu/ran/slot_point.h"
#include "ocudu/ran/srs/srs_information.h"
#include "ocudu/scheduler/dapp/quiet_reservation.h"
#include "ocudu/scheduler/dapp/scheduler_dapp_context.h"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace ocudu;
using namespace ocudu::app_services;
using namespace ocudu::dapp;
using namespace ocudu::dapp::management;
using namespace std::chrono_literals;

namespace {

using json = nlohmann::json;

json valid_manifest()
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              artifact = verifier.inspect(DAPP_VALID_FIXTURE);
  EXPECT_TRUE(artifact) << artifact.detail;
  return {{"schema_version", 1},
          {"package_id", "00112233-4455-6677-8899-aabbccddeeff"},
          {"global_name", "org.deepsig.ocudu.test.valid"},
          {"vendor", "DeepSig"},
          {"version", "1.0.0"},
          {"artifact", std::filesystem::path(DAPP_VALID_FIXTURE).filename().string()},
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
}

json valid_spdx_sbom(const json& manifest)
{
  return {{"spdxVersion", "SPDX-2.3"},
          {"dataLicense", "CC0-1.0"},
          {"SPDXID", "SPDXRef-DOCUMENT"},
          {"name", manifest.at("global_name").get<std::string>() + "-sbom"},
          {"documentNamespace", "https://ocudu.org/spdx/test/00112233"},
          {"creationInfo", {{"created", "2026-07-13T00:00:00Z"}, {"creators", {"Tool: OCUDU dApp manifest test"}}}},
          {"packages",
           {{{"SPDXID", "SPDXRef-Package"},
             {"name", manifest.at("global_name")},
             {"versionInfo", manifest.at("version")},
             {"supplier", "Organization: " + manifest.at("vendor").get<std::string>()},
             {"filesAnalyzed", true},
             {"hasFiles", {"SPDXRef-Artifact"}}}}},
          {"files",
           {{{"SPDXID", "SPDXRef-Artifact"},
             {"fileName", "./" + manifest.at("artifact").get<std::string>()},
             {"checksums", {{{"algorithm", "SHA256"}, {"checksumValue", manifest.at("sha256")}}}}}}},
          {"relationships",
           {{{"spdxElementId", "SPDXRef-DOCUMENT"},
             {"relationshipType", "DESCRIBES"},
             {"relatedSpdxElement", "SPDXRef-Package"}}}}};
}

json valid_process_manifest()
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              artifact = verifier.inspect(DAPP_CLASS_C_PROCESS_WORKER);
  EXPECT_TRUE(artifact) << artifact.detail;
  return {{"schema_version", 1},
          {"package_id", "00112233-4455-6677-8899-aabbccddee01"},
          {"global_name", "org.deepsig.ocudu.test.spectrum-process"},
          {"vendor", "DeepSig"},
          {"version", "1.0.0"},
          {"artifact", std::filesystem::path(DAPP_CLASS_C_PROCESS_WORKER).filename().string()},
          {"sha256", to_hex(artifact.digest)},
          {"placements", {"out_of_process"}},
          {"backends", {"cpu"}},
          {"interfaces",
           {{{"interface_id", OCUDU_DAPP_INTERFACE_SPECTRUM_V1},
             {"major", 1},
             {"minor", 0},
             {"class", "C"},
             {"placements", {"out_of_process"}},
             {"threading", "per_worker"},
             {"capabilities", OCUDU_DAPP_SPECTRUM_CAP_INTERFERENCE_MAP_V1}}}}};
}

json valid_process_srs_manifest()
{
  auto document                             = valid_process_manifest();
  document["package_id"]                    = "00112233-4455-6677-8899-aabbccddee04";
  document["global_name"]                   = "org.deepsig.ocudu.test.srs-process";
  document["interfaces"][0]["interface_id"] = OCUDU_DAPP_INTERFACE_SRS_ISAC_V1;
  document["interfaces"][0]["capabilities"] = 0;
  return document;
}

json valid_process_srs_optional_dmrs_manifest()
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              artifact = verifier.inspect(DAPP_CLASS_C_PROCESS_BUNDLE_WORKER);
  EXPECT_TRUE(artifact) << artifact.detail;
  auto document           = valid_process_srs_manifest();
  document["package_id"]  = "00112233-4455-6677-8899-aabbccddee07";
  document["global_name"] = "org.deepsig.ocudu.test.srs-optional-dmrs-process";
  document["artifact"]    = std::filesystem::path(DAPP_CLASS_C_PROCESS_BUNDLE_WORKER).filename().string();
  document["sha256"]      = to_hex(artifact.digest);
  document["interfaces"].push_back({{"interface_id", 0x7f010002U},
                                    {"major", 1},
                                    {"minor", 0},
                                    {"class", "C"},
                                    {"placements", {"out_of_process"}},
                                    {"threading", "per_worker"},
                                    {"binding", "optional"},
                                    {"capabilities", 0}});
  return document;
}

json valid_native_spectrum_manifest()
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              artifact = verifier.inspect(DAPP_CLASS_C_FIXTURE);
  EXPECT_TRUE(artifact) << artifact.detail;
  return {{"schema_version", 1},
          {"package_id", "00112233-4455-6677-8899-aabbccddee02"},
          {"global_name", "org.deepsig.ocudu.test.class-c"},
          {"vendor", "DeepSig"},
          {"version", "1.0.0"},
          {"artifact", std::filesystem::path(DAPP_CLASS_C_FIXTURE).filename().string()},
          {"sha256", to_hex(artifact.digest)},
          {"placements", {"in_process"}},
          {"backends", {"cpu"}},
          {"interfaces",
           {{{"interface_id", OCUDU_DAPP_INTERFACE_SPECTRUM_V1},
             {"major", 1},
             {"minor", 0},
             {"class", "C"},
             {"placements", {"in_process", "out_of_process"}},
             {"threading", "per_worker"},
             {"capabilities",
              OCUDU_DAPP_SPECTRUM_CAP_INTERFERENCE_MAP_V1 | OCUDU_DAPP_SPECTRUM_CAP_QUIET_PERIOD_INTENT_V1 |
                  OCUDU_DAPP_SPECTRUM_CAP_FUTURE_UL_PLAN_V1}}}}};
}

json valid_native_srs_manifest()
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              artifact = verifier.inspect(DAPP_CLASS_C_SRS_FIXTURE);
  EXPECT_TRUE(artifact) << artifact.detail;
  return {{"schema_version", 1},
          {"package_id", "00112233-4455-6677-8899-aabbccddee03"},
          {"global_name", "org.deepsig.ocudu.test.class-c-srs"},
          {"vendor", "DeepSig"},
          {"version", "1.0.0"},
          {"artifact", std::filesystem::path(DAPP_CLASS_C_SRS_FIXTURE).filename().string()},
          {"sha256", to_hex(artifact.digest)},
          {"placements", {"in_process"}},
          {"backends", {"cpu"}},
          {"interfaces",
           {{{"interface_id", OCUDU_DAPP_INTERFACE_SRS_ISAC_V1},
             {"major", 1},
             {"minor", 0},
             {"class", "C"},
             {"placements", {"in_process"}},
             {"threading", "per_worker"},
             {"capabilities", 0}}}}};
}

struct complex32 {
  float real;
  float imag;
};

class contiguous_process_grid final : public resource_grid_reader_spy
{
public:
  contiguous_process_grid(unsigned ports, unsigned symbols, unsigned prbs) :
    resource_grid_reader_spy(ports, symbols, prbs),
    storage(ports * symbols * prbs * NOF_SUBCARRIERS_PER_RB, to_cbf16(cf_t{}))
  {
  }

  span<const cbf16_t> get_contiguous_view() const override { return storage; }

private:
  std::vector<cbf16_t> storage;
};

ocudu_dapp_spectrum_input_v1 spectrum_input(const complex32*                      samples,
                                            uint64_t                              sequence,
                                            ocudu_dapp_spectrum_slot_metadata_v1& metadata,
                                            std::array<uint8_t, 1>&               plan)
{
  ocudu_dapp_spectrum_input_v1 input{};
  input.invocation.struct_size                     = sizeof(input.invocation);
  input.invocation.abi_major                       = OCUDU_DAPP_ABI_MAJOR_V1;
  input.invocation.abi_minor                       = OCUDU_DAPP_ABI_MINOR_V1;
  input.invocation.invocation_id                   = sequence;
  input.invocation.cell_id                         = 1;
  input.invocation.worker_id                       = 0;
  input.invocation.radio_time                      = {5, static_cast<uint16_t>(sequence), 0, 0};
  input.invocation.input_generation                = sequence;
  input.uplink_resource_grid.struct_size           = sizeof(input.uplink_resource_grid);
  input.uplink_resource_grid.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  input.uplink_resource_grid.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  input.uplink_resource_grid.domain                = OCUDU_DAPP_MEM_HOST_V1;
  input.uplink_resource_grid.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
  input.uplink_resource_grid.element_type          = OCUDU_DAPP_ELEMENT_CF32_V1;
  input.uplink_resource_grid.rank                  = 3;
  input.uplink_resource_grid.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  input.uplink_resource_grid.allocation_id         = sequence;
  input.uplink_resource_grid.allocation_generation = sequence;
  input.uplink_resource_grid.native_handle         = reinterpret_cast<uintptr_t>(samples);
  input.uplink_resource_grid.byte_length           = 12 * sizeof(complex32);
  input.uplink_resource_grid.dimensions[0]         = 1;
  input.uplink_resource_grid.dimensions[1]         = 1;
  input.uplink_resource_grid.dimensions[2]         = 12;
  input.uplink_resource_grid.byte_strides[0]       = 12 * sizeof(complex32);
  input.uplink_resource_grid.byte_strides[1]       = 12 * sizeof(complex32);
  input.uplink_resource_grid.byte_strides[2]       = sizeof(complex32);
  input.uplink_resource_grid.lease_token           = sequence;
  metadata.struct_size                             = sizeof(metadata);
  metadata.abi_major                               = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.bandwidth_hz                            = 15'000;
  metadata.rx_ports                                = 1;
  metadata.symbols                                 = 1;
  metadata.subcarriers                             = 12;
  metadata.amplitude_scale                         = 1.0F;
  metadata.flags                                   = OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1;
  metadata.source_plan_generation                  = sequence;
  plan[0]                                          = OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1;
  input.scheduled_re_mask.struct_size              = sizeof(input.scheduled_re_mask);
  input.scheduled_re_mask.abi_major                = OCUDU_DAPP_ABI_MAJOR_V1;
  input.scheduled_re_mask.domain                   = OCUDU_DAPP_MEM_HOST_V1;
  input.scheduled_re_mask.backend_id               = OCUDU_DAPP_BACKEND_HOST_V1;
  input.scheduled_re_mask.element_type             = OCUDU_DAPP_ELEMENT_U8_V1;
  input.scheduled_re_mask.rank                     = 2;
  input.scheduled_re_mask.access                   = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  input.scheduled_re_mask.allocation_id            = sequence;
  input.scheduled_re_mask.allocation_generation    = sequence;
  input.scheduled_re_mask.native_handle            = reinterpret_cast<uintptr_t>(plan.data());
  input.scheduled_re_mask.byte_length              = plan.size();
  input.scheduled_re_mask.dimensions[0]            = 1;
  input.scheduled_re_mask.dimensions[1]            = 1;
  input.scheduled_re_mask.byte_strides[0]          = 1;
  input.scheduled_re_mask.byte_strides[1]          = 1;
  input.scheduled_re_mask.layout_id                = OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1;
  input.scheduled_re_mask.layout_version           = 1;
  input.scheduled_re_mask.lease_token              = sequence;
  input.slot_metadata.struct_size                  = sizeof(input.slot_metadata);
  input.slot_metadata.abi_major                    = OCUDU_DAPP_ABI_MAJOR_V1;
  input.slot_metadata.metadata_schema_id           = OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1;
  input.slot_metadata.metadata_schema_version      = 1;
  input.slot_metadata.metadata                     = {&metadata, sizeof(metadata), 0};
  input.source_sequence                            = sequence;
  return input;
}

ocudu_dapp_srs_isac_input_v1
srs_input(const complex32* samples, uint64_t sequence, ocudu_dapp_srs_isac_metadata_v1& metadata)
{
  ocudu_dapp_srs_isac_input_v1 input{};
  input.invocation.struct_size               = sizeof(input.invocation);
  input.invocation.abi_major                 = OCUDU_DAPP_ABI_MAJOR_V1;
  input.invocation.abi_minor                 = OCUDU_DAPP_ABI_MINOR_V1;
  input.invocation.invocation_id             = sequence;
  input.invocation.cell_id                   = 1;
  input.invocation.worker_id                 = 0;
  input.invocation.radio_time                = {5, static_cast<uint16_t>(sequence), 0, 0};
  input.invocation.input_generation          = sequence;
  auto& tensor                               = input.channel_estimates;
  tensor.struct_size                         = sizeof(tensor);
  tensor.abi_major                           = OCUDU_DAPP_ABI_MAJOR_V1;
  tensor.abi_minor                           = OCUDU_DAPP_ABI_MINOR_V1;
  tensor.domain                              = OCUDU_DAPP_MEM_HOST_V1;
  tensor.backend_id                          = OCUDU_DAPP_BACKEND_HOST_V1;
  tensor.element_type                        = OCUDU_DAPP_ELEMENT_CF32_V1;
  tensor.rank                                = 3;
  tensor.access                              = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  tensor.allocation_id                       = sequence;
  tensor.allocation_generation               = sequence;
  tensor.native_handle                       = reinterpret_cast<uintptr_t>(samples);
  tensor.byte_length                         = 4 * sizeof(complex32);
  tensor.dimensions[0]                       = 1;
  tensor.dimensions[1]                       = 1;
  tensor.dimensions[2]                       = 4;
  tensor.byte_strides[0]                     = 4 * sizeof(complex32);
  tensor.byte_strides[1]                     = 4 * sizeof(complex32);
  tensor.byte_strides[2]                     = sizeof(complex32);
  tensor.lease_token                         = sequence;
  metadata.struct_size                       = sizeof(metadata);
  metadata.abi_major                         = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.abi_minor                         = OCUDU_DAPP_ABI_MINOR_V1;
  metadata.srs_resource_id                   = 17;
  metadata.srs_config_id                     = 19;
  metadata.rx_ports                          = 1;
  metadata.tx_ports                          = 1;
  metadata.comb_size                         = 2;
  metadata.subcarrier_spacing_hz             = 30'000;
  metadata.frequency_points                  = 4;
  metadata.noise_variance                    = 0.25F;
  input.srs_metadata.struct_size             = sizeof(input.srs_metadata);
  input.srs_metadata.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  input.srs_metadata.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
  input.srs_metadata.metadata_schema_id      = OCUDU_DAPP_METADATA_SRS_ISAC_V1;
  input.srs_metadata.metadata_schema_version = 1;
  input.srs_metadata.metadata                = {&metadata, sizeof(metadata), 0};
  input.source_sequence                      = sequence;
  return input;
}

class capturing_srs_sink final : public dapp_srs_isac_report_sink
{
public:
  ocudu_dapp_status_v1 publish(const ocudu_dapp_srs_isac_cir_preview_v1& value) override
  {
    if (throw_on_publish.load(std::memory_order_relaxed)) {
      throw std::runtime_error("test SRS report sink failure");
    }
    std::lock_guard<std::mutex> lock(mutex);
    report = value;
    ++count;
    changed.notify_all();
    return OCUDU_DAPP_OK_V1;
  }

  bool wait_for(uint64_t expected, std::chrono::milliseconds timeout)
  {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, timeout, [&] { return count >= expected; });
  }

  std::mutex                         mutex;
  std::condition_variable            changed;
  ocudu_dapp_srs_isac_cir_preview_v1 report{};
  uint64_t                           count{};
  std::atomic<bool>                  throw_on_publish{};
};

class process_srs_estimator final : public srs_estimator
{
public:
  srs_estimator_result estimate(const resource_grid_reader&, const srs_estimator_configuration&) override
  {
    srs_estimator_result result{};
    result.noise_variance = 0.25F;
    result.rsrp_dB        = -10.0F;
    result.epre_dB        = -8.0F;
    return result;
  }
};

class process_srs_validator final : public srs_estimator_configuration_validator
{
public:
  error_type<std::string> is_valid(const srs_estimator_configuration&) const override { return default_success_t{}; }
};

class process_srs_factory final : public srs_estimator_factory
{
public:
  std::unique_ptr<srs_estimator> create() override { return std::make_unique<process_srs_estimator>(); }
  std::unique_ptr<srs_estimator_configuration_validator> create_validator() override
  {
    return std::make_unique<process_srs_validator>();
  }
};

srs_estimator_configuration process_srs_config()
{
  srs_estimator_configuration config{};
  config.cell_id        = 1;
  config.producer_shard = 0;
  config.slot           = slot_point{0, 2};
  config.resource       = {.nof_antenna_ports   = srs_resource_configuration::one_two_four_enum::one,
                           .nof_symbols         = srs_nof_symbols::n1,
                           .start_symbol        = 13,
                           .configuration_index = 0,
                           .sequence_id         = 0,
                           .bandwidth_index     = 0,
                           .comb_size           = tx_comb_size::n2,
                           .comb_offset         = 0,
                           .cyclic_shift        = 0,
                           .freq_position       = 0,
                           .freq_shift          = 0,
                           .freq_hopping        = 0,
                           .hopping             = srs_group_or_sequence_hopping::neither,
                           .periodicity         = std::nullopt};
  config.ports          = {0};
  config.context        = srs_context(0, to_rnti(0x4601));
  return config;
}

class temporary_control_file
{
public:
  temporary_control_file(const char* prefix, const json& document) : temporary_control_file(prefix, document.dump(2)) {}

  temporary_control_file(const char* prefix, const std::string& contents)
  {
    static std::atomic<uint64_t> sequence{};
    filename = std::string(prefix) + "-" + std::to_string(::getpid()) + "-" +
               std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".json";
    path = std::filesystem::path(DAPP_TEST_CATALOG) / filename;
    std::ofstream output(path);
    output << contents;
    output.close();
    EXPECT_TRUE(output);
    EXPECT_EQ(::chmod(path.c_str(), 0444), 0);
  }

  ~temporary_control_file()
  {
    static_cast<void>(::chmod(path.c_str(), 0644));
    std::error_code error;
    std::filesystem::remove(path, error);
  }

  temporary_control_file(const temporary_control_file&)            = delete;
  temporary_control_file& operator=(const temporary_control_file&) = delete;

  std::string           filename;
  std::filesystem::path path;
};

class temporary_manifest : public temporary_control_file
{
public:
  explicit temporary_manifest(const json& document) : temporary_control_file("manifest", document) {}
  explicit temporary_manifest(const std::string& contents) : temporary_control_file("manifest", contents) {}
};

artifact_digest file_digest(const std::filesystem::path& path)
{
  std::ifstream        input(path, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return sha256_bytes(bytes.data(), bytes.size());
}

dapp_service_config service_config(const std::string& manifest)
{
  dapp_service_config result;
  result.catalog_root                            = DAPP_TEST_CATALOG;
  result.native_preflight_helper                 = DAPP_PREFLIGHT_HELPER;
  result.package_signatures.mode                 = dapp::package_signature_mode::allow_unsigned;
  result.package_signatures.minimum_policy_epoch = 0;
  result.package_manifests                       = {manifest};
  result.cells                                   = {{1, 1, 52}};
  result.nof_pusch_lanes                         = 1;
  result.nof_srs_lanes                           = 1;
  result.nof_scheduler_lanes                     = 1;
  result.maximum_transactions                    = 16;
  result.maximum_queued_jobs                     = 4;
  result.maximum_retained_jobs                   = 16;
  result.class_c_process.ring_slots              = 2;
  result.class_c_process.payload_capacity_bytes  = 64 * 1024;
  result.class_c_process.startup_timeout         = std::chrono::milliseconds(250);
  result.class_c_process.stop_timeout            = std::chrono::milliseconds(250);
  result.class_c_process.restart_backoff         = std::chrono::milliseconds(10);
  return result;
}

operation_result control(instance_manager& manager,
                         const identifier& instance,
                         uint8_t           transaction,
                         lifecycle_action  action,
                         uint64_t          generation)
{
  lifecycle_request request;
  request.transaction_id.bytes.back() = transaction;
  request.instance_id                 = instance;
  request.action                      = action;
  request.expected_generation         = generation;
  request.timeout_us                  = 1'000'000;
  return manager.control(request);
}

} // namespace

TEST(dapp_package_manifest, parses_a_bounded_catalog_relative_contract)
{
  temporary_manifest manifest(valid_manifest());
  auto               parsed = read_package_manifest(DAPP_TEST_CATALOG, manifest.filename);
  ASSERT_TRUE(parsed) << parsed.detail;
  ASSERT_TRUE(parsed.package);
  EXPECT_EQ(parsed.package->global_name, "org.deepsig.ocudu.test.valid");
  EXPECT_EQ(parsed.package->vendor, "DeepSig");
  EXPECT_EQ(parsed.package->version, "1.0.0");
  EXPECT_EQ(parsed.package->canonical_artifact_path, std::filesystem::canonical(DAPP_VALID_FIXTURE));
  EXPECT_EQ(parsed.package->backends, std::vector<backend>{backend::cpu});
  EXPECT_EQ(parsed.package->placement_mask, 1U);
  ASSERT_EQ(parsed.package->interfaces.size(), 1U);
  EXPECT_EQ(parsed.package->interfaces[0].interface_id, 0x1234U);
  EXPECT_EQ(parsed.package->interfaces[0].dapp_class, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1);
  EXPECT_EQ(parsed.package->interfaces[0].threading, OCUDU_DAPP_THREADING_REENTRANT_SHARED_V1);
  EXPECT_EQ(parsed.package->interfaces[0].requirement, binding_requirement::required);
  EXPECT_EQ(parsed.package->interfaces[0].state, binding_state::unbound);
  EXPECT_EQ(parsed.package->manifest_schema_version, 1U);
  EXPECT_FALSE(parsed.package->sbom_verified);
}

TEST(dapp_package_manifest, parses_optional_v1_binding_policy_and_rejects_unknown_values)
{
  auto document                        = valid_manifest();
  document["interfaces"][0]["binding"] = "optional";
  temporary_manifest optional_manifest(document);
  auto               parsed = read_package_manifest(DAPP_TEST_CATALOG, optional_manifest.filename);
  ASSERT_TRUE(parsed) << parsed.detail;
  ASSERT_TRUE(parsed.package);
  EXPECT_EQ(parsed.package->interfaces[0].requirement, binding_requirement::optional);
  EXPECT_EQ(parsed.package->interfaces[0].state, binding_state::unbound);

  document["interfaces"][0]["binding"] = "best_effort";
  temporary_manifest invalid_manifest(document);
  EXPECT_FALSE(read_package_manifest(DAPP_TEST_CATALOG, invalid_manifest.filename));
}

TEST(dapp_package_manifest, verifies_manifest_v1_spdx_provenance_before_admission)
{
  auto                   document = valid_manifest();
  temporary_control_file sbom("sbom", valid_spdx_sbom(document));
  document["provenance"] = {
      {"sbom", sbom.filename}, {"format", "spdx-2.3-json"}, {"sha256", to_hex(file_digest(sbom.path))}};
  temporary_manifest manifest(document);
  auto               parsed = read_package_manifest(DAPP_TEST_CATALOG, manifest.filename);
  ASSERT_TRUE(parsed) << parsed.detail;
  ASSERT_TRUE(parsed.package);
  EXPECT_EQ(parsed.package->manifest_schema_version, 1U);
  EXPECT_TRUE(parsed.package->sbom_verified);
  EXPECT_EQ(parsed.package->canonical_sbom_path, sbom.path.string());

  ASSERT_EQ(::chmod(sbom.path.c_str(), 0464), 0);
  auto writable = read_package_manifest(DAPP_TEST_CATALOG, manifest.filename);
  EXPECT_FALSE(writable);
  EXPECT_NE(writable.detail.find("ownership, mode"), std::string::npos) << writable.detail;

  ASSERT_EQ(::chmod(sbom.path.c_str(), 0644), 0);
  std::ofstream(sbom.path, std::ios::app) << ' ';
  ASSERT_EQ(::chmod(sbom.path.c_str(), 0444), 0);
  auto tampered = read_package_manifest(DAPP_TEST_CATALOG, manifest.filename);
  EXPECT_FALSE(tampered);
  EXPECT_NE(tampered.detail.find("SBOM SHA-256"), std::string::npos) << tampered.detail;
}

TEST(dapp_package_manifest, rejects_unbound_or_unsafe_manifest_v1_provenance)
{
  auto expect_rejected = [](json sbom_document) {
    auto                   document = valid_manifest();
    temporary_control_file sbom("sbom", sbom_document);
    document["provenance"] = {
        {"sbom", sbom.filename}, {"format", "spdx-2.3-json"}, {"sha256", to_hex(file_digest(sbom.path))}};
    temporary_manifest manifest(document);
    auto               parsed = read_package_manifest(DAPP_TEST_CATALOG, manifest.filename);
    EXPECT_FALSE(parsed) << manifest.filename;
  };

  auto document                      = valid_manifest();
  auto sbom                          = valid_spdx_sbom(document);
  sbom["packages"][0]["versionInfo"] = "different";
  expect_rejected(std::move(sbom));

  sbom                            = valid_spdx_sbom(document);
  sbom["packages"][0]["supplier"] = "Organization: another vendor";
  expect_rejected(std::move(sbom));

  sbom                                              = valid_spdx_sbom(document);
  sbom["files"][0]["checksums"][0]["checksumValue"] = std::string(64, '0');
  expect_rejected(std::move(sbom));

  document["provenance"] = {
      {"sbom", "not-present.spdx.json"}, {"format", "spdx-2.3-json"}, {"sha256", std::string(64, '0')}};
  temporary_manifest missing(document);
  EXPECT_FALSE(read_package_manifest(DAPP_TEST_CATALOG, missing.filename));

  document = valid_manifest();
  temporary_control_file symlink_target("sbom-target", valid_spdx_sbom(document));
  const auto             symlink_path = symlink_target.path.string() + ".link";
  ASSERT_EQ(::symlink(symlink_target.path.filename().c_str(), symlink_path.c_str()), 0);
  document["provenance"] = {{"sbom", std::filesystem::path(symlink_path).filename().string()},
                            {"format", "spdx-2.3-json"},
                            {"sha256", to_hex(file_digest(symlink_target.path))}};
  temporary_manifest symlink_manifest(document);
  auto               symlinked = read_package_manifest(DAPP_TEST_CATALOG, symlink_manifest.filename);
  EXPECT_FALSE(symlinked);
  EXPECT_NE(symlinked.detail.find("symbolic link"), std::string::npos) << symlinked.detail;
  std::error_code remove_error;
  std::filesystem::remove(symlink_path, remove_error);

  document               = valid_manifest();
  document["provenance"] = {{"sbom", "unexpected"}};
  temporary_manifest invalid_extension(document);
  EXPECT_FALSE(read_package_manifest(DAPP_TEST_CATALOG, invalid_extension.filename));
}

TEST(dapp_package_manifest, parses_and_bounds_class_a_pusch_admission_profile)
{
  auto document                              = valid_manifest();
  document["interfaces"][0]["interface_id"]  = OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1;
  document["interfaces"][0]["class"]         = "A";
  document["interfaces"][0]["threading"]     = "per_worker";
  document["interfaces"][0]["pusch_profile"] = {{"max_prb", 275},
                                                {"max_rx_ports", 64},
                                                {"max_layers", 2},
                                                {"modulation_mask", 15},
                                                {"dmrs_type_mask", 3},
                                                {"flags", 0}};
  temporary_manifest manifest(document);
  auto               parsed = read_package_manifest(DAPP_TEST_CATALOG, manifest.filename);
  ASSERT_TRUE(parsed) << parsed.detail;
  ASSERT_TRUE(parsed.package->interfaces[0].pusch);
  EXPECT_EQ(parsed.package->interfaces[0].pusch->max_nof_layers, 2U);

  document["interfaces"][0]["pusch_profile"]["max_layers"] = 0;
  temporary_manifest invalid_bound(document);
  EXPECT_FALSE(read_package_manifest(DAPP_TEST_CATALOG, invalid_bound.filename));

  document["interfaces"][0].erase("pusch_profile");
  temporary_manifest missing_profile(document);
  EXPECT_FALSE(read_package_manifest(DAPP_TEST_CATALOG, missing_profile.filename));
}

TEST(dapp_package_manifest, rejects_ambiguous_or_unsafe_authority_records)
{
  auto expect_rejected = [](json document) {
    temporary_manifest manifest(document);
    auto               parsed = read_package_manifest(DAPP_TEST_CATALOG, manifest.filename);
    EXPECT_FALSE(parsed) << manifest.filename;
  };

  auto document               = valid_manifest();
  document["unsupported_key"] = true;
  expect_rejected(std::move(document));

  document               = valid_manifest();
  document["package_id"] = "00112233--445566778899aabbccddeeff";
  expect_rejected(std::move(document));

  std::string duplicate_key = valid_manifest().dump();
  duplicate_key.insert(1, "\"schema_version\":1,");
  temporary_manifest duplicate(duplicate_key);
  EXPECT_FALSE(read_package_manifest(DAPP_TEST_CATALOG, duplicate.filename));

  document             = valid_manifest();
  document["artifact"] = "../../outside.so";
  expect_rejected(std::move(document));

  document                                = valid_manifest();
  document["placements"]                  = {"out_of_process"};
  document["interfaces"][0]["class"]      = "A";
  document["interfaces"][0]["placements"] = {"out_of_process"};
  expect_rejected(std::move(document));

  document               = valid_manifest();
  document["placements"] = {"in_process", "out_of_process"};
  expect_rejected(std::move(document));

  temporary_manifest writable(valid_manifest());
  ASSERT_EQ(::chmod(writable.path.c_str(), 0664), 0);
  EXPECT_FALSE(read_package_manifest(DAPP_TEST_CATALOG, writable.filename));
  EXPECT_FALSE(read_package_manifest(DAPP_TEST_CATALOG, "../manifest.json"));
}

TEST(dapp_package_manifest, bounded_json_mutation_corpus_never_admits_arbitrary_bytes)
{
  std::mt19937 random(0x5a17b0adU);
  for (std::size_t size = 1; size != 256; size += 3) {
    std::string bytes(size, '\0');
    for (char& value : bytes) {
      value = static_cast<char>(random());
    }
    temporary_manifest manifest(bytes);
    EXPECT_FALSE(read_package_manifest(DAPP_TEST_CATALOG, manifest.filename));
  }

  const std::string seed = valid_manifest().dump();
  for (std::size_t index = 0; index < seed.size(); index += 7) {
    auto mutation = seed;
    mutation[index] ^= static_cast<char>(1U << (index % 7U));
    temporary_manifest manifest(mutation);
    static_cast<void>(read_package_manifest(DAPP_TEST_CATALOG, manifest.filename));
  }
}

TEST(dapp_package_manifest, service_bootstrap_registers_and_loads_the_exact_native_contract)
{
  temporary_manifest manifest(valid_manifest());
  auto               created = dapp_service::create(service_config(manifest.filename));
  ASSERT_TRUE(created) << created.detail;

  auto inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.packages.size(), 1U);
  EXPECT_TRUE(inventory.instances.empty());

  load_request load;
  load.transaction_id.bytes.back() = 1;
  load.package_id                  = inventory.packages[0].package_id;
  load.selected_backend            = backend::cpu;
  auto loaded                      = created.service->instances().load(load);
  ASSERT_TRUE(loaded) << loaded.detail;
  ASSERT_TRUE(loaded.instance_id);

  lifecycle_request unload;
  unload.transaction_id.bytes.back() = 2;
  unload.instance_id                 = *loaded.instance_id;
  unload.action                      = lifecycle_action::unload;
  unload.expected_generation         = loaded.instance_generation;
  EXPECT_TRUE(created.service->instances().control(unload));
}

TEST(dapp_package_manifest, selected_native_instance_reaches_active_before_management_ingress)
{
  const auto         quarantine_before = quarantined_native_library_count();
  temporary_manifest manifest(valid_native_spectrum_manifest());
  auto               config = service_config(manifest.filename);
  config.initial_instances  = {
      {manifest.filename, OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1, backend::cpu, dapp_initial_instance_target::active, 1s}};
  auto created = dapp_service::create(std::move(config));
  ASSERT_TRUE(created) << created.detail;

  auto inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.instances.size(), 1U);
  EXPECT_EQ(inventory.instances[0].state, OCUDU_DAPP_STATE_ACTIVE_V1);
  EXPECT_EQ(inventory.instances[0].selected_backend, backend::cpu);
  EXPECT_EQ(inventory.instances[0].placement, OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1);
  EXPECT_TRUE(created.service->native_hooks()->bound(inventory.instances[0].instance_id));

  created.service->stop();
  inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.instances.size(), 1U);
  EXPECT_EQ(inventory.instances[0].state, OCUDU_DAPP_STATE_UNLOADED_V1);
  EXPECT_FALSE(created.service->native_hooks()->bound(inventory.instances[0].instance_id));
  EXPECT_EQ(quarantined_native_library_count(), quarantine_before);
}

TEST(dapp_package_manifest, selected_process_instance_can_stop_in_shadow_without_publishing)
{
  temporary_manifest manifest(valid_process_manifest());
  auto               config = service_config(manifest.filename);
  config.initial_instances  = {{manifest.filename,
                                OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1,
                                backend::cpu,
                                dapp_initial_instance_target::shadow,
                                1s}};
  auto created              = dapp_service::create(std::move(config));
  ASSERT_TRUE(created) << created.detail;

  auto inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.instances.size(), 1U);
  EXPECT_EQ(inventory.instances[0].state, OCUDU_DAPP_STATE_SHADOW_V1);
  auto metrics = created.service->instances().metrics(inventory.instances[0].instance_id);
  ASSERT_TRUE(metrics) << metrics.detail;
  const auto publication_enabled = std::find_if(metrics.value.begin(), metrics.value.end(), [](const auto& sample) {
    return sample.name == OCUDU_DAPP_METRIC_PROCESS_PUBLICATION_ENABLED_V1;
  });
  ASSERT_NE(publication_enabled, metrics.value.end());
  EXPECT_EQ(std::get<uint64_t>(publication_enabled->sample_value), 0U);

  created.service->stop();
  inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.instances.size(), 1U);
  EXPECT_EQ(inventory.instances[0].state, OCUDU_DAPP_STATE_UNLOADED_V1);
}

TEST(dapp_package_manifest, selected_instance_startup_is_atomic_and_unloads_on_authority_conflict)
{
  auto first_document           = valid_native_spectrum_manifest();
  auto second_document          = first_document;
  second_document["package_id"] = "00112233-4455-6677-8899-aabbccddee12";
  temporary_manifest first(first_document);
  temporary_manifest second(second_document);
  auto               config = service_config(first.filename);
  config.package_manifests  = {first.filename, second.filename};
  config.initial_instances  = {
      {first.filename, OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1, backend::cpu, dapp_initial_instance_target::active, 1s},
      {second.filename, OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1, backend::cpu, dapp_initial_instance_target::active, 1s}};
  const auto quarantine_before = quarantined_native_library_count();
  auto       created           = dapp_service::create(std::move(config));
  EXPECT_FALSE(created);
  EXPECT_EQ(created.service, nullptr);
  EXPECT_NE(created.detail.find("failed during activate"), std::string::npos) << created.detail;
  EXPECT_EQ(created.detail.find("startup rollback retained"), std::string::npos) << created.detail;
  EXPECT_EQ(quarantined_native_library_count(), quarantine_before);
}

TEST(dapp_package_manifest, service_construction_is_atomic_when_any_manifest_is_invalid)
{
  auto document              = valid_manifest();
  document["schema_version"] = 2;
  temporary_manifest manifest(document);
  auto               created = dapp_service::create(service_config(manifest.filename));
  EXPECT_FALSE(created);
  EXPECT_EQ(created.service, nullptr);

  temporary_manifest first(valid_manifest());
  temporary_manifest second(valid_manifest());
  auto               config = service_config(first.filename);
  config.package_manifests  = {first.filename, second.filename};
  created                   = dapp_service::create(std::move(config));
  EXPECT_FALSE(created);
}

TEST(dapp_package_manifest, service_binds_a_separate_class_c_worker_to_host_owned_spsc_routes)
{
  temporary_manifest manifest(valid_process_manifest());
  auto               created = dapp_service::create(service_config(manifest.filename));
  ASSERT_TRUE(created) << created.detail;

  auto inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.packages.size(), 1U);
  EXPECT_TRUE(inventory.packages[0].compatible) << inventory.packages[0].incompatibility;

  load_request load;
  load.transaction_id.bytes.back() = 10;
  load.package_id                  = inventory.packages[0].package_id;
  load.placement                   = OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1;
  load.selected_backend            = backend::cpu;
  auto loaded                      = created.service->instances().load(load);
  ASSERT_TRUE(loaded) << loaded.detail;
  ASSERT_TRUE(loaded.instance_id);

  load.transaction_id.bytes.back() = 11;
  auto duplicate                   = created.service->instances().load(load);
  EXPECT_EQ(duplicate.code, result_code::lifecycle_failed);

  auto result = control(created.service->instances(), *loaded.instance_id, 12, lifecycle_action::prepare, 1);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 13, lifecycle_action::arm, 2);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 14, lifecycle_action::enter_shadow, 3);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 15, lifecycle_action::activate, 4);
  ASSERT_TRUE(result) << result.detail;

  auto metrics = created.service->instances().metrics(*loaded.instance_id);
  ASSERT_TRUE(metrics) << metrics.detail;
  EXPECT_FALSE(metrics.value.empty());

  auto tap = created.service->upper_phy_hooks()->wrap_phy_tap({1, 0, 0}, nullptr);
  ASSERT_NE(tap, nullptr);
  contiguous_process_grid grid(1, 14, 2);
  tap->handle_quiet_grid(grid, slot_point{0, 2});
  scheduler_dapp_interference_lease lease;
  const auto                        result_deadline = std::chrono::steady_clock::now() + 500ms;
  do {
    const auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    lease = created.service->interference_cache()->acquire(1, 2, slot_point{0, 5}, now);
    if (!lease) {
      std::this_thread::sleep_for(1ms);
    }
  } while (!lease && std::chrono::steady_clock::now() < result_deadline);
  ASSERT_TRUE(lease);
  EXPECT_EQ(lease.context().producer_generation, result.instance_generation);
  EXPECT_NE(lease.context().producer_instance_id[0] | lease.context().producer_instance_id[1], 0U);
  const auto* confidence = reinterpret_cast<const uint8_t*>(lease.mask().native_handle);
  ASSERT_NE(confidence, nullptr);
  EXPECT_EQ(confidence[0], UINT8_MAX);
  EXPECT_EQ(confidence[1], 0);
  lease = {};

  metrics = created.service->instances().metrics(*loaded.instance_id);
  ASSERT_TRUE(metrics) << metrics.detail;
  auto accepted = std::find_if(metrics.value.begin(), metrics.value.end(), [](const auto& metric) {
    return metric.name == "process.results_accepted";
  });
  ASSERT_NE(accepted, metrics.value.end());
  EXPECT_EQ(std::get<uint64_t>(accepted->sample_value), 1U);

  result = control(created.service->instances(), *loaded.instance_id, 16, lifecycle_action::deactivate, 5);
  ASSERT_TRUE(result) << result.detail;
  const auto after_deactivate = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
  EXPECT_FALSE(created.service->interference_cache()->acquire(1, 2, slot_point{0, 5}, after_deactivate));
  result = control(created.service->instances(), *loaded.instance_id, 17, lifecycle_action::unload, 6);
  EXPECT_TRUE(result) << result.detail;
}

TEST(dapp_package_manifest, service_admits_required_srs_and_reports_optional_future_dmrs_unavailable)
{
  temporary_manifest manifest(valid_process_srs_optional_dmrs_manifest());
  auto               created = dapp_service::create(service_config(manifest.filename));
  ASSERT_TRUE(created) << created.detail;

  auto inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.packages.size(), 1U);
  ASSERT_TRUE(inventory.packages[0].compatible) << inventory.packages[0].incompatibility;
  ASSERT_EQ(inventory.packages[0].interfaces.size(), 2U);
  EXPECT_EQ(inventory.packages[0].interfaces[0].requirement, binding_requirement::required);
  EXPECT_EQ(inventory.packages[0].interfaces[0].state, binding_state::unbound);
  EXPECT_EQ(inventory.packages[0].interfaces[1].requirement, binding_requirement::optional);
  EXPECT_EQ(inventory.packages[0].interfaces[1].state, binding_state::unbound);

  load_request load;
  load.transaction_id.bytes.back() = 70;
  load.package_id                  = inventory.packages[0].package_id;
  load.placement                   = OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1;
  load.selected_backend            = backend::cpu;
  auto loaded                      = created.service->instances().load(load);
  ASSERT_TRUE(loaded && loaded.instance_id) << loaded.detail;

  auto result = control(created.service->instances(), *loaded.instance_id, 71, lifecycle_action::prepare, 1);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 72, lifecycle_action::arm, 2);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 73, lifecycle_action::enter_shadow, 3);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 74, lifecycle_action::activate, 4);
  ASSERT_TRUE(result) << result.detail;
  inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.instances.size(), 1U);
  ASSERT_EQ(inventory.instances[0].interfaces.size(), 2U);
  EXPECT_EQ(inventory.instances[0].interfaces[0].state, binding_state::bound);
  EXPECT_EQ(inventory.instances[0].interfaces[1].state, binding_state::unavailable);
  result = control(created.service->instances(), *loaded.instance_id, 75, lifecycle_action::deactivate, 5);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 76, lifecycle_action::unload, 6);
  ASSERT_TRUE(result) << result.detail;
}

TEST(dapp_package_manifest, process_srs_result_is_source_correlated_and_normalized_into_the_same_sink)
{
  temporary_manifest manifest(valid_process_srs_manifest());
  auto               sink     = std::make_shared<capturing_srs_sink>();
  auto               config   = service_config(manifest.filename);
  config.srs_isac_report_sink = sink;
  auto created                = dapp_service::create(std::move(config));
  ASSERT_TRUE(created) << created.detail;

  auto inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.packages.size(), 1U);
  ASSERT_TRUE(inventory.packages[0].compatible) << inventory.packages[0].incompatibility;
  load_request load;
  load.transaction_id.bytes.back() = 80;
  load.package_id                  = inventory.packages[0].package_id;
  load.placement                   = OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1;
  load.selected_backend            = backend::cpu;
  auto loaded                      = created.service->instances().load(load);
  ASSERT_TRUE(loaded && loaded.instance_id) << loaded.detail;
  auto result = control(created.service->instances(), *loaded.instance_id, 81, lifecycle_action::prepare, 1);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 82, lifecycle_action::arm, 2);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 83, lifecycle_action::enter_shadow, 3);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 84, lifecycle_action::activate, 4);
  ASSERT_TRUE(result) << result.detail;
  const uint64_t active_generation = result.instance_generation;

  auto fallback = std::make_shared<process_srs_factory>();
  auto factory  = created.service->upper_phy_hooks()->decorate_srs_estimator(fallback, 1);
  ASSERT_NE(factory, nullptr);
  auto estimator = factory->create();
  ASSERT_NE(estimator, nullptr);
  resource_grid_reader_spy grid(1, 14, 275);
  auto                     estimator_config = process_srs_config();
  const auto               information      = get_srs_information(estimator_config.resource, 0);
  for (uint32_t frequency = 0; frequency != information.sequence_length; ++frequency) {
    grid.write({0,
                estimator_config.resource.start_symbol.value(),
                static_cast<uint16_t>(information.mapping_initial_subcarrier + frequency * information.comb_size),
                {1.0F, 0.0F}});
  }
  estimator->estimate(grid, estimator_config);
  ASSERT_TRUE(sink->wait_for(1, 500ms));
  {
    std::lock_guard<std::mutex> lock(sink->mutex);
    EXPECT_EQ(sink->report.context.source_sequence, 1U);
    EXPECT_EQ(sink->report.context.producer_generation, active_generation);
    EXPECT_NE(sink->report.context.producer_instance_id[0] | sink->report.context.producer_instance_id[1], 0U);
    EXPECT_EQ(sink->report.source_srs_sequence, 1U);
    EXPECT_EQ(sink->report.srs_resource_id, 0U);
    EXPECT_EQ(sink->report.srs_config_id, 0U);
    EXPECT_EQ(sink->report.context.cell_id, 1U);
    EXPECT_EQ(sink->report.context.created.slot, 2U);
  }

  auto metrics = created.service->instances().metrics(*loaded.instance_id);
  ASSERT_TRUE(metrics) << metrics.detail;
  auto accepted = std::find_if(metrics.value.begin(), metrics.value.end(), [](const auto& metric) {
    return metric.name == "process.results_accepted";
  });
  ASSERT_NE(accepted, metrics.value.end());
  EXPECT_EQ(std::get<uint64_t>(accepted->sample_value), 1U);

  result = control(created.service->instances(), *loaded.instance_id, 85, lifecycle_action::deactivate, 5);
  ASSERT_TRUE(result) << result.detail;
  result = control(
      created.service->instances(), *loaded.instance_id, 86, lifecycle_action::unload, result.instance_generation);
  EXPECT_TRUE(result) << result.detail;
}

TEST(dapp_package_manifest, spectrum_result_capability_has_one_authority_but_allows_observers)
{
  auto observer_document                             = valid_process_manifest();
  observer_document["package_id"]                    = "00112233-4455-6677-8899-aabbccddee11";
  observer_document["global_name"]                   = "org.deepsig.ocudu.test.spectrum-observer";
  observer_document["interfaces"][0]["capabilities"] = 0;
  auto second_authority_document                     = valid_process_manifest();
  second_authority_document["package_id"]            = "00112233-4455-6677-8899-aabbccddee12";
  second_authority_document["global_name"]           = "org.deepsig.ocudu.test.spectrum-authority-2";
  temporary_manifest authority(valid_process_manifest());
  temporary_manifest observer(observer_document);
  temporary_manifest second_authority(second_authority_document);
  auto               config = service_config(authority.filename);
  config.package_manifests  = {authority.filename, observer.filename, second_authority.filename};
  auto created              = dapp_service::create(std::move(config));
  ASSERT_TRUE(created) << created.detail;

  auto inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.packages.size(), 3U);
  uint8_t transaction     = 50;
  auto    load_and_shadow = [&](std::string_view name) {
    auto package = std::find_if(inventory.packages.begin(), inventory.packages.end(), [&](const auto& item) {
      return item.global_name == name;
    });
    EXPECT_NE(package, inventory.packages.end());
    load_request load;
    load.transaction_id.bytes.back() = transaction++;
    load.package_id                  = package->package_id;
    load.placement                   = OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1;
    load.selected_backend            = backend::cpu;
    auto result                      = created.service->instances().load(load);
    EXPECT_TRUE(result) << result.detail;
    for (auto action : {lifecycle_action::prepare, lifecycle_action::arm, lifecycle_action::enter_shadow}) {
      result =
          control(created.service->instances(), *result.instance_id, transaction++, action, result.instance_generation);
      EXPECT_TRUE(result) << result.detail;
    }
    return result;
  };

  auto first   = load_and_shadow("org.deepsig.ocudu.test.spectrum-process");
  auto observe = load_and_shadow("org.deepsig.ocudu.test.spectrum-observer");
  auto second  = load_and_shadow("org.deepsig.ocudu.test.spectrum-authority-2");
  first        = control(created.service->instances(),
                  *first.instance_id,
                  transaction++,
                  lifecycle_action::activate,
                  first.instance_generation);
  ASSERT_TRUE(first) << first.detail;
  observe = control(created.service->instances(),
                    *observe.instance_id,
                    transaction++,
                    lifecycle_action::activate,
                    observe.instance_generation);
  EXPECT_TRUE(observe) << observe.detail;
  second = control(created.service->instances(),
                   *second.instance_id,
                   transaction++,
                   lifecycle_action::activate,
                   second.instance_generation);
  EXPECT_EQ(second.code, result_code::lifecycle_failed);
  EXPECT_EQ(second.detail, "Class-C result authority activation failed because another authority is active");
}

TEST(dapp_package_manifest, native_spectrum_output_commits_transactionally_into_the_scheduler_cache)
{
  temporary_manifest manifest(valid_native_spectrum_manifest());
  auto               created = dapp_service::create(service_config(manifest.filename));
  ASSERT_TRUE(created) << created.detail;
  auto inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.packages.size(), 1U);

  fixture_control fixture;
  fixture.publish_interference.store(true, std::memory_order_relaxed);
  fixture.publish_quiet_intent.store(true, std::memory_order_relaxed);
  load_request load;
  load.transaction_id.bytes.back() = 30;
  load.package_id                  = inventory.packages[0].package_id;
  load.selected_backend            = backend::cpu;
  load.create_args                 = &fixture;
  auto loaded                      = created.service->instances().load(load);
  ASSERT_TRUE(loaded && loaded.instance_id) << loaded.detail;
  auto result = control(created.service->instances(), *loaded.instance_id, 31, lifecycle_action::prepare, 1);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 32, lifecycle_action::arm, 2);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 33, lifecycle_action::enter_shadow, 3);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 34, lifecycle_action::activate, 4);
  ASSERT_TRUE(result) << result.detail;
  const uint64_t active_generation = result.instance_generation;

  auto spectrum = created.service->native_hooks()->find(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  ASSERT_NE(spectrum, nullptr);
  const std::array<complex32, 12>      samples{{{1.0F, 0.0F}}};
  ocudu_dapp_spectrum_slot_metadata_v1 spectrum_metadata{};
  std::array<uint8_t, 1>               plan{};
  auto                                 input     = spectrum_input(samples.data(), 3, spectrum_metadata, plan);
  auto                                 published = spectrum->publish(0, input);
  ASSERT_TRUE(published);
  const auto now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
  auto lease = created.service->interference_cache()->acquire(1, 52, slot_point{0, 5, 4}, now);
  ASSERT_TRUE(lease);
  const auto* confidence = reinterpret_cast<const uint8_t*>(lease.mask().native_handle);
  ASSERT_NE(confidence, nullptr);
  EXPECT_EQ(confidence[7], 255);
  EXPECT_EQ(lease.context().source_sequence, 1U);
  EXPECT_EQ(lease.context().producer_generation, active_generation);
  EXPECT_NE(lease.context().producer_instance_id[0] | lease.context().producer_instance_id[1], 0U);
  lease = {};
  std::array<uint8_t, 52> quiet_mask{};
  auto quiet = created.service->quiet_reservations()->lookup(1, slot_point{0, 5, 7}, now, quiet_mask);
  EXPECT_EQ(quiet.reservation_count, 1U);
  EXPECT_TRUE(std::all_of(quiet_mask.begin() + 20, quiet_mask.begin() + 24, [](uint8_t value) { return value == 1; }));

  fixture.throw_after_prepared_output.store(true, std::memory_order_relaxed);
  input     = spectrum_input(samples.data(), 4, spectrum_metadata, plan);
  published = spectrum->publish(0, input);
  EXPECT_EQ(published.status, OCUDU_DAPP_EXCEPTION_V1);
  const auto after_failure = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
  lease = created.service->interference_cache()->acquire(1, 52, slot_point{0, 5, 4}, after_failure);
  ASSERT_TRUE(lease);
  EXPECT_EQ(lease.context().source_sequence, 1U);
  lease = {};
  EXPECT_EQ(created.service->quiet_reservations()
                ->lookup(1, slot_point{0, 5, 8}, after_failure, quiet_mask)
                .reservation_count,
            0U);

  inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.instances.size(), 1U);
  result = control(created.service->instances(),
                   *loaded.instance_id,
                   35,
                   lifecycle_action::deactivate,
                   inventory.instances[0].generation);
  ASSERT_TRUE(result) << result.detail;
  EXPECT_EQ(created.service->quiet_reservations()
                ->lookup(1, slot_point{0, 5, 7}, after_failure, quiet_mask)
                .reservation_count,
            0U);
  lease = created.service->interference_cache()->acquire(1, 52, slot_point{0, 5, 4}, after_failure);
  EXPECT_FALSE(lease);
  result = control(
      created.service->instances(), *loaded.instance_id, 36, lifecycle_action::unload, result.instance_generation);
  EXPECT_TRUE(result) << result.detail;
}

TEST(dapp_package_manifest, quiet_policy_rejection_is_attributed_without_opening_the_native_breaker)
{
  temporary_manifest manifest(valid_native_spectrum_manifest());
  auto               config                                    = service_config(manifest.filename);
  config.quiet_reservation.maximum_prbs_per_slot               = 4;
  config.quiet_reservation.maximum_prb_slots_per_window        = 64;
  config.quiet_reservation.maximum_source_prb_slots_per_window = 3;
  config.quiet_reservation.maximum_retained_prb_slots          = 64;
  auto created                                                 = dapp_service::create(std::move(config));
  ASSERT_TRUE(created) << created.detail;
  const auto inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.packages.size(), 1U);

  fixture_control fixture;
  fixture.publish_quiet_intent.store(true, std::memory_order_relaxed);
  load_request load;
  load.transaction_id.bytes.back() = 40;
  load.package_id                  = inventory.packages[0].package_id;
  load.selected_backend            = backend::cpu;
  load.create_args                 = &fixture;
  auto loaded                      = created.service->instances().load(load);
  ASSERT_TRUE(loaded && loaded.instance_id) << loaded.detail;
  auto result = control(created.service->instances(), *loaded.instance_id, 41, lifecycle_action::prepare, 1);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 42, lifecycle_action::arm, 2);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 43, lifecycle_action::enter_shadow, 3);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 44, lifecycle_action::activate, 4);
  ASSERT_TRUE(result) << result.detail;

  auto spectrum = created.service->native_hooks()->find(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  ASSERT_NE(spectrum, nullptr);
  const std::array<complex32, 12>      samples{{{1.0F, 0.0F}}};
  ocudu_dapp_spectrum_slot_metadata_v1 metadata{};
  std::array<uint8_t, 1>               plan{};
  auto                                 input = spectrum_input(samples.data(), 5, metadata, plan);
  EXPECT_EQ(spectrum->publish(0, input).status, OCUDU_DAPP_BYPASS_V1);
  input = spectrum_input(samples.data(), 6, metadata, plan);
  EXPECT_EQ(spectrum->publish(0, input).status, OCUDU_DAPP_BYPASS_V1);
  EXPECT_EQ(fixture.data_invocations.load(std::memory_order_relaxed), 2U);

  EXPECT_EQ(created.service->instances().drain_incidents(), 2U);
  incident_filter filter;
  filter.instance_id = std::array<uint64_t, 2>{};
  std::memcpy(filter.instance_id->data(), loaded.instance_id->bytes.data(), loaded.instance_id->bytes.size());
  filter.cause           = incident_cause::policy_rejection;
  filter.maximum_records = 8;
  const auto incidents   = created.service->instances().incidents().query(filter);
  ASSERT_EQ(incidents.size(), 1U);
  EXPECT_EQ(incidents[0].recurrence_count, 2U);
  EXPECT_EQ(incidents[0].context.hook_id, OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  EXPECT_EQ(incidents[0].context.invocation_sequence, 6U);
  const std::string detail(incidents[0].sanitized_message.data(), incidents[0].message_size);
  EXPECT_NE(detail.find("per-source"), std::string::npos);
  const auto active = created.service->instances().instance(*loaded.instance_id);
  ASSERT_TRUE(active);
  EXPECT_EQ(active->state, OCUDU_DAPP_STATE_ACTIVE_V1);
}

TEST(dapp_package_manifest, native_srs_output_is_normalized_and_report_sink_exceptions_are_contained)
{
  temporary_manifest manifest(valid_native_srs_manifest());
  auto               sink     = std::make_shared<capturing_srs_sink>();
  auto               config   = service_config(manifest.filename);
  config.srs_isac_report_sink = sink;
  auto created                = dapp_service::create(std::move(config));
  ASSERT_TRUE(created) << created.detail;
  auto inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.packages.size(), 1U);

  fixture_control fixture;
  fixture.publish_srs_preview.store(true, std::memory_order_relaxed);
  load_request load;
  load.transaction_id.bytes.back() = 70;
  load.package_id                  = inventory.packages[0].package_id;
  load.selected_backend            = backend::cpu;
  load.create_args                 = &fixture;
  auto loaded                      = created.service->instances().load(load);
  ASSERT_TRUE(loaded && loaded.instance_id) << loaded.detail;
  auto result = control(created.service->instances(), *loaded.instance_id, 71, lifecycle_action::prepare, 1);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 72, lifecycle_action::arm, 2);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 73, lifecycle_action::enter_shadow, 3);
  ASSERT_TRUE(result) << result.detail;
  result = control(created.service->instances(), *loaded.instance_id, 74, lifecycle_action::activate, 4);
  ASSERT_TRUE(result) << result.detail;
  const uint64_t active_generation = result.instance_generation;

  auto hook = created.service->native_hooks()->find(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  ASSERT_NE(hook, nullptr);
  const std::array<complex32, 4>  samples{{{1.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 0.0F}}};
  ocudu_dapp_srs_isac_metadata_v1 metadata{};
  auto                            input     = srs_input(samples.data(), 3, metadata);
  auto                            published = hook->publish(0, input);
  ASSERT_TRUE(published);
  {
    std::lock_guard<std::mutex> lock(sink->mutex);
    ASSERT_EQ(sink->count, 1U);
    EXPECT_EQ(sink->report.context.source_sequence, 1U);
    EXPECT_EQ(sink->report.context.producer_generation, active_generation);
    EXPECT_NE(sink->report.context.producer_instance_id[0] | sink->report.context.producer_instance_id[1], 0U);
    EXPECT_EQ(sink->report.source_srs_sequence, 3U);
    EXPECT_EQ(sink->report.srs_resource_id, 17U);
    EXPECT_EQ(sink->report.srs_config_id, 19U);
    EXPECT_EQ(sink->report.peak_delay_bin, 1U);
  }

  sink->throw_on_publish.store(true, std::memory_order_relaxed);
  input     = srs_input(samples.data(), 4, metadata);
  published = hook->publish(0, input);
  EXPECT_EQ(published.status, OCUDU_DAPP_EXCEPTION_V1);

  inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.instances.size(), 1U);
  result = control(created.service->instances(),
                   *loaded.instance_id,
                   75,
                   lifecycle_action::deactivate,
                   inventory.instances[0].generation);
  ASSERT_TRUE(result) << result.detail;
  result = control(
      created.service->instances(), *loaded.instance_id, 76, lifecycle_action::unload, result.instance_generation);
  EXPECT_TRUE(result) << result.detail;
}
