// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "management_protocol.h"
#include <memory>
#include <vector>

namespace ocudu {
class quiet_reservation_manager;
}

namespace ocudu {
class ru_gain_controller;
}

namespace ocudu::dapp::e3 {

class embedded_agent;
class portable_data_store;
class e3_connection_registry;

/// Thread-safe admission boundary for mutations owned by the wider gNB.
/// Implementations enqueue onto the component's control executor and must not
/// call CU/DU state directly from an E3 transport thread.
class ran_control_handler
{
public:
  virtual ~ran_control_handler()                                                = default;
  virtual bool request_ue_release(uint16_t serving_pci, uint16_t rnti) noexcept = 0;
  /// Handover the UE to the cell with \c target_pci (PLMN/TAC resolved by the host).
  virtual bool request_handover(uint16_t /*serving_pci*/, uint16_t /*rnti*/, uint16_t /*target_pci*/) noexcept
  {
    return false;
  }
  /// Radio-unit gain controls; return false when the RU has no gain controller.
  virtual bool set_tx_gain(uint8_t /*port_id*/, double /*gain_db*/) noexcept { return false; }
  virtual bool set_rx_gain(uint8_t /*port_id*/, double /*gain_db*/) noexcept { return false; }
  /// Served cells as the host knows them (configuration plus live gains).
  virtual std::vector<cell_descriptor> cells() const noexcept { return {}; }
  virtual ran_control_capabilities     capabilities() const noexcept { return {true, false, false, false}; }
  /// Host bindings supplied once the radio unit and cell configuration exist.
  virtual void attach_ru_gain_controller(ru_gain_controller* /*controller*/) noexcept {}
  virtual void set_cells(std::vector<cell_descriptor> /*cells*/) noexcept {}
};

/// Validates and dispatches one decoded management service-model request.
/// This layer is shared by local control and E3AP transport adapters.
class management_service
{
public:
  explicit management_service(embedded_agent&                            agent_,
                              std::shared_ptr<quiet_reservation_manager> reservations_  = {},
                              std::shared_ptr<portable_data_store>       portable_data_ = {},
                              std::shared_ptr<e3_connection_registry>    connections_   = {},
                              std::shared_ptr<ran_control_handler>       ran_control_   = {}) :
    agent(agent_),
    reservations(std::move(reservations_)),
    portable_data(std::move(portable_data_)),
    connections(std::move(connections_)),
    ran_actions(std::move(ran_control_))
  {
  }

  management_response handle(const management_request& request) noexcept;

private:
  embedded_agent&                            agent;
  std::shared_ptr<quiet_reservation_manager> reservations;
  std::shared_ptr<portable_data_store>       portable_data;
  std::shared_ptr<e3_connection_registry>    connections;
  std::shared_ptr<ran_control_handler>       ran_actions;
};

} // namespace ocudu::dapp::e3
