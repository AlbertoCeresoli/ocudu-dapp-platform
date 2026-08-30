# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

add_executable(ocudu-dappctl main.cpp)
target_link_libraries(ocudu-dappctl PRIVATE ocudu_dapp_e3_client)
install(TARGETS ocudu-dappctl RUNTIME)
install(PROGRAMS ocudu_dappctl.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME ocudu-dappctl.py)
notify_binary_target(ocudu-dappctl)
