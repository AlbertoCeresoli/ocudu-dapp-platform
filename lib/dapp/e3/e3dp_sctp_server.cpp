// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/e3dp_sctp_server.h"
#include "ocudu/dapp/e3/fbs_codec.h"
#include "ocudu/dapp/e3/management_service.h"
#include <algorithm>
#include <atomic>

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;

namespace {

constexpr uint32_t e3dp_payload_protocol = 0x00E3D001U;
constexpr uint32_t data_profile_chunk    = 64 * 1024;

} // namespace

struct e3dp_sctp_server::implementation {
  implementation(management_service&                  management_,
                 std::shared_ptr<portable_data_store> store_,
                 e3dp_sctp_server_config              config_) :
    management(management_), store(std::move(store_)), config(std::move(config_))
  {
  }

  void placement_for(portable_subscription_record& record) const noexcept
  {
    if (config.cells.empty()) {
      return;
    }
    const e3dp_cell_placement* selected = &config.cells.front();
    if (record.request.cell_id) {
      for (const auto& cell : config.cells) {
        if (cell.cell_id == *record.request.cell_id) {
          selected = &cell;
        }
      }
    }
    record.rf_center_frequency_hz   = selected->rf_center_frequency_hz;
    record.cell_frequency_offset_hz = selected->cell_frequency_offset_hz;
  }

  /// Chunks per poll that fit in one SCTP message given the kernel send buffer.
  uint32_t chunk_budget(const sctp_association& association, const management::identifier& subscription) const noexcept
  {
    uint32_t   chunk_bytes = data_profile_chunk;
    const auto records     = store->subscriptions();
    for (const auto& record : records.subscriptions) {
      if (record.request.subscription_id == subscription) {
        chunk_bytes = record.request.maximum_chunk_bytes;
      }
    }
    const std::size_t buffer = association.send_buffer_bytes();
    if (buffer <= 8192 + chunk_bytes) {
      return 1;
    }
    return static_cast<uint32_t>(std::max<std::size_t>(1, (buffer - 8192) / (chunk_bytes + 512)));
  }

  management_response reject(const management::identifier& transaction, std::string detail) noexcept
  {
    management_response response;
    response.transaction_id = transaction;
    response.code           = management::result_code::invalid_request;
    response.error_text     = std::move(detail);
    return response;
  }

  /// Store subscription ids created by one association, so the association's
  /// share of the global store can be bounded and reclaimed when it closes
  /// instead of lingering until the lease lapses.
  struct association_subscriptions {
    std::vector<management::identifier> ids;
  };

  association_subscriptions& subscriptions_of(sctp_association& association)
  {
    auto* existing = static_cast<association_subscriptions*>(association.context());
    if (existing != nullptr) {
      return *existing;
    }
    auto  created = std::make_shared<association_subscriptions>();
    auto* raw     = created.get();
    association.set_context(std::move(created));
    return *raw;
  }

  management_response handle(sctp_association& association, management_request request) noexcept
  {
    if (auto* control = std::get_if<data_subscription_control>(&request)) {
      subscription_controls.fetch_add(1, std::memory_order_relaxed);
      auto& owned = subscriptions_of(association);
      if (control->action == data_subscription_action::create) {
        if (owned.ids.size() >= config.maximum_subscriptions_per_association) {
          rejected_operations.fetch_add(1, std::memory_order_relaxed);
          return reject(control->header.transaction_id, "association subscription limit reached");
        }
        // Queue depth is server-assigned and deterministic so that a renewal
        // with identical wire parameters compares equal to the live record.
        control->queue_depth_chunks = config.queue_depth_chunks;
      }
      const data_subscription_action action   = control->action;
      auto                           response = management.handle(request);
      if (auto* payload = std::get_if<data_subscription_result>(&response.payload)) {
        for (auto& record : payload->subscriptions) {
          placement_for(record);
        }
        if (response.code == management::result_code::ok) {
          if (action == data_subscription_action::create) {
            for (const auto& record : payload->subscriptions) {
              const auto& id = record.request.subscription_id;
              if (std::find(owned.ids.begin(), owned.ids.end(), id) == owned.ids.end()) {
                owned.ids.push_back(id);
              }
            }
          } else if (action == data_subscription_action::remove && control->subscription_id) {
            owned.ids.erase(std::remove(owned.ids.begin(), owned.ids.end(), *control->subscription_id),
                            owned.ids.end());
          }
        }
      }
      return response;
    }
    if (auto* poll = std::get_if<data_poll_query>(&request)) {
      polls.fetch_add(1, std::memory_order_relaxed);
      poll->maximum_chunks = std::min(poll->maximum_chunks, chunk_budget(association, poll->subscription_id));
      auto response        = management.handle(request);
      if (auto* payload = std::get_if<data_publication_result>(&response.payload)) {
        if (payload->subscription) {
          placement_for(*payload->subscription);
        }
      }
      return response;
    }
    rejected_operations.fetch_add(1, std::memory_order_relaxed);
    return reject(std::visit([](const auto& operation) { return operation.header.transaction_id; }, request),
                  "operation is not part of the data profile");
  }

