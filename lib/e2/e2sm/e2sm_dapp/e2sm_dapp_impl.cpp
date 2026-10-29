// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "e2sm_dapp_impl.h"
#include "ocudu/support/async/async_no_op_task.h"
#include <algorithm>

using namespace ocudu;

// [srs-sharing] E2SM-DAPP service model (not in upstream OCUDU).

void e2sm_dapp_report_queue::push(const e2sm_dapp_report& report)
{
  std::lock_guard<std::mutex> lock(mutex);
  if (reports.size() >= capacity) {
    reports.pop_front();
    ++nof_dropped;
  }
  reports.push_back(report);
}

std::optional<e2sm_dapp_report> e2sm_dapp_report_queue::pop()
{
  std::lock_guard<std::mutex> lock(mutex);
  if (reports.empty()) {
    return std::nullopt;
  }
  e2sm_dapp_report report = std::move(reports.front());
  reports.pop_front();
  return report;
}

uint64_t e2sm_dapp_report_queue::dropped() const
{
  std::lock_guard<std::mutex> lock(mutex);
  return nof_dropped;
}

void e2sm_dapp_report_hub::publish(const e2sm_dapp_report& report)
{
  std::lock_guard<std::mutex> lock(mutex);
  for (auto* queue : queues) {
    queue->push(report);
  }
}

void e2sm_dapp_report_hub::add(e2sm_dapp_report_queue* queue)
{
  std::lock_guard<std::mutex> lock(mutex);
  queues.push_back(queue);
}

void e2sm_dapp_report_hub::remove(e2sm_dapp_report_queue* queue)
{
  std::lock_guard<std::mutex> lock(mutex);
  queues.erase(std::remove(queues.begin(), queues.end(), queue), queues.end());
}

namespace {

/// Report style 1: one indication per E3 dApp report (header format 1, message format 1). The E2 agent polls
/// collect_measurements() every report period and sends at most one indication per poll, so a burst of reports
/// leaves one per period; the queue holds the rest (and drops the oldest past its capacity).
class e2sm_dapp_e3_data_report_service : public e2sm_report_service
{
public:
  e2sm_dapp_e3_data_report_service(std::shared_ptr<e2sm_dapp_report_hub> hub_, const e2sm_dapp_node_identity& node_) :
    hub(std::move(hub_)), node(node_)
  {
    hub->add(&queue);
  }
  ~e2sm_dapp_e3_data_report_service() override { hub->remove(&queue); }

  bool collect_measurements() override
  {
    header.clear();
    message.clear();
    // Skip reports that cannot be carried (empty, or over the 32 KB the module allows).
    while (auto report = queue.pop()) {
      message = e2sm_dapp_asn1_packer::pack_indication_message_format1(report->data);
      header  = e2sm_dapp_asn1_packer::pack_indication_header_format1(node, *report);
      if (!message.empty() && !header.empty()) {
        return true;
      }
    }
    header.clear();
    message.clear();
    return true;
  }
  bool        is_ind_msg_ready() override { return !header.empty() && !message.empty(); }
  byte_buffer get_indication_message() override { return message.copy(); }
  byte_buffer get_indication_header() override { return header.copy(); }

private:
  std::shared_ptr<e2sm_dapp_report_hub> hub;
  e2sm_dapp_node_identity               node;
  e2sm_dapp_report_queue                queue;
  byte_buffer                           header;
  byte_buffer                           message;
};

/// Report style 2: the dApp subscription map (header format 2, message format 2), sent when it first exists and then
/// whenever it changes.
class e2sm_dapp_subscription_map_report_service : public e2sm_report_service
{
public:
  e2sm_dapp_subscription_map_report_service(e2sm_dapp_bridge& bridge_, const e2sm_dapp_node_identity& node_) :
    bridge(bridge_), node(node_)
  {
  }

  bool collect_measurements() override
  {
    header.clear();
    message.clear();
    auto current = bridge.subscriptions();
    if (sent && current == last) {
      return true;
    }
    header  = e2sm_dapp_asn1_packer::pack_indication_header_format2(node);
    message = e2sm_dapp_asn1_packer::pack_indication_message_format2(current);
    if (!header.empty() && !message.empty()) {
      last = std::move(current);
      sent = true;
    }
    return true;
  }
  bool        is_ind_msg_ready() override { return !header.empty() && !message.empty(); }
  byte_buffer get_indication_message() override { return message.copy(); }
  byte_buffer get_indication_header() override { return header.copy(); }

private:
  e2sm_dapp_bridge&                        bridge;
  e2sm_dapp_node_identity                  node;
  std::vector<e2sm_dapp_subscription_item> last;
  bool                                     sent = false;
  byte_buffer                              header;
  byte_buffer                              message;
};

/// Control style 1: the control's data goes, as an E3 xApp control action, to the dApp and E3 RAN function its header
/// names. Success means the E3 server queued it for a connected dApp.
class e2sm_dapp_control_service : public e2sm_control_service
{
public:
  explicit e2sm_dapp_control_service(e2sm_dapp_bridge& bridge_) : bridge(bridge_) {}

