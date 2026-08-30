# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

if (NOT ENABLE_E3 OR NOT ENABLE_E3_ASN1_CODEC OR NOT OCUDU_DAPP_REFERENCE_BUILD_DIR)
  return()
endif ()
if (ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN OR ENABLE_RTSAN OR CMAKE_CXX_FLAGS MATCHES "-fsanitize=")
  return()
endif ()

find_program(DAPP_E2E_PYTHON_EXECUTABLE python3)
if (NOT DAPP_E2E_PYTHON_EXECUTABLE)
  return()
endif ()
execute_process(
  COMMAND ${DAPP_E2E_PYTHON_EXECUTABLE} -c "import asn1tools, jsonschema"
  RESULT_VARIABLE DAPP_E2E_ASN1TOOLS_RESULT
  OUTPUT_QUIET ERROR_QUIET)
if (NOT DAPP_E2E_ASN1TOOLS_RESULT EQUAL 0)
  return()
endif ()

add_custom_target(dapp_dummy_ru_e2e_dependencies DEPENDS odu ocudu-dappctl ocudu-dapp-preflight)
add_test(NAME dapp_dummy_ru_e2e.native_process_restart_and_continuity
  COMMAND ${DAPP_E2E_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/dummy_ru_e2e_test.py
    --odu $<TARGET_FILE:odu>
    --client ${PROJECT_SOURCE_DIR}/apps/tools/dappctl/ocudu_dappctl.py
    --schema ${PROJECT_SOURCE_DIR}/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1
    --catalog ${OCUDU_DAPP_REFERENCE_BUILD_DIR}
    --preflight-helper $<TARGET_FILE:ocudu-dapp-preflight>)
set_tests_properties(dapp_dummy_ru_e2e.native_process_restart_and_continuity PROPERTIES
  LABELS "dapp;integration;e2e;ru_dummy"
  RUN_SERIAL TRUE
  TIMEOUT 90)

add_test(NAME dapp_dummy_ru_e2e.declarative_initial_instances
  COMMAND ${DAPP_E2E_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/dummy_ru_e2e_test.py
    --odu $<TARGET_FILE:odu>
    --client ${PROJECT_SOURCE_DIR}/apps/tools/dappctl/ocudu_dappctl.py
    --schema ${PROJECT_SOURCE_DIR}/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1
    --catalog ${OCUDU_DAPP_REFERENCE_BUILD_DIR}
    --preflight-helper $<TARGET_FILE:ocudu-dapp-preflight>
    --initial-startup)
set_tests_properties(dapp_dummy_ru_e2e.declarative_initial_instances PROPERTIES
  LABELS "dapp;integration;e2e;ru_dummy;startup"
  RUN_SERIAL TRUE
  TIMEOUT 60)

add_test(NAME dapp_dummy_ru_e2e.multicell_cpu_six_interface_lifecycle
  COMMAND ${DAPP_E2E_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/dummy_ru_e2e_test.py
    --odu $<TARGET_FILE:odu>
    --client ${PROJECT_SOURCE_DIR}/apps/tools/dappctl/ocudu_dappctl.py
    --schema ${PROJECT_SOURCE_DIR}/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1
    --catalog ${OCUDU_DAPP_REFERENCE_BUILD_DIR}
    --preflight-helper $<TARGET_FILE:ocudu-dapp-preflight>
    --multicell-cpu)
set_tests_properties(dapp_dummy_ru_e2e.multicell_cpu_six_interface_lifecycle PROPERTIES
  LABELS "dapp;integration;e2e;ru_dummy;multicell;cpu"
  RUN_SERIAL TRUE
  TIMEOUT 180)

add_test(NAME dapp_dummy_ru_e2e.operator_failure_gallery
  COMMAND ${DAPP_E2E_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_failure_gallery.py
    --driver ${CMAKE_CURRENT_SOURCE_DIR}/dummy_ru_e2e_test.py
    --odu $<TARGET_FILE:odu>
    --client ${PROJECT_SOURCE_DIR}/apps/tools/dappctl/ocudu_dappctl.py
    --schema ${PROJECT_SOURCE_DIR}/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1
    --catalog ${OCUDU_DAPP_REFERENCE_BUILD_DIR}
    --preflight-helper $<TARGET_FILE:ocudu-dapp-preflight>
    --report-schema ${PROJECT_SOURCE_DIR}/schemas/dapp/failure-gallery-report-v1.schema.json
    --output ${CMAKE_CURRENT_BINARY_DIR}/dapp-failure-gallery-report.json)
set_tests_properties(dapp_dummy_ru_e2e.operator_failure_gallery PROPERTIES
  LABELS "dapp;integration;e2e;ru_dummy;failure_gallery;safety"
  RUN_SERIAL TRUE
  TIMEOUT 150)

if(NOT DISABLE_MBEDTLS)
  add_dependencies(dapp_dummy_ru_e2e_dependencies ocudu-dapp-package)
  add_test(NAME dapp_dummy_ru_e2e.signed_sdk_catalog_lifecycle
    COMMAND ${DAPP_E2E_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_signed_catalog.py
      --driver ${CMAKE_CURRENT_SOURCE_DIR}/dummy_ru_e2e_test.py
      --package-tool $<TARGET_FILE:ocudu-dapp-package>
      --source-catalog ${OCUDU_DAPP_REFERENCE_BUILD_DIR}
      --odu $<TARGET_FILE:odu>
      --client ${PROJECT_SOURCE_DIR}/apps/tools/dappctl/ocudu_dappctl.py
      --schema ${PROJECT_SOURCE_DIR}/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1
      --preflight-helper $<TARGET_FILE:ocudu-dapp-preflight>
      --signature-schema ${PROJECT_SOURCE_DIR}/schemas/dapp/package-signature-v1.schema.json
      --trust-schema ${PROJECT_SOURCE_DIR}/schemas/dapp/package-trust-policy-v1.schema.json
      --manifest-schema ${PROJECT_SOURCE_DIR}/schemas/dapp/package-manifest-v1.schema.json
      --sbom-schema ${PROJECT_SOURCE_DIR}/schemas/dapp/package-spdx-sbom-profile-v1.schema.json)
  set_tests_properties(dapp_dummy_ru_e2e.signed_sdk_catalog_lifecycle PROPERTIES
    LABELS "dapp;integration;e2e;ru_dummy;security;signing"
    RUN_SERIAL TRUE
    TIMEOUT 120)

  add_test(NAME dapp_dummy_ru_e2e.signed_declarative_initial_instances
    COMMAND ${DAPP_E2E_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_signed_catalog.py
      --driver ${CMAKE_CURRENT_SOURCE_DIR}/dummy_ru_e2e_test.py
      --package-tool $<TARGET_FILE:ocudu-dapp-package>
      --source-catalog ${OCUDU_DAPP_REFERENCE_BUILD_DIR}
      --odu $<TARGET_FILE:odu>
      --client ${PROJECT_SOURCE_DIR}/apps/tools/dappctl/ocudu_dappctl.py
      --schema ${PROJECT_SOURCE_DIR}/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1
      --preflight-helper $<TARGET_FILE:ocudu-dapp-preflight>
      --signature-schema ${PROJECT_SOURCE_DIR}/schemas/dapp/package-signature-v1.schema.json
      --trust-schema ${PROJECT_SOURCE_DIR}/schemas/dapp/package-trust-policy-v1.schema.json
      --manifest-schema ${PROJECT_SOURCE_DIR}/schemas/dapp/package-manifest-v1.schema.json
      --sbom-schema ${PROJECT_SOURCE_DIR}/schemas/dapp/package-spdx-sbom-profile-v1.schema.json
      --initial-startup)
  set_tests_properties(dapp_dummy_ru_e2e.signed_declarative_initial_instances PROPERTIES
    LABELS "dapp;integration;e2e;ru_dummy;startup;security;signing"
    RUN_SERIAL TRUE
    TIMEOUT 90)

  add_test(NAME dapp_dummy_ru_e2e.installed_release_provisioning_v1
    COMMAND ${DAPP_E2E_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_installed_release_provisioning.py
      --cmake-command ${CMAKE_COMMAND}
      --build-dir ${PROJECT_BINARY_DIR}
      --source-catalog ${OCUDU_DAPP_REFERENCE_BUILD_DIR}
      --driver ${CMAKE_CURRENT_SOURCE_DIR}/dummy_ru_e2e_test.py
      --odu $<TARGET_FILE:odu>
      --client ${PROJECT_SOURCE_DIR}/apps/tools/dappctl/ocudu_dappctl.py
      --e3-schema ${PROJECT_SOURCE_DIR}/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1
      --preflight-helper $<TARGET_FILE:ocudu-dapp-preflight>
      --output ${CMAKE_CURRENT_BINARY_DIR}/release-provisioning-report-v1.json)
  set_tests_properties(dapp_dummy_ru_e2e.installed_release_provisioning_v1 PROPERTIES
    LABELS "dapp;integration;e2e;ru_dummy;security;signing;install"
    RUN_SERIAL TRUE
    TIMEOUT 180)
endif()

if(CUDA_ACCEL_FOUND AND OCUDU_DAPP_REFERENCE_CUDA_DIR)
  add_test(NAME dapp_dummy_ru_e2e.cuda_class_a_lifecycle_and_continuity
    COMMAND ${DAPP_E2E_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/dummy_ru_e2e_test.py
      --odu $<TARGET_FILE:odu>
      --client ${PROJECT_SOURCE_DIR}/apps/tools/dappctl/ocudu_dappctl.py
      --schema ${PROJECT_SOURCE_DIR}/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1
      --catalog ${OCUDU_DAPP_REFERENCE_CUDA_DIR}
      --preflight-helper $<TARGET_FILE:ocudu-dapp-preflight>
      --cuda-class-a)
  set_tests_properties(dapp_dummy_ru_e2e.cuda_class_a_lifecycle_and_continuity PROPERTIES
    LABELS "dapp;integration;e2e;ru_dummy;cuda"
    RUN_SERIAL TRUE
    TIMEOUT 90)

  add_test(NAME dapp_dummy_ru_e2e.cuda_loaded_l1_contention_and_recovery
    COMMAND ${DAPP_E2E_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/dummy_ru_e2e_test.py
      --odu $<TARGET_FILE:odu>
      --client ${PROJECT_SOURCE_DIR}/apps/tools/dappctl/ocudu_dappctl.py
      --schema ${PROJECT_SOURCE_DIR}/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1
      --catalog ${OCUDU_DAPP_REFERENCE_CUDA_DIR}
      --preflight-helper $<TARGET_FILE:ocudu-dapp-preflight>
      --cuda-class-a --cuda-class-c --cuda-contention --cuda-contention-cycles 3)
  set_tests_properties(dapp_dummy_ru_e2e.cuda_loaded_l1_contention_and_recovery PROPERTIES
    LABELS "dapp;integration;e2e;ru_dummy;cuda;contention"
    RUN_SERIAL TRUE
    TIMEOUT 150)

  add_test(NAME dapp_dummy_ru_e2e.multicell_cuda_bounded_endurance
    COMMAND ${DAPP_E2E_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/dummy_ru_e2e_test.py
      --odu $<TARGET_FILE:odu>
      --client ${PROJECT_SOURCE_DIR}/apps/tools/dappctl/ocudu_dappctl.py
      --schema ${PROJECT_SOURCE_DIR}/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1
      --catalog ${OCUDU_DAPP_REFERENCE_CUDA_DIR}
      --preflight-helper $<TARGET_FILE:ocudu-dapp-preflight>
      --cuda-class-a --cuda-class-c --cuda-contention --multicell-cuda --cuda-contention-cycles 3)
  set_tests_properties(dapp_dummy_ru_e2e.multicell_cuda_bounded_endurance PROPERTIES
    LABELS "dapp;integration;e2e;ru_dummy;cuda;contention;multicell;endurance"
    RUN_SERIAL TRUE
    TIMEOUT 240)
endif()
