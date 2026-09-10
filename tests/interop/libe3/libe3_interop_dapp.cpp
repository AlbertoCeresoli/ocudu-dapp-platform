// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// A dApp built on upstream libe3 (dApp role, E3LinkLayer::SCTP_ASSOCIATION)
// that attaches to a running OCUDU E3AP agent and drives one complete E3AP
// session: setup, subscription, indications, a dApp control action, a dApp
// report, subscription delete and release. It links libe3 only, so it is the
// same binary a third party would build, and it prints one JSON object with
// what happened so a test driver can assert on it.
//
//   libe3_interop_dapp --port <e3ap port> [--telemetry <id>] [--indications <n>]
//                      [--control-hex <APER ManagementRequest>] [--timeout-ms <ms>]

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <libe3/libe3.hpp>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace libe3;
using namespace std::chrono_literals;

namespace {

struct options {
  uint16_t             port{0};
  uint32_t             telemetry{2};
  unsigned             indications{3};
  unsigned             timeout_ms{10000};
  std::vector<uint8_t> control;
};

std::vector<uint8_t> bytes_of_hex(const std::string& hex)
{
  std::vector<uint8_t> bytes;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(index, 2), nullptr, 16)));
  }
  return bytes;
}

options parse(int argc, char** argv)
{
  options result;
  for (int index = 1; index + 1 < argc; index += 2) {
    const std::string option = argv[index];
    const std::string value  = argv[index + 1];
    if (option == "--port") {
      result.port = static_cast<uint16_t>(std::stoul(value));
    } else if (option == "--telemetry") {
      result.telemetry = static_cast<uint32_t>(std::stoul(value));
    } else if (option == "--indications") {
      result.indications = static_cast<unsigned>(std::stoul(value));
    } else if (option == "--timeout-ms") {
      result.timeout_ms = static_cast<unsigned>(std::stoul(value));
    } else if (option == "--control-hex") {
      result.control = bytes_of_hex(value);
    } else {
      std::fprintf(stderr, "unknown option %s\n", option.c_str());
      std::exit(2);
    }
  }
  if (result.port == 0) {
    std::fprintf(stderr,
                 "usage: libe3_interop_dapp --port <e3ap port> [--telemetry id] [--indications n] "
                 "[--control-hex hex] [--timeout-ms ms]\n");
    std::exit(2);
  }
  return result;
}

struct session_log {
  std::mutex                               mutex;
  std::condition_variable                  changed;
  bool                                     setup_positive{false};
  uint32_t                                 dapp_id{0};
  std::string                              ran_identifier;
  std::vector<uint32_t>                    ran_functions;
  std::vector<uint32_t>                    telemetry_ids;
  std::vector<uint32_t>                    control_ids;
  std::map<uint32_t, SubscriptionResponse> subscription_responses;
  std::vector<std::size_t>                 indication_bytes;
  std::map<uint32_t, ResponseCode>         acks;
  unsigned                                 unexpected_pdus{0};

  template <typename Predicate>
  bool wait(Predicate&& ready, std::chrono::milliseconds limit)
  {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, limit, [&] { return ready(); });
  }
};

std::string json_list(const std::vector<uint32_t>& values)
{
  std::string text = "[";
  for (std::size_t index = 0; index != values.size(); ++index) {
    text += (index ? "," : "") + std::to_string(values[index]);
  }
  return text + "]";
}

std::string json_sizes(const std::vector<std::size_t>& values)
{
  std::string text = "[";
  for (std::size_t index = 0; index != values.size(); ++index) {
    text += (index ? "," : "") + std::to_string(values[index]);
  }
  return text + "]";
}

const char* code_text(const std::map<uint32_t, ResponseCode>& acks, uint32_t id)
{
  auto found = acks.find(id);
  if (found == acks.end()) {
    return "missing";
  }
  return found->second == ResponseCode::POSITIVE ? "positive" : "negative";
}

} // namespace