  uint32_t get_style_type() override { return e2sm_dapp_asn1_packer::control_style_e3_data; }
  // Only used by E2SM-RC's RAN function description; E2SM-DAPP packs its own (e2sm_dapp_asn1_packer).
  asn1::e2sm::ran_function_definition_ctrl_item_s get_control_style_definition() override { return {}; }
  bool add_e2sm_rc_control_action_executor(std::unique_ptr<e2sm_control_action_executor>) override { return false; }

  bool control_request_supported(const e2sm_ric_control_request& req) override
  {
    return req.service_model == e2sm_service_model_t::DAPP &&
           std::holds_alternative<e2sm_dapp_ctrl_hdr>(req.request_ctrl_hdr) &&
           std::holds_alternative<e2sm_dapp_ctrl_msg>(req.request_ctrl_msg);
  }

  async_task<e2sm_ric_control_response> execute_control_request(const e2sm_ric_control_request& req) override
  {
    e2sm_ric_control_response response;
    response.service_model = e2sm_service_model_t::DAPP;
    response.success       = false;
    if (control_request_supported(req)) {
      const auto& header = std::get<e2sm_dapp_ctrl_hdr>(req.request_ctrl_hdr);
      const auto& msg    = std::get<e2sm_dapp_ctrl_msg>(req.request_ctrl_msg);
      response.success   = bridge.send_control(header.dapp_id, header.e3_ran_function_id, msg.data);
    }
    if (!response.success) {
      response.cause.set_ric_request() = asn1::e2ap::cause_ric_request_e::options::unspecified;
    }
    return launch_no_op_task(std::move(response));
  }

private:
  e2sm_dapp_bridge& bridge;
};

} // namespace

e2sm_dapp_impl::e2sm_dapp_impl(ocudulog::basic_logger&           logger_,
                               e2sm_dapp_asn1_packer&            packer_,
                               std::shared_ptr<e2sm_dapp_bridge> bridge_,
                               e2sm_dapp_node_identity           node_) :
  logger(logger_),
  packer(packer_),
  bridge(std::move(bridge_)),
  node(node_),
  hub(std::make_shared<e2sm_dapp_report_hub>()),
  control_service(std::make_unique<e2sm_dapp_control_service>(*bridge))
{
  // The listener holds the hub, not this object: the bridge may outlive the E2 agent.
  bridge->set_report_listener([hub = hub](e2sm_dapp_report report) { hub->publish(report); });
}

bool e2sm_dapp_impl::action_supported(const asn1::e2ap::ric_action_to_be_setup_item_s& ric_action)
{
  if (ric_action.ric_action_type != asn1::e2ap::ric_action_type_e::report) {
    logger.debug("E2SM-DAPP: only report actions are supported");
    return false;
  }
  const auto definition = packer.handle_packed_e2sm_action_definition(ric_action.ric_action_definition);
  if (definition.service_model != e2sm_service_model_t::DAPP) {
    logger.debug("E2SM-DAPP: undecodable action definition");
    return false;
  }
  const auto style = std::get<e2sm_dapp_action_definition>(definition.action_definition).ric_style_type;
  return style == e2sm_dapp_asn1_packer::report_style_e3_data ||
         style == e2sm_dapp_asn1_packer::report_style_subscription_map;
}

std::unique_ptr<e2sm_report_service> e2sm_dapp_impl::get_e2sm_report_service(const byte_buffer& action_definition)
{
  const auto definition = packer.handle_packed_e2sm_action_definition(action_definition);
  if (definition.service_model != e2sm_service_model_t::DAPP) {
    return nullptr;
  }
  switch (std::get<e2sm_dapp_action_definition>(definition.action_definition).ric_style_type) {
    case e2sm_dapp_asn1_packer::report_style_e3_data:
      return std::make_unique<e2sm_dapp_e3_data_report_service>(hub, node);
    case e2sm_dapp_asn1_packer::report_style_subscription_map:
      return std::make_unique<e2sm_dapp_subscription_map_report_service>(*bridge, node);
    default:
      return nullptr;
  }
}

bool e2sm_dapp_impl::add_e2sm_control_service(std::unique_ptr<e2sm_control_service>)
{
  // The one control style is built in.
  return false;
}

e2sm_control_service* e2sm_dapp_impl::get_e2sm_control_service(const e2sm_ric_control_request& request)
{
  return request.service_model == e2sm_service_model_t::DAPP ? control_service.get() : nullptr;
}
