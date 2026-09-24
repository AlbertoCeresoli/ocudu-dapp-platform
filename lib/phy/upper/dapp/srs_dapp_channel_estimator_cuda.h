// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <cstdint>

namespace ocudu::dapp {

/// SRS resource elements to gather from a CBF16 [port, symbol, subcarrier] grid snapshot.
struct srs_rx_symbol_gather_config {
  uint32_t nof_pilot_sets{};
  uint32_t nof_rx_ports{};
  uint32_t nof_symbols{};
  uint32_t start_symbol{};
  uint32_t sequence_length{};
  uint32_t comb_size{};
  uint32_t grid_nof_symbols{};
  uint32_t grid_nof_subcarriers{};
  uint32_t rx_ports[8]{};
  uint32_t first_subcarrier[2]{};
};

/// Enqueues on \p stream the gather of the SRS resource elements into CF32 [pilot_set, rx_port, symbol, pilot].
bool launch_srs_rx_symbol_gather(const void*                        device_grid_cbf16,
                                 void*                              device_rx_symbols_cf32,
                                 const srs_rx_symbol_gather_config& config,
                                 void*                              stream) noexcept;

} // namespace ocudu::dapp
