// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "e2sm_dapp_asn1_packer.h"
#include <cstdlib>
#include <cstring>
#include <stdexcept>

extern "C" {
#include "E2SMDAPP_DAppE3Subscription-Item.h"
#include "E2SMDAPP_DAppE3Subscription-List.h"
#include "E2SMDAPP_E2SM-DAPP-ActionDefinition.h"
#include "E2SMDAPP_E2SM-DAPP-ControlHeader-Format1.h"
#include "E2SMDAPP_E2SM-DAPP-ControlHeader.h"
#include "E2SMDAPP_E2SM-DAPP-ControlMessage-Format1.h"
#include "E2SMDAPP_E2SM-DAPP-ControlMessage.h"
#include "E2SMDAPP_E2SM-DAPP-ControlOutcome-Format1.h"
#include "E2SMDAPP_E2SM-DAPP-ControlOutcome.h"
#include "E2SMDAPP_E2SM-DAPP-EventTrigger.h"
#include "E2SMDAPP_E2SM-DAPP-IndicationHeader-Format1.h"
#include "E2SMDAPP_E2SM-DAPP-IndicationHeader-Format2.h"
#include "E2SMDAPP_E2SM-DAPP-IndicationHeader.h"
#include "E2SMDAPP_E2SM-DAPP-IndicationMessage-Format1.h"
#include "E2SMDAPP_E2SM-DAPP-IndicationMessage-Format2.h"
#include "E2SMDAPP_E2SM-DAPP-IndicationMessage.h"
#include "E2SMDAPP_E2SM-DAPP-RANFunctionDefinition.h"
#include "E2SMDAPP_RANFunctionDefinition-Control-Item.h"
#include "E2SMDAPP_RANFunctionDefinition-Control.h"
#include "E2SMDAPP_RANFunctionDefinition-Report-Item.h"
#include "E2SMDAPP_RANFunctionDefinition-Report.h"
#include "E2SMDAPP_RANfunction-Name.h"
#include "aper_decoder.h"
#include "aper_encoder.h"
}

using namespace ocudu;

// [srs-sharing] E2SM-DAPP packer (not in upstream OCUDU). The generated structures are plain C: each one built here
// is owned by an asn_owner, which frees the whole tree (ASN_STRUCT_FREE) whatever path leaves the function.

