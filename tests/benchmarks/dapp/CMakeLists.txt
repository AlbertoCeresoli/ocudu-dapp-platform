# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

set_directory_properties(PROPERTIES LABELS "dapp")

add_executable(dapp_runtime_benchmark dapp_runtime_benchmark.cpp)
target_include_directories(dapp_runtime_benchmark PRIVATE
  ${PROJECT_SOURCE_DIR}/tests/unittests/dapp)
target_link_libraries(dapp_runtime_benchmark PRIVATE
  ocudu_dapp_runtime
  ocudu_phy_dapp_adapters
  ocudu_sched
  ocudu_support
  ocudulog)
add_dependencies(dapp_runtime_benchmark dapp_class_a_fixture dapp_class_c_fixture dapp_class_c_process_worker)
target_compile_definitions(dapp_runtime_benchmark PRIVATE
  DAPP_TEST_CATALOG="$<TARGET_FILE_DIR:dapp_class_c_fixture>"
  DAPP_CLASS_A_FIXTURE="$<TARGET_FILE:dapp_class_a_fixture>"
  DAPP_CLASS_C_FIXTURE="$<TARGET_FILE:dapp_class_c_fixture>"
  DAPP_CLASS_C_PROCESS_WORKER="$<TARGET_FILE:dapp_class_c_process_worker>")

if(OCUDU_DAPP_REFERENCE_BUILD_DIR)
  if(NOT OCUDU_DAPP_REFERENCE_INCLUDE_DIR)
    get_filename_component(OCUDU_DAPP_REFERENCE_INCLUDE_DIR
      "${OCUDU_DAPP_REFERENCE_BUILD_DIR}/../include" ABSOLUTE)
  endif()
  if(NOT EXISTS "${OCUDU_DAPP_REFERENCE_INCLUDE_DIR}/ocudu_dapp_sdk/reference_scheduler_pipeline.h")
    message(FATAL_ERROR
      "OCUDU_DAPP_REFERENCE_INCLUDE_DIR does not contain the reference SDK headers: "
      "${OCUDU_DAPP_REFERENCE_INCLUDE_DIR}")
  endif()
  add_executable(dapp_class_b_cpu_benchmark dapp_class_b_cpu_benchmark.cpp)
  target_include_directories(dapp_class_b_cpu_benchmark PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/tests/unittests/scheduler
    ${OCUDU_DAPP_REFERENCE_INCLUDE_DIR})
  target_link_libraries(dapp_class_b_cpu_benchmark PRIVATE
    ocudu_dapp_runtime
    ocudu_sched
    sched_rrm
    scheduler_test_suite
    scheduler_test_utils
    sched_test_doubles
    test_rng
    ocudu_support
    ocudulog)
  target_compile_definitions(dapp_class_b_cpu_benchmark PRIVATE
    OCUDU_DAPP_REFERENCE_BUILD_DIR="${OCUDU_DAPP_REFERENCE_BUILD_DIR}")
endif()

if(CUDA_ACCEL_FOUND AND OCUDU_DAPP_REFERENCE_CUDA_DIR)
  find_path(OCUDU_DAPP_NVML_INCLUDE_DIR NAMES nvml.h HINTS ${CUDAToolkit_INCLUDE_DIRS})
  find_library(OCUDU_DAPP_NVML_LIBRARY NAMES nvidia-ml)
  add_executable(dapp_class_a_cuda_benchmark dapp_class_a_cuda_benchmark.cpp)
  target_include_directories(dapp_class_a_cuda_benchmark PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/unittests/phy)
  target_link_libraries(dapp_class_a_cuda_benchmark PRIVATE
    ocudu_dapp_app_service
    ocudu_phy_dapp_adapters
    ocudu_channel_equalizer
    ocudu_dft
    ocudu_transform_precoding
    ocudu_pusch_demodulator
    ocudu_sequence_generators
    ocudu_support
    ocudu_versioning
    CUDA::cudart)
  target_compile_definitions(dapp_class_a_cuda_benchmark PRIVATE
    ENABLE_CUDA
    OCUDU_DAPP_REFERENCE_CUDA_DIR="${OCUDU_DAPP_REFERENCE_CUDA_DIR}"
    OCUDU_DAPP_PREFLIGHT_HELPER="$<TARGET_FILE:ocudu-dapp-preflight>")
  add_dependencies(dapp_class_a_cuda_benchmark ocudu-dapp-preflight)
  if(OCUDU_DAPP_NVML_INCLUDE_DIR AND OCUDU_DAPP_NVML_LIBRARY)
    target_include_directories(dapp_class_a_cuda_benchmark PRIVATE ${OCUDU_DAPP_NVML_INCLUDE_DIR})
    target_link_libraries(dapp_class_a_cuda_benchmark PRIVATE ${OCUDU_DAPP_NVML_LIBRARY})
    target_compile_definitions(dapp_class_a_cuda_benchmark PRIVATE OCUDU_DAPP_HAS_NVML)
  endif()
endif()
