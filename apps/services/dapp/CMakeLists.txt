# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

add_library(ocudu_dapp_app_service STATIC
        dapp_process_package_factory.cpp
        dapp_service.cpp)
target_include_directories(ocudu_dapp_app_service PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ocudu_dapp_app_service PUBLIC
        ocudu_dapp_runtime
        ocudu_phy_dapp_adapters
        ocudu_sched)

add_library(ocudu_dapp_app_config STATIC
        dapp_config_cli11_schema.cpp
        dapp_config_validator.cpp
        dapp_config_yaml_writer.cpp)
target_include_directories(ocudu_dapp_app_config PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ocudu_dapp_app_config PRIVATE ocudu::yaml-cpp)

if (ENABLE_E3)
  target_compile_definitions(ocudu_dapp_app_service PRIVATE OCUDU_DAPP_APP_SERVICE_E3=1)
endif ()

if (ENABLE_E3 AND ENABLE_E3_ASN1_CODEC)
  target_compile_definitions(ocudu_dapp_app_service PRIVATE
    OCUDU_DAPP_APP_SERVICE_LOCAL_E3=1
    OCUDU_DAPP_APP_SERVICE_SCTP_E3=1)
  target_compile_definitions(ocudu_dapp_app_config PRIVATE
    OCUDU_DAPP_APP_CONFIG_LOCAL_E3=1
    OCUDU_DAPP_APP_CONFIG_SCTP_E3=1)
  target_link_libraries(ocudu_dapp_app_service PRIVATE ocudu_dapp_e3_local ocudu_dapp_e3_sctp)
  if (OCUDU_DAPP_E3DP_CARRIER)
    target_compile_definitions(ocudu_dapp_app_config PRIVATE OCUDU_DAPP_APP_CONFIG_E3DP=1)
  endif ()
endif ()
