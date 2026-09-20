// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.
#pragma once

#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator.h"
#include "ocudu/support/ocudu_assert.h"

namespace ocudu {

// Internal factory wrapper, kept device-independent for forwarding/lifetime regression tests.
class pusch_demodulator_layer_select : public pusch_demodulator
{
public:
  pusch_demodulator_layer_select(std::unique_ptr<pusch_demodulator> accelerated_demodulator_,
                                 std::unique_ptr<pusch_demodulator> sw_demodulator_) :
    accelerated_demodulator(std::move(accelerated_demodulator_)), sw_demodulator(std::move(sw_demodulator_))
  {
    ocudu_assert(accelerated_demodulator, "Invalid accelerated PUSCH demodulator.");
    ocudu_assert(sw_demodulator, "Invalid software PUSCH demodulator.");
  }

  void demodulate(pusch_codeword_buffer&              codeword_buffer,
                  pusch_demodulator_notifier&         notifier,
                  const resource_grid_reader&         grid,
                  const dmrs_pusch_estimator_results& est_results,
                  const configuration&                config) override
  {
    if (supports_accelerated_demod(config)) {
      accelerated_demodulator->demodulate(codeword_buffer, notifier, grid, est_results, config);
      return;
    }

    sw_demodulator->demodulate(codeword_buffer, notifier, grid, est_results, config);
  }

  bool supports_resident_mode() const override { return accelerated_demodulator->supports_resident_mode(); }

  void enable_resident_mode() override { accelerated_demodulator->enable_resident_mode(); }

  void disable_resident_mode() override { accelerated_demodulator->disable_resident_mode(); }

  bool is_resident_mode_enabled() const override { return accelerated_demodulator->is_resident_mode_enabled(); }

  bool install_resident_dapp(std::shared_ptr<pusch_resident_dapp> dapp) override
  {
    return accelerated_demodulator->install_resident_dapp(std::move(dapp));
  }

  resident_softbit_buffer get_resident_softbits() const override
  {
    return accelerated_demodulator->get_resident_softbits();
  }

  void finish_resident_consumers() override { accelerated_demodulator->finish_resident_consumers(); }

  bool used_host_codeword_fallback() const override { return accelerated_demodulator->used_host_codeword_fallback(); }

  bool discarded_inline_input() const override { return accelerated_demodulator->discarded_inline_input(); }

  resident_uci_buffer get_resident_uci() const override { return accelerated_demodulator->get_resident_uci(); }

  void finalize_resident_uci() override { accelerated_demodulator->finalize_resident_uci(); }

  float get_last_grid_staging_us() const override { return accelerated_demodulator->get_last_grid_staging_us(); }

  bool used_device_grid() const override { return accelerated_demodulator->used_device_grid(); }

  bool used_owned_device_grid() const override { return accelerated_demodulator->used_owned_device_grid(); }

  float get_last_demod_sync_us() const override { return accelerated_demodulator->get_last_demod_sync_us(); }

  float report_deferred_sinr(pusch_demodulator_notifier& notifier) override
  {
    return accelerated_demodulator->report_deferred_sinr(notifier);
  }

  void report_deferred_dapp_completion() override { accelerated_demodulator->report_deferred_dapp_completion(); }

  void enable_resident_dapp_diagnostic_timing(bool enabled) override
  {
    accelerated_demodulator->enable_resident_dapp_diagnostic_timing(enabled);
  }

  resident_dapp_diagnostic_timing get_resident_dapp_diagnostic_timing(unsigned completion_slot) const override
  {
    return accelerated_demodulator->get_resident_dapp_diagnostic_timing(completion_slot);
  }

  // Forward constellation capture to both paths (the resident GPU receiver is the active one over the air).
  void enable_symbol_capture() override
  {
    accelerated_demodulator->enable_symbol_capture();
    sw_demodulator->enable_symbol_capture();
  }
  void disable_symbol_capture() override
  {
    accelerated_demodulator->disable_symbol_capture();
    sw_demodulator->disable_symbol_capture();
  }
  span<const std::complex<float>> get_captured_symbols() const override
  {
    auto eq = accelerated_demodulator->get_captured_symbols();
    if (!eq.empty()) {
      return eq;
    }
    return sw_demodulator->get_captured_symbols();
  }
  void wait_captured_symbols() override
  {
    accelerated_demodulator->wait_captured_symbols();
    sw_demodulator->wait_captured_symbols();
  }

private:
  static bool supports_accelerated_demod(const configuration& config)
  {
    unsigned nof_rx_ports = config.rx_ports.size();
    bool supported_layers = (config.nof_tx_layers == 1) || (config.nof_tx_layers == 2) || (config.nof_tx_layers == 3) ||
                            (config.nof_tx_layers == 4);
    bool supported_ports = (nof_rx_ports == 1) || (nof_rx_ports == 2) || (nof_rx_ports == 4) || (nof_rx_ports == 8);

    return supported_layers && supported_ports && (config.nof_tx_layers <= nof_rx_ports) &&
           (!config.enable_transform_precoding || (config.nof_tx_layers == 1));
  }

  std::unique_ptr<pusch_demodulator> accelerated_demodulator;
  std::unique_ptr<pusch_demodulator> sw_demodulator;
};

} // namespace ocudu
