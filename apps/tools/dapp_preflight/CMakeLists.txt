# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

include(GNUInstallDirs)

add_executable(ocudu-dapp-preflight main.cpp)
target_link_libraries(ocudu-dapp-preflight PRIVATE ocudu_dapp_runtime)
add_custom_command(TARGET ocudu-dapp-preflight POST_BUILD
  COMMAND /bin/chmod 0555 $<TARGET_FILE:ocudu-dapp-preflight>)
install(TARGETS ocudu-dapp-preflight RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT dapp_sdk)
notify_binary_target(ocudu-dapp-preflight)
