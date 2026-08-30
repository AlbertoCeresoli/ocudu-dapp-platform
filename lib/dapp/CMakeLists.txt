# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

include(GNUInstallDirs)

add_library(ocudu_dapp_sdk INTERFACE)
add_library(OCUDU::DAppSDK ALIAS ocudu_dapp_sdk)
set_target_properties(ocudu_dapp_sdk PROPERTIES EXPORT_NAME DAppSDK)
target_include_directories(ocudu_dapp_sdk INTERFACE
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_library(ocudu_dapp_management STATIC management/types.cpp)
add_library(OCUDU::DAppManagement ALIAS ocudu_dapp_management)
set_target_properties(ocudu_dapp_management PROPERTIES
  EXPORT_NAME DAppManagement
  # Required so static archives can be linked into shared modules (pybind, etc.).
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(ocudu_dapp_management PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_library(ocudu_dapp_runtime STATIC
  ipc/shared_spsc_ring.cpp
  ipc/seqpacket_channel.cpp
  ipc/publication_route.cpp
  runtime/artifact_verifier.cpp
  runtime/abi_validation.cpp
  runtime/circuit_breaker.cpp
  runtime/class_c_process_instance.cpp
  runtime/class_c_process_supervisor.cpp
  runtime/class_c_process_supervisor_support.cpp
  runtime/incident.cpp
  runtime/incident_store.cpp
  runtime/instance_manager.cpp
  runtime/instance_manager_lifecycle.cpp
  runtime/package_catalog.cpp
  runtime/job_dispatcher.cpp
  runtime/lifecycle.cpp
  runtime/module_loader.cpp
  runtime/native_hook_point.cpp
  runtime/native_hook_registry.cpp
  runtime/native_instance.cpp
  runtime/native_preflight.cpp
  runtime/native_hook_slot.cpp
  runtime/native_worker.cpp
  runtime/native_worker_validation.cpp
  runtime/package_manifest.cpp
  runtime/package_provenance.cpp
  runtime/package_signature.cpp
  runtime/prepared_output.cpp
  runtime/quiescence_tracker.cpp)

target_include_directories(ocudu_dapp_runtime PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(ocudu_dapp_runtime PUBLIC ocudu_dapp_management ${CMAKE_DL_LIBS})

if (NOT DISABLE_MBEDTLS)
  find_package(MbedTLS REQUIRED)
  target_compile_definitions(ocudu_dapp_runtime PRIVATE OCUDU_DAPP_PACKAGE_SIGNATURE_CRYPTO=1)
  target_include_directories(ocudu_dapp_runtime PRIVATE ${MBEDTLS_INCLUDE_DIRS})
  target_link_libraries(ocudu_dapp_runtime PRIVATE ${MBEDTLS_LIBRARIES})
endif ()

if (ENABLE_E3)
  target_sources(ocudu_dapp_runtime PRIVATE
    e3/embedded_agent.cpp
    e3/management_service.cpp
    e3/portable_data_store.cpp)
  set(E3_SCHEMA ${PROJECT_SOURCE_DIR}/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1)
  # Additional modules carried over the SCTP E3AP endpoint. Every module is
  # compiled into the same asn1c library so the codecs share one runtime.
  set(E3_SCHEMAS
    ${E3_SCHEMA}
    ${PROJECT_SOURCE_DIR}/schemas/e3/e3ap-ocudu-v1.asn1
    ${PROJECT_SOURCE_DIR}/schemas/e3/e3sm-ocudu-telemetry-v1.asn1
    ${PROJECT_SOURCE_DIR}/schemas/e3/e3sm-ocudu-dapp-mgmt-carrier-v1.asn1)
  find_program(ASN1TOOLS_EXECUTABLE asn1tools)
  if (ASN1TOOLS_EXECUTABLE)
    set(E3_SCHEMA_IR ${CMAKE_CURRENT_BINARY_DIR}/e3-schemas.json)
    add_custom_command(
      OUTPUT ${E3_SCHEMA_IR}
      COMMAND ${ASN1TOOLS_EXECUTABLE} parse ${E3_SCHEMAS} ${E3_SCHEMA_IR}
      DEPENDS ${E3_SCHEMAS}
      COMMENT "Validating OCUDU E3 ASN.1 modules")
    add_custom_target(ocudu_e3_schema_check DEPENDS ${E3_SCHEMA_IR})
    add_dependencies(ocudu_dapp_runtime ocudu_e3_schema_check)
  else ()
    message(WARNING "asn1tools not found; OCUDU E3 ASN.1 validation is unavailable")
  endif ()

  if (ENABLE_E3_ASN1_CODEC)
    find_program(ASN1C_EXECUTABLE asn1c HINTS /opt/asn1c/bin)
    if (NOT ASN1C_EXECUTABLE)
      message(FATAL_ERROR "ENABLE_E3_ASN1_CODEC requires an asn1c build with -gen-APER support")
    endif ()

    set(E3_ASN1_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/e3sm_asn1)
    math(EXPR E3_ASN1_LONG_BITS "${CMAKE_SIZEOF_VOID_P} * 8")
    if (NOT E3_ASN1_LONG_BITS EQUAL 32 AND NOT E3_ASN1_LONG_BITS EQUAL 64)
      message(FATAL_ERROR "Unsupported target long width for E3 ASN.1 generation")
    endif ()
    file(MAKE_DIRECTORY ${E3_ASN1_GENERATED_DIR})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${E3_SCHEMAS})
    file(GLOB E3_ASN1_STALE ${E3_ASN1_GENERATED_DIR}/*.c ${E3_ASN1_GENERATED_DIR}/*.h)
    if (E3_ASN1_STALE)
      file(REMOVE ${E3_ASN1_STALE})
    endif ()
    execute_process(
      COMMAND ${ASN1C_EXECUTABLE}
        -pdu=ManagementRequest -pdu=ManagementResponse
        -pdu=E3-PDU -pdu=Telemetry-IndicationData -pdu=Telemetry-RanFunctionData
        -pdu=Telemetry-SubscriptionData -pdu=Mgmt-RanFunctionData
        -gen-APER -no-gen-UPER -no-gen-JER -no-gen-BER -no-gen-XER
        -no-gen-CBOR -no-gen-OER -no-gen-print -no-gen-random-fill
        -fgen-only-pdu-deps -fno-include-deps -fcompound-names
        -findirect-choice -flong-size=${E3_ASN1_LONG_BITS} -no-gen-example
        -D ${E3_ASN1_GENERATED_DIR} ${E3_SCHEMAS}
      RESULT_VARIABLE E3_ASN1C_RESULT
      OUTPUT_QUIET ERROR_VARIABLE E3_ASN1C_ERROR)
    if (NOT E3_ASN1C_RESULT EQUAL 0)
      message(FATAL_ERROR "asn1c failed to generate the E3 APER codec: ${E3_ASN1C_ERROR}")
    endif ()

    file(GLOB E3_ASN1_SOURCES CONFIGURE_DEPENDS ${E3_ASN1_GENERATED_DIR}/*.c)
    add_library(ocudu_dapp_e3sm_asn1 STATIC ${E3_ASN1_SOURCES})
    set_target_properties(ocudu_dapp_e3sm_asn1 PROPERTIES
      EXPORT_NAME DAppE3Asn1
      POSITION_INDEPENDENT_CODE ON)
    target_include_directories(ocudu_dapp_e3sm_asn1 SYSTEM PRIVATE ${E3_ASN1_GENERATED_DIR})
    target_compile_definitions(ocudu_dapp_e3sm_asn1 PRIVATE
      ASN_DISABLE_BER_SUPPORT ASN_DISABLE_XER_SUPPORT ASN_DISABLE_OER_SUPPORT
      ASN_DISABLE_UPER_SUPPORT ASN_DISABLE_PRINT_SUPPORT ASN_DISABLE_RFILL_SUPPORT
      ASN_DISABLE_JER_SUPPORT ASN_DISABLE_CBOR_SUPPORT)
    target_compile_options(ocudu_dapp_e3sm_asn1 PRIVATE -w)
    target_link_libraries(ocudu_dapp_e3sm_asn1 PUBLIC m)

    add_library(ocudu_dapp_e3_codec STATIC e3/aper_codec.cpp)
    set_target_properties(ocudu_dapp_e3_codec PROPERTIES
      EXPORT_NAME DAppE3Codec
      POSITION_INDEPENDENT_CODE ON)
    target_include_directories(ocudu_dapp_e3_codec PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_include_directories(ocudu_dapp_e3_codec SYSTEM PRIVATE ${E3_ASN1_GENERATED_DIR})
    target_link_libraries(ocudu_dapp_e3_codec PUBLIC ocudu_dapp_e3sm_asn1 ocudu_dapp_management)

    # Management client used by out-of-tree consumers (ref_*_e3_cpu, Python pybind).
    # PUBLIC link of codec so a shared MODULE that links only the client still
    # pulls the full APER stack; PIC so that MODULE is linkable.
    add_library(ocudu_dapp_e3_client STATIC e3/local_client.cpp)
    add_library(OCUDU::DAppManagementClient ALIAS ocudu_dapp_e3_client)
    set_target_properties(ocudu_dapp_e3_client PROPERTIES
      EXPORT_NAME DAppManagementClient
      POSITION_INDEPENDENT_CODE ON)
    target_include_directories(ocudu_dapp_e3_client PUBLIC
      $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(ocudu_dapp_e3_client PUBLIC ocudu_dapp_e3_codec)

    add_library(ocudu_dapp_e3_local STATIC e3/local_connector.cpp)
    target_include_directories(ocudu_dapp_e3_local PUBLIC ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(ocudu_dapp_e3_local PUBLIC ocudu_dapp_e3_codec PRIVATE ocudu_dapp_runtime fmt)

    # E3AP-OCUDU-V1 codec: envelope plus the telemetry and management-carrier
    # service models. Independent clients may link it without the runtime.
    add_library(ocudu_dapp_e3ap_codec STATIC e3/e3ap_codec.cpp e3/e3ap_protocol.cpp)
    set_target_properties(ocudu_dapp_e3ap_codec PROPERTIES
      EXPORT_NAME DAppE3apCodec
      POSITION_INDEPENDENT_CODE ON)
    target_include_directories(ocudu_dapp_e3ap_codec PUBLIC
      $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_include_directories(ocudu_dapp_e3ap_codec SYSTEM PRIVATE ${E3_ASN1_GENERATED_DIR})
    target_link_libraries(ocudu_dapp_e3ap_codec PUBLIC ocudu_dapp_e3sm_asn1 ocudu_dapp_management)

    # SCTP carriers: shared association core, E3AP endpoint, and the optional
    # FlatBuffers data-profile endpoint compatible with the E3 SCTP reference
    # client. The telemetry publisher bridges upper-PHY diagnostics into the
    # portable data store; it depends only on headers from the PHY tree.
    add_library(ocudu_dapp_e3_sctp STATIC
      e3/sctp_association_server.cpp
      e3/e3ap_sctp_server.cpp
      e3/telemetry_publisher.cpp)
    target_include_directories(ocudu_dapp_e3_sctp PUBLIC ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(ocudu_dapp_e3_sctp PUBLIC ocudu_dapp_e3ap_codec ocudu_dapp_e3_codec PRIVATE ocudu_dapp_runtime)

  endif ()
endif ()

find_program(FLATC_EXECUTABLE flatc)
find_path(FLATBUFFERS_INCLUDE_DIR flatbuffers/verifier.h)
if (FLATC_EXECUTABLE AND FLATBUFFERS_INCLUDE_DIR)
  set(CLASS_C_SCHEMA ${PROJECT_SOURCE_DIR}/schemas/dapp/class-c-publication-v1.fbs)
  set(CLASS_C_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
  set(CLASS_C_GENERATED_HEADER ${CLASS_C_GENERATED_DIR}/class-c-publication-v1_generated.h)
  add_custom_command(
    OUTPUT ${CLASS_C_GENERATED_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CLASS_C_GENERATED_DIR}
    COMMAND ${FLATC_EXECUTABLE} --cpp --scoped-enums -o ${CLASS_C_GENERATED_DIR} ${CLASS_C_SCHEMA}
    DEPENDS ${CLASS_C_SCHEMA}
    COMMENT "Validating OCUDU Class-C publication FlatBuffers schema")
  add_custom_target(ocudu_dapp_class_c_schema DEPENDS ${CLASS_C_GENERATED_HEADER})
  add_dependencies(ocudu_dapp_runtime ocudu_dapp_class_c_schema)
  target_sources(ocudu_dapp_runtime PRIVATE
    ipc/class_c_metadata.cpp
    ipc/class_c_publication_encoder.cpp)
  target_include_directories(ocudu_dapp_runtime PRIVATE ${CLASS_C_GENERATED_DIR} ${FLATBUFFERS_INCLUDE_DIR})
else ()
  target_sources(ocudu_dapp_runtime PRIVATE ipc/class_c_publication_encoder_unavailable.cpp)
  message(WARNING "flatc or FlatBuffers headers not found; OCUDU Class-C metadata validation is unavailable")
endif ()

# Optional FlatBuffers data-profile SCTP carrier ("E3DP"), compatible with the
# standalone E3 SCTP reference client. Requires the asn1c E3 codecs and flatc.
if (ENABLE_E3 AND ENABLE_E3_ASN1_CODEC AND OCUDU_DAPP_CLASS_C_METADATA)
  set(E3DP_SCHEMA ${PROJECT_SOURCE_DIR}/schemas/dapp/e3-data-profile-v1.fbs)
  set(E3DP_GENERATED_HEADER ${CLASS_C_GENERATED_DIR}/e3-data-profile-v1_generated.h)
  add_custom_command(
    OUTPUT ${E3DP_GENERATED_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CLASS_C_GENERATED_DIR}
    COMMAND ${FLATC_EXECUTABLE} --cpp --scoped-enums -o ${CLASS_C_GENERATED_DIR} ${E3DP_SCHEMA}
    DEPENDS ${E3DP_SCHEMA}
    COMMENT "Generating OCUDU E3 data-profile FlatBuffers bindings")
  add_custom_target(ocudu_dapp_e3dp_schema DEPENDS ${E3DP_GENERATED_HEADER})
  add_dependencies(ocudu_dapp_e3_sctp ocudu_dapp_e3dp_schema)
  target_sources(ocudu_dapp_e3_sctp PRIVATE e3/fbs_codec.cpp e3/e3dp_sctp_server.cpp)
  target_include_directories(ocudu_dapp_e3_sctp PRIVATE ${CLASS_C_GENERATED_DIR} ${FLATBUFFERS_INCLUDE_DIR})
  target_compile_definitions(ocudu_dapp_e3_sctp PUBLIC OCUDU_DAPP_E3DP_CARRIER=1)
endif ()

if (ENABLE_EXPORT)
  add_dependencies(ocudu_exported_libs ocudu_dapp_runtime)
endif ()
