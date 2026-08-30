# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

include(GNUInstallDirs)
find_package(MbedTLS REQUIRED)

add_executable(ocudu-dapp-package main.cpp)
target_include_directories(ocudu-dapp-package PRIVATE ${MBEDTLS_INCLUDE_DIRS})
target_link_libraries(ocudu-dapp-package PRIVATE ocudu_dapp_runtime ${MBEDTLS_LIBRARIES})
install(TARGETS ocudu-dapp-package
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT dapp_sdk)
notify_binary_target(ocudu-dapp-package)