namespace {

/// Bound of data in indication and control messages (maxE2SMDAPPIndMsgSize, maxE2SMDAPPCtrlMsgSize).
constexpr std::size_t maximum_data_bytes = 32768;

template <typename T>
T* make_asn()
{
  auto* value = static_cast<T*>(std::calloc(1, sizeof(T)));
  if (value == nullptr) {
    throw std::bad_alloc();
  }
  return value;
}

template <typename T>
struct asn_owner {
  asn_TYPE_descriptor_t& type;
  T*                     value;
  asn_owner(asn_TYPE_descriptor_t& type_, T* value_) : type(type_), value(value_) {}
  ~asn_owner() { ASN_STRUCT_FREE(type, value); }
  asn_owner(const asn_owner&)            = delete;
  asn_owner& operator=(const asn_owner&) = delete;
  T*         operator->() const { return value; }
};

void set_octets(OCTET_STRING_t& destination, const uint8_t* data, std::size_t size)
{
  if (OCTET_STRING_fromBuf(&destination, reinterpret_cast<const char*>(data), static_cast<int>(size)) != 0) {
    throw std::bad_alloc();
  }
}

void set_text(PrintableString_t& destination, const std::string& text)
{
  set_octets(destination, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::vector<uint8_t> encode(asn_TYPE_descriptor_t& type, const void* value)
{
  void*         buffer = nullptr;
  const ssize_t size   = aper_encode_to_new_buffer(&type, nullptr, value, &buffer);
  if (size < 0 || buffer == nullptr) {
    std::free(buffer);
    throw std::runtime_error(std::string("E2SM-DAPP APER encode failed: ") + type.name);
  }
  std::vector<uint8_t> bytes(static_cast<const uint8_t*>(buffer), static_cast<const uint8_t*>(buffer) + size);
  std::free(buffer);
  return bytes;
}

byte_buffer to_byte_buffer(const std::vector<uint8_t>& bytes)
{
  auto buffer = byte_buffer::create(bytes.begin(), bytes.end());
  return buffer.has_value() ? std::move(buffer.value()) : byte_buffer{};
}

template <typename T>
T* decode(asn_TYPE_descriptor_t& type, const uint8_t* data, std::size_t size)
{
  T*                   value  = nullptr;
  const asn_dec_rval_t result = aper_decode_complete(nullptr, &type, reinterpret_cast<void**>(&value), data, size);
  if (result.code != RC_OK) {
    ASN_STRUCT_FREE(type, value);
    return nullptr;
  }
  return value;
}

std::vector<uint8_t> to_vector(const byte_buffer& buffer)
{
  return std::vector<uint8_t>(buffer.begin(), buffer.end());
}

std::vector<uint8_t> to_vector(const asn1::unbounded_octstring<true>& octets)
{
  return std::vector<uint8_t>(octets.begin(), octets.end());
}

/// Fills the node identity fields shared by indication header formats 1 and 2.
template <typename Header>
void set_node_identity(Header& header, const e2sm_dapp_node_identity& node)
{
  header.node_type = node.node_type;
  set_octets(header.node_plmn_id, node.plmn.data(), node.plmn.size());
  header.node_nb_id = static_cast<long>(node.nb_id);
  if (node.cu_du_id) {
    header.node_cu_du_id  = make_asn<long>();
    *header.node_cu_du_id = static_cast<long>(*node.cu_du_id);
  }
}

} // namespace

e2sm_action_definition e2sm_dapp_asn1_packer::handle_packed_e2sm_action_definition(const byte_buffer& buf)
{
  e2sm_action_definition result;
  result.service_model     = e2sm_service_model_t::DAPP;
  result.action_definition = e2sm_dapp_action_definition{};
  // The action definition only names the report style; format 1 is empty.
  const auto bytes = to_vector(buf);
  auto*      value =
      decode<E2SMDAPP_E2SM_DAPP_ActionDefinition_t>(asn_DEF_E2SMDAPP_E2SM_DAPP_ActionDefinition, bytes.data(), bytes.size());
  if (value == nullptr) {
    result.service_model = e2sm_service_model_t::UNKNOWN_SM;
    return result;
  }
  asn_owner<E2SMDAPP_E2SM_DAPP_ActionDefinition_t> owner(asn_DEF_E2SMDAPP_E2SM_DAPP_ActionDefinition, value);
  std::get<e2sm_dapp_action_definition>(result.action_definition).ric_style_type =
      static_cast<uint32_t>(owner->ric_Style_Type);
  return result;
}

e2sm_event_trigger_definition e2sm_dapp_asn1_packer::handle_packed_event_trigger_definition(const byte_buffer&)
{
  // E2SM-DAPP-EventTrigger format 1 is empty: reports are event-driven (one E3 report, one indication). The E2 agent
  // runs indications on a period, so the module polls its queue at poll_period_ms.
  e2sm_event_trigger_definition result;
  result.ric_service_type = e2sm_event_trigger_definition::e2sm_ric_service_type_t::REPORT;
  result.report_period    = poll_period_ms;
  return result;
}

e2sm_ric_control_request
e2sm_dapp_asn1_packer::handle_packed_ric_control_request(const asn1::e2ap::ric_ctrl_request_s& req)
{
  e2sm_ric_control_request result;
  result.service_model = e2sm_service_model_t::UNKNOWN_SM;
  // FlexRIC's xApp control call waits for the outcome, and the E2 agent only answers a control that asks for an
  // acknowledge: always answer E2SM-DAPP controls.
  result.ric_ctrl_ack_request_present = true;
  result.ric_ctrl_ack_request         = true;
  result.ric_call_process_id_present  = false;

  const auto header_bytes  = to_vector(req->ric_ctrl_hdr);
  const auto message_bytes = to_vector(req->ric_ctrl_msg);
  auto*      header        = decode<E2SMDAPP_E2SM_DAPP_ControlHeader_t>(
      asn_DEF_E2SMDAPP_E2SM_DAPP_ControlHeader, header_bytes.data(), header_bytes.size());
  if (header == nullptr) {
    return result;
  }
  asn_owner<E2SMDAPP_E2SM_DAPP_ControlHeader_t> header_owner(asn_DEF_E2SMDAPP_E2SM_DAPP_ControlHeader, header);
  auto* message = decode<E2SMDAPP_E2SM_DAPP_ControlMessage_t>(
      asn_DEF_E2SMDAPP_E2SM_DAPP_ControlMessage, message_bytes.data(), message_bytes.size());
  if (message == nullptr) {
    return result;
  }
  asn_owner<E2SMDAPP_E2SM_DAPP_ControlMessage_t> message_owner(asn_DEF_E2SMDAPP_E2SM_DAPP_ControlMessage, message);
  if (header->ric_controlHeader_formats.present !=
          E2SMDAPP_E2SM_DAPP_ControlHeader__ric_controlHeader_formats_PR_controlHeader_Format1 ||
      message->ric_controlMessage_formats.present !=
          E2SMDAPP_E2SM_DAPP_ControlMessage__ric_controlMessage_formats_PR_controlMessage_Format1) {
    return result;
  }
  const auto& h = *header->ric_controlHeader_formats.choice.controlHeader_Format1;
  const auto& m = *message->ric_controlMessage_formats.choice.controlMessage_Format1;
  e2sm_dapp_ctrl_hdr ctrl_hdr;
  ctrl_hdr.e3_ran_function_id = static_cast<uint32_t>(h.ran_function_id);
  ctrl_hdr.dapp_id            = static_cast<uint32_t>(h.dapp_id);
  e2sm_dapp_ctrl_msg ctrl_msg;
  ctrl_msg.data.assign(m.data.buf, m.data.buf + m.data.size);
  result.service_model    = e2sm_service_model_t::DAPP;
  result.request_ctrl_hdr = ctrl_hdr;
  result.request_ctrl_msg = std::move(ctrl_msg);
  return result;
}

e2_ric_control_response e2sm_dapp_asn1_packer::pack_ric_control_response(const e2sm_ric_control_response& e2sm_response)
{
  e2_ric_control_response response = {};
  response.success                 = e2sm_response.success;
  // E2SM-DAPP-ControlOutcome format 1 is empty, but FlexRIC's RIC side decodes an outcome, as its own agent sends one.
  std::vector<uint8_t> outcome;
  try {
    asn_owner<E2SMDAPP_E2SM_DAPP_ControlOutcome_t> value(asn_DEF_E2SMDAPP_E2SM_DAPP_ControlOutcome,
                                                         make_asn<E2SMDAPP_E2SM_DAPP_ControlOutcome_t>());
    value->ric_controlOutcome_formats.present =
        E2SMDAPP_E2SM_DAPP_ControlOutcome__ric_controlOutcome_formats_PR_controlOutcome_Format1;
    value->ric_controlOutcome_formats.choice.controlOutcome_Format1 =
        make_asn<E2SMDAPP_E2SM_DAPP_ControlOutcome_Format1_t>();
    outcome = encode(asn_DEF_E2SMDAPP_E2SM_DAPP_ControlOutcome, value.value);
  } catch (...) {
    outcome.clear();
  }
  if (response.success) {
    if (!outcome.empty() && response.ack->ric_ctrl_outcome.resize(outcome.size())) {
      response.ack->ric_ctrl_outcome_present = true;
      std::copy(outcome.begin(), outcome.end(), response.ack->ric_ctrl_outcome.begin());
    }
  } else {
    response.failure->cause = e2sm_response.cause;
  }
  return response;
}

asn1::unbounded_octstring<true> e2sm_dapp_asn1_packer::pack_ran_function_description()
{
  // Same definition as OAI's E2SM-DAPP RAN function (ran_func_dapp.c): report styles 1 and 2, control style 1, no
  // event trigger styles. The dApp subscription map is not attached here (no dApp is connected at E2 setup); xApps
  // follow it through report style 2.
  asn1::unbounded_octstring<true> result;
  try {
    asn_owner<E2SMDAPP_E2SM_DAPP_RANFunctionDefinition_t> value(asn_DEF_E2SMDAPP_E2SM_DAPP_RANFunctionDefinition,
                                                                make_asn<E2SMDAPP_E2SM_DAPP_RANFunctionDefinition_t>());
    value->ranFunction_Name = make_asn<E2SMDAPP_RANfunction_Name_t>();
    set_text(value->ranFunction_Name->ranFunction_ShortName, short_name);
    set_text(value->ranFunction_Name->ranFunction_E2SM_OID, oid);
    set_text(value->ranFunction_Name->ranFunction_Description, description);

    value->ranFunctionDefinition_Report = make_asn<E2SMDAPP_RANFunctionDefinition_Report_t>();
    const auto add_report_style = [&](uint32_t style, const std::string& name, long format) {
      auto* item                             = make_asn<E2SMDAPP_RANFunctionDefinition_Report_Item_t>();
      item->ric_ReportStyle_Type             = style;
      item->ric_IndicationHeaderFormat_Type  = format;
      item->ric_IndicationMessageFormat_Type = format;
      set_text(item->ric_ReportStyle_Name, name);
      if (ASN_SEQUENCE_ADD(&value->ranFunctionDefinition_Report->ric_ReportStyle_List.list, item) != 0) {
        ASN_STRUCT_FREE(asn_DEF_E2SMDAPP_RANFunctionDefinition_Report_Item, item);
        throw std::bad_alloc();
      }
    };
    add_report_style(report_style_e3_data, "DAPP-E3-DATA-REPORT", 1);
    add_report_style(report_style_subscription_map, "DAPP-E3-SUBSCRIPTION-MAP", 2);

    value->ranFunctionDefinition_Control = make_asn<E2SMDAPP_RANFunctionDefinition_Control_t>();
    auto* control                          = make_asn<E2SMDAPP_RANFunctionDefinition_Control_Item_t>();
    control->ric_ControlStyle_Type         = control_style_e3_data;
    control->ric_ControlHeaderFormat_Type  = 1;
    control->ric_ControlMessageFormat_Type = 1;
    control->ric_ControlOutcomeFormat_Type = 1;
    set_text(control->ric_ControlStyle_Name, "DAPP-CONTROL-STYLE-1");
    if (ASN_SEQUENCE_ADD(&value->ranFunctionDefinition_Control->ric_ControlStyle_List.list, control) != 0) {
      ASN_STRUCT_FREE(asn_DEF_E2SMDAPP_RANFunctionDefinition_Control_Item, control);
      throw std::bad_alloc();
    }

    const auto bytes = encode(asn_DEF_E2SMDAPP_E2SM_DAPP_RANFunctionDefinition, value.value);
    if (result.resize(bytes.size())) {
      std::copy(bytes.begin(), bytes.end(), result.begin());
    }
  } catch (...) {
    result.clear();
  }
  return result;
}

byte_buffer e2sm_dapp_asn1_packer::pack_indication_header_format1(const e2sm_dapp_node_identity& node,
                                                                 const e2sm_dapp_report&        report)
{
  try {
    asn_owner<E2SMDAPP_E2SM_DAPP_IndicationHeader_t> value(asn_DEF_E2SMDAPP_E2SM_DAPP_IndicationHeader,
                                                           make_asn<E2SMDAPP_E2SM_DAPP_IndicationHeader_t>());
    value->ric_indicationHeader_formats.present =
        E2SMDAPP_E2SM_DAPP_IndicationHeader__ric_indicationHeader_formats_PR_indicationHeader_Format1;
    auto* header = value->ric_indicationHeader_formats.choice.indicationHeader_Format1 =
        make_asn<E2SMDAPP_E2SM_DAPP_IndicationHeader_Format1_t>();
    header->ran_function_id = static_cast<long>(report.e3_ran_function_id);
    header->dapp_id         = static_cast<long>(report.dapp_id);
    set_node_identity(*header, node);
    return to_byte_buffer(encode(asn_DEF_E2SMDAPP_E2SM_DAPP_IndicationHeader, value.value));
  } catch (...) {
    return {};
  }
}

byte_buffer e2sm_dapp_asn1_packer::pack_indication_message_format1(const std::vector<uint8_t>& data)
{
  // data is OCTET STRING (SIZE(1..maxE2SMDAPPIndMsgSize)): an empty or oversized report cannot be carried.
  if (data.empty() || data.size() > maximum_data_bytes) {
    return {};
  }
  try {
    asn_owner<E2SMDAPP_E2SM_DAPP_IndicationMessage_t> value(asn_DEF_E2SMDAPP_E2SM_DAPP_IndicationMessage,
                                                            make_asn<E2SMDAPP_E2SM_DAPP_IndicationMessage_t>());
    value->ric_indicationMessage_formats.present =
        E2SMDAPP_E2SM_DAPP_IndicationMessage__ric_indicationMessage_formats_PR_indicationMessage_Format1;
    auto* message = value->ric_indicationMessage_formats.choice.indicationMessage_Format1 =
        make_asn<E2SMDAPP_E2SM_DAPP_IndicationMessage_Format1_t>();
    message->data_size = static_cast<long>(data.size());
    set_octets(message->data, data.data(), data.size());
    return to_byte_buffer(encode(asn_DEF_E2SMDAPP_E2SM_DAPP_IndicationMessage, value.value));
  } catch (...) {
    return {};
  }
}

byte_buffer e2sm_dapp_asn1_packer::pack_indication_header_format2(const e2sm_dapp_node_identity& node)
{
  try {
    asn_owner<E2SMDAPP_E2SM_DAPP_IndicationHeader_t> value(asn_DEF_E2SMDAPP_E2SM_DAPP_IndicationHeader,
                                                           make_asn<E2SMDAPP_E2SM_DAPP_IndicationHeader_t>());
    value->ric_indicationHeader_formats.present =
        E2SMDAPP_E2SM_DAPP_IndicationHeader__ric_indicationHeader_formats_PR_indicationHeader_Format2;
    auto* header = value->ric_indicationHeader_formats.choice.indicationHeader_Format2 =
        make_asn<E2SMDAPP_E2SM_DAPP_IndicationHeader_Format2_t>();
    set_node_identity(*header, node);
    return to_byte_buffer(encode(asn_DEF_E2SMDAPP_E2SM_DAPP_IndicationHeader, value.value));
  } catch (...) {
    return {};
  }
}

byte_buffer
e2sm_dapp_asn1_packer::pack_indication_message_format2(const std::vector<e2sm_dapp_subscription_item>& subscriptions)
{
  try {
    asn_owner<E2SMDAPP_E2SM_DAPP_IndicationMessage_t> value(asn_DEF_E2SMDAPP_E2SM_DAPP_IndicationMessage,
                                                            make_asn<E2SMDAPP_E2SM_DAPP_IndicationMessage_t>());
    value->ric_indicationMessage_formats.present =
        E2SMDAPP_E2SM_DAPP_IndicationMessage__ric_indicationMessage_formats_PR_indicationMessage_Format2;
    auto* message = value->ric_indicationMessage_formats.choice.indicationMessage_Format2 =
        make_asn<E2SMDAPP_E2SM_DAPP_IndicationMessage_Format2_t>();
    message->dappE3Subscriptions = make_asn<E2SMDAPP_DAppE3Subscription_List_t>();
    for (const auto& subscription : subscriptions) {
      // subscribed-e3-ran-functions is SIZE(1..64): a dApp with no E3 subscription yet cannot be listed.
      if (subscription.e3_ran_function_ids.empty()) {
        continue;
      }
      auto* item    = make_asn<E2SMDAPP_DAppE3Subscription_Item_t>();
      item->dapp_id = static_cast<long>(subscription.dapp_id);
      for (std::size_t i = 0; i != subscription.e3_ran_function_ids.size() && i != 64; ++i) {
        auto* id = make_asn<E2SMDAPP_SubscribedE3RANFunction_ID_t>();
        *id      = static_cast<long>(subscription.e3_ran_function_ids[i]);
        if (ASN_SEQUENCE_ADD(&item->subscribed_e3_ran_functions.list, id) != 0) {
          std::free(id);
          ASN_STRUCT_FREE(asn_DEF_E2SMDAPP_DAppE3Subscription_Item, item);
          throw std::bad_alloc();
        }
      }
      if (ASN_SEQUENCE_ADD(&message->dappE3Subscriptions->list, item) != 0) {
        ASN_STRUCT_FREE(asn_DEF_E2SMDAPP_DAppE3Subscription_Item, item);
        throw std::bad_alloc();
      }
    }
    return to_byte_buffer(encode(asn_DEF_E2SMDAPP_E2SM_DAPP_IndicationMessage, value.value));
  } catch (...) {
    return {};
  }
}
