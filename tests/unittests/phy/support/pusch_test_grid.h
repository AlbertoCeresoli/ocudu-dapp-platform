// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "resource_grid_test_doubles.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include <array>
#include <vector>

namespace ocudu::test_support {

/// Fills a one- or two-layer PUSCH fixture with port-1000/1001 DMRS over an identity channel.
inline void populate_identity_pusch_grid(resource_grid_reader_spy&               grid,
                                         const pusch_demodulator::configuration& config,
                                         unsigned                                nof_prb)
{
  ocudu_assert(config.nof_tx_layers == 1 || config.nof_tx_layers == 2, "Unsupported test layer count.");
  ocudu_assert(config.dmrs_type == dmrs_config_type::type1 && config.nof_cdm_groups_without_data == 1,
               "Unsupported test DMRS profile.");
  ocudu_assert(config.rx_ports.size() == config.nof_tx_layers && grid.get_nof_ports() >= config.nof_tx_layers &&
                   config.rb_mask.count() == nof_prb,
               "Inconsistent test grid dimensions.");
  for (unsigned port = 0; port != config.nof_tx_layers; ++port) {
    for (unsigned symbol = 0; symbol != grid.get_nof_symbols(); ++symbol) {
      for (unsigned subcarrier = 0; subcarrier != grid.get_nof_subc(); ++subcarrier) {
        grid.write({static_cast<uint8_t>(port), static_cast<uint8_t>(symbol), static_cast<uint16_t>(subcarrier), {}});
      }
    }
  }

  constexpr float           inverse_sqrt_two = 0.70710678118654752440F;
  const std::array<cf_t, 2> data_symbols{{{inverse_sqrt_two, inverse_sqrt_two}, {-inverse_sqrt_two, inverse_sqrt_two}}};
  auto                      generator = create_pseudo_random_generator_sw_factory()->create();
  std::vector<cf_t>         sequence(nof_prb * 6);
  config.dmrs_symb_pos.for_each(0, config.dmrs_symb_pos.size(), [&](unsigned symbol) {
    const unsigned c_init = ((get_nsymb_per_slot(config.c_prefix) * config.slot.slot_index() + symbol + 1) *
                                 (2 * config.dmrs_scrambling_id + 1) * (1U << 17U) +
                             2 * config.dmrs_scrambling_id + (config.n_scid ? 1U : 0U)) %
                            (1U << 31U);
    generator->init(c_init);
    generator->generate(sequence, inverse_sqrt_two);
    for (unsigned rb = 0; rb != nof_prb; ++rb) {
      for (unsigned pilot = 0; pilot != 6; ++pilot) {
        const unsigned subcarrier = rb * NOF_SUBCARRIERS_PER_RB + 2 * pilot;
        const cf_t     layer0     = sequence[rb * 6 + pilot] * config.dmrs_scaling;
        grid.write({0, static_cast<uint8_t>(symbol), static_cast<uint16_t>(subcarrier), layer0});
        if (config.nof_tx_layers == 2) {
          const cf_t layer1 = layer0 * (pilot % 2 == 0 ? 1.0F : -1.0F);
          grid.write({1, static_cast<uint8_t>(symbol), static_cast<uint16_t>(subcarrier), layer1});
        }
      }
    }
  });

  for (unsigned symbol = config.start_symbol_index; symbol != config.start_symbol_index + config.nof_symbols;
       ++symbol) {
    for (unsigned subcarrier = 0; subcarrier != nof_prb * NOF_SUBCARRIERS_PER_RB; ++subcarrier) {
      if (config.dmrs_symb_pos.test(symbol) && subcarrier % 2 == 0) {
        continue;
      }
      for (unsigned port = 0; port != config.nof_tx_layers; ++port) {
        grid.write({static_cast<uint8_t>(port),
                    static_cast<uint8_t>(symbol),
                    static_cast<uint16_t>(subcarrier),
                    data_symbols[port]});
      }
    }
  }
}

} // namespace ocudu::test_support
