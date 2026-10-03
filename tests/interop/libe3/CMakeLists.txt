# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# End-to-end OCUDU <-> libe3 interoperability. Builds the upstream libe3
# library (Northeastern University, Apache-2.0) at a pinned commit, plus the
# patches in ./patches, as an external project, then builds two small
# binaries that link libe3 only (never an OCUDU codec: both carry an asn1c
# runtime and an E3-PDU family, and must not share a process):
#
#   libe3_interop_dapp     a libe3 dApp over E3LinkLayer::SCTP_ASSOCIATION
#                          that drives a full session against e3ap_test_server
#   libe3_interop_vectors  libe3's own encoder/decoder as a command-line tool
#
# and registers the Python driver (python/tests/test_libe3_e2e.py) with CTest.
#
# Off by default: it needs the third_party/libe3 submodule (or network access
# for the fallback fetch), the SCTP headers, and a few minutes of build time.

if (NOT OCUDU_DAPP_LIBE3_INTEROP)
  return()
endif ()
if (NOT (ENABLE_E3 AND ENABLE_E3_ASN1_CODEC))
  message(FATAL_ERROR "OCUDU_DAPP_LIBE3_INTEROP requires ENABLE_E3 and ENABLE_E3_ASN1_CODEC")
endif ()
if (NOT DAPP_TEST_PYTHON_EXECUTABLE OR NOT DAPP_TEST_ASN1TOOLS_RESULT EQUAL 0)
  message(FATAL_ERROR "OCUDU_DAPP_LIBE3_INTEROP requires Python and asn1tools; interoperability must not silently skip")
endif ()

include(ExternalProject)

# The ZMQ interop leg runs the stock (unpatched) libe3 ZMQ connector against OCUDU's ZMQ carrier, so it
# needs libzmq; without it only the SCTP leg is built.
if (ZEROMQ_FOUND)
  set(OCUDU_LIBE3_ZMQ ON)
  set(OCUDU_LIBE3_HAS_ZMQ 1)
else ()
  set(OCUDU_LIBE3_ZMQ OFF)
  set(OCUDU_LIBE3_HAS_ZMQ 0)
  message(WARNING "libe3 interop: ZeroMQ not found; the ZMQ interop leg is not built")
endif ()

# The Protobuf legs run libe3 built with LIBE3_ENABLE_PROTOBUF (libprotobuf + protoc) against the agent's
# hand-written Protobuf codec; without Protobuf only the APER legs are built.
find_package(Protobuf QUIET)
if (Protobuf_FOUND AND Protobuf_PROTOC_EXECUTABLE)
  set(OCUDU_LIBE3_PROTOBUF ON)
  set(OCUDU_LIBE3_HAS_PROTOBUF 1)
else ()
  set(OCUDU_LIBE3_PROTOBUF OFF)
  set(OCUDU_LIBE3_HAS_PROTOBUF 0)
  message(WARNING "libe3 interop: Protobuf (libprotobuf-dev, protobuf-compiler) not found; the Protobuf interop legs are not built")
endif ()

# The source is the third_party/libe3 submodule (github.com/wineslab/libe3,
# pinned; the vendored grammar in schemas/e3/interop/libe3 is checked against
# it at configure time). It is copied into the build tree before the patches
# are applied so the submodule stays clean. OCUDU_LIBE3_SOURCE_DIR points at
# another checkout instead; the git fetch is a fallback for trees without the
# submodule.
set(OCUDU_LIBE3_GIT_REPOSITORY "https://github.com/wineslab/libe3.git" CACHE STRING "Upstream libe3 repository (fallback when the submodule is absent)")
set(OCUDU_LIBE3_GIT_TAG "0.2.1" CACHE STRING "Upstream libe3 commit (fallback when the submodule is absent)")
set(OCUDU_LIBE3_SOURCE_DIR "" CACHE PATH "libe3 checkout to build instead of the submodule")
set(OCUDU_LIBE3_SUBMODULE_DIR ${PROJECT_SOURCE_DIR}/third_party/libe3)

file(GLOB OCUDU_LIBE3_PATCHES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch)
list(SORT OCUDU_LIBE3_PATCHES)
# A CMake list would be split into separate arguments by the patch step; the
# script splits on '|' instead.
string(REPLACE ";" "|" OCUDU_LIBE3_PATCH_ARGUMENT "${OCUDU_LIBE3_PATCHES}")
set(OCUDU_LIBE3_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/libe3-prefix)
set(OCUDU_LIBE3_INSTALL_DIR ${OCUDU_LIBE3_PREFIX}/install)
set(OCUDU_LIBE3_LIBRARY ${OCUDU_LIBE3_INSTALL_DIR}/lib/liblibe3.a)
set(OCUDU_LIBE3_ASN1_LIBRARY ${OCUDU_LIBE3_INSTALL_DIR}/lib/libasn1_e3ap.a)
set(OCUDU_LIBE3_PB_LIBRARY ${OCUDU_LIBE3_INSTALL_DIR}/lib/libpb_e3ap.a)
set(OCUDU_LIBE3_BYPRODUCTS ${OCUDU_LIBE3_LIBRARY} ${OCUDU_LIBE3_ASN1_LIBRARY})
if (OCUDU_LIBE3_PROTOBUF)
  list(APPEND OCUDU_LIBE3_BYPRODUCTS ${OCUDU_LIBE3_PB_LIBRARY})
