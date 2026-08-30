// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "srs_diagnostic_decorator.h"
#include "ocudu/ocuduvec/add.h"
#include "ocudu/ocuduvec/copy.h"
#include "ocudu/ocuduvec/prod.h"
#include "ocudu/ocuduvec/sc_prod.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/upper/sequence_generators/low_papr_sequence_generator.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_configuration.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include "ocudu/ran/srs/srs_information.h"
#include "ocudu/ran/srs/srs_resource_configuration.h"
#include "ocudu/ran/subcarrier_spacing.h"
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

using namespace ocudu;

namespace {

/// Publishes the per-subcarrier least-squares SRS channel estimate for every
/// (receive port, transmit port) pair.
///
/// The estimator implementations only return the wideband channel matrix (one
/// coefficient per port pair), which is what the scheduler needs, but the
/// telemetry consumers plot the frequency-selective response. The decorator
/// therefore recomputes the LSE from the resource grid when a subscriber admits
/// the stream: the SRS sequence is regenerated, the received sequence is
/// averaged across the SRS symbols, correlated with the sequence, and the
/// linear phase ramp caused by the measured time alignment is removed exactly as
/// the generic estimator does before it averages across subcarriers. This works
/// identically over the host and CUDA estimators and costs one grid read per
/// port and symbol at the (rate limited) telemetry cadence.
class srs_diagnostic_decorator final : public srs_estimator
{
public:
  srs_diagnostic_decorator(std::unique_ptr<srs_estimator>               inner_,
                           std::shared_ptr<phy_diagnostic_sink>         sink_,
                           std::unique_ptr<low_papr_sequence_generator> generator_) :
    inner(std::move(inner_)), sink(std::move(sink_)), generator(std::move(generator_))
  {
  }

  srs_estimator_result estimate(const resource_grid_reader& grid, const srs_estimator_configuration& config) override
  {
    srs_estimator_result result = inner->estimate(grid, config);
    publish(grid, config, result);
    return result;
  }

private:
  void publish(const resource_grid_reader&        grid,
               const srs_estimator_configuration& config,
               const srs_estimator_result&        result) noexcept
  {
    if (!config.context.has_value()) {
      return;
    }
    const uint16_t rnti = static_cast<uint16_t>(config.context->get_rnti());
    // The gate consumes the per-RNTI publication token once for the whole port matrix.
    if (!sink->should_collect(phy_diagnostic_stream::srs_channel, rnti)) {
      return;
    }
    const unsigned nof_rx      = result.channel_matrix.get_nof_rx_ports();
    const unsigned nof_tx      = result.channel_matrix.get_nof_tx_ports();
    const unsigned nof_symbols = static_cast<unsigned>(config.resource.nof_symbols);
    if (nof_rx == 0 || nof_tx == 0 || nof_symbols == 0 || nof_rx > config.ports.size()) {
      return;
    }
    const srs_information info = get_srs_information(config.resource, 0);
    if (info.sequence_length == 0 || info.sequence_length > std::numeric_limits<uint16_t>::max()) {
      return;
    }

    srs_channel_diagnostic event;
    event.rnti                       = rnti;
    event.slot                       = config.slot;
    event.nof_subcarriers            = static_cast<uint16_t>(info.sequence_length);
    event.comb_size                  = static_cast<uint8_t>(info.comb_size);
    event.nof_rx_ports               = static_cast<uint8_t>(nof_rx);
    event.nof_tx_ports               = static_cast<uint8_t>(nof_tx);
    event.rsrp_dB                    = result.rsrp_dB.value_or(std::numeric_limits<float>::quiet_NaN());
    event.epre_dB                    = result.epre_dB.value_or(std::numeric_limits<float>::quiet_NaN());
    event.noise_variance             = result.noise_variance.value_or(std::numeric_limits<float>::quiet_NaN());
    event.time_alignment_ns          = static_cast<float>(result.time_alignment.time_alignment * 1e9);
    event.start_symbol               = config.resource.start_symbol.value();
    event.nof_symbols                = static_cast<uint8_t>(nof_symbols);
    event.mapping_initial_subcarrier = static_cast<uint16_t>(info.mapping_initial_subcarrier);
    event.comb_offset                = config.resource.comb_offset.value();
    event.cyclic_shift               = config.resource.cyclic_shift.value();
    event.sequence_id                = config.resource.sequence_id.value();
    event.sequence_group             = static_cast<uint16_t>(info.sequence_group);
    event.sequence_number            = static_cast<uint16_t>(info.sequence_number);
    event.n_cs                       = static_cast<uint16_t>(info.n_cs);
    event.n_cs_max                   = static_cast<uint16_t>(info.n_cs_max);
    event.bandwidth_index            = config.resource.bandwidth_index.value();
    event.freq_position              = config.resource.freq_position.value();
    event.freq_shift                 = config.resource.freq_shift.value();
    event.freq_hopping               = config.resource.freq_hopping.value();

    // Phase ramp across the comb subcarriers caused by the measured time alignment.
    const subcarrier_spacing scs = to_subcarrier_spacing(config.slot.numerology());
    const double             phase_shift_subcarrier =
        2.0 * M_PI * result.time_alignment.time_alignment * scs_to_khz(scs) * 1000.0 * info.comb_size;

    sequence.resize(info.sequence_length);
    received.resize(info.sequence_length);
    lse.resize(info.sequence_length);
    ramp.resize(info.sequence_length);

    for (unsigned tx = 0; tx != nof_tx; ++tx) {
      const srs_information port_info = get_srs_information(config.resource, tx);
      if (port_info.sequence_length != info.sequence_length) {
        return;
      }
      generator->generate(
          sequence, port_info.sequence_group, port_info.sequence_number, port_info.n_cs, port_info.n_cs_max);

      const double phase_shift_offset =
          phase_shift_subcarrier * static_cast<double>(port_info.mapping_initial_subcarrier) / info.comb_size;
      for (unsigned n = 0; n != info.sequence_length; ++n) {
        ramp[n] = std::polar(1.0F, static_cast<float>(n * phase_shift_subcarrier + phase_shift_offset));
      }

      for (unsigned rx = 0; rx != nof_rx; ++rx) {
        const unsigned rx_port = config.ports[rx];
        for (unsigned symbol = config.resource.start_symbol.value(), end = symbol + nof_symbols; symbol != end;
             ++symbol) {
          grid.get(received, rx_port, symbol, port_info.mapping_initial_subcarrier, port_info.comb_size);
          if (symbol == config.resource.start_symbol.value()) {
            ocuduvec::copy(lse, received);
          } else {
            ocuduvec::add(lse, received, lse);
          }
        }
        // Least-squares estimate, averaged over the SRS symbols, with the TA phase ramp removed.
        ocuduvec::prod_conj(lse, lse, sequence);
        if (nof_symbols > 1) {
          ocuduvec::sc_prod(lse, lse, 1.0F / static_cast<float>(nof_symbols));
        }
        ocuduvec::prod(lse, ramp, lse);

        event.rx_port_index  = static_cast<uint8_t>(rx);
        event.tx_port_index  = static_cast<uint8_t>(tx);
        event.h_coefficients = span<const std::complex<float>>(lse.data(), lse.size());
        sink->on_srs_channel(event);
      }
    }
  }

