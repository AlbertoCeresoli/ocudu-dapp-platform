# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

set_directory_properties(PROPERTIES LABELS "phy")

# The geometry is what the accelerated detector prepares its backend for; the test needs no GPU.
add_executable(prach_detector_geometry_test prach_detector_geometry_test.cpp)
target_include_directories(prach_detector_geometry_test PRIVATE ${PROJECT_SOURCE_DIR}/lib/phy/upper)
target_link_libraries(prach_detector_geometry_test
        ocudu_prach_detector
        ocudu_ran
        ocudulog
        GTest::gtest
        GTest::gtest_main)
add_test(prach_detector_geometry_test prach_detector_geometry_test)

if(CUDA_ACCEL_FOUND)
    # Container format of the compiled-kernel cache; needs no GPU, but ships with the CUDA backend.
    add_executable(vkfft_kernel_cache_test vkfft_kernel_cache_test.cpp)
    target_include_directories(vkfft_kernel_cache_test PRIVATE ${CUDA_ACCEL_INCLUDE_DIRS})
    target_link_libraries(vkfft_kernel_cache_test GTest::gtest GTest::gtest_main)
    add_test(vkfft_kernel_cache_test vkfft_kernel_cache_test)

    add_executable(prach_detector_cuda_test prach_detector_cuda_test.cpp)
    target_include_directories(prach_detector_cuda_test
            PRIVATE ${PROJECT_SOURCE_DIR}/lib/phy/upper ${CUDA_ACCEL_INCLUDE_DIRS})
    target_link_libraries(prach_detector_cuda_test
            ocudu_prach_detector
            ocudu_upper_phy
            ocudu_phy_support
            ocudu_dft
            ocudulog
            GTest::gtest
            GTest::gtest_main)
    add_test(prach_detector_cuda_test prach_detector_cuda_test)
endif()

# GPU tests must not run concurrently with each other. The CUDA runtime rejects allocations with
# cudaErrorDevicesUnavailable while several exclusive device contexts are active, which surfaces as intermittent ctest
# failures that look like defects in the accelerated code. The shared lock lets ctest keep running the rest of the suite
# in parallel while serialising the tests that drive the GPU.
if (CUDA_ACCEL_FOUND)
    set_tests_properties(
            prach_detector_cuda_test
            PROPERTIES RESOURCE_LOCK ocudu_cuda_device)
endif ()
