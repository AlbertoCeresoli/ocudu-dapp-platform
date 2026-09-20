# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


set_directory_properties(PROPERTIES LABELS "phy")

# Compile the real resident decoder branch against a CPU fake; no CUDA runtime or device is required.
add_executable(pusch_resident_acceptance_test
        pusch_resident_acceptance_test.cpp
        ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_processors/pusch/pusch_decoder_impl.cpp
        ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_processors/pusch/pusch_codeblock_decoder.cpp
        ${PROJECT_SOURCE_DIR}/lib/fapi_adaptor/phy/p7/phy_to_fapi_results_event_fastpath_translator.cpp)
target_compile_definitions(pusch_resident_acceptance_test PRIVATE ENABLE_CUDA)
target_include_directories(pusch_resident_acceptance_test PRIVATE
        ${PROJECT_SOURCE_DIR}/lib
        ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_processors/pusch
        ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_coding
        ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc
        ${PROJECT_SOURCE_DIR}/lib/fapi_adaptor/phy/p7)
target_link_libraries(pusch_resident_acceptance_test PRIVATE ocudu_ldpc ocudu_crc_calculator
        log_likelihood_ratio ocuduvec ocudu_ran ocudu_instrumentation ocudu_support ocudulog
        GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_resident_acceptance_test)

add_executable(pusch_processing_activity_test pusch_processing_activity_test.cpp)
target_include_directories(pusch_processing_activity_test PRIVATE
        ${PROJECT_SOURCE_DIR}/lib
        ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_processors/pusch)
target_link_libraries(pusch_processing_activity_test PRIVATE ocuduvec ocudu_ran ocudu_instrumentation
        ocudu_support ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_processing_activity_test)

add_executable(pusch_resource_loan_test pusch_resource_loan_test.cpp)
target_include_directories(pusch_resource_loan_test PRIVATE
        ${PROJECT_SOURCE_DIR}/lib
        ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_processors/pusch)
target_link_libraries(pusch_resource_loan_test PRIVATE ocudu_pusch_processor ocuduvec ocudu_ran ocudu_instrumentation
        ocudu_support ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_resource_loan_test)

add_executable(pusch_inline_replacement_test pusch_inline_replacement_test.cpp)
target_include_directories(pusch_inline_replacement_test PRIVATE ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_processors/pusch)
target_link_libraries(pusch_inline_replacement_test PRIVATE ocudu_support GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_inline_replacement_test)

add_executable(pusch_test_cold_trace_test pusch_test_cold_trace_test.cpp)
target_include_directories(pusch_test_cold_trace_test PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/integrationtests/phy/upper/channel_processors)
target_link_libraries(pusch_test_cold_trace_test ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_test_cold_trace_test)

add_executable(pusch_test_primary_context_test pusch_test_primary_context_test.cpp)
target_include_directories(pusch_test_primary_context_test PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/integrationtests/phy/upper/channel_processors)
target_link_libraries(pusch_test_primary_context_test GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_test_primary_context_test)

add_executable(pusch_transform_deprecoder_selection_test pusch_transform_deprecoder_selection_test.cpp)
target_include_directories(pusch_transform_deprecoder_selection_test PRIVATE ${PROJECT_SOURCE_DIR}/lib/phy/cuda/include)
target_link_libraries(pusch_transform_deprecoder_selection_test GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_transform_deprecoder_selection_test)

add_executable(pusch_test_pooled_processor_test pusch_test_pooled_processor_test.cpp)
target_include_directories(pusch_test_pooled_processor_test PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/tests/integrationtests/phy/upper/channel_processors)
target_link_libraries(pusch_test_pooled_processor_test ocuduvec ocudu_ran ocudu_instrumentation
        ocudu_support ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_test_pooled_processor_test)

# Exercise the actual production wrapper chain without constructing CUDA workers.
add_executable(pusch_demodulator_forwarding_test pusch_demodulator_forwarding_test.cpp)
target_include_directories(pusch_demodulator_forwarding_test PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/tests/integrationtests/phy/upper/channel_processors)
target_link_libraries(pusch_demodulator_forwarding_test ocuduvec ocudu_ran ocudu_instrumentation
        ocudu_support ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_demodulator_forwarding_test)

