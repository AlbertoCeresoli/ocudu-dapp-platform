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

# The source is the third_party/libe3 submodule (github.com/wineslab/libe3,
# pinned; the vendored grammar in schemas/e3/interop/libe3 is checked against
# it at configure time). It is copied into the build tree before the patches
# are applied so the submodule stays clean. OCUDU_LIBE3_SOURCE_DIR points at
# another checkout instead; the git fetch is a fallback for trees without the
# submodule.
set(OCUDU_LIBE3_GIT_REPOSITORY "https://github.com/wineslab/libe3.git" CACHE STRING "Upstream libe3 repository (fallback when the submodule is absent)")
set(OCUDU_LIBE3_GIT_TAG "6f44b6f" CACHE STRING "Upstream libe3 commit (fallback when the submodule is absent)")
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
    -DLIBE3_ENABLE_PROTOBUF=OFF
    -DLIBE3_ENABLE_ZMQ=OFF
    -DLIBE3_ENABLE_SCTP=ON
    -DLIBE3_BUILD_TESTS=OFF
    -DLIBE3_BUILD_EXAMPLES=OFF
    -DLIBE3_BUILD_INTEGRATION_TESTS=OFF
  BUILD_BYPRODUCTS ${OCUDU_LIBE3_LIBRARY} ${OCUDU_LIBE3_ASN1_LIBRARY})

add_library(libe3_upstream_asn1 STATIC IMPORTED)
set_target_properties(libe3_upstream_asn1 PROPERTIES IMPORTED_LOCATION ${OCUDU_LIBE3_ASN1_LIBRARY})
add_library(libe3_upstream_lib STATIC IMPORTED)
set_target_properties(libe3_upstream_lib PROPERTIES
  IMPORTED_LOCATION ${OCUDU_LIBE3_LIBRARY}
  INTERFACE_INCLUDE_DIRECTORIES ${OCUDU_LIBE3_INSTALL_DIR}/include
  INTERFACE_COMPILE_DEFINITIONS "LIBE3_ENABLE_ASN1;LIBE3_ENABLE_SCTP;LIBE3_HAS_ZMQ=0"
  INTERFACE_LINK_LIBRARIES "libe3_upstream_asn1;Threads::Threads")
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
  add_test(NAME dapp_e3ap_libe3.e2e
    COMMAND ${DAPP_TEST_PYTHON_EXECUTABLE} ${PROJECT_SOURCE_DIR}/python/tests/test_libe3_e2e.py
      --server $<TARGET_FILE:e3ap_test_server>
      --dapp $<TARGET_FILE:libe3_interop_dapp>
      --vectors $<TARGET_FILE:libe3_interop_vectors>)
  set_tests_properties(dapp_e3ap_libe3.e2e PROPERTIES
    ENVIRONMENT "OCUDU_E3_SCHEMA_DIR=${PROJECT_SOURCE_DIR}/schemas/e3"
    TIMEOUT 300
    LABELS "dapp;e3;python;interop;libe3")
endif ()
