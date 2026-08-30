// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/fbs_codec.h"
#include "e3-data-profile-v1_generated.h"
#include <cstring>

namespace fb = Ocudu::Dapp::E3Data::V1;

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;

namespace {

fb::Id128 to_wire(const management::identifier& id)
{
  uint64_t low;
  uint64_t high;
  std::memcpy(&low, id.bytes.data(), sizeof(low));
  std::memcpy(&high, id.bytes.data() + 8, sizeof(high));
  return fb::Id128(low, high);
}

management::identifier from_wire(const fb::Id128& id)
{
  management::identifier out;
  const uint64_t         low  = id.low();
  const uint64_t         high = id.high();
  std::memcpy(out.bytes.data(), &low, sizeof(low));
  std::memcpy(out.bytes.data() + 8, &high, sizeof(high));
  return out;
}

/// The data profile enumerates a subset of the service-model result codes.
fb::ResultCode to_wire(management::result_code code)
{
  switch (code) {
    case management::result_code::ok:
    case management::result_code::dry_run_ok:
    case management::result_code::accepted:
      return fb::ResultCode::Ok;
    case management::result_code::not_found:
      return fb::ResultCode::NotFound;
    case management::result_code::conflict:
    case management::result_code::transaction_mismatch:
      return fb::ResultCode::Conflict;
    case management::result_code::invalid_request:
    case management::result_code::verification_failed:
    case management::result_code::load_failed:
    case management::result_code::lifecycle_failed:
      return fb::ResultCode::InvalidRequest;
    case management::result_code::incompatible:
      return fb::ResultCode::Incompatible;
    case management::result_code::resource_exhausted:
      return fb::ResultCode::ResourceExhausted;
  }
  return fb::ResultCode::InvalidRequest;
}

management::result_code from_wire(fb::ResultCode code, std::string& detail)
{
  switch (code) {
    case fb::ResultCode::Ok:
      return management::result_code::ok;
    case fb::ResultCode::NotFound:
      return management::result_code::not_found;
    case fb::ResultCode::Conflict:
      return management::result_code::conflict;
    case fb::ResultCode::InvalidRequest:
      return management::result_code::invalid_request;
    case fb::ResultCode::Incompatible:
      return management::result_code::incompatible;
    case fb::ResultCode::ResourceExhausted:
      return management::result_code::resource_exhausted;
  }
  detail = "response code is invalid";
  return management::result_code::invalid_request;
}

flatbuffers::Offset<fb::DataSubscriptionControl> build_control(flatbuffers::FlatBufferBuilder&  builder,
                                                               const data_subscription_control& control)
{
  const fb::Id128 id = control.subscription_id ? to_wire(*control.subscription_id) : fb::Id128(0, 0);
  return fb::CreateDataSubscriptionControl(builder,
                                           static_cast<fb::DataSubscriptionAction>(control.action),
                                           control.subscription_id ? &id : nullptr,
                                           control.interface_id,
                                           control.cell_id ? flatbuffers::Optional<uint32_t>(*control.cell_id)
                                                           : flatbuffers::nullopt,
                                           control.sample_every_n,
                                           control.maximum_chunk_bytes,
                                           control.lease_us,
                                           static_cast<fb::DataDropPolicy>(control.drop_policy));
}

bool read_control(const fb::DataSubscriptionControl& wire, data_subscription_control& out, std::string& detail)
{
  if (wire.action() > fb::DataSubscriptionAction::List) {
    detail = "subscription action is invalid";
    return false;
  }
  out.action = static_cast<data_subscription_action>(wire.action());
  if (wire.drop_policy() > fb::DataDropPolicy::CoalesceLatest) {
    detail = "drop policy is invalid";
    return false;
  }
  out.drop_policy = static_cast<portable_drop_policy>(wire.drop_policy());
  if (wire.subscription_id() != nullptr) {
    out.subscription_id = from_wire(*wire.subscription_id());
  }
  out.interface_id = wire.interface_id();
  if (wire.cell_id().has_value()) {
    if (*wire.cell_id() > 65535) {
      detail = "cell identifier is out of range";
      return false;
    }
    out.cell_id = *wire.cell_id();
  }
  if (wire.sample_every_n() == 0 || wire.sample_every_n() > 65535 || wire.maximum_chunk_bytes() < 1024 ||
      wire.maximum_chunk_bytes() > 65536 || wire.lease_us() == 0 || wire.lease_us() > 60'000'000) {
    detail = "subscription parameters are out of range";
    return false;
  }
  out.sample_every_n      = wire.sample_every_n();
  out.maximum_chunk_bytes = wire.maximum_chunk_bytes();
  out.lease_us            = wire.lease_us();
  return true;
}

flatbuffers::Offset<fb::SubscriptionRecord> build_record(flatbuffers::FlatBufferBuilder&     builder,
                                                         const portable_subscription_record& record)
{
  data_subscription_control control;
  control.action              = data_subscription_action::list; // records are parameter snapshots
  control.subscription_id     = record.request.subscription_id;
  control.interface_id        = record.request.interface_id;
  control.cell_id             = record.request.cell_id;
  control.sample_every_n      = record.request.sample_every_n;
  control.maximum_chunk_bytes = record.request.maximum_chunk_bytes;
  control.lease_us            = record.request.lease_us;
  control.drop_policy         = record.request.drop_policy;
  const auto request          = build_control(builder, control);
  return fb::CreateSubscriptionRecord(builder,
                                      request,
                                      record.lease_expiry_ns,
                                      record.queued_chunks,
                                      record.published_objects,
                                      record.published_tensor_bytes,
                                      record.dropped_objects,
                                      record.busy_drops,
                                      record.sampled_objects,
                                      record.active,
                                      record.rf_center_frequency_hz,
                                      record.cell_frequency_offset_hz);
}

bool read_record(const fb::SubscriptionRecord& wire, portable_subscription_record& out, std::string& detail)
{
  if (wire.request() == nullptr) {
    detail = "subscription record misses its request";
    return false;
  }
  data_subscription_control control;
  if (!read_control(*wire.request(), control, detail)) {
    return false;
  }
  if (!control.subscription_id) {
    detail = "subscription record misses its identifier";
    return false;
  }
  out.request.subscription_id     = *control.subscription_id;
  out.request.interface_id        = control.interface_id;
  out.request.cell_id             = control.cell_id;
  out.request.sample_every_n      = control.sample_every_n;
  out.request.maximum_chunk_bytes = control.maximum_chunk_bytes;
  out.request.lease_us            = control.lease_us;
  out.request.drop_policy         = control.drop_policy;
  out.lease_expiry_ns             = wire.lease_expiry_ns();
  out.queued_chunks               = wire.queued_chunks();
  out.published_objects           = wire.published_objects();
  out.published_tensor_bytes      = wire.published_tensor_bytes();
  out.dropped_objects             = wire.dropped_objects();
  out.busy_drops                  = wire.busy_drops();
  out.sampled_objects             = wire.sampled_objects();
  out.active                      = wire.active();
  out.rf_center_frequency_hz      = wire.rf_center_frequency_hz();
  out.cell_frequency_offset_hz    = wire.cell_frequency_offset_hz();
  return true;
}

codec_result<std::vector<uint8_t>> finish(flatbuffers::FlatBufferBuilder&  builder,
                                          flatbuffers::Offset<fb::Message> message)
{
  builder.FinishSizePrefixed(message, fb::MessageIdentifier());
  if (builder.GetSize() > fbs_codec::maximum_message_size) {
    return {std::nullopt, "message exceeds the configured bound"};
  }
  return {std::vector<uint8_t>(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()), {}};
}

const fb::Message* verify(ocudu::span<const uint8_t> bytes, std::string& detail)
{
  if (bytes.size() < sizeof(uint32_t) || bytes.size() > fbs_codec::maximum_message_size) {
    detail = "message size is out of bounds";
    return nullptr;
  }
  uint32_t payload = 0;
  std::memcpy(&payload, bytes.data(), sizeof(payload));
  if (static_cast<std::size_t>(payload) + sizeof(uint32_t) != bytes.size()) {
    detail = "declared message size does not match the received bytes";
    return nullptr;
  }
  flatbuffers::Verifier verifier(bytes.data(), bytes.size());
  if (!fb::VerifySizePrefixedMessageBuffer(verifier)) {
    detail = "message failed FlatBuffers verification";
    return nullptr;
  }
  return fb::GetSizePrefixedMessage(bytes.data());
}

} // namespace

