// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Cross-encoder vectors for the OCUDU <-> libe3 interoperability gate. Links
// upstream libe3 only (never an OCUDU codec, whose asn1c symbols would clash),
// and speaks JSON lines on stdout:
//
//   libe3_interop_vectors encode [asn1|protobuf]
//       one {"name": <procedure>, "hex": <bytes>} per libe3 E3AP 1.0.0
//       procedure, encoded by libe3's own Asn1E3Encoder (APER, the default) or
//       ProtobufE3Encoder with fixed content (message id 42, timestamp
//       1756800000123456789);
//   libe3_interop_vectors decode <hex> [asn1|protobuf]
//       {"name": ..., "id": ..., "timestamp": ..., <procedure fields>} for the
//       PDU libe3's decoder reads from those bytes.
//
// OCUDU's e3ap_libe3_codec (APER) and e3ap_protobuf_codec (Protobuf) must
// decode the encode output and reproduce it byte for byte, and libe3 must
// decode what they (and asn1tools) emit.

#include <cstdio>
#include <cstring>
#include <iostream>
#include <libe3/e3_encoder.hpp>
#include <libe3/logger.hpp>
#include <libe3/types.hpp>
#include <string>
#include <vector>

using namespace libe3;

namespace {

constexpr uint32_t vector_message_id = 42;
constexpr uint64_t vector_timestamp  = 1756800000123456789ULL;

std::string hex_of(const std::vector<uint8_t>& bytes)
{
  static const char digits[] = "0123456789abcdef";
  std::string       text;
  for (const uint8_t byte : bytes) {
    text.push_back(digits[byte >> 4]);
    text.push_back(digits[byte & 15]);
  }
  return text;
}

std::vector<uint8_t> bytes_of_hex(const std::string& hex)
{
  std::vector<uint8_t> bytes;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(index, 2), nullptr, 16)));
  }
  return bytes;
}

std::string json_list(const std::vector<uint32_t>& values)
{
  std::string text = "[";
  for (std::size_t index = 0; index != values.size(); ++index) {
    text += (index ? "," : "") + std::to_string(values[index]);
  }
  return text + "]";
}

std::string json_string(const std::string& value)
{
  std::string text = "\"";
  for (const char character : value) {
    if (character == '"' || character == '\\') {
      text.push_back('\\');
    }
    text.push_back(character);
  }
  return text + "\"";
}

Pdu make(PduType type)
{
  Pdu pdu(type);
  pdu.message_id = vector_message_id;
  pdu.timestamp  = vector_timestamp;
  return pdu;
}

std::vector<Pdu> vectors()
{
  std::vector<Pdu> result;
  {
    SetupRequest request;
    request.e3ap_protocol_version = "1.0.0";
    request.dapp_name             = "vector-dapp";
    request.dapp_version          = "0.2.1";
    request.vendor                = "wineslab";
    Pdu pdu                       = make(PduType::SETUP_REQUEST);
    pdu.choice                    = request;
    result.push_back(pdu);
  }
  {
    SetupResponse response;
    response.request_id            = vector_message_id;
    response.response_code         = ResponseCode::POSITIVE;
    response.ran_identifier        = "vector-ran";
    response.e3ap_protocol_version = "1.0.0";
    response.dapp_identifier       = 7;
    RanFunctionDef function;
    function.ran_function_identifier   = 1;
    function.telemetry_identifier_list = {1, 2};
    function.control_identifier_list   = {1};
    function.ran_function_data         = {0xDE, 0xAD};
    response.ran_function_list.push_back(function);
    Pdu pdu    = make(PduType::SETUP_RESPONSE);
    pdu.choice = response;
    result.push_back(pdu);
  }
  {
    SubscriptionRequest request;
    request.dapp_identifier           = 7;
    request.ran_function_identifier   = 1;
    request.telemetry_identifier_list = {1, 2};
    request.subscription_time         = 60;
    request.periodicity               = 100;
    Pdu pdu                           = make(PduType::SUBSCRIPTION_REQUEST);
    pdu.choice                        = request;
    result.push_back(pdu);
  }
  {
    SubscriptionDelete remove;
    remove.dapp_identifier = 7;
    remove.subscription_id = 1;
    Pdu pdu                = make(PduType::SUBSCRIPTION_DELETE);
    pdu.choice             = remove;
    result.push_back(pdu);
  }
  {
    SubscriptionResponse response;
    response.request_id      = vector_message_id;
    response.dapp_identifier = 7;
    response.response_code   = ResponseCode::POSITIVE;
    response.subscription_id = 1;
    Pdu pdu                  = make(PduType::SUBSCRIPTION_RESPONSE);
    pdu.choice               = response;
    result.push_back(pdu);
  }
  {
    IndicationMessage indication;
    indication.dapp_identifier         = 7;
    indication.ran_function_identifier = 1;
    for (uint8_t byte = 1; byte <= 16; ++byte) {
      indication.protocol_data.push_back(byte);
    }
    Pdu pdu    = make(PduType::INDICATION_MESSAGE);
    pdu.choice = indication;
    result.push_back(pdu);
  }
  {
    DAppControlAction control;
    control.dapp_identifier         = 7;
    control.ran_function_identifier = 1;
    control.control_identifier      = 1;
    control.action_data             = {0xAA, 0xBB};
    control.sequence_id             = 3000000000U; // optional field, present (a re-issued xApp control)
    Pdu pdu                         = make(PduType::DAPP_CONTROL_ACTION);
    pdu.choice                      = control;
    result.push_back(pdu);
  }
  {
    DAppReport report;
    report.dapp_identifier         = 300;  // beyond the 0.1.x 1..100 range
    report.ran_function_identifier = 1000; // likewise
    report.report_data             = {0xCC};
    report.sequence_id             = 4242;
    Pdu pdu                        = make(PduType::DAPP_REPORT);
    pdu.choice                     = report;
    result.push_back(pdu);
  }
  {
    XAppControlAction xapp;
    xapp.dapp_identifier         = 40000;
    xapp.ran_function_identifier = 1;
    xapp.xapp_control_data       = {0xEE, 0xFF};
    xapp.sequence_id             = 70000;
    Pdu pdu                      = make(PduType::XAPP_CONTROL_ACTION);
    pdu.choice                   = xapp;
    result.push_back(pdu);
  }
  {
    ReleaseMessage release;
    release.dapp_identifier = 7;
    Pdu pdu                 = make(PduType::RELEASE_MESSAGE);
    pdu.choice              = release;
    result.push_back(pdu);
  }
  {
    MessageAck ack;
    ack.request_id    = vector_message_id;
    ack.response_code = ResponseCode::NEGATIVE;
    Pdu pdu           = make(PduType::MESSAGE_ACK);
    pdu.choice        = ack;
    result.push_back(pdu);
  }
  return result;
}

