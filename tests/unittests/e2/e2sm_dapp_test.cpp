// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// [srs-sharing] E2SM-DAPP service model tests (not in upstream OCUDU).
//
// The reference bytes below come from wineslab/dApp-flexric's own E2SM-DAPP ASN.1 codec (src/sm/dapp_sm,
// dapp_enc_asn.c), so these tests pin wire compatibility with a FlexRIC near-RT RIC and its xApps: the messages an
// xApp sends must decode here, and the indications packed here must equal what FlexRIC (and OAI's agent) produce.

#include "lib/e2/e2sm/e2sm_dapp/e2sm_dapp_asn1_packer.h"
#include "lib/e2/e2sm/e2sm_dapp/e2sm_dapp_impl.h"
#include "ocudu/support/async/async_test_utils.h"
#include <gtest/gtest.h>
#include <mutex>

using namespace ocudu;

namespace {

std::vector<uint8_t> from_hex(const std::string& hex)
{
  std::vector<uint8_t> bytes;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  }
  return bytes;
}

std::string to_hex(const byte_buffer& buf)
{
  static constexpr char digits[] = "0123456789abcdef";
  std::string           hex;
  for (uint8_t byte : buf) {
    hex.push_back(digits[byte >> 4U]);
    hex.push_back(digits[byte & 0xfU]);
  }
  return hex;
}

byte_buffer to_buffer(const std::string& hex)
{
  auto bytes = from_hex(hex);
  return byte_buffer::create(bytes).value();
}

// FlexRIC encodings (dapp_enc_asn.c) of the values used throughout.
constexpr const char* flexric_event_trigger     = "00";
constexpr const char* flexric_action_def_style1 = "00010100";
constexpr const char* flexric_action_def_style2 = "00010200";
// Control header {ran-function-id 7, dapp-id 3}, control message {data 01 02 03 04}.
constexpr const char* flexric_ctrl_hdr = "00070003";
constexpr const char* flexric_ctrl_msg = "000004000301020304";
constexpr const char* flexric_ctrl_out = "00";
// Indication header format 1 {ran-function-id 7, dapp-id 3, node-type 7, PLMN 00 f1 10, nb-id 411, cu-du-id 1}.
constexpr const char* flexric_ind_hdr_fmt1 = "08070003000700f11020019b0001";
// Indication header format 2 {node-type 7, PLMN 00 f1 10, nb-id 411}, without cu-du-id.
constexpr const char* flexric_ind_hdr_fmt2 = "200700f11020019b";
constexpr const char* flexric_ind_msg_fmt1 = "000004000301020304";
// Indication message format 2 {dApp 3: E3 RAN functions 1, 2; dApp 9: E3 RAN function 5}.
constexpr const char* flexric_ind_msg_fmt2 = "200002000304000100020009000005";

e2sm_dapp_node_identity test_node(bool with_cu_du_id)
{
  e2sm_dapp_node_identity node;
  node.plmn  = {0x00, 0xf1, 0x10};
  node.nb_id = 411;
  if (with_cu_du_id) {
    node.cu_du_id = 1;
  }
  return node;
}

asn1::e2ap::ric_ctrl_request_s make_control_request(const std::string& header_hex, const std::string& message_hex)
{
  asn1::e2ap::ric_ctrl_request_s request;
  request->ric_ctrl_hdr.from_string(header_hex);
  request->ric_ctrl_msg.from_string(message_hex);
  return request;
}

/// E3 side of the bridge: records controls and lets the test inject reports and subscription maps.
class fake_dapp_bridge : public e2sm_dapp_bridge
{
public:
  void set_report_listener(std::function<void(e2sm_dapp_report)> listener_) override { listener = std::move(listener_); }

  bool send_control(uint32_t dapp_id, uint32_t e3_ran_function_id, std::vector<uint8_t> data) override
  {
    controls.push_back({dapp_id, e3_ran_function_id, std::move(data)});
    return accept_controls;
  }

  std::vector<e2sm_dapp_subscription_item> subscriptions() const override
  {
    std::lock_guard<std::mutex> lock(mutex);
    return map;
  }

  void report(e2sm_dapp_report r)
  {
    ASSERT_TRUE(listener);
    listener(std::move(r));
  }

