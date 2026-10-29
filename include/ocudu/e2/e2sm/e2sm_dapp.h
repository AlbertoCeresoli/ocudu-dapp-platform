// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// [srs-sharing] E2SM-DAPP (SM ID 255) bridge between the E2 agent and the dApps on E3 (not in upstream OCUDU).
//
// E2SM-DAPP (schemas/e2/e2sm-dapp-v0.asn1, from wineslab/dApp-flexric) carries dApp data between E3 and E2 without
// interpreting it: an E3 dApp report becomes an E2 indication (report style 1) whose header names the dApp and the E3
// RAN function, and an E2 control (control style 1) becomes an E3 xApp control action for the dApp it names. Report
// style 2 tells xApps which dApps are connected and which E3 RAN functions each is subscribed to.
//
// The E2 library only sees this interface; the dApp service implements it on top of its E3 server, so lib/e2 needs
// no E3 code and a gNB without dApps simply has no bridge.

#include <cstdint>
#include <functional>
#include <vector>

namespace ocudu {

/// One E3 dApp report, as the E2 indication of report style 1 carries it.
struct e2sm_dapp_report {
  /// dApp identifier assigned at E3 setup.
  uint32_t dapp_id = 0;
  /// E3 RAN function the report was sent on (E2SM-DAPP calls it ran-function-id).
  uint32_t e3_ran_function_id = 0;
  /// Report payload, opaque to E2 (an E3 service model message).
  std::vector<uint8_t> data;
};

/// One connected dApp and the E3 RAN functions it is subscribed to (report style 2).
struct e2sm_dapp_subscription_item {
  uint32_t              dapp_id = 0;
  std::vector<uint32_t> e3_ran_function_ids;

  bool operator==(const e2sm_dapp_subscription_item& other) const
  {
    return dapp_id == other.dapp_id && e3_ran_function_ids == other.e3_ran_function_ids;
  }
};

/// E3 side of the E2SM-DAPP bridge, implemented by the dApp service.
class e2sm_dapp_bridge
{
public:
  virtual ~e2sm_dapp_bridge() = default;

  /// Installs the consumer of every E3 dApp report from now on (replaces any previous one). Called once by the
  /// E2SM-DAPP module; the listener runs on the E3 server's thread and must not block.
  virtual void set_report_listener(std::function<void(e2sm_dapp_report)> listener) = 0;

  /// Sends an xApp control (E2SM-DAPP control message data) to dApp \p dapp_id on E3 RAN function
  /// \p e3_ran_function_id. Returns false when the dApp is not connected or the E3 queue is full.
  virtual bool send_control(uint32_t dapp_id, uint32_t e3_ran_function_id, std::vector<uint8_t> data) = 0;

  /// Connected dApps and their E3 subscriptions, ordered by dApp identifier.
  virtual std::vector<e2sm_dapp_subscription_item> subscriptions() const = 0;
};

} // namespace ocudu
