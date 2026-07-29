// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "puxch_processor_impl.h"
#include "ocudu/ocuduvec/conversion.h"
#include "ocudu/phy/antenna_ports.h"
#include "ocudu/phy/lower/lower_phy_rx_symbol_context.h"
#include "ocudu/phy/support/resource_grid_context.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include <array>

using namespace ocudu;

bool puxch_processor_impl::process_symbol(const baseband_gateway_buffer_reader& samples,
                                          const lower_phy_rx_symbol_context&    context)
{
  ocudu_assert(notifier != nullptr, "Notifier has not been connected.");

  // Check if the slot has changed.
  if (context.slot != current_slot) {
    flush_pending_ci16_symbol_batch();

    // Update slot.
    current_slot = context.slot;

    // Exchange an empty request with the current slot with a stored request.
    auto request = requests.exchange({context.slot, shared_resource_grid()});

    // Handle the returned request.
    if (!request.resource) {
      // If the request resource grid pointer is invalid, the request is empty.
      current_grid.release();
    } else if (current_slot != request.slot) {
      // If the slot of the request does not match the current slot, then notify a late event.
      resource_grid_context late_context = {.slot = request.slot, .sector = context.sector};
      notifier->on_puxch_request_late(late_context);
      current_grid.release();
    } else {
      // If the request is valid, then select request grid.
      current_grid = std::move(request.resource);
    }
  }

  // Skip symbol processing if the context slot does not match with the current slot or no resource grid is available.
  if (!current_grid) {
    return false;
  }

  // Symbol index within the subframe.
  unsigned symbol_index_subframe = context.nof_symbols + context.slot.subframe_slot_index() * nof_symbols_per_slot;

  ci16_port_input_ptrs.resize(nof_rx_ports);
  ci16_port_sample_counts.resize(nof_rx_ports);
  for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
    span<const ci16_t> channel_buffer_ci16 = samples.get_channel_buffer(i_port);
    ci16_port_input_ptrs[i_port]           = channel_buffer_ci16.data();
    ci16_port_sample_counts[i_port]        = channel_buffer_ci16.size();
  }

  span<const ci16_t* const> port_inputs(ci16_port_input_ptrs.data(), nof_rx_ports);
  span<const unsigned>      port_sample_counts(ci16_port_sample_counts.data(), nof_rx_ports);
  if (try_enqueue_ci16_symbol_batch(port_inputs, port_sample_counts, context, symbol_index_subframe)) {
    return true;
  }

  return process_symbol_immediate_ci16(port_inputs, port_sample_counts, context, symbol_index_subframe);
}

bool puxch_processor_impl::process_symbol_immediate_ci16(span<const ci16_t* const>          port_inputs,
                                                         span<const unsigned>               port_sample_counts,
                                                         const lower_phy_rx_symbol_context& context,
                                                         unsigned                           symbol_index_subframe)
{
  if (!current_grid || (port_inputs.size() != nof_rx_ports) || (port_sample_counts.size() != nof_rx_ports)) {
    return false;
  }

  if (nof_rx_ports == 1) {
    span<const ci16_t> single_port_input(port_inputs[0], port_sample_counts[0]);
    if (demodulator->demodulate_ci16(
            current_grid.get().get_writer(), single_port_input, scaling_factor_ci16_to_cf, 0, symbol_index_subframe)) {
      notify_symbol_processed(context);
      return true;
    }
  }

  // Buffer view holding the float-based complex samples.
  span<cf_t> channel_buffer_cf;
  // View over the original int16-based input samples.
  span<const ci16_t> channel_buffer_ci16;

  std::array<const ci16_t*, MAX_PORTS> port_input_ptrs  = {};
  std::array<unsigned, MAX_PORTS>      port_indices     = {};
  unsigned                             nof_samples      = 0;
  bool                                 try_batched_ci16 = nof_rx_ports <= MAX_PORTS;
  for (unsigned i_port = 0; (i_port != nof_rx_ports) && try_batched_ci16; ++i_port) {
    unsigned port_nof_samples = port_sample_counts[i_port];
    if ((i_port != 0) && (port_nof_samples != nof_samples)) {
      try_batched_ci16 = false;
      break;
    }
    nof_samples             = port_nof_samples;
    port_input_ptrs[i_port] = port_inputs[i_port];
    port_indices[i_port]    = i_port;
  }

  if (try_batched_ci16 &&
      demodulator->demodulate_ci16_ports(current_grid.get().get_writer(),
                                         span<const ci16_t* const>(port_input_ptrs.data(), nof_rx_ports),
                                         nof_samples,
                                         scaling_factor_ci16_to_cf,
                                         span<const unsigned>(port_indices.data(), nof_rx_ports),
                                         symbol_index_subframe)) {
    notify_symbol_processed(context);
    return true;
  }

  // Demodulate each of the ports.
  for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
    channel_buffer_ci16 = span<const ci16_t>(port_inputs[i_port], port_sample_counts[i_port]);
    if (demodulator->demodulate_ci16(current_grid.get().get_writer(),
                                     channel_buffer_ci16,
                                     scaling_factor_ci16_to_cf,
                                     i_port,
                                     symbol_index_subframe)) {
      continue;
    }

    // Convert integer based complex samples to floating-point based.
    channel_buffer_cf = cf_buffer.get_view({i_port}).subspan(0, channel_buffer_ci16.size());
    ocuduvec::convert(channel_buffer_cf, channel_buffer_ci16, scaling_factor_ci16_to_cf);
    demodulator->demodulate(current_grid.get().get_writer(), channel_buffer_cf, i_port, symbol_index_subframe);
  }

  // Notify.
  notify_symbol_processed(context);

  return true;
}