codec_result<std::vector<uint8_t>> fbs_codec::encode(const management_request& request) noexcept
{
  try {
    flatbuffers::FlatBufferBuilder builder(1024);
    if (const auto* control = std::get_if<data_subscription_control>(&request)) {
      const auto body        = build_control(builder, *control);
      const auto transaction = to_wire(control->header.transaction_id);
      return finish(builder,
                    fb::CreateMessage(builder, &transaction, fb::MessageBody::DataSubscriptionControl, body.Union()));
    }
    if (const auto* poll = std::get_if<data_poll_query>(&request)) {
      const auto id          = to_wire(poll->subscription_id);
      const auto body        = fb::CreateDataPoll(builder, &id, poll->maximum_chunks);
      const auto transaction = to_wire(poll->header.transaction_id);
      return finish(builder, fb::CreateMessage(builder, &transaction, fb::MessageBody::DataPoll, body.Union()));
    }
    return {std::nullopt, "operation is not part of the FlatBuffers data profile"};
  } catch (const std::exception& e) {
    return {std::nullopt, std::string("FlatBuffers encode failed: ") + e.what()};
  }
}

codec_result<std::vector<uint8_t>> fbs_codec::encode(const management_response& response) noexcept
{
  try {
    flatbuffers::FlatBufferBuilder builder(4096);
    fb::ResponsePayload            payload_type = fb::ResponsePayload::NONE;
    flatbuffers::Offset<void>      payload;

    if (const auto* subscriptions = std::get_if<data_subscription_result>(&response.payload)) {
      std::vector<flatbuffers::Offset<fb::SubscriptionRecord>> records;
      records.reserve(subscriptions->subscriptions.size());
      for (const auto& record : subscriptions->subscriptions) {
        records.push_back(build_record(builder, record));
      }
      payload_type = fb::ResponsePayload::DataSubscriptionResult;
      payload      = fb::CreateDataSubscriptionResult(builder, builder.CreateVector(records)).Union();
    } else if (const auto* publications = std::get_if<data_publication_result>(&response.payload)) {
      std::vector<flatbuffers::Offset<fb::PublicationChunk>> chunks;
      chunks.reserve(publications->chunks.size());
      for (const auto& chunk : publications->chunks) {
        const auto metadata =
            chunk.metadata.empty() ? 0 : builder.CreateVector(chunk.metadata.data(), chunk.metadata.size());
        const auto tensor = builder.CreateVector(chunk.tensor_chunk.data(), chunk.tensor_chunk.size());
        const auto id     = to_wire(chunk.subscription_id);
        chunks.push_back(fb::CreatePublicationChunk(builder,
                                                    &id,
                                                    chunk.cell_id,
                                                    chunk.publication_sequence,
                                                    chunk.route_epoch,
                                                    chunk.chunk_index,
                                                    chunk.chunk_count,
                                                    chunk.tensor_size,
                                                    chunk.dropped_objects,
                                                    metadata,
                                                    tensor));
      }
      const auto record = publications->subscription ? build_record(builder, *publications->subscription)
                                                     : flatbuffers::Offset<fb::SubscriptionRecord>(0);
      payload_type      = fb::ResponsePayload::DataPublicationResult;
      payload           = fb::CreateDataPublicationResult(builder, builder.CreateVector(chunks), record).Union();
    } else if (!std::holds_alternative<std::monostate>(response.payload)) {
      return {std::nullopt, "payload is not part of the FlatBuffers data profile"};
    }

    const auto error_text  = response.error_text.empty() ? 0 : builder.CreateString(response.error_text);
    const auto body        = fb::CreateResponse(builder, to_wire(response.code), error_text, payload_type, payload);
    const auto transaction = to_wire(response.transaction_id);
    return finish(builder, fb::CreateMessage(builder, &transaction, fb::MessageBody::Response, body.Union()));
  } catch (const std::exception& e) {
    return {std::nullopt, std::string("FlatBuffers encode failed: ") + e.what()};
  }
}

