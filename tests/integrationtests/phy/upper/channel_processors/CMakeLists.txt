# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


set_directory_properties(PROPERTIES LABELS "phy;tsan;NO_MEMCHECK")

add_executable(pxsch_bler_test pxsch_bler_test.cpp)

set(PXSCH_BLER_TEST_LIBRARIES ocudu_ran
        ocudu_upper_phy
        ocudu_upper_phy_support
        ocudu_channel_precoder
        ocudulog
        ocuduvec)

if (ENABLE_PDSCH_HWACC AND ENABLE_PUSCH_HWACC)
    set_source_files_properties(pxsch_bler_test.cpp pxsch_bler_test_factories.cpp PROPERTIES COMPILE_DEFINITIONS "HWACC_PDSCH_ENABLED; HWACC_PUSCH_ENABLED")
    list(APPEND PXSCH_BLER_TEST_LIBRARIES ocudu_hal_pusch
            ocudu_hal_pdsch
            ocudu_hal_bbdev)
endif (ENABLE_PDSCH_HWACC AND ENABLE_PUSCH_HWACC)

add_library(ocudu_pxsch_bler_test_helpers STATIC
        pxsch_bler_test_channel_emulator.cpp
        pxsch_bler_test_factories.cpp)
target_link_libraries(ocudu_pxsch_bler_test_helpers ${PXSCH_BLER_TEST_LIBRARIES})

target_link_libraries(pxsch_bler_test ocudu_pxsch_bler_test_helpers)
add_test(pxsch_bler_test pxsch_bler_test -R 10)

add_executable(pxsch_chain_test pxsch_chain_test.cpp)
target_link_libraries(pxsch_chain_test
        ocudu_ran
        ocudu_channel_processors
        ocudulog
        ocuduvec
        gtest
        gtest_main
)
gtest_discover_tests(pxsch_chain_test)

add_executable(pusch_gpu_cpu_comparison_test pusch_gpu_cpu_comparison_test.cpp)
target_link_libraries(pusch_gpu_cpu_comparison_test ocudu_pxsch_bler_test_helpers)
add_test(pusch_gpu_cpu_comparison_test pusch_gpu_cpu_comparison_test -R 10)

add_executable(pdsch_gpu_e2e_test pdsch_gpu_e2e_test.cpp)
target_link_libraries(pdsch_gpu_e2e_test
        ocudu_ran
        ocudu_channel_processors
        ocudu_phy_support
        ocudu_upper_phy_support
        ocudu_channel_precoder
        ocudulog
        ocuduvec
        gtest
        gtest_main
)
add_test(pdsch_gpu_e2e_test pdsch_gpu_e2e_test)
if(CUDA_ACCEL_FOUND)
  target_include_directories(pdsch_gpu_e2e_test PRIVATE
          ${CMAKE_SOURCE_DIR}/lib/phy/upper
          ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc)
  target_link_libraries(pdsch_gpu_e2e_test ocudu_ldpc_cuda ocudu_ofh_compression CUDA::cudart)
  target_compile_definitions(pdsch_gpu_e2e_test PRIVATE ENABLE_CUDA)
  add_test(NAME pdsch_gpu_issue_1_regression
           COMMAND pdsch_gpu_e2e_test --gtest_filter=PdschGpuIssueProfile.*)
  set_tests_properties(pdsch_gpu_issue_1_regression PROPERTIES LABELS "cuda;pdsch;regression" TIMEOUT 120)
endif()

add_executable(pdsch_gpu_latency_benchmark pdsch_gpu_latency_benchmark.cpp)
target_link_libraries(pdsch_gpu_latency_benchmark
        ocudu_ran
        ocudu_channel_processors
        ocudu_phy_support
        ocudu_upper_phy_support
        ocudu_channel_precoder
        ocudulog
        ocuduvec
)
if(CUDA_ACCEL_FOUND)
  target_include_directories(pdsch_gpu_latency_benchmark PRIVATE
          ${CMAKE_SOURCE_DIR}/lib/phy/upper
          ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc)
  target_link_libraries(pdsch_gpu_latency_benchmark ocudu_ldpc_cuda CUDA::cudart)
  target_compile_definitions(pdsch_gpu_latency_benchmark PRIVATE ENABLE_CUDA)
