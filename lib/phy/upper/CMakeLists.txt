# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


add_subdirectory(channel_coding)
add_subdirectory(channel_modulation)
add_subdirectory(channel_processors)
add_subdirectory(equalization)
add_subdirectory(metrics)
add_subdirectory(sequence_generators)
add_subdirectory(signal_processors)
if(ENABLE_DAPP)
    add_subdirectory(dapp)
endif()

# ZMQ / OCDG upper-PHY diagnostics.
find_program(OCUDU_FLATC_EXECUTABLE flatc)
find_path(OCUDU_FLATBUFFERS_INCLUDE_DIR flatbuffers/flatbuffers.h)
find_library(OCUDU_FLATBUFFERS_LIBRARY flatbuffers)
if(NOT OCUDU_FLATC_EXECUTABLE OR NOT OCUDU_FLATBUFFERS_INCLUDE_DIR)
    message(FATAL_ERROR "FlatBuffers flatc and headers are required for ZMQ diagnostics.")
endif()
set(OCUDU_ZMQ_DIAG_FLATBUFFERS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/diagnostics/generated)
set(OCUDU_ZMQ_DIAG_FLATBUFFERS_GENERATED_HEADER
        ${OCUDU_ZMQ_DIAG_FLATBUFFERS_GENERATED_DIR}/zmq_diag_generated.h)
add_custom_command(
        OUTPUT ${OCUDU_ZMQ_DIAG_FLATBUFFERS_GENERATED_HEADER}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OCUDU_ZMQ_DIAG_FLATBUFFERS_GENERATED_DIR}
        COMMAND ${OCUDU_FLATC_EXECUTABLE}
                --cpp
                --scoped-enums
                -o ${OCUDU_ZMQ_DIAG_FLATBUFFERS_GENERATED_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/zmq_diag.fbs
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/zmq_diag.fbs
        COMMENT "Generating FlatBuffers bindings for ZMQ diagnostics")
add_custom_target(ocudu_zmq_diag_flatbuffers_generated
        DEPENDS ${OCUDU_ZMQ_DIAG_FLATBUFFERS_GENERATED_HEADER})

add_library(ocudu_downlink_processor
        downlink_processor_multi_executor_impl.cpp
        downlink_processor_pool_impl.cpp
        pdsch_grid_output_strategy.cpp)
target_link_libraries(ocudu_downlink_processor
        ocudu_instrumentation
        ocudu_channel_processors)
if(CUDA_ACCEL_FOUND)
    target_compile_definitions(ocudu_downlink_processor PRIVATE ENABLE_CUDA)
    target_include_directories(ocudu_downlink_processor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/channel_coding/ldpc)
    target_link_libraries(ocudu_downlink_processor ocudu_ldpc_cuda CUDA::cudart)
endif()

add_library(ocudu_uplink_processor
        uplink_processor_impl.cpp
        uplink_processor_pool_impl.cpp
        uplink_request_processor_impl.cpp)
target_link_libraries(ocudu_uplink_processor ocudu_channel_processors)

set(OCUDU_UPPER_PHY_SOURCES
        diagnostics/zmq_diag_wire_format.cpp
        diagnostics/diagnostic_plugin_sink.cpp
        diagnostics/srs_diagnostic_decorator.cpp
        phy_acceleration_prach_buffer_factory.cpp
        phy_acceleration_resource_grid_factory.cpp
        upper_phy_error_handler_impl.cpp
        upper_phy_factories.cpp
        upper_phy_impl.cpp
        upper_phy_rx_results_notifier_wrapper.cpp
        upper_phy_rx_symbol_handler_impl.cpp)
if(ZEROMQ_FOUND AND ENABLE_ZEROMQ)
    list(APPEND OCUDU_UPPER_PHY_SOURCES diagnostics/zmq_diag_publisher.cpp)
endif()

add_library(ocudu_upper_phy ${OCUDU_UPPER_PHY_SOURCES})
target_link_libraries(ocudu_upper_phy
        ocudu_channel_coding
        ocudu_channel_equalizer
        ocudu_channel_precoder
        ocudu_channel_processors
        ocudu_downlink_processor
        ocudu_signal_processors
        ocudu_dft
        ocudu_instrumentation
        ocudu_phy_metrics
        ocudu_phy_support
        ocudu_transform_precoding
        ocudu_uplink_processor
        ocudu_upper_phy_metrics
        $<$<BOOL:${CUDA_ACCEL_FOUND}>:CUDA::cudart>)
if(ENABLE_DAPP)
    target_link_libraries(ocudu_upper_phy ocudu_phy_dapp_adapters)
endif()
if(CUDA_ACCEL_FOUND)
    target_compile_definitions(ocudu_upper_phy PRIVATE ENABLE_CUDA)
    # Owned-snapshot copy kernel for CUDA-visible grids (host reads never touch the live managed VA).
    target_sources(ocudu_upper_phy PRIVATE owned_grid_snapshot_copy.cu)
endif()
if(ZEROMQ_FOUND AND ENABLE_ZEROMQ)
    target_compile_definitions(ocudu_upper_phy PRIVATE ENABLE_ZEROMQ)
    target_include_directories(ocudu_upper_phy PRIVATE ${ZEROMQ_INCLUDE_DIRS})
    target_link_libraries(ocudu_upper_phy ${ZEROMQ_LIBRARIES})
endif()
add_dependencies(ocudu_upper_phy ocudu_zmq_diag_flatbuffers_generated)
target_include_directories(ocudu_upper_phy
        PUBLIC
        ${OCUDU_ZMQ_DIAG_FLATBUFFERS_GENERATED_DIR}
        ${OCUDU_FLATBUFFERS_INCLUDE_DIR})
if(OCUDU_FLATBUFFERS_LIBRARY)
    target_link_libraries(ocudu_upper_phy ${OCUDU_FLATBUFFERS_LIBRARY})
endif()
target_link_libraries(ocudu_upper_phy ${CMAKE_DL_LIBS})

add_library(log_likelihood_ratio log_likelihood_ratio.cpp)
target_link_libraries(log_likelihood_ratio ocuduvec)


add_library(ocudu_upper_phy_support
        rx_buffer_pool_impl.cpp
)

add_to_exported_libs(log_likelihood_ratio ocudu_upper_phy_support)
