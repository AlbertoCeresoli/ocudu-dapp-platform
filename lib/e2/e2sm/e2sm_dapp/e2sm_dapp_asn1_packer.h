// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// [srs-sharing] APER packing of E2SM-DAPP (SM ID 255, schemas/e2/e2sm-dapp-v0.asn1) with the asn1c codec generated
// from the same module as wineslab/dApp-flexric, so a FlexRIC near-RT RIC and its xApps decode what this packs.

#include "ocudu/e2/e2sm/e2sm.h"
#include "ocudu/e2/e2sm/e2sm_dapp.h"
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace ocudu {

/// Identity of this E2 node, as the E2SM-DAPP indication headers carry it.
struct e2sm_dapp_node_identity {
  /// node-type: the value OAI's E2SM-DAPP RAN function uses for a gNB-DU (ngran_gNB_DU = 7), so xApps written for
  /// it read OCUDU's headers the same way.
  uint8_t                 node_type = 7;
  /// node-plmn-id: TS 38.413 PLMN Identity octets.
  std::array<uint8_t, 3>  plmn      = {};
  /// node-nb-id: gNB identifier.
  uint32_t                nb_id     = 0;
  /// node-cu-du-id: gNB-DU identifier, when configured.
  std::optional<uint64_t> cu_du_id;
};

class e2sm_dapp_asn1_packer : public e2sm_handler
{
public:
  /// The E2 RAN function ID equals the SM ID: FlexRIC's xApps look the RAN function up by SM_DAPP_ID (255).
  static constexpr uint16_t    ran_func_id = 255;
  static constexpr const char* oid         = "1.3.6.1.4.1.53148.1.1.255.3";
  static constexpr const char* short_name  = "E2SM-DAPP";
  static constexpr const char* description = "E2SM-DAPP for xApp-dApp synchronization";
  /// Report and control style numbers of the E2SM-DAPP RAN function definition.
  static constexpr uint32_t report_style_e3_data          = 1;
  static constexpr uint32_t report_style_subscription_map = 2;
  static constexpr uint32_t control_style_e3_data         = 1;

  /// \param poll_period_ms Indication period handed to the E2 agent: E2SM-DAPP's event trigger carries no period, and
  /// the agent's indication loop is periodic, so queued dApp reports leave at most this late.
  explicit e2sm_dapp_asn1_packer(unsigned poll_period_ms_ = 10) : poll_period_ms(poll_period_ms_) {}

  e2sm_action_definition        handle_packed_e2sm_action_definition(const byte_buffer& buf) override;
  e2sm_event_trigger_definition handle_packed_event_trigger_definition(const byte_buffer& buf) override;
  e2sm_ric_control_request      handle_packed_ric_control_request(const asn1::e2ap::ric_ctrl_request_s& req) override;
  e2_ric_control_response       pack_ric_control_response(const e2sm_ric_control_response& e2sm_response) override;
  asn1::unbounded_octstring<true> pack_ran_function_description() override;

  /// Report style 1: indication header format 1 (node identity, dApp, E3 RAN function) and message format 1 (data).
  static byte_buffer pack_indication_header_format1(const e2sm_dapp_node_identity& node, const e2sm_dapp_report& r);
  static byte_buffer pack_indication_message_format1(const std::vector<uint8_t>& data);
  /// Report style 2: indication header format 2 (node identity) and message format 2 (dApp subscription map).
  static byte_buffer pack_indication_header_format2(const e2sm_dapp_node_identity& node);
  static byte_buffer pack_indication_message_format2(const std::vector<e2sm_dapp_subscription_item>& subscriptions);

private:
  unsigned poll_period_ms;
};

} // namespace ocudu
