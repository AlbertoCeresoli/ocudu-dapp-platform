# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


set_directory_properties(PROPERTIES LABELS "phy")

add_executable(pusch_decoder_batch_extent_test pusch_decoder_batch_extent_test.cpp)
target_include_directories(pusch_decoder_batch_extent_test PRIVATE ${PROJECT_SOURCE_DIR}/lib)
target_link_libraries(pusch_decoder_batch_extent_test GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_decoder_batch_extent_test)

add_executable(pusch_metric_provenance_test pusch_metric_provenance_test.cpp)
target_include_directories(pusch_metric_provenance_test PRIVATE
        ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_processors/pusch
        ${PROJECT_SOURCE_DIR}/lib/phy/cuda/include)
target_link_libraries(pusch_metric_provenance_test ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(pusch_metric_provenance_test)

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
