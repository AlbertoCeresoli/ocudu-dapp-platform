# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


add_library(ocudu_ru_channel_emulator STATIC channel_emulator_hook.cpp)
target_link_libraries(ocudu_ru_channel_emulator ocudulog ${CMAKE_DL_LIBS})

add_subdirectory(dummy)
add_subdirectory(ofh)
add_subdirectory(sdr)
