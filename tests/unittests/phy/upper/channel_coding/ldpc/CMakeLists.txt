# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


add_executable(ldpc_chain_test ldpc_chain_test.cpp)
target_link_libraries(ldpc_chain_test ocudu_channel_coding ocudulog GTest::gtest GTest::gtest_main)
add_test(ldpc_chain_test ldpc_chain_test)

add_executable(ldpc_rate_dematcher_test ldpc_rate_dematcher_test.cpp)
target_link_libraries(ldpc_rate_dematcher_test ocudu_channel_coding ocudulog GTest::gtest GTest::gtest_main)
add_test(ldpc_rate_dematcher_test ldpc_rate_dematcher_test)

add_executable(gpu_demod_llr_sign_test gpu_demod_llr_sign_test.cpp)
target_link_libraries(gpu_demod_llr_sign_test gtest gtest_main)
add_test(gpu_demod_llr_sign_test gpu_demod_llr_sign_test)

if(CUDA_ACCEL_FOUND)
    add_executable(ldpc_encoder_gpu_cpu_test ldpc_encoder_gpu_cpu_test.cpp)
    target_include_directories(ldpc_encoder_gpu_cpu_test PRIVATE
        ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc/cuda)
    target_link_libraries(ldpc_encoder_gpu_cpu_test
        ocudu_channel_coding
        ocudu_ldpc_cuda
        ocudulog
        gtest
        gtest_main)
    add_test(ldpc_encoder_gpu_cpu_test ldpc_encoder_gpu_cpu_test)

    add_executable(ldpc_decoder_gpu_cpu_test ldpc_decoder_gpu_cpu_test.cpp)
    target_include_directories(ldpc_decoder_gpu_cpu_test PRIVATE
        ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc/cuda)
    target_link_libraries(ldpc_decoder_gpu_cpu_test
        ocudu_channel_coding
        ocudu_ldpc_cuda
        ocudulog
        gtest
        gtest_main)
    add_test(ldpc_decoder_gpu_cpu_test ldpc_decoder_gpu_cpu_test)

endif()

# GPU tests must not run concurrently with each other. The CUDA runtime rejects allocations with
# cudaErrorDevicesUnavailable while several exclusive device contexts are active, which surfaces as intermittent ctest
# failures that look like defects in the accelerated code. The shared lock lets ctest keep running the rest of the suite
# in parallel while serialising the tests that drive the GPU.
if (CUDA_ACCEL_FOUND)
    set_tests_properties(
            ldpc_encoder_gpu_cpu_test
            ldpc_decoder_gpu_cpu_test
            PROPERTIES RESOURCE_LOCK ocudu_cuda_device)
endif ()