codec_result<management_request> fbs_codec::decode_request(ocudu::span<const uint8_t> bytes) noexcept
{
  try {
    std::string        detail;
    const fb::Message* message = verify(bytes, detail);
    if (message == nullptr) {
      return {std::nullopt, detail};
    }
    if (message->transaction_id() == nullptr) {
      return {std::nullopt, "message misses its transaction identifier"};
    }
    const management::identifier transaction = from_wire(*message->transaction_id());
    if (const auto* control = message->body_as_DataSubscriptionControl()) {
      data_subscription_control out;
      out.header.transaction_id = transaction;
      if (!read_control(*control, out, detail)) {
        return {std::nullopt, detail};
      }
      return {management_request{out}, {}};
    }
    if (const auto* poll = message->body_as_DataPoll()) {
      if (poll->subscription_id() == nullptr || poll->maximum_chunks() == 0 || poll->maximum_chunks() > 8) {
        return {std::nullopt, "poll parameters are out of range"};
      }
      data_poll_query out;
      out.header.transaction_id = transaction;
      out.subscription_id       = from_wire(*poll->subscription_id());
      out.maximum_chunks        = poll->maximum_chunks();
      return {management_request{out}, {}};
    }
    return {std::nullopt, "operation is not part of the FlatBuffers data profile"};
  } catch (const std::exception& e) {
    return {std::nullopt, std::string("FlatBuffers decode failed: ") + e.what()};
  }
}