endif ()

set(OCUDU_LIBE3_SOURCE_ARGS)
if (OCUDU_LIBE3_SOURCE_DIR)
  set(OCUDU_LIBE3_SOURCE_ARGS SOURCE_DIR ${OCUDU_LIBE3_SOURCE_DIR})
elseif (EXISTS ${OCUDU_LIBE3_SUBMODULE_DIR}/CMakeLists.txt)
  set(OCUDU_LIBE3_SOURCE_ARGS
    # The submodule's .git is a gitfile with a path relative to the checkout;
    # it must not travel with the copy, and the patches apply to plain files.
    DOWNLOAD_COMMAND ${CMAKE_COMMAND} -E rm -rf <SOURCE_DIR>
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${OCUDU_LIBE3_SUBMODULE_DIR} <SOURCE_DIR>
            COMMAND ${CMAKE_COMMAND} -E rm -f <SOURCE_DIR>/.git)
else ()
  message(STATUS "libe3 submodule not checked out; fetching ${OCUDU_LIBE3_GIT_TAG} from ${OCUDU_LIBE3_GIT_REPOSITORY}")
  set(OCUDU_LIBE3_SOURCE_ARGS
    GIT_REPOSITORY ${OCUDU_LIBE3_GIT_REPOSITORY}
    GIT_TAG ${OCUDU_LIBE3_GIT_TAG}
    GIT_SHALLOW OFF
    UPDATE_DISCONNECTED ON)
endif ()

ExternalProject_Add(libe3_upstream
  PREFIX ${OCUDU_LIBE3_PREFIX}
  ${OCUDU_LIBE3_SOURCE_ARGS}
  PATCH_COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=<SOURCE_DIR>
    "-DPATCHES=${OCUDU_LIBE3_PATCH_ARGUMENT}"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/apply_patches.cmake
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=${OCUDU_LIBE3_INSTALL_DIR}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DASN1C_EXEC=${ASN1C_EXECUTABLE}
    -DLIBE3_ENABLE_ASN1=ON
    -DLIBE3_ENABLE_JSON=OFF
    -DLIBE3_ENABLE_PROTOBUF=${OCUDU_LIBE3_PROTOBUF}
    -DProtobuf_PROTOC_EXECUTABLE=${Protobuf_PROTOC_EXECUTABLE}
    -DLIBE3_ENABLE_ZMQ=${OCUDU_LIBE3_ZMQ}
    -DLIBE3_ENABLE_SCTP=ON
    -DLIBE3_BUILD_TESTS=OFF
    -DLIBE3_BUILD_EXAMPLES=OFF
    -DLIBE3_BUILD_INTEGRATION_TESTS=OFF
  BUILD_BYPRODUCTS ${OCUDU_LIBE3_BYPRODUCTS})

add_library(libe3_upstream_asn1 STATIC IMPORTED)
set_target_properties(libe3_upstream_asn1 PROPERTIES IMPORTED_LOCATION ${OCUDU_LIBE3_ASN1_LIBRARY})
set(OCUDU_LIBE3_INTERFACE_LIBRARIES libe3_upstream_asn1)
set(OCUDU_LIBE3_DEFINITIONS LIBE3_ENABLE_ASN1 LIBE3_ENABLE_SCTP LIBE3_HAS_ZMQ=${OCUDU_LIBE3_HAS_ZMQ}
    LIBE3_HAS_PROTOBUF=${OCUDU_LIBE3_HAS_PROTOBUF})
if (OCUDU_LIBE3_PROTOBUF)
  add_library(libe3_upstream_pb STATIC IMPORTED)
  set_target_properties(libe3_upstream_pb PROPERTIES IMPORTED_LOCATION ${OCUDU_LIBE3_PB_LIBRARY})
  list(APPEND OCUDU_LIBE3_INTERFACE_LIBRARIES libe3_upstream_pb protobuf::libprotobuf)
  list(APPEND OCUDU_LIBE3_DEFINITIONS LIBE3_ENABLE_PROTOBUF)
