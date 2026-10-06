// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Random E3AP PDUs of every procedure within the bounds of a profile (libe3 or OCUDU), shared by
// the Protobuf conformance test and the APER/Protobuf equivalence test.

#pragma once

#include "ocudu/dapp/e3/e3ap_protocol.h"
#include <algorithm>
#include <random>

namespace ocudu::dapp::e3::testing {

/// Random PDUs of every procedure within the bounds of a profile.
class pdu_generator
{
public:
  pdu_generator(uint32_t seed, e3ap_profile profile_) : random(seed), profile(profile_) {}

  e3ap_pdu next(std::size_t kind)
  {
    e3ap_pdu pdu;
    pdu.id = static_cast<uint32_t>(range(1, 0xFFFFFFFFULL));
    if (coin()) {
      pdu.timestamp_ns = static_cast<int64_t>(range(1, 0x7FFFFFFFFFFFFFFFULL));
    }
    switch (kind % (ocudu() ? 12 : 11)) {
      case 0:
        pdu.message = setup_request();
        break;
      case 1:
        pdu.message = setup_response();
        break;
      case 2:
        pdu.message = subscription_request();
        break;
      case 3:
        pdu.message = e3ap_subscription_delete{id(), id()};
        break;
      case 4:
        pdu.message = subscription_response();
        break;
      case 5:
        pdu.message = indication();
        break;
      case 6:
        pdu.message = control_action();
        break;
      case 7: {
        e3ap_dapp_report report;
        report.dapp_identifier         = id();
        report.ran_function_identifier = id();
        report.report_data             = payload();
        report.sequence_identifier     = sequence(true);
        pdu.message                    = report;
        break;
      }
      case 8: {
        e3ap_xapp_control_action xapp;
        xapp.dapp_identifier         = id();
        xapp.ran_function_identifier = id();
        xapp.xapp_control_data       = payload();
        xapp.sequence_identifier     = sequence(true);
        pdu.message                  = xapp;
        break;
      }
      case 9:
        pdu.message = e3ap_release_message{id()};
        break;
      case 10: {
        e3ap_message_ack ack;
        ack.request_id    = message_id();
        ack.response_code = code();
        if (ocudu()) {
          ack.cause  = cause();
          ack.detail = text(0, 512);
        }
        pdu.message = ack;
        break;
      }
      default:
        pdu.message = control_response();
        break;
    }
    return pdu;
  }

private:
  bool     ocudu() const { return profile == e3ap_profile::ocudu; }
  uint64_t range(uint64_t low, uint64_t high)
  {
    // Bias toward the edges, where varint lengths change.
    switch (random() % 4) {
      case 0:
        return low;
      case 1:
        return high;
      default:
        return std::uniform_int_distribution<uint64_t>(low, high)(random);
    }
  }
  bool     coin() { return random() % 2 == 0; }
  uint16_t id() { return static_cast<uint16_t>(range(1, 65535)); }
  uint32_t message_id() { return static_cast<uint32_t>(range(1, 0xFFFFFFFFULL)); }
  std::optional<uint32_t> sequence(bool required)
  {
    if (!required && coin()) {
      return std::nullopt;
    }
    return message_id();
  }
  e3ap_response_code code() { return coin() ? e3ap_response_code::positive : e3ap_response_code::negative; }
  std::optional<e3ap_cause> cause()
  {
    if (coin()) {
      return std::nullopt;
    }
    return static_cast<e3ap_cause>(range(0, static_cast<uint64_t>(e3ap_cause::shutting_down)));
  }
  std::string text(std::size_t minimum, std::size_t maximum)
  {
    static const std::string pieces[] = {"a", "Z", "0", "-", ".", "é", "✓", "𝄞"};
    std::string              result;
    const std::size_t        target = range(minimum, maximum);
    while (result.size() < target) {
      const auto& piece = pieces[random() % 8];
      if (result.size() + piece.size() > maximum) {
        break;
      }
      result += piece;
    }
    while (result.size() < minimum) {
      result += "x";
    }
    return result;
  }
  std::vector<uint8_t> bytes(std::size_t minimum, std::size_t maximum)
  {
    std::vector<uint8_t> result(static_cast<std::size_t>(range(minimum, maximum)));
    for (auto& byte : result) {
      byte = static_cast<uint8_t>(random());
    }
    return result;
  }
  std::vector<uint8_t>  payload() { return bytes(1, 4096); }
  std::vector<uint16_t> ids(std::size_t maximum)
  {
    std::vector<uint16_t> result(static_cast<std::size_t>(range(0, maximum)));
    for (auto& value : result) {
      value = id();
    }
    return result;
  }
  std::size_t version_bytes() const { return 11; }