  void set_subscriptions(std::vector<e2sm_dapp_subscription_item> items)
  {
    std::lock_guard<std::mutex> lock(mutex);
    map = std::move(items);
  }

  std::function<void(e2sm_dapp_report)>    listener;
  std::vector<e2sm_dapp_report>            controls;
  bool                                     accept_controls = true;
  mutable std::mutex                       mutex;
  std::vector<e2sm_dapp_subscription_item> map;
};

asn1::e2ap::ric_action_to_be_setup_item_s make_action(asn1::e2ap::ric_action_type_e type, const std::string& definition)
{
  asn1::e2ap::ric_action_to_be_setup_item_s action;
  action.ric_action_id   = 1;
  action.ric_action_type = type;
  auto bytes             = from_hex(definition);
  EXPECT_TRUE(action.ric_action_definition.append(bytes));
  return action;
}

} // namespace

// --- Packer against FlexRIC's encodings --------------------------------------------------------------------------

TEST(e2sm_dapp_asn1_packer_test, decodes_flexric_event_trigger_as_periodic_report)
{
  e2sm_dapp_asn1_packer packer(25);
  auto                  trigger = packer.handle_packed_event_trigger_definition(to_buffer(flexric_event_trigger));
  EXPECT_EQ(trigger.ric_service_type, e2sm_event_trigger_definition::REPORT);
  // E2SM-DAPP carries no period; the packer hands the agent its own poll period.
  EXPECT_EQ(trigger.report_period, 25U);
}

TEST(e2sm_dapp_asn1_packer_test, decodes_flexric_action_definitions)
{
  e2sm_dapp_asn1_packer packer;
  for (auto [hex, style] : {std::pair{flexric_action_def_style1, 1U}, std::pair{flexric_action_def_style2, 2U}}) {
    auto definition = packer.handle_packed_e2sm_action_definition(to_buffer(hex));
    ASSERT_EQ(definition.service_model, e2sm_service_model_t::DAPP);
    EXPECT_EQ(std::get<e2sm_dapp_action_definition>(definition.action_definition).ric_style_type, style);
  }
}

TEST(e2sm_dapp_asn1_packer_test, rejects_undecodable_action_definition)
{
  e2sm_dapp_asn1_packer packer;
  auto                  definition = packer.handle_packed_e2sm_action_definition(to_buffer("ff"));
  EXPECT_NE(definition.service_model, e2sm_service_model_t::DAPP);
}

TEST(e2sm_dapp_asn1_packer_test, decodes_flexric_control_request_and_always_asks_for_an_ack)
{
  e2sm_dapp_asn1_packer packer;
  auto request = packer.handle_packed_ric_control_request(make_control_request(flexric_ctrl_hdr, flexric_ctrl_msg));
  ASSERT_EQ(request.service_model, e2sm_service_model_t::DAPP);
  const auto& header = std::get<e2sm_dapp_ctrl_hdr>(request.request_ctrl_hdr);
  EXPECT_EQ(header.e3_ran_function_id, 7U);
  EXPECT_EQ(header.dapp_id, 3U);
  EXPECT_EQ(std::get<e2sm_dapp_ctrl_msg>(request.request_ctrl_msg).data, (std::vector<uint8_t>{1, 2, 3, 4}));
  // FlexRIC's xApp waits for the outcome, which the agent only sends when asked to acknowledge.
  EXPECT_TRUE(request.ric_ctrl_ack_request_present);
  EXPECT_TRUE(request.ric_ctrl_ack_request);
}

TEST(e2sm_dapp_asn1_packer_test, rejects_control_request_with_undecodable_header)
{
  e2sm_dapp_asn1_packer packer;
  auto request = packer.handle_packed_ric_control_request(make_control_request("ff", flexric_ctrl_msg));
  EXPECT_NE(request.service_model, e2sm_service_model_t::DAPP);
}

TEST(e2sm_dapp_asn1_packer_test, control_ack_carries_flexric_control_outcome)
{
  e2sm_dapp_asn1_packer     packer;
  e2sm_ric_control_response e2sm_response;
  e2sm_response.service_model = e2sm_service_model_t::DAPP;
  e2sm_response.success       = true;
  auto response               = packer.pack_ric_control_response(e2sm_response);
  ASSERT_TRUE(response.success);
  ASSERT_TRUE(response.ack->ric_ctrl_outcome_present);
  EXPECT_EQ(response.ack->ric_ctrl_outcome.to_string(), flexric_ctrl_out);
}