endif()

add_executable(pusch_gpu_cpu_result_parity_test pusch_gpu_cpu_result_parity_test.cpp)
target_link_libraries(pusch_gpu_cpu_result_parity_test ocudu_pxsch_bler_test_helpers)
add_test(pusch_gpu_cpu_result_parity_test pusch_gpu_cpu_result_parity_test)

add_executable(pusch_64qam_stress_test pusch_64qam_stress_test.cpp)
target_link_libraries(pusch_64qam_stress_test ocudu_pxsch_bler_test_helpers)
if(CUDA_ACCEL_FOUND)
  target_compile_definitions(pusch_64qam_stress_test PRIVATE ENABLE_CUDA)
  # Add include paths to access internal headers for timing profiling
  target_include_directories(pusch_64qam_stress_test PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc
  )
endif()
add_test(pusch_64qam_stress_test pusch_64qam_stress_test)


add_executable(pusch_e2e_pipeline_test pusch_e2e_pipeline_test.cpp)
target_link_libraries(pusch_e2e_pipeline_test ocudu_pxsch_bler_test_helpers)
if(CUDA_ACCEL_FOUND)
  target_compile_definitions(pusch_e2e_pipeline_test PRIVATE ENABLE_CUDA)
  target_sources(pusch_e2e_pipeline_test PRIVATE pusch_test_pilot_admission_trace.cpp)
  target_link_options(pusch_e2e_pipeline_test PRIVATE -Wl,--wrap=pusch_e2e_validate_pilot_metrics)
  target_include_directories(pusch_e2e_pipeline_test PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc
  )
  target_link_libraries(pusch_e2e_pipeline_test ocudu_ldpc_cuda CUDA::cudart CUDA::cuda_driver)
  if(TARGET ocudu_dapp_app_service AND TARGET ocudu-dapp-preflight)
    # Test-only receipt/query diagnostics. Internal C++ symbols are pinned to
    # this host build; an ABI name drift fails the link, never changes dispatch.
    target_sources(pusch_e2e_pipeline_test PRIVATE pusch_test_receiver_retirement_trace.cpp pusch_test_receiver_capture.cpp)
    target_link_options(pusch_e2e_pipeline_test PRIVATE
      -Wl,--wrap=cudaStreamQuery
      -Wl,--wrap=_ZN5ocudu4dapp17native_hook_point24invoke_receiver_deferredEjRK28ocudu_dapp_receiver_input_v1R29ocudu_dapp_receiver_output_v1RNS0_22native_receiver_ticketERSt10shared_ptrINS0_13native_workerEE
      -Wl,--wrap=scrambler_descramble_llr_half_inplace
      -Wl,--wrap=rate_matcher_deinterleave_and_dematch_batch_half
      -Wl,--wrap=ldpc_decoder_decode_batch_half_from_rate_matched
      -Wl,--wrap=_ZN5ocudu4dapp13native_worker16inspect_receiverERKNS0_22native_receiver_ticketER40ocudu_dapp_receiver_completion_result_v1
      -Wl,--wrap=_ZN5ocudu4dapp13native_worker16release_receiverERKNS0_22native_receiver_ticketE)
    target_link_libraries(pusch_e2e_pipeline_test ocudu_dapp_app_service ocudu_phy_dapp_adapters)
    target_compile_definitions(pusch_e2e_pipeline_test PRIVATE
      OCUDU_PUSCH_TEST_DAPP
      OCUDU_DAPP_PREFLIGHT_HELPER="$<TARGET_FILE:ocudu-dapp-preflight>")
    add_dependencies(pusch_e2e_pipeline_test ocudu-dapp-preflight)
  endif()
endif()
add_test(pusch_e2e_pipeline_test pusch_e2e_pipeline_test)