  e3ap_setup_request setup_request()
  {
    e3ap_setup_request setup;
    setup.e3ap_protocol_version = text(1, version_bytes());
    setup.dapp_name             = text(1, 64);
    setup.dapp_version          = text(1, version_bytes());
    setup.vendor                = text(1, 64);
    if (ocudu() && coin()) {
      setup.ocudu_profile = static_cast<uint8_t>(range(1, 255));
    }
    if (ocudu() && coin()) {
      setup.dapp_instance.emplace();
      setup.dapp_instance->bytes[random() % 16] = static_cast<uint8_t>(1 + random() % 255);
    }
    return setup;
  }

  e3ap_setup_response setup_response()
  {
    e3ap_setup_response response;
    response.request_id     = message_id();
    response.response_code  = code();
    response.ran_identifier = text(1, 64);
    if (coin()) {
      response.e3ap_protocol_version = text(1, version_bytes());
    }
    if (coin()) {
      response.dapp_identifier = id();
    }
    for (std::size_t index = 0, count = range(0, 4); index != count; ++index) {
      e3ap_ran_function_definition function;
      function.ran_function_identifier = id();
      function.telemetry_identifiers   = ids(20);
      function.control_identifiers     = ids(5);
      function.ran_function_data       = bytes(1, 300);
      if (ocudu()) {
        function.ran_function_name    = text(0, 64);
        function.ran_function_version = text(0, 11);
      }
      response.ran_functions.push_back(function);
    }
    response.ran_functions_present = !response.ran_functions.empty() || (ocudu() && coin());
    if (ocudu()) {
      if (coin()) {
        response.ocudu_profile = static_cast<uint8_t>(range(1, 255));
      }
      response.cause  = cause();
      response.detail = text(0, 64);
      if (coin()) {
        response.ran_instance.emplace();
        response.ran_instance->bytes[0] = 1;
      }
      if (coin()) {
        response.maximum_payload_bytes = static_cast<uint32_t>(range(1, e3ap_maximum_payload_bytes));
      }
      if (coin()) {
        response.maximum_fragment_bytes = static_cast<uint32_t>(range(1, e3ap_maximum_fragment_bytes));
      }
    }
    return response;
  }

  e3ap_subscription_request subscription_request()
  {
    e3ap_subscription_request request;
    request.dapp_identifier         = id();
    request.ran_function_identifier = id();
    request.telemetry_identifiers   = ids(30);
    request.control_identifiers     = ids(3);
    if (coin()) {
      request.subscription_time_s = static_cast<uint32_t>(range(0, ocudu() ? 86400 : 3600));
    }
    if (coin()) {
      request.periodicity = static_cast<uint32_t>(range(0, ocudu() ? 60000 : 1000));
    }
    if (ocudu()) {
      request.subscription_data = coin() ? bytes(1, 200) : std::vector<uint8_t>{};
      if (coin()) {
        request.delivery_mode = coin() ? e3ap_delivery_mode::in_band : e3ap_delivery_mode::by_reference;
      }
      if (coin()) {
        request.minimum_by_reference_bytes = static_cast<uint32_t>(range(0, e3ap_maximum_payload_bytes));
      }
    }
    return request;
  }

  e3ap_shared_region shared_region()
  {
    e3ap_shared_region region;
    region.memory_domain  = static_cast<e3ap_memory_domain>(range(0, 5));
    region.region_name    = text(1, 64);
    region.region_bytes   = range(0, ~uint64_t{0});
    region.header_bytes   = static_cast<uint16_t>(range(0, 65535));
    region.row_bytes      = range(1, ~uint64_t{0});
    region.nof_rows       = static_cast<uint16_t>(range(1, 65535));
    region.layout_version = static_cast<uint16_t>(range(0, 65535));
    return region;
  }

  e3ap_subscription_response subscription_response()
  {
    e3ap_subscription_response response;
    response.request_id      = message_id();
    response.dapp_identifier = id();
    response.response_code   = code();
    if (coin()) {
      response.subscription_id = id();
    }
    if (ocudu()) {
      if (coin()) {
        response.telemetry_identifiers = ids(10);
      }
      if (coin()) {
        response.control_identifiers = ids(3);
      }
      response.cause  = cause();
      response.detail = text(0, 100);
      if (coin()) {
        response.shared_region = shared_region();
      }
    }
    return response;
  }