int main(int argc, char** argv)
{
  const options opts  = parse(argc, argv);
  const auto    limit = std::chrono::milliseconds(opts.timeout_ms);

  E3Config config;
  config.role            = E3Role::DAPP;
  config.link_layer      = E3LinkLayer::SCTP_ASSOCIATION;
  config.transport_layer = E3TransportLayer::SCTP;
  config.encoding        = EncodingFormat::ASN1;
  config.setup_endpoint  = "sctp://127.0.0.1";
  config.setup_port      = opts.port;
  config.dapp_name       = "libe3-interop-dapp";
  config.dapp_version    = LIBE3_VERSION_STRING;
  config.vendor          = "DeepSig";
  config.log_level       = 1;

  session_log log;
  E3Agent     dapp(config);
  dapp.set_setup_response_handler([&](const SetupResponse& response) {
    std::lock_guard<std::mutex> lock(log.mutex);
    log.setup_positive = response.response_code == ResponseCode::POSITIVE;
    log.dapp_id        = response.dapp_identifier.value_or(0);
    log.ran_identifier = response.ran_identifier;
    for (const auto& function : response.ran_function_list) {
      log.ran_functions.push_back(function.ran_function_identifier);
      if (function.ran_function_identifier == 10) {
        log.telemetry_ids = function.telemetry_identifier_list;
      }
      if (function.ran_function_identifier == 11) {
        log.control_ids = function.control_identifier_list;
      }
    }
    log.changed.notify_all();
  });
  dapp.set_subscription_response_handler([&](const SubscriptionResponse& response) {
    std::lock_guard<std::mutex> lock(log.mutex);
    log.subscription_responses[response.request_id] = response;
    log.changed.notify_all();
  });
  dapp.set_indication_handler([&](const IndicationMessage& message) {
    std::lock_guard<std::mutex> lock(log.mutex);
    log.indication_bytes.push_back(message.protocol_data.size());
    log.changed.notify_all();
  });
  dapp.set_message_ack_handler([&](const MessageAck& ack) {
    std::lock_guard<std::mutex> lock(log.mutex);
    log.acks[ack.request_id] = ack.response_code;
    log.changed.notify_all();
  });

  bool ok = true;
  if (dapp.start() != ErrorCode::SUCCESS) {
    std::cout << "{\"error\": \"start failed\"}\n";
    return 1;
  }
  const bool setup = dapp.wait_for_setup(limit) == ErrorCode::SUCCESS;
  ok               = ok && setup && dapp.dapp_id().has_value();

  uint32_t subscribe_id = 0;
  bool     subscribed   = false;
  if (ok) {
    ok = dapp.subscribe(10, {opts.telemetry}, {}, std::nullopt, std::nullopt, &subscribe_id) == ErrorCode::SUCCESS;
    subscribed = ok && log.wait([&] { return log.subscription_responses.count(subscribe_id) != 0; }, limit) &&
                 log.subscription_responses[subscribe_id].response_code == ResponseCode::POSITIVE;
    ok = ok && subscribed;
  }
  const bool got_indications = ok && log.wait([&] { return log.indication_bytes.size() >= opts.indications; }, limit);
  ok                         = ok && got_indications;

  uint32_t control_id = 0;
  if (ok && !opts.control.empty()) {
    ok = dapp.send_control(11, 1, opts.control, &control_id) == ErrorCode::SUCCESS &&
         log.wait([&] { return log.acks.count(control_id) != 0; }, limit);
  }
  uint32_t report_id = 0;
  if (ok) {
    ok = dapp.send_report(10, {0x01, 0x02, 0x03}, &report_id) == ErrorCode::SUCCESS &&
         log.wait([&] { return log.acks.count(report_id) != 0; }, limit);
  }
  bool unsubscribed = false;
  if (ok) {
    ok                  = dapp.unsubscribe(10) == ErrorCode::SUCCESS;
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (ok && !dapp.subscribed_ran_functions().empty() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(10ms);
    }
    unsubscribed = ok && dapp.subscribed_ran_functions().empty();
    ok           = ok && unsubscribed;
  }
  bool released = false;
  if (ok) {
    released = dapp.release() == ErrorCode::SUCCESS;
    ok       = ok && released;
    std::this_thread::sleep_for(100ms);
  }
  dapp.stop();

  std::lock_guard<std::mutex> lock(log.mutex);
  std::cout << "{\"libe3_version\": \"" << LIBE3_VERSION_STRING << "\"" << ", \"setup\": " << (setup ? "true" : "false")
            << ", \"dapp_id\": " << log.dapp_id << ", \"ran_identifier\": \"" << log.ran_identifier << "\""
            << ", \"ran_functions\": " << json_list(log.ran_functions)
            << ", \"telemetry_ids\": " << json_list(log.telemetry_ids)
            << ", \"control_ids\": " << json_list(log.control_ids)
            << ", \"subscribed\": " << (subscribed ? "true" : "false");
  if (subscribed) {
    const auto& response = log.subscription_responses[subscribe_id];
    std::cout << ", \"subscription_id\": " << response.subscription_id.value_or(0);
  }
  std::cout << ", \"indications\": " << log.indication_bytes.size()
            << ", \"indication_bytes\": " << json_sizes(log.indication_bytes) << ", \"control_ack\": \""
            << (opts.control.empty() ? "skipped" : code_text(log.acks, control_id)) << "\"" << ", \"report_ack\": \""
            << code_text(log.acks, report_id) << "\"" << ", \"unsubscribed\": " << (unsubscribed ? "true" : "false")
            << ", \"released\": " << (released ? "true" : "false") << ", \"ok\": " << (ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}