bool puxch_processor_impl::try_enqueue_ci16_symbol_batch(span<const ci16_t* const>          port_inputs,
                                                         span<const unsigned>               port_sample_counts,
                                                         const lower_phy_rx_symbol_context& context,
                                                         unsigned                           symbol_index_subframe)
{
  if ((max_ci16_symbol_batch_size <= 1) || (nof_rx_ports > MAX_PORTS) || (port_inputs.size() != nof_rx_ports) ||
      (port_sample_counts.size() != nof_rx_ports) || !current_grid ||
      !current_grid.get().get_writer().supports_device_grid_mapping()) {
    return false;
  }

  unsigned nof_samples = port_sample_counts[0];
  if (nof_samples == 0) {
    return false;
  }
  for (unsigned i_port = 1; i_port != nof_rx_ports; ++i_port) {
    if (port_sample_counts[i_port] != nof_samples) {
      return false;
    }
  }

  if (pending_ci16_symbol_count != 0) {
    const auto& last_entry = ci16_symbol_batch_storage[pending_ci16_symbol_count - 1];
    if ((context.slot != last_entry.context.slot) || (symbol_index_subframe != last_entry.symbol_index_subframe + 1)) {
      std::array<const ci16_t*, MAX_PORTS> saved_port_inputs   = {};
      std::array<unsigned, MAX_PORTS>      saved_sample_counts = {};
      for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
        saved_port_inputs[i_port]   = port_inputs[i_port];
        saved_sample_counts[i_port] = port_sample_counts[i_port];
      }
      flush_pending_ci16_symbol_batch();
      ci16_port_input_ptrs.resize(nof_rx_ports);
      ci16_port_sample_counts.resize(nof_rx_ports);
      for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
        ci16_port_input_ptrs[i_port]    = saved_port_inputs[i_port];
        ci16_port_sample_counts[i_port] = saved_sample_counts[i_port];
      }
      port_inputs        = span<const ci16_t* const>(ci16_port_input_ptrs.data(), nof_rx_ports);
      port_sample_counts = span<const unsigned>(ci16_port_sample_counts.data(), nof_rx_ports);
    }
  }

  if (!current_grid) {
    return false;
  }

  if (pending_ci16_symbol_count == 0) {
    bool has_next_symbol_in_slot = context.nof_symbols + 1 < nof_symbols_per_slot;
    if (!has_next_symbol_in_slot) {
      return false;
    }
  }

  auto& entry                 = ci16_symbol_batch_storage[pending_ci16_symbol_count++];
  entry.context               = context;
  entry.symbol_index_subframe = symbol_index_subframe;
  entry.nof_samples           = nof_samples;
  for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
    entry.port_samples[i_port].assign(port_inputs[i_port], port_inputs[i_port] + nof_samples);
  }

  if ((pending_ci16_symbol_count >= max_ci16_symbol_batch_size) || (context.nof_symbols == nof_symbols_per_slot - 1)) {
    flush_pending_ci16_symbol_batch();
  }

  return true;
}

