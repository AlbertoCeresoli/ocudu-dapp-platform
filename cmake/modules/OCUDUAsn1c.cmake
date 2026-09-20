# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Provides ASN1C_EXECUTABLE, the asn1c compiler the E3 APER codecs are generated
# with. The reference generator is the third_party/asn1c submodule
# (github.com/mouse07410/asn1c, pinned), built once at configure time into the
# build tree so that every checkout generates the same code; the codecs are
# generated at configure time too, which is why this cannot be a build step.
#
#   ASN1C_EXECUTABLE=<path>   use that compiler instead (CI images, experiments)
#   OCUDU_ASN1C_FROM_SUBMODULE=OFF
#                             skip the submodule build and search PATH and
#                             /opt/asn1c/bin as before
#
# Rebuilds are keyed on the submodule commit: bumping the submodule rebuilds
# the compiler and regenerates the codecs on the next configure.

option(OCUDU_ASN1C_FROM_SUBMODULE "Build asn1c from third_party/asn1c at configure time" ON)

function(_ocudu_asn1c_from_submodule out_var)
  set(source_dir ${PROJECT_SOURCE_DIR}/third_party/asn1c)
  if (NOT EXISTS ${source_dir}/configure.ac)
    message(STATUS "asn1c submodule not checked out (run: git submodule update --init third_party/asn1c)")
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif ()
  find_package(Git QUIET)
  set(commit "unknown")
  if (GIT_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} -C ${source_dir} rev-parse HEAD
      OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  endif ()
  set(work_dir ${PROJECT_BINARY_DIR}/third_party/asn1c-build)
  set(install_dir ${PROJECT_BINARY_DIR}/third_party/asn1c)
  set(stamp ${install_dir}/.ocudu-asn1c-commit)
  set(executable ${install_dir}/bin/asn1c)
  if (EXISTS ${executable} AND EXISTS ${stamp})
    file(READ ${stamp} built_commit)
    string(STRIP "${built_commit}" built_commit)
    if (built_commit STREQUAL commit)
      set(${out_var} ${executable} PARENT_SCOPE)
      return()
    endif ()
  endif ()
  message(STATUS "Building asn1c ${commit} from third_party/asn1c (once per submodule commit)")
  find_program(AUTORECONF_EXECUTABLE autoreconf)
  find_program(LIBTOOLIZE_EXECUTABLE libtoolize)
  find_program(BISON_EXECUTABLE bison)
  find_program(FLEX_EXECUTABLE flex)
  if (NOT AUTORECONF_EXECUTABLE OR NOT LIBTOOLIZE_EXECUTABLE OR NOT BISON_EXECUTABLE OR NOT FLEX_EXECUTABLE)
    message(FATAL_ERROR "Building asn1c needs autoconf, automake, libtool, bison and flex "
      "(Debian/Ubuntu: apt install autoconf automake libtool bison flex), or set ASN1C_EXECUTABLE")
  endif ()
  # Work on a copy: autoreconf and configure write into the source tree, and
  # the submodule must stay clean.
  file(REMOVE_RECURSE ${work_dir} ${install_dir})
  file(COPY ${source_dir}/ DESTINATION ${work_dir})
  set(log ${PROJECT_BINARY_DIR}/third_party/asn1c-build.log)
  execute_process(
    COMMAND ${AUTORECONF_EXECUTABLE} -iv
    WORKING_DIRECTORY ${work_dir}
    RESULT_VARIABLE result OUTPUT_FILE ${log} ERROR_FILE ${log})
  if (result EQUAL 0)
    execute_process(
      COMMAND ${work_dir}/configure --prefix=${install_dir} --disable-dependency-tracking
      WORKING_DIRECTORY ${work_dir}
      RESULT_VARIABLE result OUTPUT_FILE ${log} ERROR_FILE ${log})
  endif ()
  if (result EQUAL 0)
    include(ProcessorCount)
    ProcessorCount(jobs)
    if (jobs EQUAL 0)
      set(jobs 4)
    endif ()
    execute_process(
      COMMAND make -j${jobs} install
      WORKING_DIRECTORY ${work_dir}
      RESULT_VARIABLE result OUTPUT_FILE ${log} ERROR_FILE ${log})
  endif ()
  if (NOT result EQUAL 0 OR NOT EXISTS ${executable})
    message(FATAL_ERROR "asn1c build failed; see ${log} (or set ASN1C_EXECUTABLE)")
  endif ()
  file(WRITE ${stamp} "${commit}\n")
  set(${out_var} ${executable} PARENT_SCOPE)
endfunction()

if (NOT ASN1C_EXECUTABLE AND OCUDU_ASN1C_FROM_SUBMODULE)
  _ocudu_asn1c_from_submodule(_ocudu_asn1c)
  if (_ocudu_asn1c)
    set(ASN1C_EXECUTABLE ${_ocudu_asn1c} CACHE FILEPATH "asn1c compiler (built from third_party/asn1c)" FORCE)
  endif ()
endif ()
if (NOT ASN1C_EXECUTABLE)
  find_program(ASN1C_EXECUTABLE asn1c HINTS /opt/asn1c/bin)
endif ()
if (ASN1C_EXECUTABLE)
  execute_process(COMMAND ${ASN1C_EXECUTABLE} -v OUTPUT_VARIABLE _ocudu_asn1c_version ERROR_VARIABLE _ocudu_asn1c_version)
  string(REGEX MATCH "ASN.1 Compiler, [^\n]*" _ocudu_asn1c_version "${_ocudu_asn1c_version}")
  message(STATUS "asn1c: ${ASN1C_EXECUTABLE} (${_ocudu_asn1c_version})")
endif ()
