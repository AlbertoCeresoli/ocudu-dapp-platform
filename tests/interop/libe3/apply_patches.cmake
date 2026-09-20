# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# ExternalProject patch step for upstream libe3: applies every patch in
# PATCHES to SOURCE_DIR, skipping patches that are already present (a local
# checkout that carries the change, or a repeated configure).

find_package(Git REQUIRED)
# SOURCE_DIR is a plain copy of the submodule inside the OCUDU build tree, which
# itself sits inside the OCUDU repository. Without a ceiling git would discover
# that enclosing repository, resolve the patch paths against its root, ignore
# every one of them as "outside the current directory", and report success
# having changed nothing. The ceiling makes git apply work as a plain patcher.
get_filename_component(_ceiling ${SOURCE_DIR} DIRECTORY)
set(_git ${CMAKE_COMMAND} -E env GIT_CEILING_DIRECTORIES=${_ceiling} ${GIT_EXECUTABLE})
string(REPLACE "|" ";" PATCHES "${PATCHES}")
foreach (patch ${PATCHES})
  execute_process(
    COMMAND ${_git} apply --check --reverse ${patch}
    WORKING_DIRECTORY ${SOURCE_DIR}
    RESULT_VARIABLE already_applied
    OUTPUT_QUIET ERROR_QUIET)
  if (already_applied EQUAL 0)
    message(STATUS "libe3: ${patch} already applied")
    continue()
  endif ()
  execute_process(
    COMMAND ${_git} apply --whitespace=nowarn ${patch}
    WORKING_DIRECTORY ${SOURCE_DIR}
    RESULT_VARIABLE result
    ERROR_VARIABLE error)
  if (NOT result EQUAL 0)
    message(FATAL_ERROR "libe3: cannot apply ${patch}: ${error}")
  endif ()
  message(STATUS "libe3: applied ${patch}")
endforeach ()
