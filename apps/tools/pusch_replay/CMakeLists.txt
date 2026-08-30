# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

include(GNUInstallDirs)

# Offline PUSCH replay: decodes bursts recorded over E3 (pusch.symbols with
# grant parameters) with the gNB's own demapper, descrambler, UL-SCH
# demultiplexer and LDPC decoder and reports the transport-block CRC.
add_executable(ocudu-pusch-replay ocudu_pusch_replay.cpp)
target_link_libraries(ocudu-pusch-replay PRIVATE
  ocudu_pusch_processor
  ocudu_pusch_demodulator
  ocudu_channel_modulation
  ocudu_sequence_generators
  ocudu_channel_coding
  ocudu_upper_phy_support
  ocudu_phy_support
  ocudu_instrumentation
  ocudu_ran
  ocudulog)
install(TARGETS ocudu-pusch-replay RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT dapp_sdk)
notify_binary_target(ocudu-pusch-replay)