codec_result<management_response> fbs_codec::decode_response(ocudu::span<const uint8_t> bytes) noexcept
{
  try {
    std::string        detail;
    const fb::Message* message = verify(bytes, detail);
    if (message == nullptr) {
      return {std::nullopt, detail};
    }
    const auto* body = message->body_as_Response();
    if (body == nullptr || message->transaction_id() == nullptr) {
      return {std::nullopt, "message is not a response"};
    }
    management_response response;
    response.transaction_id = from_wire(*message->transaction_id());
    response.code           = from_wire(body->code(), detail);
    if (!detail.empty()) {
      return {std::nullopt, detail};
    }
    if (body->error_text() != nullptr) {
      response.error_text = body->error_text()->str();
    }
    if (const auto* subscriptions = body->payload_as_DataSubscriptionResult()) {
      data_subscription_result result;
      if (subscriptions->subscriptions() != nullptr) {
        for (const auto* record : *subscriptions->subscriptions()) {
          portable_subscription_record out;
          if (record == nullptr || !read_record(*record, out, detail)) {
            return {std::nullopt, detail.empty() ? "subscription record is missing" : detail};
          }
          result.subscriptions.push_back(std::move(out));
        }
      }
      response.payload = std::move(result);
    } else if (const auto* publications = body->payload_as_DataPublicationResult()) {
      data_publication_result result;
      if (publications->chunks() != nullptr) {
        for (const auto* chunk : *publications->chunks()) {
          if (chunk == nullptr || chunk->subscription_id() == nullptr) {
            return {std::nullopt, "publication chunk misses its subscription identifier"};
          }
          portable_publication_chunk out;
          out.subscription_id      = from_wire(*chunk->subscription_id());
          out.cell_id              = chunk->cell_id();
          out.publication_sequence = chunk->publication_sequence();
          out.route_epoch          = chunk->route_epoch();
          out.chunk_index          = chunk->chunk_index();
          out.chunk_count          = chunk->chunk_count();
          out.tensor_size          = chunk->tensor_size();
          out.dropped_objects      = chunk->dropped_objects();
          if (chunk->metadata() != nullptr) {
            out.metadata.assign(chunk->metadata()->begin(), chunk->metadata()->end());
          }
          if (chunk->tensor() != nullptr) {
            out.tensor_chunk.assign(chunk->tensor()->begin(), chunk->tensor()->end());
          }
          result.chunks.push_back(std::move(out));
        }
      }
      if (publications->subscription() != nullptr) {
        portable_subscription_record record;
        if (!read_record(*publications->subscription(), record, detail)) {
          return {std::nullopt, detail};
        }
        result.subscription = std::move(record);
      }
      response.payload = std::move(result);
    }
    return {std::move(response), {}};
  } catch (const std::exception& e) {
    return {std::nullopt, std::string("FlatBuffers decode failed: ") + e.what()};
  }
}