TEST(e2sm_dapp_asn1_packer_test, indication_header_format1_matches_flexric)
{
  e2sm_dapp_report report;
  report.dapp_id            = 3;
  report.e3_ran_function_id = 7;
  EXPECT_EQ(to_hex(e2sm_dapp_asn1_packer::pack_indication_header_format1(test_node(true), report)),
            flexric_ind_hdr_fmt1);
}

TEST(e2sm_dapp_asn1_packer_test, indication_header_format2_matches_flexric)
{
  EXPECT_EQ(to_hex(e2sm_dapp_asn1_packer::pack_indication_header_format2(test_node(false))), flexric_ind_hdr_fmt2);
}

TEST(e2sm_dapp_asn1_packer_test, indication_message_format1_matches_flexric)
{
  EXPECT_EQ(to_hex(e2sm_dapp_asn1_packer::pack_indication_message_format1({1, 2, 3, 4})), flexric_ind_msg_fmt1);
}

TEST(e2sm_dapp_asn1_packer_test, indication_message_format1_drops_data_it_cannot_carry)
{
  EXPECT_TRUE(e2sm_dapp_asn1_packer::pack_indication_message_format1({}).empty());
  EXPECT_TRUE(e2sm_dapp_asn1_packer::pack_indication_message_format1(std::vector<uint8_t>(32769, 0)).empty());
  EXPECT_FALSE(e2sm_dapp_asn1_packer::pack_indication_message_format1(std::vector<uint8_t>(32768, 0)).empty());
}

TEST(e2sm_dapp_asn1_packer_test, indication_message_format2_matches_flexric)
{
  EXPECT_EQ(to_hex(e2sm_dapp_asn1_packer::pack_indication_message_format2({{3, {1, 2}}, {9, {5}}})),
            flexric_ind_msg_fmt2);
}

TEST(e2sm_dapp_asn1_packer_test, ran_function_description_is_packed)
{
  e2sm_dapp_asn1_packer packer;
  EXPECT_GT(packer.pack_ran_function_description().size(), 0U);
}

// --- Service model --------------------------------------------------------------------------------------------

class e2sm_dapp_impl_test : public ::testing::Test
{
protected:
  ocudulog::basic_logger&           logger = ocudulog::fetch_basic_logger("E2SM-DAPP");
  e2sm_dapp_asn1_packer             packer;
  std::shared_ptr<fake_dapp_bridge> bridge = std::make_shared<fake_dapp_bridge>();
  e2sm_dapp_impl                    e2sm{logger, packer, bridge, test_node(true)};
};

TEST_F(e2sm_dapp_impl_test, supports_report_styles_1_and_2_only)
{
  EXPECT_TRUE(e2sm.action_supported(make_action(asn1::e2ap::ric_action_type_e::report, flexric_action_def_style1)));
  EXPECT_TRUE(e2sm.action_supported(make_action(asn1::e2ap::ric_action_type_e::report, flexric_action_def_style2)));
  EXPECT_FALSE(e2sm.action_supported(make_action(asn1::e2ap::ric_action_type_e::report, "00010300")));
  EXPECT_FALSE(e2sm.action_supported(make_action(asn1::e2ap::ric_action_type_e::insert, flexric_action_def_style1)));
}

TEST_F(e2sm_dapp_impl_test, report_style1_sends_one_queued_dapp_report_per_poll)
{
  auto service = e2sm.get_e2sm_report_service(to_buffer(flexric_action_def_style1));
  ASSERT_NE(service, nullptr);
  ASSERT_TRUE(service->collect_measurements());
  EXPECT_FALSE(service->is_ind_msg_ready());

  bridge->report({3, 7, {1, 2, 3, 4}});
  bridge->report({3, 7, {5}});
  ASSERT_TRUE(service->collect_measurements());
  ASSERT_TRUE(service->is_ind_msg_ready());
  EXPECT_EQ(to_hex(service->get_indication_header()), flexric_ind_hdr_fmt1);
  EXPECT_EQ(to_hex(service->get_indication_message()), flexric_ind_msg_fmt1);

  ASSERT_TRUE(service->collect_measurements());
  ASSERT_TRUE(service->is_ind_msg_ready());
  EXPECT_EQ(to_hex(service->get_indication_message()),
            to_hex(e2sm_dapp_asn1_packer::pack_indication_message_format1({5})));

  ASSERT_TRUE(service->collect_measurements());
  EXPECT_FALSE(service->is_ind_msg_ready());
}

