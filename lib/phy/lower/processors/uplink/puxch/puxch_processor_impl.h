// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "../../resource_request_pool.h"
#include "ocudu/adt/circular_array.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_dynamic.h"
#include "ocudu/phy/lower/lower_phy_rx_symbol_context.h"
#include "ocudu/phy/lower/modulation/ofdm_demodulator.h"
#include "ocudu/phy/lower/processors/lower_phy_center_freq_controller.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_baseband.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_notifier.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_request_handler.h"
#include "ocudu/phy/support/shared_resource_grid.h"
#include "ocudu/ran/slot_point.h"
#include <algorithm>
#include <vector>

namespace ocudu {

/// Implements PUxCH baseband processor.
class puxch_processor_impl : public puxch_processor,
                             private puxch_processor_baseband,
                             private lower_phy_center_freq_controller,
                             private puxch_processor_request_handler
{
public:
  struct configuration {
    cyclic_prefix cp;
    unsigned      nof_rx_ports;
    unsigned      dft_size;
  };

  puxch_processor_impl(std::unique_ptr<ofdm_symbol_demodulator> demodulator_, const configuration& config) :
    nof_symbols_per_slot(get_nsymb_per_slot(config.cp)),
    nof_rx_ports(config.nof_rx_ports),
    demodulator(std::move(demodulator_)),
    cf_buffer({2 * config.dft_size, nof_rx_ports}),
    max_symbol_samples(2 * config.dft_size)
  {
    ocudu_assert(demodulator, "Invalid demodulator.");
    max_ci16_symbol_batch_size =
        std::max(1U, std::min(nof_symbols_per_slot, demodulator->get_max_ci16_symbol_batch_size()));
    ci16_port_input_ptrs.resize(nof_rx_ports);
    ci16_port_sample_counts.resize(nof_rx_ports);
    ci16_port_indices.resize(nof_rx_ports);
    ci16_symbol_batch_sample_counts.reserve(max_ci16_symbol_batch_size);
    ci16_symbol_batch_input_ptrs.reserve(max_ci16_symbol_batch_size * nof_rx_ports);
    ci16_symbol_batch_storage.resize(max_ci16_symbol_batch_size);
    for (auto& entry : ci16_symbol_batch_storage) {
      entry.port_samples.resize(nof_rx_ports);
      for (auto& port_samples : entry.port_samples) {
        port_samples.reserve(max_symbol_samples);
      }
    }
  }

  // See interface for documentation.
  void connect(puxch_processor_notifier& notifier_) override { notifier = &notifier_; }

  // See interface for documentation.
  void stop() override { stopped = true; }

  // See interface for documentation.
  puxch_processor_request_handler& get_request_handler() override { return *this; }

  // See interface for documentation.
  puxch_processor_baseband& get_baseband() override { return *this; }

  // See interface for documentation.
  lower_phy_center_freq_controller& get_center_freq_control() override;

private:
  // See interface for documentation.
  bool process_symbol(const baseband_gateway_buffer_reader& samples,
                      const lower_phy_rx_symbol_context&    context) override;

  // See interface for documentation.
  void handle_request(const shared_resource_grid& grid, const resource_grid_context& context) override;

  // See interface for documentation.
  bool set_carrier_center_frequency(double carrier_center_frequency_Hz) override;

  /// Processes a symbol immediately using packed signed-16 IQ input pointers.
  bool process_symbol_immediate_ci16(span<const ci16_t* const>          port_inputs,
                                     span<const unsigned>               port_sample_counts,
                                     const lower_phy_rx_symbol_context& context,
                                     unsigned                           symbol_index_subframe);

  /// Attempts to stage a symbol for a same-CP packed signed-16 IQ symbol batch.
  bool try_enqueue_ci16_symbol_batch(span<const ci16_t* const>          port_inputs,
                                     span<const unsigned>               port_sample_counts,
                                     const lower_phy_rx_symbol_context& context,
                                     unsigned                           symbol_index_subframe);

  /// Flushes the staged packed signed-16 IQ symbol batch to the demodulator or scalar fallback.
  void flush_pending_ci16_symbol_batch();

  /// Notifies upper layers that a symbol has been processed and releases the grid at slot end.
  void notify_symbol_processed(const lower_phy_rx_symbol_context& context);

  struct ci16_symbol_batch_entry {
    lower_phy_rx_symbol_context      context;
    unsigned                         symbol_index_subframe = 0;
    unsigned                         nof_samples           = 0;
    std::vector<std::vector<ci16_t>> port_samples;
  };

  /// Scaling factor for converting from 16-bit complex integer to complex float.
  static constexpr float scaling_factor_ci16_to_cf = std::numeric_limits<int16_t>::max();

  std::atomic<bool>                           stopped = false;
  unsigned                                    nof_symbols_per_slot;
  unsigned                                    nof_rx_ports;
  puxch_processor_notifier*                   notifier = nullptr;
  std::unique_ptr<ofdm_symbol_demodulator>    demodulator;
  slot_point                                  current_slot;
  shared_resource_grid                        current_grid;
  resource_request_pool<shared_resource_grid> requests;
  /// \brief Temporary intermediate buffer to hold a symbol's complex-based floating-point samples for demodulation.
  ///
  /// Set to twice the DFT size to ensure that it can accommodate the maximum number of samples for demodulating an OFDM
  /// symbol.
  dynamic_tensor<2, cf_t>              cf_buffer;
  unsigned                             max_symbol_samples         = 0;
  unsigned                             max_ci16_symbol_batch_size = 1;
  unsigned                             pending_ci16_symbol_count  = 0;
  std::vector<const ci16_t*>           ci16_port_input_ptrs;
  std::vector<unsigned>                ci16_port_sample_counts;
  std::vector<unsigned>                ci16_port_indices;
  std::vector<unsigned>                ci16_symbol_batch_sample_counts;
  std::vector<const ci16_t*>           ci16_symbol_batch_input_ptrs;
  std::vector<ci16_symbol_batch_entry> ci16_symbol_batch_storage;
};

} // namespace ocudu
