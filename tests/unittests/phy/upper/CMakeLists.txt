# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test_data)
set(TEST_DATA_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_data)

add_subdirectory(channel_coding)
add_subdirectory(channel_modulation)
add_subdirectory(channel_processors)
add_subdirectory(equalization)
add_subdirectory(sequence_generators)
add_subdirectory(signal_processors)

set_directory_properties(PROPERTIES LABELS "phy")

add_executable(channel_state_information_test channel_state_information_test.cpp)
target_link_libraries(channel_state_information_test ocudulog fmt GTest::gtest GTest::gtest_main)
add_test(channel_state_information_test channel_state_information_test)

add_executable(downlink_processor_pool_test downlink_processor_pool_test.cpp)
target_link_libraries(downlink_processor_pool_test ocudu_ran ocudu_upper_phy ocudulog GTest::gtest GTest::gtest_main)
add_test(downlink_processor_pool_test downlink_processor_pool_test)

add_executable(downlink_processor_test downlink_processor_test.cpp)
target_link_libraries(downlink_processor_test ocudu_downlink_processor ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(downlink_processor_test)

add_executable(hard_decision_test hard_decision_test.cpp)
target_link_libraries(hard_decision_test log_likelihood_ratio ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(hard_decision_test)

add_executable(phy_acceleration_runtime_options_test phy_acceleration_runtime_options_test.cpp)
target_include_directories(phy_acceleration_runtime_options_test PRIVATE ${PROJECT_SOURCE_DIR}/lib/phy/upper)
target_link_libraries(phy_acceleration_runtime_options_test GTest::gtest GTest::gtest_main)
gtest_discover_tests(phy_acceleration_runtime_options_test)

add_executable(log_likelihood_ratio_test log_likelihood_ratio_test.cpp)
target_include_directories(log_likelihood_ratio_test PRIVATE ${CMAKE_SOURCE_DIR}/tests/unittests)
target_link_libraries(log_likelihood_ratio_test log_likelihood_ratio ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(log_likelihood_ratio_test)

add_executable(rx_buffer_pool_test rx_buffer_pool_test.cpp)
target_include_directories(rx_buffer_pool_test PRIVATE ${CMAKE_SOURCE_DIR}/tests/unittests)
target_link_libraries(rx_buffer_pool_test ocudu_upper_phy_support ocudulog GTest::gtest ocudu_support)
gtest_discover_tests(rx_buffer_pool_test)

add_executable(uplink_processor_test uplink_processor_test.cpp)
target_link_libraries(uplink_processor_test ocudu_uplink_processor ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(uplink_processor_test)

add_executable(uplink_request_processor_test uplink_request_processor_test.cpp)
target_link_libraries(uplink_request_processor_test ocudu_uplink_processor ocudu_phy_support ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(uplink_request_processor_test)

add_executable(upper_phy_rx_symbol_handler_test upper_phy_rx_symbol_handler_test.cpp)
target_link_libraries(upper_phy_rx_symbol_handler_test ocudu_upper_phy ocudu_phy_support ocudulog GTest::gtest GTest::gtest_main)
gtest_discover_tests(upper_phy_rx_symbol_handler_test)

if(ENABLE_DAPP)
  add_executable(dapp_resource_grid_tensor_adapter_test dapp_resource_grid_tensor_adapter_test.cpp)
  target_link_libraries(dapp_resource_grid_tensor_adapter_test
          ocudu_phy_dapp_adapters
          ocudu_dapp_runtime
          ocudu_phy_support
          ocudulog
          GTest::gtest
          GTest::gtest_main)
  if(ENABLE_CUDA)
    # The adapter enqueues cudaStreamWaitEvent on the owned-snapshot event, so
    # the fixtures must supply real CUDA objects in accelerated builds.
    target_link_libraries(dapp_resource_grid_tensor_adapter_test CUDA::cudart)
    target_compile_definitions(dapp_resource_grid_tensor_adapter_test PRIVATE ENABLE_CUDA)
  endif()
  gtest_discover_tests(dapp_resource_grid_tensor_adapter_test)

  add_executable(dapp_hook_provider_test
          dapp_channel_estimator_test.cpp
          dapp_demodulator_test.cpp
          dapp_data_router_test.cpp
          dapp_hook_provider_test.cpp
          dapp_process_target_test.cpp)
  if(ENABLE_E3)
    target_sources(dapp_hook_provider_test PRIVATE dapp_portable_e3_target_test.cpp)
  endif()
  target_link_libraries(dapp_hook_provider_test
          ocudu_phy_dapp_adapters
          ocudu_phy_support
          ocudu_srs_estimator
          ocudu_support
          ocudulog
          GTest::gtest
          GTest::gtest_main)
  gtest_discover_tests(dapp_hook_provider_test PROPERTIES LABELS "tsan")
endif()

if(CUDA_ACCEL_FOUND)
  add_executable(pusch_resident_dapp_adapter_test pusch_resident_dapp_adapter_test.cpp)
  target_link_libraries(pusch_resident_dapp_adapter_test
          ocudu_dapp_app_service
          ocudu_phy_dapp_adapters
          ocudu_channel_equalizer
          ocudu_dft
          ocudu_transform_precoding
          ocudu_pusch_demodulator
          ocudu_sequence_generators
          CUDA::cudart
          GTest::gtest
          GTest::gtest_main)
  target_compile_definitions(pusch_resident_dapp_adapter_test PRIVATE ENABLE_CUDA)
  add_dependencies(pusch_resident_dapp_adapter_test ocudu-dapp-preflight)
  target_compile_definitions(pusch_resident_dapp_adapter_test PRIVATE
          OCUDU_DAPP_PREFLIGHT_HELPER="$<TARGET_FILE:ocudu-dapp-preflight>")
  if(OCUDU_DAPP_REFERENCE_CUDA_DIR)
    target_compile_definitions(pusch_resident_dapp_adapter_test PRIVATE
            OCUDU_DAPP_REFERENCE_CUDA_DIR="${OCUDU_DAPP_REFERENCE_CUDA_DIR}")
  endif()
  if(OCUDU_DAPP_SCAFFOLD_CUDA_DIR)
    target_compile_definitions(pusch_resident_dapp_adapter_test PRIVATE
            OCUDU_DAPP_SCAFFOLD_CUDA_DIR="${OCUDU_DAPP_SCAFFOLD_CUDA_DIR}")
  endif()
  gtest_discover_tests(pusch_resident_dapp_adapter_test PROPERTIES RUN_SERIAL TRUE)

  add_executable(pdsch_grid_output_strategy_test pdsch_grid_output_strategy_test.cpp)
  target_include_directories(pdsch_grid_output_strategy_test PRIVATE
          ${PROJECT_SOURCE_DIR}/lib/phy/upper
          ${PROJECT_SOURCE_DIR}/lib/phy/upper/channel_coding/ldpc)
  target_link_libraries(pdsch_grid_output_strategy_test
          ocudu_downlink_processor
          ocudu_upper_phy
          ocudu_phy_support
          ocudulog
          CUDA::cudart
          GTest::gtest
          GTest::gtest_main)
  target_compile_definitions(pdsch_grid_output_strategy_test PRIVATE ENABLE_CUDA)
  # Serialise GPU tests: the CUDA runtime rejects allocations with cudaErrorDevicesUnavailable while several
    # exclusive device contexts are active, which surfaces as intermittent failures rather than real defects.
    gtest_discover_tests(pdsch_grid_output_strategy_test PROPERTIES RESOURCE_LOCK ocudu_cuda_device)

  add_executable(cuda_visible_phy_support_test cuda_visible_phy_support_test.cpp)
  target_include_directories(cuda_visible_phy_support_test PRIVATE ${PROJECT_SOURCE_DIR}/lib/phy/upper)
  target_link_libraries(cuda_visible_phy_support_test
          ocudu_upper_phy
          ocudu_phy_support
          CUDA::cudart
          GTest::gtest
          GTest::gtest_main)
  target_compile_definitions(cuda_visible_phy_support_test PRIVATE ENABLE_CUDA)
  # Serialise GPU tests: the CUDA runtime rejects allocations with cudaErrorDevicesUnavailable while several
    # exclusive device contexts are active, which surfaces as intermittent failures rather than real defects.
    gtest_discover_tests(cuda_visible_phy_support_test PROPERTIES RESOURCE_LOCK ocudu_cuda_device)
endif()
