// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/phy/upper/channel_processors/pdsch/factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/factories.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_hook_provider.h"
#include "ocudu/ran/pusch/pusch_constants.h"

namespace ocudu {

namespace test_support {
class pusch_cold_trace;
}

/// Optional real host adapters for isolated full-PUSCH dApp qualification.
struct pxsch_test_dapp_config {
  std::shared_ptr<upper_phy_dapp_hook_provider> hooks;
  upper_phy_dapp_pusch_factory_config           limits{};
};

/// \brief Creates a PDSCH processor factory.
/// \param[in] executor        Asynchronous executor.
/// \param[in] max_nof_threads Maximum number of concurrent threads.
/// \param[in] eal_arguments   String containing the EAL arguments to be used in case of hardware-acceleration.
/// \param[in] pxsch_type      Type of PxSCH implementation to be tested.
/// \return A PDSCH processor factory if it is successful.
std::shared_ptr<pdsch_processor_factory> create_sw_pdsch_processor_factory(task_executor&     executor,
                                                                           unsigned           max_nof_threads,
                                                                           const std::string& eal_arguments,
                                                                           const std::string& pxsch_type);

/// \brief Creates a PUSCH processor factory.
/// \param[in] executor                  Asynchornous executor.
/// \param[in] max_nof_threads           Maximum number of concurrent threads.
/// \param[in] nof_ldpc_iterations       Maximum number of LDPC decoder iterations.
/// \param[in] pxsch_type                Type of PxSCH implementation to be tested.
/// \param[in] dec_enable_early_stop     Set to true to enable LDPC decoder early stop.
/// \param[in] td_interpolation_strategy Channel estimator time-domain interpolation strategy.
/// \return A PUSCH processor factory if it is successful.
std::shared_ptr<pusch_processor_factory>
create_sw_pusch_processor_factory(task_executor&                                   executor,
                                  unsigned                                         max_nof_threads,
                                  unsigned                                         nof_ldpc_iterations,
                                  bool                                             dec_enable_early_stop,
                                  const std::string&                               pxsch_type,
                                  port_channel_estimator_td_interpolation_strategy td_interpolation_strategy,
                                  channel_equalizer_algorithm_type                 equalizer_algorithm_type,
                                  const pxsch_test_dapp_config&                    dapps = {},
                                  unsigned                                        decoder_max_prb = MAX_NOF_PRBS,
                                  unsigned                                        decoder_max_layers = pusch_constants::MAX_NOF_LAYERS,
                                  channel_state_information::sinr_type sinr_method =
                                      channel_state_information::sinr_type::post_equalization,
                                  unsigned receiver_max_ports = pusch_constants::MAX_NOF_RX_PORTS,
                                  test_support::pusch_cold_trace* cold_trace = nullptr);

} // namespace ocudu
