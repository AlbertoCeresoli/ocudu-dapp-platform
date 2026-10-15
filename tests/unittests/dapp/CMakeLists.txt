# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

add_library(dapp_c_abi_compile_test OBJECT dapp_c_abi_compile_test.c)
target_include_directories(dapp_c_abi_compile_test PRIVATE ${PROJECT_SOURCE_DIR}/include)

set(DAPP_TEST_CATALOG ${CMAKE_CURRENT_BINARY_DIR}/catalog)
foreach(fixture
    valid v1_pre_profile class_a class_a_srs class_b class_c class_c_srs multi optional_multi missing_query
    throwing_query crashing_query hanging_query
    throwing_create crashing_prepare hanging_start throwing_worker throwing_invoke rejected_query invalid_descriptor
    executable_stack sandbox_probe)
  add_library(dapp_${fixture}_fixture SHARED fixtures/${fixture}.cpp)
  target_include_directories(dapp_${fixture}_fixture PRIVATE ${PROJECT_SOURCE_DIR}/include)
  set_target_properties(dapp_${fixture}_fixture PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${DAPP_TEST_CATALOG}
    PREFIX "")
  add_custom_command(TARGET dapp_${fixture}_fixture POST_BUILD
    COMMAND /bin/chmod 0555 $<TARGET_FILE:dapp_${fixture}_fixture>)
endforeach()
target_include_directories(dapp_v1_pre_profile_fixture BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/v1_pre_profile
  ${PROJECT_SOURCE_DIR}/include/ocudu/dapp/abi/v1)
target_link_options(dapp_executable_stack_fixture PRIVATE "LINKER:-z,execstack")

set(DAPP_PREFLIGHT_PROCESS_TREE_PID_FILE "${CMAKE_CURRENT_BINARY_DIR}/preflight-process-tree.pid")
add_executable(dapp_preflight_process_tree_helper fixtures/preflight_process_tree_helper.cpp)
target_compile_definitions(dapp_preflight_process_tree_helper PRIVATE
  DAPP_PREFLIGHT_PROCESS_TREE_PID_FILE="${DAPP_PREFLIGHT_PROCESS_TREE_PID_FILE}")