TEST_F(e2sm_dapp_impl_test, report_style1_skips_reports_it_cannot_carry)
{
  auto service = e2sm.get_e2sm_report_service(to_buffer(flexric_action_def_style1));
  ASSERT_NE(service, nullptr);
  bridge->report({3, 7, {}});
  bridge->report({3, 7, {1, 2, 3, 4}});
  ASSERT_TRUE(service->collect_measurements());
  ASSERT_TRUE(service->is_ind_msg_ready());
  EXPECT_EQ(to_hex(service->get_indication_message()), flexric_ind_msg_fmt1);
}

TEST_F(e2sm_dapp_impl_test, report_style1_fans_reports_out_to_every_action)
{
  auto first  = e2sm.get_e2sm_report_service(to_buffer(flexric_action_def_style1));
  auto second = e2sm.get_e2sm_report_service(to_buffer(flexric_action_def_style1));
  bridge->report({3, 7, {1, 2, 3, 4}});
  for (auto* service : {first.get(), second.get()}) {
    ASSERT_TRUE(service->collect_measurements());
    EXPECT_TRUE(service->is_ind_msg_ready());
  }
  // A removed action no longer receives reports.
  second.reset();
  bridge->report({3, 7, {1, 2, 3, 4}});
  ASSERT_TRUE(first->collect_measurements());
  EXPECT_TRUE(first->is_ind_msg_ready());
}

TEST_F(e2sm_dapp_impl_test, report_style2_sends_the_subscription_map_when_it_changes)
{
  auto service = e2sm.get_e2sm_report_service(to_buffer(flexric_action_def_style2));
  ASSERT_NE(service, nullptr);
  bridge->set_subscriptions({{3, {1, 2}}, {9, {5}}});

  ASSERT_TRUE(service->collect_measurements());
  ASSERT_TRUE(service->is_ind_msg_ready());
  EXPECT_EQ(to_hex(service->get_indication_message()), flexric_ind_msg_fmt2);
  EXPECT_EQ(to_hex(service->get_indication_header()),
            to_hex(e2sm_dapp_asn1_packer::pack_indication_header_format2(test_node(true))));

  ASSERT_TRUE(service->collect_measurements());
  EXPECT_FALSE(service->is_ind_msg_ready());

  bridge->set_subscriptions({{3, {1, 2}}});
  ASSERT_TRUE(service->collect_measurements());
  EXPECT_TRUE(service->is_ind_msg_ready());
}

TEST_F(e2sm_dapp_impl_test, control_reaches_the_named_dapp)
{
  auto request = packer.handle_packed_ric_control_request(make_control_request(flexric_ctrl_hdr, flexric_ctrl_msg));
  auto* control = e2sm.get_e2sm_control_service(request);
  ASSERT_NE(control, nullptr);
  ASSERT_TRUE(control->control_request_supported(request));

  async_task<e2sm_ric_control_response>         task = control->execute_control_request(request);
  lazy_task_launcher<e2sm_ric_control_response> launcher(task);
  ASSERT_TRUE(task.ready());
  EXPECT_TRUE(task.get().success);
  ASSERT_EQ(bridge->controls.size(), 1U);
  EXPECT_EQ(bridge->controls[0].dapp_id, 3U);
  EXPECT_EQ(bridge->controls[0].e3_ran_function_id, 7U);
  EXPECT_EQ(bridge->controls[0].data, (std::vector<uint8_t>{1, 2, 3, 4}));
}

TEST_F(e2sm_dapp_impl_test, control_fails_when_the_dapp_is_unreachable)
{
  bridge->accept_controls = false;
  auto request = packer.handle_packed_ric_control_request(make_control_request(flexric_ctrl_hdr, flexric_ctrl_msg));
  auto* control = e2sm.get_e2sm_control_service(request);
  ASSERT_NE(control, nullptr);

  async_task<e2sm_ric_control_response>         task = control->execute_control_request(request);
  lazy_task_launcher<e2sm_ric_control_response> launcher(task);
  ASSERT_TRUE(task.ready());
  EXPECT_FALSE(task.get().success);
}