void puxch_processor_impl::flush_pending_ci16_symbol_batch()
{
  if (pending_ci16_symbol_count == 0) {
    return;
  }

  if (!current_grid) {
    pending_ci16_symbol_count = 0;
    return;
  }

  ci16_port_input_ptrs.resize(nof_rx_ports);
  ci16_port_sample_counts.resize(nof_rx_ports);

  auto process_entry_immediately = [this](const ci16_symbol_batch_entry& entry) {
    for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
      ci16_port_input_ptrs[i_port]    = entry.port_samples[i_port].data();
      ci16_port_sample_counts[i_port] = entry.port_samples[i_port].size();
    }
    (void)process_symbol_immediate_ci16(span<const ci16_t* const>(ci16_port_input_ptrs.data(), nof_rx_ports),
                                        span<const unsigned>(ci16_port_sample_counts.data(), nof_rx_ports),
                                        entry.context,
                                        entry.symbol_index_subframe);
  };

  if (pending_ci16_symbol_count == 1) {
    process_entry_immediately(ci16_symbol_batch_storage[0]);
    pending_ci16_symbol_count = 0;
    return;
  }

  ci16_port_indices.resize(nof_rx_ports);
  for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
    ci16_port_indices[i_port] = i_port;
  }

  ci16_symbol_batch_input_ptrs.resize(pending_ci16_symbol_count * nof_rx_ports);
  ci16_symbol_batch_sample_counts.resize(pending_ci16_symbol_count);
  for (unsigned i_symbol = 0; i_symbol != pending_ci16_symbol_count; ++i_symbol) {
    const auto& entry                         = ci16_symbol_batch_storage[i_symbol];
    ci16_symbol_batch_sample_counts[i_symbol] = entry.nof_samples;
    for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
      ci16_symbol_batch_input_ptrs[i_symbol * nof_rx_ports + i_port] = entry.port_samples[i_port].data();
    }
  }

  const auto& first_entry     = ci16_symbol_batch_storage[0];
  bool        batch_processed = demodulator->demodulate_ci16_symbol_batch_ports(
      current_grid.get().get_writer(),
      span<const ci16_t* const>(ci16_symbol_batch_input_ptrs.data(), ci16_symbol_batch_input_ptrs.size()),
      span<const unsigned>(ci16_symbol_batch_sample_counts.data(), pending_ci16_symbol_count),
      scaling_factor_ci16_to_cf,
      span<const unsigned>(ci16_port_indices.data(), nof_rx_ports),
      first_entry.symbol_index_subframe,
      pending_ci16_symbol_count);

  if (batch_processed) {
    for (unsigned i_symbol = 0; i_symbol != pending_ci16_symbol_count; ++i_symbol) {
      notify_symbol_processed(ci16_symbol_batch_storage[i_symbol].context);
    }
    pending_ci16_symbol_count = 0;
    return;
  }

  for (unsigned i_symbol = 0; i_symbol != pending_ci16_symbol_count; ++i_symbol) {
    process_entry_immediately(ci16_symbol_batch_storage[i_symbol]);
  }
  pending_ci16_symbol_count = 0;
}

void puxch_processor_impl::notify_symbol_processed(const lower_phy_rx_symbol_context& context)
{
  notifier->on_rx_symbol(current_grid, context, true);

  if (context.nof_symbols == nof_symbols_per_slot - 1) {
    current_grid.release();
  }
}

void puxch_processor_impl::handle_request(const shared_resource_grid& grid, const resource_grid_context& context)
{
  // Ignore request if the processor has stopped.
  if (stopped.load(std::memory_order_relaxed)) {
    return;
  }

  ocudu_assert(notifier != nullptr, "Notifier has not been connected.");

  // Swap the new request by the current request in the circular array.
  auto request = requests.exchange({context.slot, grid.copy()});

  // If there was a request at the same request index, notify a late event with the context of the discarded request.
  if (request.resource) {
    resource_grid_context late_context = {.slot = request.slot, .sector = context.sector};
    notifier->on_puxch_request_late(late_context);
  }
}

lower_phy_center_freq_controller& puxch_processor_impl::get_center_freq_control()
{
  return *this;
}

bool puxch_processor_impl::set_carrier_center_frequency(double carrier_center_frequency_Hz)
{
  flush_pending_ci16_symbol_batch();
  demodulator->set_center_frequency(carrier_center_frequency_Hz);
  return true;
}