  e3ap_tensor_descriptor tensor()
  {
    e3ap_tensor_descriptor descriptor;
    descriptor.memory_domain = static_cast<e3ap_memory_domain>(range(0, 5));
    if (coin()) {
      descriptor.location = e3ap_posix_shared_location{
          text(1, 64), static_cast<uint16_t>(range(0, 65535)), range(0, ~uint64_t{0})};
    } else {
      e3ap_accelerator_ipc_location ipc;
      ipc.device_ordinal = static_cast<uint16_t>(range(0, 65535));
      ipc.allocation_id  = range(0, ~uint64_t{0});
      for (auto& byte : ipc.memory_handle) {
        byte = static_cast<uint8_t>(random());
      }
      if (coin()) {
        ipc.ready_event_handle.emplace();
        (*ipc.ready_event_handle)[5] = static_cast<uint8_t>(random());
      }
      ipc.byte_offset     = range(0, ~uint64_t{0});
      descriptor.location = ipc;
    }
    descriptor.byte_length = range(0, ~uint64_t{0});
    descriptor.generation  = range(0, ~uint64_t{0});
    if (coin()) {
      descriptor.element_type = static_cast<e3ap_element_type>(range(0, 11));
    }
    for (std::size_t index = 0, rank = range(0, 8); index != rank; ++index) {
      descriptor.dimensions.push_back(static_cast<uint32_t>(range(0, 0xFFFFFFFFULL)));
      descriptor.byte_strides.push_back(range(0, ~uint64_t{0}));
    }
    if (coin()) {
      descriptor.layout_id = static_cast<uint16_t>(range(0, 65535));
    }
    if (coin()) {
      descriptor.lease_expiry_ns = range(0, ~uint64_t{0});
    }
    return descriptor;
  }

  e3ap_indication_message indication()
  {
    e3ap_indication_message indication;
    indication.dapp_identifier         = id();
    indication.ran_function_identifier = id();
    indication.protocol_data           = payload();
    if (ocudu()) {
      if (coin()) {
        indication.subscription_id = id();
      }
      if (coin()) {
        indication.telemetry_identifier = id();
      }
      if (coin()) {
        indication.sequence = range(0, ~uint64_t{0});
      }
      indication.fragment_count = static_cast<uint32_t>(range(1, 65536));
      indication.fragment_index = static_cast<uint16_t>(range(0, std::min<uint32_t>(indication.fragment_count, 65536) - 1));
      if (coin()) {
        indication.payload_bytes = static_cast<uint32_t>(range(1, e3ap_maximum_payload_bytes));
      }
      if (coin()) {
        indication.by_reference = tensor();
      }
    }
    return indication;
  }

  e3ap_dapp_control_action control_action()
  {
    e3ap_dapp_control_action control;
    control.dapp_identifier         = id();
    control.ran_function_identifier = id();
    control.control_identifier      = id();
    control.action_data             = payload();
    control.sequence_identifier     = sequence(false);
    if (ocudu()) {
      control.fragment_count = static_cast<uint32_t>(range(1, 65536));
      control.fragment_index = static_cast<uint16_t>(range(0, std::min<uint32_t>(control.fragment_count, 65536) - 1));
      if (coin()) {
        control.payload_bytes = static_cast<uint32_t>(range(1, e3ap_maximum_payload_bytes));
      }
    }
    return control;
  }

  e3ap_control_response control_response()
  {
    e3ap_control_response response;
    response.request_id              = message_id();
    response.dapp_identifier         = id();
    response.ran_function_identifier = id();
    response.control_identifier      = id();
    response.response_code           = code();
    response.cause                   = cause();
    response.detail                  = text(0, 100);
    response.control_outcome         = coin() ? payload() : std::vector<uint8_t>{};
    response.fragment_count          = static_cast<uint32_t>(range(1, 65536));
    response.fragment_index  = static_cast<uint16_t>(range(0, std::min<uint32_t>(response.fragment_count, 65536) - 1));
    response.payload_bytes   = static_cast<uint32_t>(range(0, e3ap_maximum_payload_bytes));
    return response;
  }

  std::mt19937_64       random;
  e3ap_profile profile;
};

} // namespace ocudu::dapp::e3::testing
