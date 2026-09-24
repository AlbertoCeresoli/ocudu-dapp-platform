// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

/// \file
/// \brief End-to-end parity of a host SRS channel-estimator dApp with the gNB's host estimator.
///
///     export OCUDU_DAPP_SRS_REFERENCE_MODULE=<sdk build>/ref_srs_channel_estimator_cpu.so
///     export OCUDU_DAPP_SRS_REFERENCE_CUDA_MODULE=<sdk build>/ref_srs_channel_estimator_cuda.so
///     srs_dapp_reference_parity_test
///
/// The module is verified, loaded and activated by the native runtime, bound to a hook point, and served through the
/// Class-A SRS adapter on the golden occasions; every estimate is compared with srs_estimator_generic_impl on the same
/// grid. The host module reads host grids; the CUDA module reads CUDA-visible grids through the owned device snapshot,
/// the path of an accelerated gNB. A test whose variable is unset is skipped.

#include "srs_dapp_test_occasions.h"
#ifdef ENABLE_CUDA
#include "../../../../lib/phy/upper/phy_acceleration_resource_grid_factory.h"
#endif
#include "ocudu/dapp/runtime/artifact_verifier.h"
#include "ocudu/dapp/runtime/module_loader.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/dapp/runtime/native_instance.h"
#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include "ocudu/phy/support/time_alignment_estimator/time_alignment_estimator_factories.h"
#include "ocudu/phy/upper/dapp/srs_dapp_channel_estimator.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_factory.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <sys/stat.h>

using namespace ocudu;
using namespace ocudu::srs_dapp_test;
using namespace std::chrono_literals;

namespace {

/// A private catalog holding a read-only copy of the module, as the runtime requires.
class staged_module
{
public:
  explicit staged_module(const std::filesystem::path& source)
  {
    char pattern[] = "/tmp/srs-dapp-parity-XXXXXX";
    if (::mkdtemp(pattern) == nullptr) {
      return;
    }
    directory = pattern;
    path      = directory / source.filename();
    std::filesystem::copy_file(source, path);
    ::chmod(path.c_str(), 0555);
  }

  ~staged_module()
  {
    std::error_code error;
    std::filesystem::permissions(path, std::filesystem::perms::owner_all, error);
    std::filesystem::remove_all(directory, error);
  }