  std::unique_ptr<srs_estimator>               inner;
  std::shared_ptr<phy_diagnostic_sink>         sink;
  std::unique_ptr<low_papr_sequence_generator> generator;
  std::vector<cf_t>                            sequence;
  std::vector<cf_t>                            received;
  std::vector<cf_t>                            lse;
  std::vector<cf_t>                            ramp;
};

class srs_diagnostic_decorator_factory final : public srs_estimator_factory
{
public:
  srs_diagnostic_decorator_factory(std::shared_ptr<srs_estimator_factory>               inner_,
                                   std::shared_ptr<phy_diagnostic_sink>                 sink_,
                                   std::shared_ptr<low_papr_sequence_generator_factory> generators_) :
    inner(std::move(inner_)), sink(std::move(sink_)), generators(std::move(generators_))
  {
  }

  std::unique_ptr<srs_estimator> create() override { return wrap(inner->create()); }

  std::unique_ptr<srs_estimator> create(ocudulog::basic_logger& logger) override { return wrap(inner->create(logger)); }

  std::unique_ptr<srs_estimator_configuration_validator> create_validator() override
  {
    return inner->create_validator();
  }

private:
  std::unique_ptr<srs_estimator> wrap(std::unique_ptr<srs_estimator> estimator)
  {
    if (!estimator) {
      return nullptr;
    }
    auto generator = generators->create();
    if (!generator) {
      return estimator;
    }
    return std::make_unique<srs_diagnostic_decorator>(std::move(estimator), sink, std::move(generator));
  }

  std::shared_ptr<srs_estimator_factory>               inner;
  std::shared_ptr<phy_diagnostic_sink>                 sink;
  std::shared_ptr<low_papr_sequence_generator_factory> generators;
};

} // namespace

std::shared_ptr<srs_estimator_factory>
ocudu::create_srs_diagnostic_decorator_factory(std::shared_ptr<srs_estimator_factory> factory,
                                               std::shared_ptr<phy_diagnostic_sink>   sink)
{
  if (!factory || !sink) {
    return factory;
  }
  auto generators = create_low_papr_sequence_generator_sw_factory();
  if (!generators) {
    return factory;
  }
  return std::make_shared<srs_diagnostic_decorator_factory>(std::move(factory), std::move(sink), std::move(generators));
}