if(CUDA_ACCEL_FOUND)
  add_executable(pusch_resident_dematch_scramble_test pusch_resident_dematch_scramble_test.cpp)
  target_include_directories(pusch_resident_dematch_scramble_test PRIVATE
    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
    ${CUDA_ACCEL_INCLUDE_DIRS}
  )
  target_link_libraries(pusch_resident_dematch_scramble_test ocudu_ldpc_cuda CUDA::cudart)
  target_compile_definitions(pusch_resident_dematch_scramble_test PRIVATE ENABLE_CUDA)
  add_test(pusch_resident_dematch_scramble_test pusch_resident_dematch_scramble_test)
endif()

add_executable(pusch_mimo_eq_parity_test pusch_mimo_eq_parity_test.cpp)
target_link_libraries(pusch_mimo_eq_parity_test
  ocudu_pxsch_bler_test_helpers
  ocudu_channel_processors
  ocudu_phy_support
  ocudu_upper_phy_support
  ocudu_channel_precoder
  ocudu_channel_equalizer
  ocudu_dft
  ocudulog
  ocuduvec
)
if(CUDA_ACCEL_FOUND)
  target_compile_definitions(pusch_mimo_eq_parity_test PRIVATE ENABLE_CUDA)
  target_include_directories(pusch_mimo_eq_parity_test PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc
    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
    ${CUDA_ACCEL_INCLUDE_DIRS}
  )
  target_link_libraries(pusch_mimo_eq_parity_test ocudu_ldpc_cuda CUDA::cudart)
endif()

add_executable(pusch_e2e_correctness_sweep pusch_e2e_correctness_sweep.cpp)
target_link_libraries(pusch_e2e_correctness_sweep ocudu_pxsch_bler_test_helpers)
target_include_directories(pusch_e2e_correctness_sweep PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc
)
if(CUDA_ACCEL_FOUND)
  target_compile_definitions(pusch_e2e_correctness_sweep PRIVATE ENABLE_CUDA)
  target_include_directories(pusch_e2e_correctness_sweep PRIVATE
    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
    ${CUDA_ACCEL_INCLUDE_DIRS}
  )
endif()
add_test(pusch_e2e_correctness_sweep pusch_e2e_correctness_sweep)

add_executable(pusch_e2e_sensitivity_sweep pusch_e2e_sensitivity_sweep.cpp)
target_link_libraries(pusch_e2e_sensitivity_sweep ocudu_pxsch_bler_test_helpers)
target_include_directories(pusch_e2e_sensitivity_sweep PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc
)
if(CUDA_ACCEL_FOUND)
  target_compile_definitions(pusch_e2e_sensitivity_sweep PRIVATE ENABLE_CUDA)
  target_include_directories(pusch_e2e_sensitivity_sweep PRIVATE
    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
    ${CUDA_ACCEL_INCLUDE_DIRS}
  )
endif()
# The built-in default sweep covers 8 PRB/MCS configs up to 273PRB and takes hours: run a reduced set in CI and
# leave the full sweep for manual invocation. See --help for the knobs.
add_test(NAME pusch_e2e_sensitivity_sweep
         COMMAND pusch_e2e_sensitivity_sweep --nof_prb 25 --mcs_index 11,27 --nof_frames 20)
set_tests_properties(pusch_e2e_sensitivity_sweep PROPERTIES TIMEOUT 600)

# GPU tests must not run concurrently with each other. The CUDA runtime rejects allocations with
# cudaErrorDevicesUnavailable while several exclusive device contexts are active, which surfaces as intermittent ctest
# failures that look like defects in the accelerated code. The shared lock lets ctest keep running the rest of the suite
# in parallel while serialising the tests that drive the GPU.
if (CUDA_ACCEL_FOUND)
    set_tests_properties(
            pusch_gpu_cpu_comparison_test
            pdsch_gpu_e2e_test
            pdsch_gpu_issue_1_regression
            pusch_gpu_cpu_result_parity_test
            pusch_64qam_stress_test
            pusch_e2e_pipeline_test
            pusch_resident_dematch_scramble_test
            pusch_e2e_correctness_sweep
            pusch_e2e_sensitivity_sweep
            PROPERTIES RESOURCE_LOCK ocudu_cuda_device)
endif ()
