# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

cmake_minimum_required(VERSION 3.14)
project(ocudu_dapp_sdk_consumer LANGUAGES C CXX)

find_package(OCUDUDAppSDK 1 CONFIG REQUIRED)

if(NOT OCUDUDAppSDK_ABI_MAJOR EQUAL 1)
  message(FATAL_ERROR "Unexpected OCUDU dApp ABI major: ${OCUDUDAppSDK_ABI_MAJOR}")
endif()
if(NOT EXISTS "${OCUDUDAppSDK_CLASS_C_SCHEMA}")
  message(FATAL_ERROR "Installed Class-C schema is missing: ${OCUDUDAppSDK_CLASS_C_SCHEMA}")
endif()

add_library(test_dapp MODULE test_dapp.c)
target_link_libraries(test_dapp PRIVATE OCUDU::DAppSDK)

if(TARGET OCUDU::DAppManagementClient)
  if(NOT EXISTS "${OCUDUDAppSDK_E3_MANAGEMENT_SCHEMA}")
    message(FATAL_ERROR "Installed E3 management schema is missing: ${OCUDUDAppSDK_E3_MANAGEMENT_SCHEMA}")
  endif()
  add_executable(test_management_client management_client.cpp)
  target_compile_features(test_management_client PRIVATE cxx_std_17)
  target_link_libraries(test_management_client PRIVATE OCUDU::DAppManagementClient)
endif()