  std::filesystem::path directory;
  std::filesystem::path path;
};

std::shared_ptr<srs_estimator_factory> host_estimator_factory()
{
  auto dft = create_dft_processor_factory_fftw_slow();
  if (!dft) {
    dft = create_dft_processor_factory_generic();
  }
  return create_srs_estimator_generic_factory(create_low_papr_sequence_generator_sw_factory(),
                                              create_time_alignment_estimator_dft_factory(dft),
                                              MAX_NOF_PRBS,
                                              "disabled");
}

/// Serves the golden occasions through \p module on \p backend and compares every estimate with the host estimator.
void expect_parity(const char*              module,
                   uint32_t                 backend,
                   resource_grid_factory&   grids,
                   std::chrono::nanoseconds completion_deadline = std::chrono::milliseconds(1))
{
  staged_module staged(module);
  ASSERT_FALSE(staged.directory.empty());

  dapp::artifact_verifier verifier(staged.directory.string());
  auto                    inspected = verifier.inspect(staged.path.string());
  ASSERT_TRUE(inspected);
  auto approved = verifier.verify(staged.path.string(), inspected.digest);
  ASSERT_TRUE(approved);
  ocudu_dapp_host_info_v1 host{};
  host.struct_size = sizeof(host);
  host.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  host.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  auto library     = dapp::load_native_module(std::move(*approved.artifact), host);
  ASSERT_TRUE(library.library) << library.detail;
  ocudu_dapp_host_api_v1 api{};
  api.struct_size = sizeof(api);
  api.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  api.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  auto created    = dapp::native_instance::create(library.library, api, nullptr, backend);
  ASSERT_TRUE(created) << created.operation.detail;
  auto& instance = *created.instance;
  ASSERT_TRUE(instance.prepare(nullptr, nullptr));
  ASSERT_TRUE(instance.arm(nullptr));
  ASSERT_TRUE(instance.enter_shadow());
  ASSERT_TRUE(instance.activate());

  auto point = dapp::native_hook_point::create(OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1, 0, 1);
  ASSERT_TRUE(point) << point.detail;
  std::shared_ptr<dapp::native_hook_point> hook(std::move(point.point));
  ASSERT_EQ(hook->activate(instance), OCUDU_DAPP_OK_V1);

  auto host_factory = host_estimator_factory();
  auto dapp_factory = create_srs_dapp_channel_estimator_factory(
      host_estimator_factory(),
      create_native_srs_dapp_channel_estimator_target(hook),
      create_low_papr_sequence_generator_sw_factory(),
      {.nof_lanes = 1, .max_nof_prb = MAX_NOF_PRBS, .completion_deadline = completion_deadline});
  ASSERT_NE(dapp_factory, nullptr);
  auto host_estimator = host_factory->create();
  auto dapp_estimator = dapp_factory->create();
  ASSERT_NE(dapp_estimator, nullptr);

  unsigned index = 0;
  for (const auto& profile : golden_occasions()) {
    SCOPED_TRACE("occasion " + std::to_string(index++));
    const auto config = make_configuration(profile);
    auto       grid   = grids.create(
        profile.nof_rx_ports, get_nsymb_per_slot(cyclic_prefix::NORMAL), profile.grid_nof_prb * NOF_SUBCARRIERS_PER_RB);
    ASSERT_NE(grid, nullptr);
    ASSERT_TRUE(write_occasion(*grid, profile, config));
    const auto expected = host_estimator->estimate(grid->get_reader(), config);
    const auto actual   = dapp_estimator->estimate(grid->get_reader(), config);
    EXPECT_TRUE(actual.channel_matrix.is_near(expected.channel_matrix, 1e-4F));
    EXPECT_NEAR(actual.noise_variance.value(), expected.noise_variance.value(), 1e-5F * *expected.noise_variance);
    EXPECT_NEAR(actual.epre_dB.value(), expected.epre_dB.value(), 1e-4F);
    EXPECT_NEAR(actual.rsrp_dB.value(), expected.rsrp_dB.value(), 1e-4F);
    EXPECT_NEAR(actual.time_alignment.time_alignment,
                expected.time_alignment.time_alignment,
                1e-4 * expected.time_alignment.resolution);
    EXPECT_NEAR(actual.time_alignment.resolution, expected.time_alignment.resolution, 1e-12);
  }
  // The dApp served every occasion: nothing fell back to the host estimator. A CUDA module past the soft
  // completion wall is counted, and its result is still used.
  const auto health = instance.health();
  EXPECT_EQ(health.class_a_fallbacks.reasons, dapp::class_a_fallback_snapshot{}.reasons);
  if (backend == OCUDU_DAPP_BACKEND_CUDA_V1 && completion_deadline < std::chrono::microseconds(1)) {
    EXPECT_GT(health.class_a_completion.deadline_misses, 0U);
  } else {
    EXPECT_EQ(health.class_a_completion.deadline_misses, 0U);
  }

  ASSERT_EQ(hook->deactivate(std::chrono::steady_clock::now() + 1s), OCUDU_DAPP_OK_V1);
  EXPECT_TRUE(instance.drain(std::chrono::steady_clock::now() + 1s));
  EXPECT_TRUE(instance.unload());
}

TEST(srs_dapp_reference_parity, the_host_reference_dapp_reproduces_the_host_estimator_through_the_gnb_adapter)
{
  const char* module = std::getenv("OCUDU_DAPP_SRS_REFERENCE_MODULE");
  if (module == nullptr) {
    GTEST_SKIP() << "OCUDU_DAPP_SRS_REFERENCE_MODULE not set";
  }
  expect_parity(module, OCUDU_DAPP_BACKEND_HOST_V1, *create_resource_grid_factory());
}

TEST(srs_dapp_reference_parity, the_cuda_reference_dapp_reproduces_the_host_estimator_from_the_device_snapshot)
{
#ifdef ENABLE_CUDA
  const char* module = std::getenv("OCUDU_DAPP_SRS_REFERENCE_CUDA_MODULE");
  if (module == nullptr) {
    GTEST_SKIP() << "OCUDU_DAPP_SRS_REFERENCE_CUDA_MODULE not set";
  }
  // Managed CUDA-visible uplink grids: the adapter reads their owned device snapshot, never the live grid.
  auto grids = create_phy_acceleration_resource_grid_factory(create_resource_grid_factory(),
                                                             phy_acceleration_resource_grid_direction::uplink,
                                                             true,
                                                             0,
                                                             phy_acceleration_resource_grid_memory_mode::managed);
  ASSERT_NE(grids, nullptr);
  expect_parity(module, OCUDU_DAPP_BACKEND_CUDA_V1, *grids);
  // A completion wall no invocation can meet: every miss reaches class_a.completion.deadline_misses.
  expect_parity(module, OCUDU_DAPP_BACKEND_CUDA_V1, *grids, std::chrono::nanoseconds(1));
#else
  GTEST_SKIP() << "built without CUDA";
#endif
}

} // namespace
