// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_configuration.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/ran/srs/srs_information.h"
#include "ocudu/ran/subcarrier_spacing.h"
#include "ocudu/support/math/math_utils.h"
#include <complex>
#include <memory>
#include <random>
#include <vector>

namespace ocudu::srs_dapp_test {

/// An SRS occasion on a synthetic uplink grid: every transmit port reaches every receive port through a flat channel
/// with a common delay, plus white Gaussian noise on the SRS resource elements.
struct occasion_profile {
  unsigned           nof_rx_ports        = 1;
  unsigned           nof_tx_ports        = 1;
  srs_nof_symbols    nof_symbols         = srs_nof_symbols::n1;
  unsigned           start_symbol        = 13;
  tx_comb_size       comb_size           = tx_comb_size::n4;
  unsigned           comb_offset         = 0;
  unsigned           cyclic_shift        = 0;
  unsigned           configuration_index = 14;
  unsigned           bandwidth_index     = 0;
  unsigned           freq_position       = 0;
  unsigned           freq_shift          = 0;
  unsigned           sequence_id         = 0;
  subcarrier_spacing scs                 = subcarrier_spacing::kHz30;
  unsigned           grid_nof_prb        = 52;
  /// Propagation delay (positive: late), seconds.
  double delay_s = 0.0;
  /// SNR per resource element, dB.
  float    snr_db = 20.0F;
  unsigned seed   = 1;
};

inline srs_estimator_configuration make_configuration(const occasion_profile& profile)
{
  srs_resource_configuration resource;
  resource.nof_antenna_ports   = static_cast<srs_resource_configuration::one_two_four_enum>(profile.nof_tx_ports);
  resource.nof_symbols         = profile.nof_symbols;
  resource.start_symbol        = profile.start_symbol;
  resource.configuration_index = profile.configuration_index;
  resource.sequence_id         = profile.sequence_id;
  resource.bandwidth_index     = profile.bandwidth_index;
  resource.comb_size           = profile.comb_size;
  resource.comb_offset         = profile.comb_offset;
  resource.cyclic_shift        = profile.cyclic_shift;
  resource.freq_position       = profile.freq_position;
  resource.freq_shift          = profile.freq_shift;
  resource.freq_hopping        = profile.bandwidth_index;
  resource.hopping             = srs_group_or_sequence_hopping::neither;
  resource.periodicity         = srs_resource_configuration::periodicity_and_offset{0, 0};

  srs_estimator_configuration config;
  config.slot     = slot_point(to_numerology_value(profile.scs), 0);
  config.resource = resource;
  config.context  = srs_context(0, to_rnti(0x4601));
  config.ports.resize(profile.nof_rx_ports);
  for (unsigned rx = 0; rx != profile.nof_rx_ports; ++rx) {
    config.ports[rx] = rx;
  }
  return config;
}

/// Channel coefficient of one port pair.
inline cf_t channel_coefficient(unsigned rx, unsigned tx)
{
  return std::polar(0.45F + 0.07F * static_cast<float>(rx + 1) + 0.05F * static_cast<float>(tx + 1),
                    0.37F * static_cast<float>(rx + 1) - 0.29F * static_cast<float>(tx + 1));
}

/// A grid sized for the profile.
inline std::unique_ptr<resource_grid> make_grid(const occasion_profile& profile)
{
  return create_resource_grid_factory()->create(
      profile.nof_rx_ports, get_nsymb_per_slot(cyclic_prefix::NORMAL), profile.grid_nof_prb * NOF_SUBCARRIERS_PER_RB);
}

/// Writes the occasion into \p grid. Returns false when the SRS does not fit in the grid.
inline bool
write_occasion(resource_grid& grid, const occasion_profile& profile, const srs_estimator_configuration& config)
{
  grid.set_all_zero();
  const unsigned nof_subcarriers = grid.get_reader().get_nof_subc();
  const double   scs_hz          = scs_to_khz(profile.scs) * 1000.0;
  auto           generator       = create_low_papr_sequence_generator_sw_factory()->create();

  std::vector<std::vector<cf_t>> symbols(profile.nof_rx_ports, std::vector<cf_t>(nof_subcarriers));
  std::vector<bool>              occupied(nof_subcarriers);
  std::vector<cf_t>              sequence;
  for (unsigned tx = 0; tx != profile.nof_tx_ports; ++tx) {
    const srs_information info = get_srs_information(config.resource, tx);
    if (info.sequence_length == 0 ||
        info.mapping_initial_subcarrier + (info.sequence_length - 1) * info.comb_size >= nof_subcarriers) {
      return false;
    }
    sequence.resize(info.sequence_length);
    generator->generate(sequence, info.sequence_group, info.sequence_number, info.n_cs, info.n_cs_max);
    for (unsigned n = 0; n != info.sequence_length; ++n) {
      const unsigned k     = info.mapping_initial_subcarrier + n * info.comb_size;
      const auto     delay = std::polar(1.0, -2.0 * M_PI * static_cast<double>(k) * scs_hz * profile.delay_s);
      occupied[k]          = true;
      for (unsigned rx = 0; rx != profile.nof_rx_ports; ++rx) {
        symbols[rx][k] += channel_coefficient(rx, tx) * sequence[n] * cf_t(delay);
      }
    }
  }

  // Noise relative to the mean received power per port pair.
  float signal = 0.0F;
  for (unsigned rx = 0; rx != profile.nof_rx_ports; ++rx) {
    for (unsigned tx = 0; tx != profile.nof_tx_ports; ++tx) {
      signal += std::norm(channel_coefficient(rx, tx));
    }
  }
  signal /= static_cast<float>(profile.nof_rx_ports);
  std::mt19937                    random(profile.seed);
  std::normal_distribution<float> normal(0.0F, std::sqrt(signal / convert_dB_to_power(profile.snr_db) / 2.0F));
  resource_grid_writer&           writer = grid.get_writer();
  const unsigned                  start  = config.resource.start_symbol.value();
  for (unsigned symbol = start; symbol != start + static_cast<unsigned>(profile.nof_symbols); ++symbol) {
    for (unsigned rx = 0; rx != profile.nof_rx_ports; ++rx) {
      span<cbf16_t> view = writer.get_view(rx, symbol);
      for (unsigned k = 0; k != nof_subcarriers; ++k) {
        if (occupied[k]) {
          view[k] = to_cbf16(symbols[rx][k] + cf_t(normal(random), normal(random)));
        }
      }
    }
  }
  return true;
}

/// The occasions of the golden vector set (tests/data/srs_channel_estimator_vectors.bin in the dApp SDK).
inline std::vector<occasion_profile> golden_occasions()
{
  constexpr double              us = 1e-6;
  std::vector<occasion_profile> list;
  auto                          add = [&list](occasion_profile profile) {
    profile.seed = static_cast<unsigned>(list.size() + 1);
    list.push_back(profile);
  };
  // One port, comb 4, the shapes of the live cell.
  add({.nof_rx_ports = 1});
  add({.nof_rx_ports = 2, .delay_s = 0.3 * us, .snr_db = 25});
  add({.nof_rx_ports = 4, .nof_symbols = srs_nof_symbols::n2, .start_symbol = 12, .delay_s = -0.2 * us, .snr_db = 10});
  add({.nof_rx_ports = 2, .nof_symbols = srs_nof_symbols::n4, .start_symbol = 10, .delay_s = 0.12 * us});
  // Comb 2.
  add({.nof_rx_ports = 2, .comb_size = tx_comb_size::n2, .configuration_index = 9, .delay_s = 0.1 * us, .snr_db = 30});
  add({.nof_rx_ports        = 1,
       .nof_symbols         = srs_nof_symbols::n4,
       .start_symbol        = 8,
       .comb_size           = tx_comb_size::n2,
       .comb_offset         = 1,
       .configuration_index = 14,
       .delay_s             = -0.05 * us});
  // Two and four ports, sharing one pilot set.
  add({.nof_rx_ports = 2, .nof_tx_ports = 2, .delay_s = 0.05 * us});
  add({.nof_rx_ports = 4, .nof_tx_ports = 2, .comb_size = tx_comb_size::n2, .cyclic_shift = 3, .snr_db = 15});
  add({.nof_rx_ports = 2, .nof_tx_ports = 4, .cyclic_shift = 1, .delay_s = 0.2 * us});
  add({.nof_rx_ports = 4, .nof_tx_ports = 4, .comb_size = tx_comb_size::n2, .cyclic_shift = 2, .snr_db = 28});
  // Four ports with interleaved pilots.
  add({.nof_rx_ports = 2, .nof_tx_ports = 4, .cyclic_shift = 6, .delay_s = 0.2 * us});
  add({.nof_rx_ports = 1, .nof_tx_ports = 4, .comb_size = tx_comb_size::n2, .cyclic_shift = 4, .snr_db = 18});
  add({.nof_rx_ports = 4,
       .nof_tx_ports = 4,
       .nof_symbols  = srs_nof_symbols::n2,
       .start_symbol = 12,
       .cyclic_shift = 11,
       .delay_s      = -0.1 * us});
  // Numerology, sequence identity and frequency position.
  add({.nof_rx_ports = 1, .scs = subcarrier_spacing::kHz15, .grid_nof_prb = 52, .delay_s = 0.4 * us});
  add({.nof_rx_ports = 2, .sequence_id = 517, .delay_s = 0.07 * us});
  add({.nof_rx_ports = 1, .configuration_index = 3, .freq_position = 2, .freq_shift = 5, .snr_db = 12});
  add({.nof_rx_ports = 2, .configuration_index = 14, .bandwidth_index = 1, .delay_s = -0.15 * us});
  add({.nof_rx_ports = 1, .comb_size = tx_comb_size::n2, .configuration_index = 0, .snr_db = 22});
  // Extremes of SNR and delay.
  add({.nof_rx_ports = 2, .snr_db = -5});
  add({.nof_rx_ports = 2, .snr_db = 0});
  add({.nof_rx_ports = 1, .snr_db = 45});
  add({.nof_rx_ports = 2, .delay_s = 0.55 * us});
  add({.nof_rx_ports = 2, .delay_s = -0.55 * us});
  // Many receive ports and wide sounding bandwidths.
  add({.nof_rx_ports = 8, .delay_s = 0.1 * us});
  add({.nof_rx_ports        = 1,
       .comb_size           = tx_comb_size::n2,
       .configuration_index = 25,
       .grid_nof_prb        = 106,
       .delay_s             = 0.25 * us});
  add({.nof_rx_ports        = 1,
       .comb_size           = tx_comb_size::n2,
       .configuration_index = 63,
       .grid_nof_prb        = 273,
       .delay_s             = -0.08 * us});
  return list;
}

} // namespace ocudu::srs_dapp_test
