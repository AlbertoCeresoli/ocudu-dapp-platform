# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

file(REMOVE_RECURSE "${CONSUMER_BUILD_DIR}" "${INSTALL_PREFIX}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${OCUDU_BUILD_DIR}" --prefix "${INSTALL_PREFIX}" --component dapp_sdk
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "OCUDU dApp SDK installation failed: ${result}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE_DIR}" -B "${CONSUMER_BUILD_DIR}"
          -DCMAKE_PREFIX_PATH=${INSTALL_PREFIX}
          -DCMAKE_C_FLAGS=${CONSUMER_C_FLAGS}
          -DCMAKE_CXX_FLAGS=${CONSUMER_CXX_FLAGS}
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "OCUDU dApp SDK consumer configuration failed: ${result}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${CONSUMER_BUILD_DIR}"
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "OCUDU dApp SDK consumer build failed: ${result}")
endif()