add_executable(dapp_class_c_process_worker fixtures/class_c_process_worker.cpp)
target_include_directories(dapp_class_c_process_worker PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(dapp_class_c_process_worker PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  RUNTIME_OUTPUT_DIRECTORY ${DAPP_TEST_CATALOG})
add_custom_command(TARGET dapp_class_c_process_worker POST_BUILD
  COMMAND /bin/chmod 0555 $<TARGET_FILE:dapp_class_c_process_worker>)

add_executable(dapp_class_c_process_bundle_worker fixtures/class_c_process_worker.cpp)
target_include_directories(dapp_class_c_process_bundle_worker PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(dapp_class_c_process_bundle_worker PRIVATE DAPP_CLASS_C_PROCESS_MULTI_BUNDLE_DEFAULT=1)
set_target_properties(dapp_class_c_process_bundle_worker PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  RUNTIME_OUTPUT_DIRECTORY ${DAPP_TEST_CATALOG})
add_custom_command(TARGET dapp_class_c_process_bundle_worker POST_BUILD
  COMMAND /bin/chmod 0555 $<TARGET_FILE:dapp_class_c_process_bundle_worker>)

add_executable(dapp_runtime_test
  abi_validation_test.cpp
  artifact_verifier_test.cpp
  circuit_breaker_test.cpp
  class_c_process_supervisor_test.cpp
  dapp_config_test.cpp
  dapp_service_test.cpp
  guarded_call_test.cpp
  incident_ring_test.cpp
  incident_store_test.cpp
  seqpacket_channel_test.cpp
  publication_route_test.cpp
  publication_record_test.cpp
  instance_manager_test.cpp
  job_dispatcher_test.cpp
  module_loader_test.cpp
  native_preflight_test.cpp
  native_hook_registry_test.cpp
  native_instance_test.cpp
  pusch_route_table_test.cpp
  package_manifest_test.cpp
  package_signature_test.cpp
  prepared_output_test.cpp
  quiescence_tracker_test.cpp
  lifecycle_test.cpp
  $<TARGET_OBJECTS:dapp_c_abi_compile_test>)
if (ENABLE_E3)
  target_sources(dapp_runtime_test PRIVATE embedded_agent_test.cpp portable_data_store_test.cpp)
  # Separate executable: allocator instrumentation must not affect unrelated
  # runtime/lifecycle tests or dynamically loaded fixtures.
  add_executable(dapp_telemetry_allocation_test telemetry_allocation_test.cpp)
  target_link_libraries(dapp_telemetry_allocation_test ocudu_dapp_runtime ocudu_gtest_main)
  add_test(NAME dapp_telemetry_isolation.no_producer_allocation COMMAND dapp_telemetry_allocation_test)
  set_tests_properties(dapp_telemetry_isolation.no_producer_allocation PROPERTIES TIMEOUT 30 LABELS "dapp;e3;isolation")
  target_compile_definitions(dapp_runtime_test PRIVATE OCUDU_DAPP_TEST_E3=1)
  if (ENABLE_E3_ASN1_CODEC)
    target_sources(dapp_runtime_test PRIVATE
      e3_aper_codec_test.cpp
      local_connector_test.cpp
      e3ap_codec_test.cpp
      e3ap_libe3_codec_test.cpp
      e3ap_protobuf_codec_test.cpp
      e3ap_v2_codec_test.cpp
      telemetry_publisher_test.cpp
      e3ap_sctp_server_test.cpp)
    target_link_libraries(dapp_runtime_test
      ocudu_dapp_e3_codec ocudu_dapp_e3_client ocudu_dapp_e3_local ocudu_dapp_e3ap_codec ocudu_dapp_e3_sctp)
    target_compile_definitions(dapp_runtime_test PRIVATE OCUDU_DAPP_TEST_LOCAL_E3=1)
    if (OCUDU_DAPP_E3DP_CARRIER)
      target_sources(dapp_runtime_test PRIVATE e3dp_sctp_server_test.cpp)
    endif ()

    # Loopback E3 agent used by the Python client tests and manual interop runs.
    add_executable(e3ap_test_server fixtures/e3ap_test_server.cpp)
    target_include_directories(e3ap_test_server PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(e3ap_test_server ocudu_dapp_e3_sctp ocudu_dapp_e3_codec ocudu_dapp_runtime ocudu_support)

    find_program(DAPP_TEST_PYTHON_EXECUTABLE python3)
    if (DAPP_TEST_PYTHON_EXECUTABLE)
      add_test(NAME dapp_e3ap_libe3.report
        COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/python/tests/test_libe3_interop_report.py)
      set_tests_properties(dapp_e3ap_libe3.report PROPERTIES TIMEOUT 30 LABELS "dapp;e3;python;interop")
      add_test(NAME dapp_e3_shared_region.validation
        COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/python/tests/test_shared_region.py)
      set_tests_properties(dapp_e3_shared_region.validation PROPERTIES TIMEOUT 30 LABELS "dapp;e3;python")
      execute_process(
        COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE} -c "import asn1tools"
        RESULT_VARIABLE DAPP_TEST_ASN1TOOLS_RESULT
        OUTPUT_QUIET ERROR_QUIET)
      if (DAPP_TEST_ASN1TOOLS_RESULT EQUAL 0)
        set(DAPP_E3_SCHEMA ${PROJECT_SOURCE_DIR}/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1)
        set(DAPP_PYTHON_CLIENT ${PROJECT_SOURCE_DIR}/apps/tools/dappctl/ocudu_dappctl.py)
        add_test(NAME dapp_e3_python_codec.golden
          COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE} ${DAPP_PYTHON_CLIENT}
            --schema ${DAPP_E3_SCHEMA} codec-self-test)
        add_test(NAME dapp_e3_cli.cross_language
          COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e3_cli_interop_test.py
            --cpp-client $<TARGET_FILE:ocudu-dappctl>
            --python-client ${DAPP_PYTHON_CLIENT}
            --schema ${DAPP_E3_SCHEMA})
        add_test(NAME dapp_e3ap_python.cross_language
          COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/python/tests/test_e3ap_client.py
            --server $<TARGET_FILE:e3ap_test_server>)
        set_tests_properties(dapp_e3ap_python.cross_language PROPERTIES
          ENVIRONMENT "OCUDU_E3_SCHEMA_DIR=${PROJECT_SOURCE_DIR}/schemas/e3"
          TIMEOUT 120
          LABELS "dapp;e3;python")
        # Structural and byte-level gate against the vendored upstream libe3 E3AP module.
        add_test(NAME dapp_e3ap_libe3.interop
          COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/python/tests/test_libe3_interop.py)
        set_tests_properties(dapp_e3ap_libe3.interop PROPERTIES
          ENVIRONMENT "OCUDU_E3_SCHEMA_DIR=${PROJECT_SOURCE_DIR}/schemas/e3"
          TIMEOUT 120
          LABELS "dapp;e3;python;interop")
      endif ()
    endif ()
    # The hand-written E3AP Protobuf codec against libprotobuf built from the vendored libe3 schema
    # and the OCUDU superset schema (needs libprotobuf-dev and protobuf-compiler; the gNB itself
    # never links libprotobuf).
    find_package(Protobuf QUIET)
    if (Protobuf_FOUND AND Protobuf_PROTOC_EXECUTABLE)
      set(DAPP_E3AP_PROTO_OUT ${CMAKE_CURRENT_BINARY_DIR}/e3ap_proto)
      set(DAPP_E3AP_LIBE3_PROTO ${PROJECT_SOURCE_DIR}/schemas/e3/interop/libe3/e3ap-1.0.0.proto)
      set(DAPP_E3AP_OCUDU_PROTO ${PROJECT_SOURCE_DIR}/schemas/e3/protobuf/e3ap-ocudu-v2.proto)
      file(MAKE_DIRECTORY ${DAPP_E3AP_PROTO_OUT})
      add_custom_command(
        OUTPUT ${DAPP_E3AP_PROTO_OUT}/e3ap-1.0.0.pb.cc ${DAPP_E3AP_PROTO_OUT}/e3ap-1.0.0.pb.h
               ${DAPP_E3AP_PROTO_OUT}/e3ap-ocudu-v2.pb.cc ${DAPP_E3AP_PROTO_OUT}/e3ap-ocudu-v2.pb.h
        COMMAND ${Protobuf_PROTOC_EXECUTABLE} --cpp_out=${DAPP_E3AP_PROTO_OUT}
                -I ${PROJECT_SOURCE_DIR}/schemas/e3/interop/libe3 ${DAPP_E3AP_LIBE3_PROTO}
        COMMAND ${Protobuf_PROTOC_EXECUTABLE} --cpp_out=${DAPP_E3AP_PROTO_OUT}
                -I ${PROJECT_SOURCE_DIR}/schemas/e3/protobuf ${DAPP_E3AP_OCUDU_PROTO}
        DEPENDS ${DAPP_E3AP_LIBE3_PROTO} ${DAPP_E3AP_OCUDU_PROTO}
        COMMENT "protoc: E3AP libe3 and OCUDU schemas")
      add_library(dapp_e3ap_proto_messages STATIC
        ${DAPP_E3AP_PROTO_OUT}/e3ap-1.0.0.pb.cc ${DAPP_E3AP_PROTO_OUT}/e3ap-ocudu-v2.pb.cc)
      target_include_directories(dapp_e3ap_proto_messages SYSTEM PUBLIC ${DAPP_E3AP_PROTO_OUT})
      target_link_libraries(dapp_e3ap_proto_messages PUBLIC protobuf::libprotobuf)
      # Generated code: libprotobuf wants RTTI and is not held to the project's warning set.
      target_compile_options(dapp_e3ap_proto_messages PRIVATE -w -frtti)
      add_executable(dapp_e3ap_protobuf_conformance_test e3ap_protobuf_conformance_test.cpp)
      target_compile_options(dapp_e3ap_protobuf_conformance_test PRIVATE -frtti)
      target_link_libraries(dapp_e3ap_protobuf_conformance_test
        dapp_e3ap_proto_messages ocudu_dapp_e3ap_codec GTest::gtest ocudu_gtest_main)
      gtest_discover_tests(dapp_e3ap_protobuf_conformance_test PROPERTIES LABELS "dapp;e3;interop;protobuf")
    else ()
      message(WARNING "E3AP Protobuf conformance test not built: install libprotobuf-dev and protobuf-compiler")
    endif ()

    # End-to-end tests against the real upstream libe3 (opt-in, see the README there).
    add_subdirectory(${PROJECT_SOURCE_DIR}/tests/interop/libe3 ${CMAKE_CURRENT_BINARY_DIR}/interop/libe3)
  endif ()
endif ()
if (OCUDU_DAPP_CLASS_C_METADATA)
  target_sources(dapp_runtime_test PRIVATE
    class_c_metadata_test.cpp
    class_c_publication_encoder_test.cpp)
  target_include_directories(dapp_runtime_test PRIVATE ${PROJECT_BINARY_DIR}/lib/dapp/generated ${FLATBUFFERS_INCLUDE_DIR})
endif ()
target_link_libraries(dapp_runtime_test
  ocudu_dapp_runtime
  ocudu_dapp_app_config
  ocudu_dapp_app_service
  ocudu_yaml_util
  GTest::gtest
  ocudu_gtest_main)
if (NOT DISABLE_MBEDTLS)
  find_package(MbedTLS REQUIRED)
  target_compile_definitions(dapp_runtime_test PRIVATE OCUDU_DAPP_TEST_SIGNATURE_CRYPTO=1)
  target_include_directories(dapp_runtime_test PRIVATE ${MBEDTLS_INCLUDE_DIRS})
  target_link_libraries(dapp_runtime_test ${MBEDTLS_LIBRARIES})
endif ()
add_dependencies(dapp_runtime_test
  dapp_valid_fixture
  dapp_v1_pre_profile_fixture
  dapp_class_a_fixture
  dapp_class_a_srs_fixture
  dapp_class_b_fixture
  dapp_class_c_fixture
  dapp_class_c_srs_fixture
  dapp_multi_fixture
  dapp_optional_multi_fixture
  dapp_missing_query_fixture
  dapp_throwing_query_fixture
  dapp_crashing_query_fixture
  dapp_hanging_query_fixture
  dapp_throwing_create_fixture
  dapp_crashing_prepare_fixture
  dapp_hanging_start_fixture
  dapp_throwing_worker_fixture
  dapp_throwing_invoke_fixture
  dapp_rejected_query_fixture
  dapp_invalid_descriptor_fixture)
add_dependencies(dapp_runtime_test
  dapp_executable_stack_fixture
  dapp_sandbox_probe_fixture
  dapp_preflight_process_tree_helper)
add_dependencies(dapp_runtime_test dapp_class_c_process_worker dapp_class_c_process_bundle_worker)

if (NOT DAPP_TEST_PYTHON_EXECUTABLE)
  find_program(DAPP_TEST_PYTHON_EXECUTABLE python3)
endif ()
if (DAPP_TEST_PYTHON_EXECUTABLE)
  add_test(NAME dapp_release_docs
    COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_dapp_release_docs.py)
  set_tests_properties(dapp_release_docs PROPERTIES LABELS "dapp;unit;docs")
  if (NOT DISABLE_MBEDTLS)
    add_test(NAME dapp_package_signing_protocol
      COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/package_signing_protocol_test.py
        --tool $<TARGET_FILE:ocudu-dapp-package>
        --artifact $<TARGET_FILE:dapp_valid_fixture>
        --signature-schema ${PROJECT_SOURCE_DIR}/schemas/dapp/package-signature-v1.schema.json
        --trust-schema ${PROJECT_SOURCE_DIR}/schemas/dapp/package-trust-policy-v1.schema.json)
    set_tests_properties(dapp_package_signing_protocol PROPERTIES LABELS "dapp;unit;security")
    add_dependencies(dapp_runtime_test ocudu-dapp-package)
  endif ()
  add_test(NAME dapp_failure_gallery_protocol
    COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/failure_gallery_protocol_test.py
      --runner ${PROJECT_SOURCE_DIR}/tests/integrationtests/dapp/run_failure_gallery.py
      --report-schema ${PROJECT_SOURCE_DIR}/schemas/dapp/failure-gallery-report-v1.schema.json)
  set_tests_properties(dapp_failure_gallery_protocol PROPERTIES LABELS "dapp;unit")
  add_test(NAME dapp_class_a_cuda_qualification_protocol
    COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/class_a_cuda_qualification_protocol_test.py
      --runner ${PROJECT_SOURCE_DIR}/tests/benchmarks/dapp/run_class_a_cuda_qualification.py)
  set_tests_properties(dapp_class_a_cuda_qualification_protocol PROPERTIES LABELS "dapp;unit")
  add_test(NAME dapp_class_a_cuda_contention_protocol
    COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/class_a_cuda_contention_protocol_test.py
      --runner ${PROJECT_SOURCE_DIR}/tests/benchmarks/dapp/run_class_a_cuda_contention.py)
  set_tests_properties(dapp_class_a_cuda_contention_protocol PROPERTIES LABELS "dapp;unit")
  add_test(NAME dapp_cuda_loaded_l1_soak_protocol
    COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/cuda_dapp_soak_protocol_test.py
      --runner ${PROJECT_SOURCE_DIR}/tests/benchmarks/dapp/run_cuda_dapp_soak.py)
  set_tests_properties(dapp_cuda_loaded_l1_soak_protocol PROPERTIES LABELS "dapp;unit")
  add_test(NAME dapp_class_c_cuda_throughput_protocol
    COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/class_c_cuda_throughput_protocol_test.py
      --runner ${PROJECT_SOURCE_DIR}/tests/benchmarks/dapp/run_class_c_cuda_throughput.py)
  set_tests_properties(dapp_class_c_cuda_throughput_protocol PROPERTIES LABELS "dapp;unit")
  add_test(NAME dapp_class_b_cpu_timing_protocol
    COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/class_b_cpu_timing_protocol_test.py
      --runner ${PROJECT_SOURCE_DIR}/tests/benchmarks/dapp/run_class_b_cpu_timing.py
      --policy-schema ${PROJECT_SOURCE_DIR}/schemas/dapp/class-b-cpu-timing-policy-v1.schema.json
      --report-schema ${PROJECT_SOURCE_DIR}/schemas/dapp/class-b-cpu-timing-report-v1.schema.json)
  set_tests_properties(dapp_class_b_cpu_timing_protocol PROPERTIES LABELS "dapp;unit")
endif ()
add_dependencies(dapp_runtime_test ocudu-dapp-preflight)
target_compile_definitions(dapp_runtime_test PRIVATE
  DAPP_TEST_CATALOG="${DAPP_TEST_CATALOG}"
  DAPP_VALID_FIXTURE="$<TARGET_FILE:dapp_valid_fixture>"
  DAPP_V1_PRE_PROFILE_FIXTURE="$<TARGET_FILE:dapp_v1_pre_profile_fixture>"
  DAPP_CLASS_A_FIXTURE="$<TARGET_FILE:dapp_class_a_fixture>"
  DAPP_CLASS_A_SRS_FIXTURE="$<TARGET_FILE:dapp_class_a_srs_fixture>"
  DAPP_CLASS_B_FIXTURE="$<TARGET_FILE:dapp_class_b_fixture>"
  DAPP_CLASS_C_FIXTURE="$<TARGET_FILE:dapp_class_c_fixture>"
  DAPP_CLASS_C_SRS_FIXTURE="$<TARGET_FILE:dapp_class_c_srs_fixture>"
  DAPP_MULTI_FIXTURE="$<TARGET_FILE:dapp_multi_fixture>"
  DAPP_OPTIONAL_MULTI_FIXTURE="$<TARGET_FILE:dapp_optional_multi_fixture>"
  DAPP_MISSING_QUERY_FIXTURE="$<TARGET_FILE:dapp_missing_query_fixture>"
  DAPP_THROWING_QUERY_FIXTURE="$<TARGET_FILE:dapp_throwing_query_fixture>"
  DAPP_CRASHING_QUERY_FIXTURE="$<TARGET_FILE:dapp_crashing_query_fixture>"
  DAPP_HANGING_QUERY_FIXTURE="$<TARGET_FILE:dapp_hanging_query_fixture>"
  DAPP_THROWING_CREATE_FIXTURE="$<TARGET_FILE:dapp_throwing_create_fixture>"
  DAPP_CRASHING_PREPARE_FIXTURE="$<TARGET_FILE:dapp_crashing_prepare_fixture>"
  DAPP_HANGING_START_FIXTURE="$<TARGET_FILE:dapp_hanging_start_fixture>"
  DAPP_THROWING_WORKER_FIXTURE="$<TARGET_FILE:dapp_throwing_worker_fixture>"
  DAPP_THROWING_INVOKE_FIXTURE="$<TARGET_FILE:dapp_throwing_invoke_fixture>"
  DAPP_REJECTED_QUERY_FIXTURE="$<TARGET_FILE:dapp_rejected_query_fixture>"
  DAPP_INVALID_DESCRIPTOR_FIXTURE="$<TARGET_FILE:dapp_invalid_descriptor_fixture>"
  DAPP_SANDBOX_PROBE_FIXTURE="$<TARGET_FILE:dapp_sandbox_probe_fixture>"
  DAPP_PREFLIGHT_PROCESS_TREE_HELPER="$<TARGET_FILE:dapp_preflight_process_tree_helper>"
  DAPP_PREFLIGHT_PROCESS_TREE_PID_FILE="${DAPP_PREFLIGHT_PROCESS_TREE_PID_FILE}"
  DAPP_PREFLIGHT_HELPER="$<TARGET_FILE:ocudu-dapp-preflight>")
target_compile_definitions(dapp_runtime_test PRIVATE
  DAPP_EXECUTABLE_STACK_FIXTURE="$<TARGET_FILE:dapp_executable_stack_fixture>"
  DAPP_CLASS_C_PROCESS_WORKER="$<TARGET_FILE:dapp_class_c_process_worker>"
  DAPP_CLASS_C_PROCESS_BUNDLE_WORKER="$<TARGET_FILE:dapp_class_c_process_bundle_worker>")
gtest_discover_tests(dapp_runtime_test)

add_executable(dapp_composition_test spectrum_scheduler_composition_test.cpp)
target_link_libraries(dapp_composition_test
  ocudu_dapp_app_service
  scheduler_test_suite
  scheduler_test_utils
  sched_test_doubles
  GTest::gtest
  ocudu_gtest_main)
add_dependencies(dapp_composition_test dapp_class_b_fixture dapp_class_c_fixture ocudu-dapp-preflight)
target_compile_definitions(dapp_composition_test PRIVATE
  DAPP_TEST_CATALOG="${DAPP_TEST_CATALOG}"
  DAPP_CLASS_B_FIXTURE="$<TARGET_FILE:dapp_class_b_fixture>"
  DAPP_CLASS_C_FIXTURE="$<TARGET_FILE:dapp_class_c_fixture>"
  DAPP_PREFLIGHT_HELPER="$<TARGET_FILE:ocudu-dapp-preflight>")
set(OCUDU_DAPP_REFERENCE_BUILD_DIR "" CACHE PATH "Optional standalone reference dApp build for composition tests")
if (OCUDU_DAPP_REFERENCE_BUILD_DIR)
  target_compile_definitions(dapp_composition_test PRIVATE
    DAPP_REFERENCE_BUILD_DIR="${OCUDU_DAPP_REFERENCE_BUILD_DIR}")
endif ()
gtest_discover_tests(dapp_composition_test PROPERTIES LABELS "dapp;integration;tsan")

add_executable(dapp_native_hook_tsan_test native_instance_test.cpp)
target_link_libraries(dapp_native_hook_tsan_test ocudu_dapp_runtime GTest::gtest ocudu_gtest_main)
add_dependencies(dapp_native_hook_tsan_test
  dapp_valid_fixture dapp_class_a_fixture dapp_class_a_srs_fixture dapp_class_b_fixture dapp_class_c_fixture
  dapp_class_c_srs_fixture)
target_compile_definitions(dapp_native_hook_tsan_test PRIVATE
  DAPP_TEST_CATALOG="${DAPP_TEST_CATALOG}"
  DAPP_VALID_FIXTURE="$<TARGET_FILE:dapp_valid_fixture>"
  DAPP_CLASS_A_FIXTURE="$<TARGET_FILE:dapp_class_a_fixture>"
  DAPP_CLASS_A_SRS_FIXTURE="$<TARGET_FILE:dapp_class_a_srs_fixture>"
  DAPP_CLASS_B_FIXTURE="$<TARGET_FILE:dapp_class_b_fixture>"
  DAPP_CLASS_C_FIXTURE="$<TARGET_FILE:dapp_class_c_fixture>"
  DAPP_CLASS_C_SRS_FIXTURE="$<TARGET_FILE:dapp_class_c_srs_fixture>")
gtest_discover_tests(dapp_native_hook_tsan_test
  TEST_FILTER "dapp_native_hook_*.*"
  TEST_PREFIX "tsan."
  PROPERTIES LABELS "tsan")

add_executable(dapp_shared_spsc_ring_test shared_spsc_ring_test.cpp)
target_link_libraries(dapp_shared_spsc_ring_test ocudu_dapp_runtime GTest::gtest ocudu_gtest_main)
gtest_discover_tests(dapp_shared_spsc_ring_test PROPERTIES LABELS "tsan")

# Keep dApp-owned benchmarks in the Clear-licensed dApp build graph while
# retaining the conventional tests/benchmarks source and output layout.
add_subdirectory(${PROJECT_SOURCE_DIR}/tests/benchmarks/dapp ${PROJECT_BINARY_DIR}/tests/benchmarks/dapp)

add_test(NAME dapp_sdk_package.installed_c_consumer
  COMMAND ${CMAKE_COMMAND}
    -DOCUDU_BUILD_DIR=${PROJECT_BINARY_DIR}
    -DCONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/sdk_consumer
    -DCONSUMER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/sdk_consumer_build
    -DINSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/sdk_install
    -DCONSUMER_C_FLAGS=${CMAKE_C_FLAGS}
    -DCONSUMER_CXX_FLAGS=${CMAKE_CXX_FLAGS}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_sdk_package.cmake)