std::string describe(const Pdu& pdu)
{
  std::string text = "{\"name\": " + json_string(pdu_type_to_string(pdu.type)) +
                     ", \"id\": " + std::to_string(pdu.message_id) +
                     ", \"timestamp\": " + std::to_string(pdu.timestamp);
  const auto code = [](ResponseCode value) { return std::string(response_code_to_string(value)); };
  if (const auto* body = pdu.get_if<SetupRequest>()) {
    text += ", \"e3apProtocolVersion\": " + json_string(body->e3ap_protocol_version) +
            ", \"dAppName\": " + json_string(body->dapp_name) +
            ", \"dAppVersion\": " + json_string(body->dapp_version) + ", \"vendor\": " + json_string(body->vendor);
  } else if (const auto* body = pdu.get_if<SetupResponse>()) {
    text += ", \"requestId\": " + std::to_string(body->request_id) +
            ", \"responseCode\": " + json_string(code(body->response_code)) +
            ", \"ranIdentifier\": " + json_string(body->ran_identifier);
    if (body->e3ap_protocol_version) {
      text += ", \"e3apProtocolVersion\": " + json_string(*body->e3ap_protocol_version);
    }
    if (body->dapp_identifier) {
      text += ", \"dAppIdentifier\": " + std::to_string(*body->dapp_identifier);
    }
    text += ", \"ranFunctionList\": [";
    for (std::size_t index = 0; index != body->ran_function_list.size(); ++index) {
      const auto& function = body->ran_function_list[index];
      text += std::string(index ? ", " : "") +
              "{\"ranFunctionIdentifier\": " + std::to_string(function.ran_function_identifier) +
              ", \"telemetryIdentifierList\": " + json_list(function.telemetry_identifier_list) +
              ", \"controlIdentifierList\": " + json_list(function.control_identifier_list) +
              ", \"ranFunctionData\": " + json_string(hex_of(function.ran_function_data)) + "}";
    }
    text += "]";
  } else if (const auto* body = pdu.get_if<SubscriptionRequest>()) {
    text += ", \"dAppIdentifier\": " + std::to_string(body->dapp_identifier) +
            ", \"ranFunctionIdentifier\": " + std::to_string(body->ran_function_identifier) +
            ", \"telemetryIdentifierList\": " + json_list(body->telemetry_identifier_list) +
            ", \"controlIdentifierList\": " + json_list(body->control_identifier_list);
    if (body->subscription_time) {
      text += ", \"subscriptionTime\": " + std::to_string(*body->subscription_time);
    }
    if (body->periodicity) {
      text += ", \"periodicity\": " + std::to_string(*body->periodicity);
    }
  } else if (const auto* body = pdu.get_if<SubscriptionDelete>()) {
    text += ", \"dAppIdentifier\": " + std::to_string(body->dapp_identifier) +
            ", \"subscriptionId\": " + std::to_string(body->subscription_id);
  } else if (const auto* body = pdu.get_if<SubscriptionResponse>()) {
    text += ", \"requestId\": " + std::to_string(body->request_id) +
            ", \"dAppIdentifier\": " + std::to_string(body->dapp_identifier) +
            ", \"responseCode\": " + json_string(code(body->response_code));
    if (body->subscription_id) {
      text += ", \"subscriptionId\": " + std::to_string(*body->subscription_id);
    }
  } else if (const auto* body = pdu.get_if<IndicationMessage>()) {
    text += ", \"dAppIdentifier\": " + std::to_string(body->dapp_identifier) +
            ", \"ranFunctionIdentifier\": " + std::to_string(body->ran_function_identifier) +
            ", \"protocolData\": " + json_string(hex_of(body->protocol_data));
  } else if (const auto* body = pdu.get_if<DAppControlAction>()) {
    text += ", \"dAppIdentifier\": " + std::to_string(body->dapp_identifier) +
            ", \"ranFunctionIdentifier\": " + std::to_string(body->ran_function_identifier) +
            ", \"controlIdentifier\": " + std::to_string(body->control_identifier) +
            (body->sequence_id != 0 ? ", \"sequenceId\": " + std::to_string(body->sequence_id) : std::string()) +
            ", \"actionData\": " + json_string(hex_of(body->action_data));
  } else if (const auto* body = pdu.get_if<DAppReport>()) {
    text += ", \"dAppIdentifier\": " + std::to_string(body->dapp_identifier) +
            ", \"ranFunctionIdentifier\": " + std::to_string(body->ran_function_identifier) +
            ", \"sequenceId\": " + std::to_string(body->sequence_id) +
            ", \"reportData\": " + json_string(hex_of(body->report_data));
  } else if (const auto* body = pdu.get_if<XAppControlAction>()) {
    text += ", \"dAppIdentifier\": " + std::to_string(body->dapp_identifier) +
            ", \"ranFunctionIdentifier\": " + std::to_string(body->ran_function_identifier) +
            ", \"sequenceId\": " + std::to_string(body->sequence_id) +
            ", \"xAppControlData\": " + json_string(hex_of(body->xapp_control_data));
  } else if (const auto* body = pdu.get_if<ReleaseMessage>()) {
    text += ", \"dAppIdentifier\": " + std::to_string(body->dapp_identifier);
  } else if (const auto* body = pdu.get_if<MessageAck>()) {
    text += ", \"requestId\": " + std::to_string(body->request_id) +
            ", \"responseCode\": " + json_string(code(body->response_code));
  }
  return text + "}";
}

} // namespace