add_executable(pusch_test_failure_trace_test pusch_test_failure_trace_test.cpp)
target_include_directories(pusch_test_failure_trace_test PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/integrationtests/phy/upper/channel_processors)
target_link_libraries(pusch_test_failure_trace_test GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_test_failure_trace_test)

if(CUDA_ACCEL_FOUND)
  add_executable(pusch_test_receiver_retirement_trace_test
          pusch_test_receiver_retirement_trace_test.cpp
          ${PROJECT_SOURCE_DIR}/tests/integrationtests/phy/upper/channel_processors/pusch_test_receiver_retirement_trace.cpp)
  target_include_directories(pusch_test_receiver_retirement_trace_test PRIVATE
          ${PROJECT_SOURCE_DIR}/tests/integrationtests/phy/upper/channel_processors
          ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})
  # Only headers and fake entry points: no CUDA runtime or GPU work in this test.
  target_link_libraries(pusch_test_receiver_retirement_trace_test ocudulog ${CMAKE_DL_LIBS}
          GTest::gtest GTest::gtest_main)
  gtest_discover_tests(pusch_test_receiver_retirement_trace_test)

  # Host-only allocation fakes: compile the CUDA-backed buffer branch, but never
  # link cudart or execute a GPU. Safe to run alongside the live sector.
  add_executable(pusch_test_payload_buffer_test pusch_test_payload_buffer_test.cpp)
  target_include_directories(pusch_test_payload_buffer_test PRIVATE
          ${PROJECT_SOURCE_DIR}/tests/integrationtests/phy/upper/channel_processors
          ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})
  target_compile_definitions(pusch_test_payload_buffer_test PRIVATE ENABLE_CUDA)
  target_link_libraries(pusch_test_payload_buffer_test ocudulog GTest::gtest GTest::gtest_main)
  gtest_discover_tests(pusch_test_payload_buffer_test)
endif()

add_executable(pusch_metric_provenance_test pusch_metric_provenance_test.cpp)
target_include_directories(pusch_metric_provenance_test PRIVATE
        ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_processors/pusch
        ${PROJECT_SOURCE_DIR}/lib/phy/cuda/include)
target_link_libraries(pusch_metric_provenance_test ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_metric_provenance_test)

add_executable(pusch_resident_admission_test pusch_resident_admission_test.cpp)
target_link_libraries(pusch_resident_admission_test ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_resident_admission_test)

add_executable(pusch_processor_validator_test pusch_processor_validator_test.cpp)
target_link_libraries(pusch_processor_validator_test
        ocudu_channel_equalizer
        ocudu_channel_processors
        ocudu_phy_support
        ocudulog
        ocudu_upper_phy_support
        ocudu_transform_precoding
        GTest::gtest
        GTest::gtest_main)
add_test(pusch_processor_validator_test pusch_processor_validator_test)

add_executable(pusch_processor_unittest pusch_processor_unittest.cpp)
target_include_directories(pusch_processor_unittest PRIVATE ${CMAKE_SOURCE_DIR}/tests/unittests)
target_link_libraries(pusch_processor_unittest ocudu_channel_processors ocudulog GTest::gtest GTest::gtest_main)
add_test(pusch_processor_unittest pusch_processor_unittest)

add_executable(pusch_acceleration_runtime_options_test pusch_acceleration_runtime_options_test.cpp)
target_include_directories(pusch_acceleration_runtime_options_test PRIVATE
        ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_processors/pusch)
target_link_libraries(pusch_acceleration_runtime_options_test GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_acceleration_runtime_options_test)

add_executable(pusch_sinr_comparison_test pusch_sinr_comparison_test.cpp)
target_link_libraries(pusch_sinr_comparison_test
        ocudu_channel_equalizer
        ocudu_channel_processors
        ocudu_phy_support
        ocudulog
        ocudu_upper_phy_support
        ocudu_transform_precoding
        gtest
        gtest_main)
add_test(pusch_sinr_comparison_test pusch_sinr_comparison_test)