endif ()
add_library(libe3_upstream_lib STATIC IMPORTED)
set_target_properties(libe3_upstream_lib PROPERTIES
  IMPORTED_LOCATION ${OCUDU_LIBE3_LIBRARY}
  INTERFACE_INCLUDE_DIRECTORIES ${OCUDU_LIBE3_INSTALL_DIR}/include
  INTERFACE_COMPILE_DEFINITIONS "${OCUDU_LIBE3_DEFINITIONS}"
  INTERFACE_LINK_LIBRARIES "${OCUDU_LIBE3_INTERFACE_LIBRARIES};Threads::Threads;$<$<BOOL:${OCUDU_LIBE3_ZMQ}>:${ZEROMQ_LIBRARIES}>")
add_dependencies(libe3_upstream_lib libe3_upstream)
find_package(Threads REQUIRED)
file(MAKE_DIRECTORY ${OCUDU_LIBE3_INSTALL_DIR}/include)

foreach (tool libe3_interop_dapp libe3_interop_vectors)
  add_executable(${tool} ${tool}.cpp)
  target_link_libraries(${tool} PRIVATE libe3_upstream_lib)
  # Third-party headers: keep the project's warning set off them.
  target_compile_options(${tool} PRIVATE -w)
  add_dependencies(${tool} libe3_upstream)
endforeach ()

if (DAPP_TEST_PYTHON_EXECUTABLE AND TARGET e3ap_test_server)
  # One end-to-end suite per link layer, so CI can run the ZMQ leg on runners without kernel SCTP:
  #  - e2e_zmq:  libe3's own (unpatched) ZMQ connector over ipc:// and tcp:// against the agent's ZMQ
  #              carrier, plus libe3's encoder vectors (APER and Protobuf). Needs only libzmq.
  #  - e2e_sctp: libe3's SCTP association link layer (our patch) against the agent's SCTP carrier.
  #              Exits 77 (CTest: skipped) when the kernel cannot open SCTP sockets.
  # Each link layer runs once per encoding libe3 was built with: APER always, Protobuf with libprotobuf.
  if (OCUDU_LIBE3_PROTOBUF)
    set(OCUDU_LIBE3_ENCODINGS asn1,protobuf)
  else ()
    set(OCUDU_LIBE3_ENCODINGS asn1)
  endif ()
  set(OCUDU_LIBE3_E2E_COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/python/tests/test_libe3_e2e.py
      --server $<TARGET_FILE:e3ap_test_server> --dapp $<TARGET_FILE:libe3_interop_dapp>
      --encodings ${OCUDU_LIBE3_ENCODINGS})
  if (OCUDU_LIBE3_ZMQ)
    add_test(NAME dapp_e3ap_libe3.e2e_zmq
      COMMAND ${OCUDU_LIBE3_E2E_COMMAND} --vectors $<TARGET_FILE:libe3_interop_vectors> --transports zmq-ipc,zmq-tcp)
    set_tests_properties(dapp_e3ap_libe3.e2e_zmq PROPERTIES
      ENVIRONMENT "OCUDU_E3_SCHEMA_DIR=${PROJECT_SOURCE_DIR}/schemas/e3"
      TIMEOUT 300
      LABELS "dapp;e3;python;interop;libe3;zmq")
    add_test(NAME dapp_e3ap_libe3.e2e_sctp COMMAND ${OCUDU_LIBE3_E2E_COMMAND} --transports sctp)
  else ()
    add_test(NAME dapp_e3ap_libe3.e2e_sctp
      COMMAND ${OCUDU_LIBE3_E2E_COMMAND} --vectors $<TARGET_FILE:libe3_interop_vectors> --transports sctp)
  endif ()
  set_tests_properties(dapp_e3ap_libe3.e2e_sctp PROPERTIES
    ENVIRONMENT "OCUDU_E3_SCHEMA_DIR=${PROJECT_SOURCE_DIR}/schemas/e3"
    TIMEOUT 300
    SKIP_RETURN_CODE 77
    LABELS "dapp;e3;python;interop;libe3;sctp")
  # Runs the fail-closed interop gate and renders its report (Markdown, and PDF when pdflatex is
  # present) into <build>/dapp-interop-report; see scripts/libe3_interop_report.py.
  add_custom_target(libe3_interop_report
    COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/libe3_interop_report.py --build ${PROJECT_BINARY_DIR}
    USES_TERMINAL
    COMMENT "libe3 interoperability gate + report")
  add_dependencies(libe3_interop_report libe3_interop_dapp libe3_interop_vectors e3ap_test_server
                   dapp_runtime_test dapp_telemetry_allocation_test)
  if (TARGET dapp_e3ap_protobuf_conformance_test)
    add_dependencies(libe3_interop_report dapp_e3ap_protobuf_conformance_test)
  endif ()
endif ()