int main(int argc, char** argv)
{
  Logger::instance().set_level(LogLevel::ERROR);
  const std::string mode     = argc > 1 ? argv[1] : "";
  const std::string encoding = mode == "encode" ? (argc > 2 ? argv[2] : "asn1") : (argc > 3 ? argv[3] : "asn1");
  if (encoding != "asn1" && encoding != "protobuf") {
    std::fprintf(stderr, "unknown encoding %s (asn1 | protobuf)\n", encoding.c_str());
    return 2;
  }
  auto encoder = create_encoder(encoding == "protobuf" ? EncodingFormat::PROTOBUF : EncodingFormat::ASN1);
  if (!encoder) {
    std::fprintf(stderr, "libe3 was built without the %s encoder\n", encoding.c_str());
    return 2;
  }
  if (mode == "encode") {
    for (const Pdu& pdu : vectors()) {
      auto encoded = encoder->encode(pdu);
      if (!encoded) {
        std::fprintf(stderr, "encode failed for %s\n", pdu_type_to_string(pdu.type));
        return 1;
      }
      std::cout << "{\"name\": \"" << pdu_type_to_string(pdu.type) << "\", \"hex\": \"" << hex_of(encoded->buffer)
                << "\"}\n";
    }
    return 0;
  }
  if (mode == "decode" && argc > 2) {
    const auto bytes   = bytes_of_hex(argv[2]);
    auto       decoded = encoder->decode(bytes.data(), bytes.size());
    if (!decoded) {
      std::cout << "{\"error\": \"decode failed\"}\n";
      return 1;
    }
    std::cout << describe(*decoded) << "\n";
    return 0;
  }
  std::fprintf(stderr, "usage: %s encode [asn1|protobuf] | decode <hex> [asn1|protobuf]\n", argv[0]);
  return 2;
}