  management_service&                      management;
  std::shared_ptr<portable_data_store>     store;
  e3dp_sctp_server_config                  config;
  std::unique_ptr<sctp_association_server> transport;
  std::atomic<uint64_t>                    requests{0};
  std::atomic<uint64_t>                    malformed{0};
  std::atomic<uint64_t>                    rejected_operations{0};
  std::atomic<uint64_t>                    subscription_controls{0};
  std::atomic<uint64_t>                    polls{0};
};

void e3dp_sctp_server::on_message(sctp_association& association, ocudu::span<const uint8_t> message)
{
  state->requests.fetch_add(1, std::memory_order_relaxed);
  auto                request = fbs_codec::decode_request(message);
  management_response response;
  if (!request) {
    state->malformed.fetch_add(1, std::memory_order_relaxed);
    response = state->reject(management::identifier{}, request.detail);
  } else {
    response = state->handle(association, std::move(*request.value));
  }
  auto encoded = fbs_codec::encode(response);
  if (!encoded) {
    // A response that cannot be encoded is replaced by a bare error so the
    // client still receives exactly one reply per request.
    encoded = fbs_codec::encode(state->reject(response.transaction_id, "response exceeds the message bound"));
  }
  if (!encoded || !association.send(std::move(*encoded.value), 0)) {
    association.close();
  }
}

void e3dp_sctp_server::on_closed(sctp_association& association)
{
  // Mirror the E3AP carrier: reclaim the peer's store subscriptions when the
  // association drops instead of letting them linger until the lease lapses.
  auto* owned = static_cast<implementation::association_subscriptions*>(association.context());
  if (owned == nullptr) {
    return;
  }
  for (const auto& id : owned->ids) {
    data_subscription_control request;
    // The management service requires a nonzero transaction id; reuse the
    // (nonzero by construction) subscription id so the internally generated
    // removal passes validation.
    request.header.transaction_id = id;
    request.action                = data_subscription_action::remove;
    request.subscription_id       = id;
    static_cast<void>(state->management.handle(management_request{std::move(request)}));
  }
  owned->ids.clear();
}

e3dp_sctp_server::e3dp_sctp_server(std::unique_ptr<implementation> state_) : state(std::move(state_)) {}

e3dp_sctp_server_create_result e3dp_sctp_server::create(management_service&                  management,
                                                        std::shared_ptr<portable_data_store> store,
                                                        e3dp_sctp_server_config              config) noexcept
{
  if (!store) {
    return {nullptr, "E3 data-profile server requires the portable data store"};
  }
  if (config.queue_depth_chunks == 0 || config.queue_depth_chunks > 1024) {
    return {nullptr, "invalid E3 data-profile queue depth"};
  }
  if (config.maximum_subscriptions_per_association == 0 || config.maximum_subscriptions_per_association > 1024) {
    return {nullptr, "invalid E3 data-profile per-association subscription limit"};
  }
  try {
    // Port zero binds an ephemeral port (tests); the application configuration
    // supplies the reference client's 38472 default.
    config.transport.payload_protocol_id   = e3dp_payload_protocol;
    config.transport.maximum_message_bytes = fbs_codec::maximum_message_size;
    config.transport.nof_streams           = std::max<uint16_t>(config.transport.nof_streams, 1);
    auto state     = std::make_unique<implementation>(management, std::move(store), std::move(config));
    auto server    = std::unique_ptr<e3dp_sctp_server>(new e3dp_sctp_server(std::move(state)));
    auto transport = sctp_association_server::create(*server, server->state->config.transport);
    if (!transport) {
      return {nullptr, std::move(transport.detail)};
    }
    server->state->transport = std::move(transport.server);
    return {std::move(server), {}};
  } catch (const std::exception& exception) {
    return {nullptr, exception.what()};
  } catch (...) {
    return {nullptr, "unknown E3 data-profile server construction failure"};
  }
}

e3dp_sctp_server::~e3dp_sctp_server()
{
  stop();
}

void e3dp_sctp_server::stop() noexcept
{
  if (state != nullptr && state->transport != nullptr) {
    state->transport->stop();
  }
}

uint16_t e3dp_sctp_server::port() const noexcept
{
  return state != nullptr && state->transport != nullptr ? state->transport->port() : 0;
}

e3dp_sctp_server_metrics e3dp_sctp_server::metrics() const noexcept
{
  e3dp_sctp_server_metrics result;
  if (state == nullptr) {
    return result;
  }
  if (state->transport != nullptr) {
    result.transport = state->transport->metrics();
  }
  result.requests              = state->requests.load(std::memory_order_relaxed);
  result.malformed_requests    = state->malformed.load(std::memory_order_relaxed);
  result.rejected_operations   = state->rejected_operations.load(std::memory_order_relaxed);
  result.subscription_controls = state->subscription_controls.load(std::memory_order_relaxed);
  result.polls                 = state->polls.load(std::memory_order_relaxed);
  return result;
}
