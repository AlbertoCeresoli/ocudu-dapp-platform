// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/e3ap_sctp_server.h"
#include "ocudu/dapp/e3/aper_codec.h"
#include "ocudu/dapp/e3/e3_connection_registry.h"
#include "ocudu/dapp/e3/e3ap_codec.h"
#include "ocudu/dapp/e3/e3ap_libe3_codec.h"
#include "ocudu/dapp/e3/management_service.h"
#include "ocudu/dapp/e3/shared_region.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <unistd.h>
#include <unordered_map>

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;

namespace {

int64_t realtime_ns() noexcept
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

uint64_t monotonic_ns() noexcept
{
  const auto value =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return value > 0 ? static_cast<uint64_t>(value) : 1;
}

/// Partially reassembled inbound control action (fragments share the PDU id).
struct control_assembly {
  uint32_t             pdu_id{};
  uint16_t             ran_function{};
  uint16_t             control_id{};
  uint32_t             fragment_count{};
  uint32_t             next_index{};
  uint32_t             payload_bytes{};
  uint64_t             started_ns{};
  std::vector<uint8_t> bytes;
};

/// Partially reassembled published object drained from the store.
struct object_assembly {
  bool active{};
  /// Chunks go straight into the shared-region row (by-reference delivery).
  bool direct{};
  /// The object failed its filters at chunk 0; later chunks are ignored silently.
  bool                 skipping{};
  uint64_t             sequence{};
  uint64_t             route_epoch{};
  uint32_t             cell_id{};
  uint32_t             chunk_count{};
  uint32_t             next_index{};
  uint64_t             tensor_size{};
  uint64_t             dropped_objects{};
  std::vector<uint8_t> metadata;
  std::vector<uint8_t> tensor;

  void reset() noexcept
  {
    active   = false;
    direct   = false;
    skipping = false;
    metadata.clear();
    tensor.clear();
  }
};

/// One telemetry stream inside one subscription.
struct stream_binding {
  uint16_t                               telemetry_id{};
  uint32_t                               interface_id{};
  management::identifier                 store_id;
  std::vector<uint16_t>                  rnti_filter;
  uint64_t                               minimum_interval_ns{};
  std::unordered_map<uint16_t, uint64_t> last_emission_ns;
  object_assembly                        assembly;
};

struct subscription_state {
  uint16_t                    subscription_id{};
  uint16_t                    ran_function{};
  std::vector<stream_binding> streams;
  /// Present when the dApp asked for by-reference delivery.
  std::unique_ptr<e3_shared_region> region;
  uint32_t                          minimum_by_reference_bytes{};
};

/// Element type of the bulk field of each telemetry record kind, when known.
std::optional<e3ap_element_type> element_type_of(uint32_t interface_id) noexcept
{
  const auto kind = telemetry_kind_of(interface_id);
  if (!kind) {
    return std::nullopt;
  }
  switch (*kind) {
    case telemetry_kind::pusch_symbols:
    case telemetry_kind::srs_channel:
    case telemetry_kind::pusch_dmrs_channel:
    case telemetry_kind::pusch_grid:
      return e3ap_element_type::complex_fp32;
    case telemetry_kind::pusch_crc:
    case telemetry_kind::gnb_metrics_json:
      return e3ap_element_type::uint8;
  }
  return std::nullopt;
}

struct dapp_session {
  uint64_t    association_id{};
  uint64_t    opened_ns{};
  std::string peer_address;
  bool        setup_done{};
  /// The peer stamped its setup request, so it decodes the timestamp field;
  /// a peer built against the pre-timestamp module cannot, and gets none.
  bool peer_timestamps{};
  /// The peer speaks the upstream libe3 E3AP 1.0.0 module (fixed at setup).
  bool                                   libe3{};
  uint16_t                               dapp_id{};
  std::string                            dapp_name;
  uint32_t                               next_pdu_id{1};
  uint16_t                               next_subscription_id{1};
  std::map<uint16_t, subscription_state> subscriptions;
  std::optional<control_assembly>        control;
  uint64_t                               control_requests{};

  uint32_t take_pdu_id() noexcept
  {
    const uint32_t id   = next_pdu_id;
    const uint32_t last = libe3 ? e3ap_libe3_codec::maximum_message_id : 0xFFFFFFFFU;
    next_pdu_id         = next_pdu_id >= last ? 1 : next_pdu_id + 1;
    return id;
  }

  /// Encodes with the module the peer speaks.
  codec_result<std::vector<uint8_t>> encode(const e3ap_pdu& pdu) const noexcept
  {
    return libe3 ? e3ap_libe3_codec::encode(pdu) : e3ap_codec::encode(pdu);
  }
};

std::optional<uint16_t> record_rnti(uint32_t interface_id, ocudu::span<const uint8_t> metadata) noexcept
{
  const auto kind = telemetry_kind_of(interface_id);
  if (!kind || metadata.size() < sizeof(telemetry_metadata_header) + sizeof(uint16_t)) {
    return std::nullopt;
  }
  switch (*kind) {
    case telemetry_kind::pusch_symbols:
    case telemetry_kind::pusch_crc:
    case telemetry_kind::srs_channel:
    case telemetry_kind::pusch_dmrs_channel:
    case telemetry_kind::pusch_grid: {
      // Every per-UE record places the RNTI directly after the common header.
      uint16_t rnti = 0;
      std::memcpy(&rnti, metadata.data() + sizeof(telemetry_metadata_header), sizeof(rnti));
      return rnti;
    }
    case telemetry_kind::gnb_metrics_json:
      return std::nullopt;
  }
  return std::nullopt;
}

portable_drop_policy to_store_policy(telemetry_drop_policy policy) noexcept
{
  switch (policy) {
    case telemetry_drop_policy::drop_newest:
      return portable_drop_policy::drop_newest;
    case telemetry_drop_policy::drop_oldest:
      return portable_drop_policy::drop_oldest;
    case telemetry_drop_policy::coalesce_latest:
      return portable_drop_policy::coalesce_latest;
  }
  return portable_drop_policy::coalesce_latest;
}

bool protocol_version_supported(const std::string& version) noexcept
{
  return version.rfind("1.", 0) == 0;
}

} // namespace

uint32_t ocudu::dapp::e3::telemetry_queue_depth(uint16_t telemetry_id, uint32_t large_object_depth) noexcept
{
  switch (telemetry_id) {
    case telemetry_id_pusch_crc:
      // One byte per object, but every transport block publishes: sixteen
      // chunks absorb multi-UE grant bursts between 5 ms drains.
      return 16;
    case telemetry_id_srs_channel:
    case telemetry_id_pusch_dmrs_channel:
      // Coefficient vectors are a few KiB; eight objects cover several UEs.
      return 8;
    case telemetry_id_gnb_metrics_json:
      // Reports are tens of KiB and periodic; two whole reports of up to 256 KiB.
      return 8;
    default:
      return std::max<uint32_t>(1, large_object_depth);
  }
}

struct e3ap_sctp_server::implementation {
  implementation(management_service*                     management_,
                 std::shared_ptr<portable_data_store>    store_,
                 e3ap_sctp_server_config                 config_,
                 std::shared_ptr<e3_connection_registry> connections_) :
    management(management_), store(std::move(store_)), config(std::move(config_)), connections(std::move(connections_))
  {
  }

  // ------------------------------------------------------------------------
  // Helpers
  // ------------------------------------------------------------------------

  dapp_session* session_of(sctp_association& association) noexcept
  {
    return static_cast<dapp_session*>(association.context());
  }

  struct pending_xapp_control {
    uint16_t             dapp_identifier{};
    uint16_t             ran_function_identifier{};
    std::vector<uint8_t> data;
    uint64_t             association_id{};
  };
  static constexpr std::size_t maximum_pending_xapp_controls = 64;
  static constexpr std::size_t maximum_recent_reports        = 16;

  /// One namespace, partitioned by wire bounds. Protected by cross_thread_mutex.
  /// Each ID is bound to a non-reused transport association, including queued controls.
  std::array<uint64_t, 65536> dapp_associations{};
  uint16_t                    next_native_id{e3ap_libe3_codec::maximum_identifier + 1};
  uint64_t                    shared_region_bytes{};

  std::optional<uint16_t> allocate_dapp_id(const dapp_session& session) noexcept
  {
    std::lock_guard<std::mutex> lock(cross_thread_mutex);
    if (session.libe3) {
      for (uint16_t id = e3ap_libe3_codec::maximum_identifier; id >= 1; --id) {
        if (dapp_associations[id] == 0) {
          dapp_associations[id] = session.association_id;
          return id;
        }
      }
    } else {
      constexpr uint16_t first = e3ap_libe3_codec::maximum_identifier + 1;
      for (uint32_t remaining = 65536U - first; remaining != 0; --remaining) {
        const uint16_t id = next_native_id;
        next_native_id    = id == 65535 ? first : id + 1;
        if (dapp_associations[id] == 0) {
          dapp_associations[id] = session.association_id;
          return id;
        }
      }
    }
    return std::nullopt;
  }

  void release_dapp_id(const dapp_session& session) noexcept
  {
    std::lock_guard<std::mutex> lock(cross_thread_mutex);
    if (dapp_associations[session.dapp_id] == session.association_id) {
      dapp_associations[session.dapp_id] = 0;
    }
  }

  mutable std::mutex                                 cross_thread_mutex;
  std::atomic<uint32_t>                              pending_xapp_count{0};
  std::deque<pending_xapp_control>                   pending_xapp_controls;
  std::function<void(const e3ap_dapp_report_event&)> report_handler;
  std::deque<e3ap_dapp_report_event>                 recent_reports;

  void handle_report(dapp_session&           session,
                     uint32_t                pdu_id,
                     std::optional<int64_t>  timestamp_ns,
                     const e3ap_dapp_report& body) noexcept
  {
    static_cast<void>(pdu_id);
    e3ap_dapp_report_event event;
    event.dapp_identifier         = session.dapp_id;
    event.dapp_name               = session.dapp_name;
    event.ran_function_identifier = body.ran_function_identifier;
    event.report_data             = body.report_data;
    event.timestamp_ns            = timestamp_ns;
    event.received_ns             = monotonic_ns();
    event.association_id          = session.association_id;
    reports_received.fetch_add(1, std::memory_order_relaxed);
    std::function<void(const e3ap_dapp_report_event&)> handler;
    {
      std::lock_guard<std::mutex> lock(cross_thread_mutex);
      if (recent_reports.size() >= maximum_recent_reports) {
        recent_reports.pop_front();
      }
      recent_reports.push_back(event);
      handler = report_handler;
    }
    if (handler) {
      handler(event);
    }
  }

  /// Sends queued xApp control actions to their dApps; unknown dApps are dropped.
  void flush_xapp_controls(std::vector<sctp_association*>& associations) noexcept
  {
    // One relaxed load per tick; the mutex is taken only when work is queued.
    if (pending_xapp_count.load(std::memory_order_relaxed) == 0) {
      return;
    }
    std::deque<pending_xapp_control> queued;
    {
      std::lock_guard<std::mutex> lock(cross_thread_mutex);
      queued.swap(pending_xapp_controls);
      pending_xapp_count.store(0, std::memory_order_relaxed);
    }
    for (auto& item : queued) {
      bool delivered = false;
      for (auto* association : associations) {
        auto* session = session_of(*association);
        if (session == nullptr || !session->setup_done || session->dapp_id != item.dapp_identifier ||
            session->association_id != item.association_id) {
          continue;
        }
        e3ap_xapp_control_action action;
        action.dapp_identifier         = item.dapp_identifier;
        action.ran_function_identifier = item.ran_function_identifier;
        action.xapp_control_data       = std::move(item.data);
        delivered                      = send_pdu(*association, *session, std::move(action), e3ap_procedure_stream);
        break;
      }
      if (delivered) {
        xapp_controls_sent.fetch_add(1, std::memory_order_relaxed);
      } else {
        xapp_controls_dropped.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  void publish_connection(const dapp_session& session) noexcept
  {
    if (!connections) {
      return;
    }
    e3_connection_record record;
    record.association_id = session.association_id;
    record.opened_ns      = session.opened_ns;
    record.peer_address   = session.peer_address;
    record.setup_complete = session.setup_done;
    if (session.setup_done) {
      record.dapp_id   = session.dapp_id;
      record.dapp_name = session.dapp_name;
    }
    record.control_requests = session.control_requests;
    record.subscriptions.reserve(session.subscriptions.size());
    for (const auto& [id, subscription] : session.subscriptions) {
      e3_subscription_record item;
      item.subscription_id = id;
      item.ran_function_id = subscription.ran_function;
      item.telemetry_identifiers.reserve(subscription.streams.size());
      for (const auto& stream : subscription.streams) {
        item.telemetry_identifiers.push_back(stream.telemetry_id);
      }
      record.subscriptions.push_back(std::move(item));
    }
    static_cast<void>(connections->upsert(std::move(record)));
  }

  bool send_pdu(sctp_association& association, dapp_session& session, e3ap_message message, uint16_t stream) noexcept
  {
    e3ap_pdu pdu;
    pdu.id = session.take_pdu_id();
    if (session.peer_timestamps) {
      pdu.timestamp_ns = realtime_ns();
    }
    pdu.message  = std::move(message);
    auto encoded = session.encode(pdu);
    if (!encoded) {
      protocol_errors.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    return association.send(std::move(*encoded.value), stream);
  }

  bool
  send_reply(sctp_association& association, dapp_session& session, uint32_t request_id, e3ap_message message) noexcept
  {
    e3ap_pdu pdu;
    pdu.id = request_id;
    if (session.peer_timestamps) {
      pdu.timestamp_ns = realtime_ns();
    }
    pdu.message  = std::move(message);
    auto encoded = session.encode(pdu);
    if (!encoded) {
      protocol_errors.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    return association.send(std::move(*encoded.value), e3ap_procedure_stream);
  }

  void ack(sctp_association&         association,
           dapp_session&             session,
           uint32_t                  request_id,
           bool                      positive,
           std::optional<e3ap_cause> cause  = std::nullopt,
           std::string               detail = {}) noexcept
  {
    e3ap_message_ack reply;
    reply.request_id    = request_id;
    reply.response_code = positive ? e3ap_response_code::positive : e3ap_response_code::negative;
    reply.cause         = cause;
    // Truncate to the E3-Text schema bound here: an over-long detail would fail
    // the ASN.1 size constraint inside encode and silently suppress the reply —
    // exactly the diagnostic an integrator relies on.
    if (detail.size() > 512) {
      detail.resize(512);
    }
    reply.detail = std::move(detail);
    send_reply(association, session, request_id, reply);
  }

  management::identifier
  store_identifier(const dapp_session& session, uint16_t subscription_id, uint16_t telemetry_id) const noexcept
  {
    management::identifier id;
    std::memcpy(id.bytes.data(), "E3AP", 4);
    const auto association = static_cast<uint32_t>(session.association_id);
    std::memcpy(id.bytes.data() + 4, &association, sizeof(association));
    std::memcpy(id.bytes.data() + 8, &subscription_id, sizeof(subscription_id));
    std::memcpy(id.bytes.data() + 10, &telemetry_id, sizeof(telemetry_id));
    id.bytes[12] = static_cast<uint8_t>(session.dapp_id >> 8);
    id.bytes[13] = static_cast<uint8_t>(session.dapp_id);
    return id;
  }

  std::vector<e3ap_ran_function_definition> ran_functions() const
  {
    std::vector<e3ap_ran_function_definition> functions;
    {
      telemetry_ran_function_data data;
      data.streams               = config.streams;
      data.maximum_object_bytes  = config.maximum_payload_bytes;
      data.maximum_subscriptions = config.maximum_subscriptions_per_dapp;
      for (auto& stream : data.streams) {
        if (stream.cell_ids.empty()) {
          stream.cell_ids = config.cell_ids;
        }
      }
      auto encoded = e3ap_codec::encode(data);
      if (encoded) {
        e3ap_ran_function_definition function;
        function.ran_function_identifier = e3ap_ran_function_telemetry;
        for (const auto& stream : config.streams) {
          function.telemetry_identifiers.push_back(stream.telemetry_id);
        }
        function.ran_function_data    = std::move(*encoded.value);
        function.ran_function_name    = "OCUDU-TELEMETRY-SM";
        function.ran_function_version = "1.0.0";
        functions.push_back(std::move(function));
      }
    }
    if (management != nullptr) {
      management_ran_function_data data;
      auto                         encoded = e3ap_codec::encode(data);
      if (encoded) {
        e3ap_ran_function_definition function;
        function.ran_function_identifier = e3ap_ran_function_management;
        function.control_identifiers     = {e3ap_management_control_request};
        function.ran_function_data       = std::move(*encoded.value);
        function.ran_function_name       = "E3SM-OCUDU-DAPP-MGMT-V1";
        function.ran_function_version    = "1.0.0";
        functions.push_back(std::move(function));
      }
    }
    return functions;
  }

  const telemetry_stream_record* stream_record(uint16_t telemetry_id) const noexcept
  {
    for (const auto& stream : config.streams) {
      if (stream.telemetry_id == telemetry_id) {
        return &stream;
      }
    }
    return nullptr;
  }

  // ------------------------------------------------------------------------
  // Procedures
  // ------------------------------------------------------------------------

  bool allocate_dapp_id_into(dapp_session& session) noexcept
  {
    const auto id = allocate_dapp_id(session);
    if (!id) {
      return false;
    }
    session.dapp_id = *id;
    return true;
  }

  void handle_setup(sctp_association&         association,
                    dapp_session&             session,
                    uint32_t                  pdu_id,
                    const e3ap_setup_request& request) noexcept
  {
    e3ap_setup_response response;
    response.request_id            = pdu_id;
    response.ran_identifier        = config.ran_identifier;
    response.e3ap_protocol_version = std::string(e3ap_version_string);
    if (session.setup_done) {
      response.cause  = e3ap_cause::setup_already_complete;
      response.detail = "association already completed E3 setup";
      setups_rejected.fetch_add(1, std::memory_order_relaxed);
    } else if (!protocol_version_supported(request.e3ap_protocol_version)) {
      response.cause  = e3ap_cause::unsupported_protocol_version;
      response.detail = "E3AP protocol version must be 1.x";
      setups_rejected.fetch_add(1, std::memory_order_relaxed);
    } else if (!allocate_dapp_id_into(session)) {
      response.cause  = e3ap_cause::resource_exhausted;
      response.detail = "no dApp identifier is free for this wire profile";
      setups_rejected.fetch_add(1, std::memory_order_relaxed);
    } else {
      session.setup_done = true;
      if (session.libe3) {
        libe3_setups_completed.fetch_add(1, std::memory_order_relaxed);
      }
      session.dapp_name               = request.dapp_name;
      response.response_code          = e3ap_response_code::positive;
      response.dapp_identifier        = session.dapp_id;
      response.ran_functions          = ran_functions();
      response.ran_functions_present  = true;
      response.ran_instance           = ran_instance;
      response.maximum_payload_bytes  = config.maximum_payload_bytes;
      response.maximum_fragment_bytes = config.fragment_bytes;
      setups_completed.fetch_add(1, std::memory_order_relaxed);
    }
    publish_connection(session);
    send_reply(association, session, pdu_id, response);
  }

  void handle_subscription(sctp_association&                association,
                           dapp_session&                    session,
                           uint32_t                         pdu_id,
                           const e3ap_subscription_request& request) noexcept
  {
    e3ap_subscription_response response;
    response.request_id      = pdu_id;
    response.dapp_identifier = session.dapp_id;
    const auto reject        = [&](e3ap_cause cause, std::string detail) {
      response.cause = cause;
      // Same truncation rationale as ack(): keep the reply encodable.
      if (detail.size() > 512) {
        detail.resize(512);
      }
      response.detail = std::move(detail);
      send_reply(association, session, pdu_id, response);
    };
    if (!session.setup_done) {
      response.dapp_identifier = request.dapp_identifier;
      return reject(e3ap_cause::setup_required, "complete E3 setup before subscribing");
    }
    if (request.dapp_identifier != session.dapp_id) {
      return reject(e3ap_cause::invalid_request, "dApp identifier does not match this association");
    }
    if (request.ran_function_identifier != e3ap_ran_function_telemetry) {
      return reject(e3ap_cause::unsupported_ran_function, "only the telemetry RAN function accepts subscriptions");
    }
    if (request.telemetry_identifiers.empty() || !request.control_identifiers.empty()) {
      return reject(e3ap_cause::invalid_request, "telemetry subscriptions name at least one telemetry and no control");
    }
    if (session.subscriptions.size() >= config.maximum_subscriptions_per_dapp) {
      return reject(e3ap_cause::resource_exhausted, "subscription limit for this dApp reached");
    }
    telemetry_subscription_data data;
    if (!request.subscription_data.empty()) {
      auto decoded = e3ap_codec::decode_subscription(request.subscription_data);
      if (!decoded) {
        return reject(e3ap_cause::invalid_request,
                      "subscriptionData is not Telemetry-SubscriptionData: " + decoded.detail);
      }
      data = std::move(*decoded.value);
    }

    subscription_state created;
    created.ran_function = e3ap_ran_function_telemetry;
    const uint16_t last_subscription_id =
        session.libe3 ? static_cast<uint16_t>(e3ap_libe3_codec::maximum_identifier) : uint16_t{65535};
    if (session.subscriptions.size() >= last_subscription_id) {
      return reject(e3ap_cause::resource_exhausted, "subscription identifiers exhausted");
    }
    // Find a free alias; wrapping must never revoke another live subscription.
    while (session.subscriptions.count(session.next_subscription_id) != 0) {
      session.next_subscription_id =
          session.next_subscription_id >= last_subscription_id ? 1 : session.next_subscription_id + 1;
    }
    created.subscription_id = session.next_subscription_id;
    std::string failure;
    for (const uint16_t telemetry_id : request.telemetry_identifiers) {
      const auto* record    = stream_record(telemetry_id);
      const auto  interface = telemetry_interface_for(telemetry_id);
      if (record == nullptr || !interface) {
        failure = "unknown telemetry identifier " + std::to_string(telemetry_id);
        break;
      }
      if (std::any_of(created.streams.begin(), created.streams.end(), [&](const auto& item) {
            return item.telemetry_id == telemetry_id;
          })) {
        failure = "telemetry identifier repeated";
        break;
      }
      telemetry_subscription_config config_for;
      config_for.telemetry_id = telemetry_id;
      for (const auto& candidate : data.configs) {
        if (candidate.telemetry_id == telemetry_id) {
          config_for = candidate;
        }
      }
      stream_binding binding;
      binding.telemetry_id = telemetry_id;
      binding.interface_id = *interface;
      binding.store_id     = store_identifier(session, created.subscription_id, telemetry_id);
      binding.rnti_filter  = config_for.rnti_filter;
      // The subscriber's request (per-telemetry config first, then the
      // procedure-level periodicity) becomes the producer's capture demand;
      // an unspecified request keeps the advertised default floor. The
      // carrier thins this subscription back to its own interval when another
      // subscriber drives the producer faster, with 10 % slack so capture
      // jitter never halves the rate.
      const std::optional<uint32_t> requested_ms =
          config_for.minimum_interval_ms ? config_for.minimum_interval_ms : request.periodicity_ms;
      uint64_t effective_us =
          requested_ms ? uint64_t{*requested_ms} * 1000ULL : record->minimum_interval_us.value_or(0);
      // Enforce the advertised hardFloorIntervalUs at admission: it "bounds what
      // any subscriber may request", so a faster request must not drive the
      // store's demand signal below the operator's floor.
      effective_us                = std::max(effective_us, record->hard_floor_interval_us.value_or(0));
      binding.minimum_interval_ns = effective_us * 1000ULL - effective_us * 100ULL;

      portable_subscription_request store_request;
      store_request.subscription_id = binding.store_id;
      store_request.interface_id    = binding.interface_id;
      store_request.cell_id         = config_for.cell_id ? std::optional<uint32_t>(*config_for.cell_id) : std::nullopt;
      store_request.sample_every_n  = config_for.sample_every_n;
      store_request.queue_depth_chunks    = telemetry_queue_depth(telemetry_id, config.queue_depth_chunks);
      store_request.maximum_chunk_bytes   = 64 * 1024;
      store_request.lease_us              = config.lease_us;
      store_request.drop_policy           = to_store_policy(config_for.drop_policy);
      store_request.requested_interval_us = effective_us;
      auto admitted                       = store->subscribe(store_request);
      if (!admitted) {
        failure = "store rejected telemetry " + std::to_string(telemetry_id) + ": " + admitted.detail;
        break;
      }
      created.streams.push_back(std::move(binding));
    }
    if (!failure.empty()) {
      for (const auto& binding : created.streams) {
        static_cast<void>(store->unsubscribe(binding.store_id));
      }
      return reject(failure.rfind("store rejected", 0) == 0 ? e3ap_cause::resource_exhausted
                                                            : e3ap_cause::unsupported_identifier,
                    failure);
    }
    if (request.delivery_mode == e3ap_delivery_mode::by_reference) {
      const uint64_t row_bytes      = (64ULL + config.maximum_payload_bytes + 63) / 64 * 64;
      const uint64_t required_bytes = 64 + row_bytes * config.shared_region_rows;
      if (required_bytes > config.maximum_shared_region_bytes - shared_region_bytes) {
        for (const auto& binding : created.streams) {
          static_cast<void>(store->unsubscribe(binding.store_id));
        }
        return reject(e3ap_cause::resource_exhausted, "shared region admission budget exhausted");
      }
      e3_shared_region_config region_config;
      region_config.name = config.shared_region_name_prefix + "-" + std::to_string(::getpid()) + "-" +
                           std::to_string(session.association_id) + "-" + std::to_string(created.subscription_id);
      region_config.row_payload_bytes = config.maximum_payload_bytes;
      region_config.nof_rows          = std::max<uint16_t>(1, config.shared_region_rows);
      region_config.mode              = config.shared_region_mode;
      auto region                     = e3_shared_region::create(region_config);
      if (!region.first) {
        for (const auto& binding : created.streams) {
          static_cast<void>(store->unsubscribe(binding.store_id));
        }
        return reject(e3ap_cause::resource_exhausted, "cannot map a shared region: " + region.second);
      }
      created.region = std::move(region.first);
      shared_region_bytes += required_bytes;
      created.minimum_by_reference_bytes =
          request.minimum_by_reference_bytes.value_or(config.default_minimum_by_reference_bytes);
      response.shared_region = created.region->descriptor();
    }
    session.next_subscription_id =
        session.next_subscription_id >= last_subscription_id ? 1 : session.next_subscription_id + 1;
    const uint16_t id         = created.subscription_id;
    session.subscriptions[id] = std::move(created);
    subscriptions_created.fetch_add(1, std::memory_order_relaxed);
    live_subscriptions.fetch_add(1, std::memory_order_relaxed);
    publish_connection(session);

    response.response_code         = e3ap_response_code::positive;
    response.subscription_id       = id;
    response.telemetry_identifiers = request.telemetry_identifiers;
    response.control_identifiers   = std::vector<uint16_t>{};
    send_reply(association, session, pdu_id, response);
  }

  void remove_subscription(dapp_session& session, subscription_state& subscription) noexcept
  {
    if (subscription.region) {
      shared_region_bytes -= subscription.region->descriptor().region_bytes;
      subscription.region.reset();
    }
    for (const auto& binding : subscription.streams) {
      static_cast<void>(store->unsubscribe(binding.store_id));
    }
    subscriptions_removed.fetch_add(1, std::memory_order_relaxed);
    if (live_subscriptions.load(std::memory_order_relaxed) != 0) {
      live_subscriptions.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  void handle_subscription_delete(sctp_association&               association,
                                  dapp_session&                   session,
                                  uint32_t                        pdu_id,
                                  const e3ap_subscription_delete& request) noexcept
  {
    if (!session.setup_done) {
      return ack(association, session, pdu_id, false, e3ap_cause::setup_required, "complete E3 setup first");
    }
    if (request.dapp_identifier != session.dapp_id) {
      return ack(association, session, pdu_id, false, e3ap_cause::invalid_request, "dApp identifier mismatch");
    }
    auto found = session.subscriptions.find(request.subscription_id);
    if (found == session.subscriptions.end()) {
      return ack(association, session, pdu_id, false, e3ap_cause::not_found, "subscription does not exist");
    }
    remove_subscription(session, found->second);
    session.subscriptions.erase(found);
    publish_connection(session);
    if (session.libe3) {
      // libe3 confirms a delete with an E3-SubscriptionResponse that echoes
      // the subscription identifier; its dApp state machine waits for it.
      e3ap_subscription_response confirmation;
      confirmation.request_id      = pdu_id;
      confirmation.dapp_identifier = session.dapp_id;
      confirmation.response_code   = e3ap_response_code::positive;
      confirmation.subscription_id = request.subscription_id;
      send_reply(association, session, pdu_id, confirmation);
      return;
    }
    ack(association, session, pdu_id, true);
  }

  void handle_control(sctp_association&               association,
                      dapp_session&                   session,
                      uint32_t                        pdu_id,
                      const e3ap_dapp_control_action& action) noexcept
  {
    ++session.control_requests;
    if (!session.setup_done) {
      return ack(association, session, pdu_id, false, e3ap_cause::setup_required, "complete E3 setup first");
    }
    if (action.dapp_identifier != session.dapp_id) {
      return ack(association, session, pdu_id, false, e3ap_cause::invalid_request, "dApp identifier mismatch");
    }
    if (action.ran_function_identifier != e3ap_ran_function_management || management == nullptr) {
      return ack(association,
                 session,
                 pdu_id,
                 false,
                 e3ap_cause::unsupported_ran_function,
                 "no control endpoint for this RAN function");
    }
    if (action.control_identifier != e3ap_management_control_request) {
      return ack(association, session, pdu_id, false, e3ap_cause::unsupported_identifier, "unknown control identifier");
    }
    const uint32_t total = action.payload_bytes.value_or(static_cast<uint32_t>(action.action_data.size()));
    if (total == 0 || total > config.maximum_payload_bytes || total > aper_codec::maximum_message_size) {
      session.control.reset();
      return ack(association,
                 session,
                 pdu_id,
                 false,
                 e3ap_cause::payload_too_large,
                 "management request exceeds the payload bound");
    }
    if (action.fragment_index == 0) {
      control_assembly assembly;
      assembly.pdu_id         = pdu_id;
      assembly.ran_function   = action.ran_function_identifier;
      assembly.control_id     = action.control_identifier;
      assembly.fragment_count = action.fragment_count;
      assembly.payload_bytes  = total;
      assembly.started_ns     = monotonic_ns();
      // Grow with the fragments that actually arrive instead of committing the
      // full declared size on fragment 0: a peer that never sends fragment 1
      // must not pin megabytes of reserved-but-unused memory per association.
      assembly.bytes.reserve(std::min<uint32_t>(total, 64 * 1024));
      session.control = std::move(assembly);
    }
    if (!session.control || session.control->pdu_id != pdu_id || session.control->next_index != action.fragment_index ||
        session.control->fragment_count != action.fragment_count || session.control->payload_bytes != total ||
        session.control->bytes.size() + action.action_data.size() > total) {
      session.control.reset();
      protocol_errors.fetch_add(1, std::memory_order_relaxed);
      return ack(association, session, pdu_id, false, e3ap_cause::protocol_error, "control fragments are inconsistent");
    }
    session.control->bytes.insert(session.control->bytes.end(), action.action_data.begin(), action.action_data.end());
    ++session.control->next_index;
    if (session.control->next_index != session.control->fragment_count) {
      return; // wait for the remaining fragments; one ack answers the whole action
    }
    control_assembly complete = std::move(*session.control);
    session.control.reset();
    if (complete.bytes.size() != complete.payload_bytes) {
      protocol_errors.fetch_add(1, std::memory_order_relaxed);
      return ack(association, session, pdu_id, false, e3ap_cause::protocol_error, "control payload length mismatch");
    }
    auto request = aper_codec::decode_request(complete.bytes);
    if (!request) {
      control_failures.fetch_add(1, std::memory_order_relaxed);
      return ack(association,
                 session,
                 pdu_id,
                 false,
                 e3ap_cause::invalid_request,
                 "actionData is not an APER ManagementRequest: " + request.detail);
    }
    control_requests.fetch_add(1, std::memory_order_relaxed);
    // Published once per complete, validated action rather than per inbound
    // fragment: publish_connection takes the registry mutex and deep-copies the
    // record, so a fragmented action must not multiply that cost.
    publish_connection(session);
    // libe3 answers a control action with exactly one E3-MessageAck (its
    // module has no control response), so for those peers the ack is sent
    // after the service model has produced its outcome.
    if (!session.libe3) {
      ack(association, session, pdu_id, true);
    }

    // Remote-facing ceiling: the wire schema permits timeoutUs up to one hour,
    // but the runtime has a single lifecycle job worker — one long remote
    // action would pin it and starve every subsequent management operation,
    // including operator-initiated ones on the local UNIX socket.
    constexpr uint64_t maximum_remote_lifecycle_timeout_us = 30'000'000ULL;
    if (auto* lifecycle = std::get_if<lifecycle_control>(&*request.value)) {
      lifecycle->timeout_us = std::min(lifecycle->timeout_us, maximum_remote_lifecycle_timeout_us);
    }

    const auto response = management->handle(*request.value);
    auto       encoded  = aper_codec::encode(response);
    if (session.libe3) {
      const bool ok = response.code == management::result_code::ok ||
                      response.code == management::result_code::dry_run_ok ||
                      response.code == management::result_code::accepted;
      if (!ok) {
        control_failures.fetch_add(1, std::memory_order_relaxed);
      }
      ack(association,
          session,
          pdu_id,
          ok,
          ok ? std::nullopt : std::optional<e3ap_cause>(e3ap_cause::invalid_request),
          ok ? std::string{}
             : (response.error_text.empty() ? std::string(management::to_string(response.code)) : response.error_text));
      return;
    }
    e3ap_control_response outcome;
    outcome.request_id              = pdu_id;
    outcome.dapp_identifier         = session.dapp_id;
    outcome.ran_function_identifier = complete.ran_function;
    outcome.control_identifier      = complete.control_id;
    const bool accepted             = response.code == management::result_code::ok ||
                          response.code == management::result_code::dry_run_ok ||
                          response.code == management::result_code::accepted;
    outcome.response_code = accepted ? e3ap_response_code::positive : e3ap_response_code::negative;
    if (!accepted) {
      outcome.cause = e3ap_cause::invalid_request;
      outcome.detail =
          response.error_text.empty() ? std::string(management::to_string(response.code)) : response.error_text;
      if (outcome.detail.size() > 512) {
        outcome.detail.resize(512);
      }
    }
    if (!encoded) {
      control_failures.fetch_add(1, std::memory_order_relaxed);
      outcome.response_code = e3ap_response_code::negative;
      outcome.cause         = e3ap_cause::unspecified;
      outcome.detail        = "management response could not be encoded";
      send_reply(association, session, pdu_id, outcome);
      return;
    }
    const auto fragments = e3ap_codec::fragment(*encoded.value, config.fragment_bytes);
    if (fragments.size() > 65536 || association.queue_capacity_remaining() < fragments.size()) {
      control_failures.fetch_add(1, std::memory_order_relaxed);
      outcome.response_code = e3ap_response_code::negative;
      outcome.cause         = e3ap_cause::transport_overrun;
      outcome.detail        = "outbound queue cannot hold the management response";
      send_reply(association, session, pdu_id, outcome);
      return;
    }
    for (std::size_t index = 0; index != fragments.size(); ++index) {
      outcome.control_outcome.assign(fragments[index].begin(), fragments[index].end());
      outcome.fragment_index = static_cast<uint16_t>(index);
      outcome.fragment_count = static_cast<uint32_t>(fragments.size());
      outcome.payload_bytes  = static_cast<uint32_t>(encoded.value->size());
      send_reply(association, session, pdu_id, outcome);
    }
  }

  void handle_release(sctp_association&           association,
                      dapp_session&               session,
                      uint32_t                    pdu_id,
                      const e3ap_release_message& release) noexcept
  {
    if (!session.setup_done || release.dapp_identifier != session.dapp_id) {
      ack(association, session, pdu_id, false, e3ap_cause::invalid_request, "dApp identifier mismatch");
    } else {
      for (auto& item : session.subscriptions) {
        remove_subscription(session, item.second);
      }
      session.subscriptions.clear();
      ack(association, session, pdu_id, true);
    }
    association.close();
  }

  // ------------------------------------------------------------------------
  // Publication draining
  // ------------------------------------------------------------------------

  /// Reserves enough queue slots for one maximum-size object plus procedures.
  std::size_t required_headroom() const noexcept
  {
    return static_cast<std::size_t>((config.maximum_payload_bytes + config.fragment_bytes - 1) /
                                    config.fragment_bytes) +
           8;
  }

  void drain(sctp_association& association, dapp_session& session) noexcept
  {
    for (auto& item : session.subscriptions) {
      for (auto& binding : item.second.streams) {
        for (unsigned round = 0; round != 8; ++round) {
          if (association.queue_capacity_remaining() < required_headroom()) {
            return;
          }
          auto polled = store->poll(binding.store_id, 8);
          if (!polled) {
            if (polled.code == portable_subscription_code::not_found) {
              // The lease lapsed (for example while the association was stalled); renew.
              resubscribe(binding, item.second, session);
            }
            break;
          }
          if (polled.chunks.empty()) {
            break;
          }
          for (const auto& chunk : polled.chunks) {
            assemble(association, session, item.second, binding, chunk);
          }
          if (polled.chunks.size() < 8) {
            break;
          }
        }
      }
    }
  }

  void
  resubscribe(stream_binding& binding, const subscription_state& subscription, const dapp_session& session) noexcept
  {
    static_cast<void>(subscription);
    static_cast<void>(session);
    portable_subscription_request request;
    request.subscription_id     = binding.store_id;
    request.interface_id        = binding.interface_id;
    request.queue_depth_chunks  = telemetry_queue_depth(binding.telemetry_id, config.queue_depth_chunks);
    request.maximum_chunk_bytes = 64 * 1024;
    request.lease_us            = config.lease_us;
    request.drop_policy         = portable_drop_policy::coalesce_latest;
    static_cast<void>(store->subscribe(request));
    binding.assembly.reset();
  }

  void assemble(sctp_association&                 association,
                dapp_session&                     session,
                subscription_state&               subscription,
                stream_binding&                   binding,
                const portable_publication_chunk& chunk) noexcept
  {
    auto& assembly = binding.assembly;
    if (chunk.chunk_index == 0) {
      if (assembly.direct && subscription.region) {
        subscription.region->abort();
      }
      assembly.reset();
      assembly.active          = true;
      assembly.sequence        = chunk.publication_sequence;
      assembly.route_epoch     = chunk.route_epoch;
      assembly.cell_id         = chunk.cell_id;
      assembly.chunk_count     = chunk.chunk_count;
      assembly.next_index      = 0;
      assembly.tensor_size     = chunk.tensor_size;
      assembly.dropped_objects = chunk.dropped_objects;
      assembly.metadata        = chunk.metadata;
      if (chunk.tensor_size > config.maximum_payload_bytes) {
        assembly.reset();
        indications_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      // By reference: decide at chunk 0 (filters only need the metadata) and
      // stream the chunks straight into the shared-region row, so the tensor
      // is copied exactly once on this thread and never assembled in between.
      if (subscription.region && chunk.tensor_size >= subscription.minimum_by_reference_bytes) {
        if (!passes_filters(binding, assembly)) {
          indications_filtered.fetch_add(1, std::memory_order_relaxed);
          assembly.active   = false;
          assembly.skipping = true;
          return;
        }
        if (!subscription.region->begin(chunk.publication_sequence,
                                        chunk.route_epoch,
                                        binding.telemetry_id,
                                        static_cast<uint16_t>(chunk.cell_id),
                                        chunk.tensor_size)) {
          assembly.reset();
          indications_dropped.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        assembly.direct = true;
      } else {
        assembly.tensor.reserve(static_cast<std::size_t>(chunk.tensor_size));
      }
    } else if (assembly.skipping && chunk.publication_sequence == assembly.sequence &&
               chunk.route_epoch == assembly.route_epoch) {
      return;
    }
    if (!assembly.active || chunk.chunk_index != assembly.next_index || chunk.chunk_count != assembly.chunk_count ||
        chunk.publication_sequence != assembly.sequence || chunk.route_epoch != assembly.route_epoch ||
        chunk.tensor_size != assembly.tensor_size ||
        (assembly.direct ? false : assembly.tensor.size() + chunk.tensor_chunk.size() > assembly.tensor_size)) {
      if (assembly.direct && subscription.region) {
        subscription.region->abort();
      }
      assembly.reset();
      indications_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (assembly.direct) {
      if (!subscription.region->append(chunk.tensor_chunk)) {
        subscription.region->abort();
        assembly.reset();
        indications_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    } else {
      assembly.tensor.insert(assembly.tensor.end(), chunk.tensor_chunk.begin(), chunk.tensor_chunk.end());
    }
    ++assembly.next_index;
    if (assembly.next_index != assembly.chunk_count) {
      return;
    }
    if (assembly.direct) {
      auto written = subscription.region->commit();
      if (!written) {
        assembly.reset();
        indications_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      publish_object(association, session, subscription, binding, written);
    } else {
      if (assembly.tensor.size() != assembly.tensor_size) {
        assembly.reset();
        indications_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      publish_object(association, session, subscription, binding, std::nullopt);
    }
    assembly.reset();
  }

  bool passes_filters(stream_binding& binding, const object_assembly& assembly) noexcept
  {
    if (binding.rnti_filter.empty() && binding.minimum_interval_ns == 0) {
      return true;
    }
    const auto rnti = record_rnti(binding.interface_id, assembly.metadata);
    if (!binding.rnti_filter.empty()) {
      if (!rnti ||
          std::find(binding.rnti_filter.begin(), binding.rnti_filter.end(), *rnti) == binding.rnti_filter.end()) {
        return false;
      }
    }
    if (binding.minimum_interval_ns != 0) {
      const uint16_t key  = rnti.value_or(0);
      const uint64_t now  = monotonic_ns();
      auto&          last = binding.last_emission_ns[key];
      if (last != 0 && now - last < binding.minimum_interval_ns) {
        return false;
      }
      last = now;
      if (binding.last_emission_ns.size() > 4096) {
        binding.last_emission_ns.clear();
      }
    }
    return true;
  }

  void publish_object(sctp_association&                            association,
                      dapp_session&                                session,
                      const subscription_state&                    subscription,
                      stream_binding&                              binding,
                      const std::optional<e3_shared_region_write>& written) noexcept
  {
    auto& assembly = binding.assembly;
    // In-band objects are filtered here; by-reference objects were filtered at chunk 0.
    if (!written && !passes_filters(binding, assembly)) {
      indications_filtered.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // By reference the tensor already sits in the row: the record is built
    // without its bulk field (validated against the row bytes, no copy) and
    // the indication carries the descriptor.
    const ocudu::span<const uint8_t> tensor_view =
        written ? subscription.region->bytes_of(*written) : ocudu::span<const uint8_t>(assembly.tensor);
    auto indication = e3ap_codec::from_publication(binding.interface_id,
                                                   assembly.cell_id,
                                                   assembly.sequence,
                                                   assembly.route_epoch,
                                                   assembly.dropped_objects,
                                                   assembly.metadata,
                                                   tensor_view,
                                                   !written.has_value());
    if (!indication) {
      indications_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    std::optional<e3ap_tensor_descriptor> by_reference;
    if (written) {
      e3ap_posix_shared_location location;
      location.region_name = subscription.region->name();
      location.row_index   = written->row_index;
      location.byte_offset = written->byte_offset;
      e3ap_tensor_descriptor descriptor;
      descriptor.memory_domain = e3ap_memory_domain::posix_shared;
      descriptor.location      = location;
      descriptor.byte_length   = written->byte_length;
      descriptor.generation    = written->generation;
      descriptor.element_type  = element_type_of(binding.interface_id);
      by_reference             = std::move(descriptor);
    }
    auto encoded = e3ap_codec::encode(*indication.value);
    if (!encoded || encoded.value->size() > config.maximum_payload_bytes) {
      indications_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // The libe3 module carries whole payloads of at most 32 KiB and no
    // by-reference descriptor: larger objects cannot reach such a peer.
    if (session.libe3 && (encoded.value->size() > e3ap_libe3_codec::maximum_payload_bytes || by_reference)) {
      libe3_indications_dropped.fetch_add(1, std::memory_order_relaxed);
      indications_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const auto fragments = e3ap_codec::fragment(*encoded.value, config.fragment_bytes);
    if (fragments.empty() || fragments.size() > 65536 || association.queue_capacity_remaining() < fragments.size()) {
      indications_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    e3ap_indication_message message;
    message.dapp_identifier         = session.dapp_id;
    message.ran_function_identifier = subscription.ran_function;
    message.subscription_id         = subscription.subscription_id;
    message.telemetry_identifier    = binding.telemetry_id;
    message.sequence                = assembly.sequence;
    message.fragment_count          = static_cast<uint32_t>(fragments.size());
    message.payload_bytes           = static_cast<uint32_t>(encoded.value->size());
    message.by_reference            = std::move(by_reference);
    if (message.by_reference) {
      indications_by_reference.fetch_add(1, std::memory_order_relaxed);
    }
    for (std::size_t index = 0; index != fragments.size(); ++index) {
      message.fragment_index = static_cast<uint16_t>(index);
      message.protocol_data.assign(fragments[index].begin(), fragments[index].end());
      if (!send_pdu(association, session, message, e3ap_indication_stream)) {
        indications_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
    indications_sent.fetch_add(1, std::memory_order_relaxed);
  }

  management_service*                      management;
  std::shared_ptr<portable_data_store>     store;
  e3ap_sctp_server_config                  config;
  std::shared_ptr<e3_connection_registry>  connections;
  management::identifier                   ran_instance;
  std::unique_ptr<sctp_association_server> transport;
  std::atomic<uint64_t>                    setups_completed{0};
  std::atomic<uint64_t>                    setups_rejected{0};
  std::atomic<uint64_t>                    subscriptions_created{0};
  std::atomic<uint64_t>                    subscriptions_removed{0};
  std::atomic<uint64_t>                    control_requests{0};
  std::atomic<uint64_t>                    control_failures{0};
  std::atomic<uint64_t>                    indications_sent{0};
  std::atomic<uint64_t>                    indications_filtered{0};
  std::atomic<uint64_t>                    indications_dropped{0};
  std::atomic<uint64_t>                    protocol_errors{0};
  std::atomic<uint64_t>                    indications_by_reference{0};
  std::atomic<uint64_t>                    reports_received{0};
  std::atomic<uint64_t>                    xapp_controls_sent{0};
  std::atomic<uint64_t>                    xapp_controls_dropped{0};
  std::atomic<uint64_t>                    libe3_setups_completed{0};
  std::atomic<uint64_t>                    libe3_indications_dropped{0};
  std::atomic<uint32_t>                    live_subscriptions{0};
};

// ---------------------------------------------------------------------------
// Carrier callbacks
// ---------------------------------------------------------------------------

void e3ap_sctp_server::on_opened(sctp_association& association)
{
  auto session            = std::make_shared<dapp_session>();
  session->association_id = association.id();
  session->opened_ns      = monotonic_ns();
  session->peer_address   = association.peer_address();
  association.set_context(session);
  state->publish_connection(*session);
}

void e3ap_sctp_server::on_message(sctp_association& association, ocudu::span<const uint8_t> message)
{
  auto* session = state->session_of(association);
  if (session == nullptr) {
    association.close();
    return;
  }
  // The dialect is fixed by the setup request: until setup completes a PDU
  // that fails the native module is retried under the libe3 module, and a
  // libe3 setup request binds the association to that module for good.
  codec_result<e3ap_pdu> decoded = session->libe3 ? e3ap_libe3_codec::decode(message) : e3ap_codec::decode(message);
  if (!session->setup_done && !session->libe3 && state->config.libe3_profile) {
    auto       alternative  = e3ap_libe3_codec::decode(message);
    const bool native_setup = decoded && std::holds_alternative<e3ap_setup_request>(decoded.value->message);
    const bool libe3_setup  = alternative && std::holds_alternative<e3ap_setup_request>(alternative.value->message);
    // A foreign setup can parse as a DIFFERENT native procedure. Successful
    // decoding alone is not profile negotiation; only a setup can select it.
    if (native_setup && libe3_setup) {
      state->protocol_errors.fetch_add(1, std::memory_order_relaxed);
      association.close();
      return;
    }
    if (libe3_setup) {
      session->libe3 = true;
      decoded        = std::move(alternative);
    }
  }
  if (!decoded) {
    state->protocol_errors.fetch_add(1, std::memory_order_relaxed);
    // A malformed PDU leaves the peer state untrusted; libe3-style peers reconnect.
    dapp_session& s = *session;
    state->ack(association, s, 1, false, e3ap_cause::protocol_error, "malformed E3-PDU: " + decoded.detail);
    association.close();
    return;
  }
  const uint32_t pdu_id = decoded.value->id;
  std::visit(
      [&](auto& body) {
        using body_type = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<body_type, e3ap_setup_request>) {
          session->peer_timestamps = decoded.value->timestamp_ns.has_value();
          state->handle_setup(association, *session, pdu_id, body);
        } else if constexpr (std::is_same_v<body_type, e3ap_subscription_request>) {
          state->handle_subscription(association, *session, pdu_id, body);
        } else if constexpr (std::is_same_v<body_type, e3ap_subscription_delete>) {
          state->handle_subscription_delete(association, *session, pdu_id, body);
        } else if constexpr (std::is_same_v<body_type, e3ap_dapp_control_action>) {
          state->handle_control(association, *session, pdu_id, body);
        } else if constexpr (std::is_same_v<body_type, e3ap_release_message>) {
          state->handle_release(association, *session, pdu_id, body);
        } else if constexpr (std::is_same_v<body_type, e3ap_dapp_report>) {
          const bool accepted = session->setup_done && body.dapp_identifier == session->dapp_id;
          if (accepted) {
            state->handle_report(*session, pdu_id, decoded.value->timestamp_ns, body);
          }
          state->ack(association,
                     *session,
                     pdu_id,
                     accepted,
                     session->setup_done ? std::nullopt : std::optional<e3ap_cause>(e3ap_cause::setup_required));
        } else if constexpr (std::is_same_v<body_type, e3ap_message_ack>) {
          // Acknowledgements from the dApp carry no state; nothing to do.
        } else {
          state->protocol_errors.fetch_add(1, std::memory_order_relaxed);
          state->ack(
              association, *session, pdu_id, false, e3ap_cause::protocol_error, "message is not valid from a dApp");
        }
      },
      decoded.value->message);
}

void e3ap_sctp_server::on_closed(sctp_association& association)
{
  auto* session = state->session_of(association);
  if (session == nullptr) {
    return;
  }
  for (auto& item : session->subscriptions) {
    state->remove_subscription(*session, item.second);
  }
  session->subscriptions.clear();
  if (session->setup_done) {
    state->release_dapp_id(*session);
  }
  if (state->connections) {
    state->connections->erase(session->association_id);
  }
}

void e3ap_sctp_server::on_timer(std::vector<sctp_association*>& associations)
{
  const uint64_t now = monotonic_ns();
  const uint64_t setup_ns =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(state->config.setup_timeout).count());
  for (auto* association : associations) {
    auto* session = state->session_of(*association);
    if (session == nullptr) {
      continue;
    }
    if (!session->setup_done) {
      if (setup_ns != 0 && now - session->opened_ns > setup_ns) {
        association->close();
      }
      continue;
    }
    // A stalled control reassembly must not be pinned forever by keepalives
    // that defeat the idle timer; drop it after 10 s and let the peer retry.
    constexpr uint64_t assembly_timeout_ns = 10ULL * 1000 * 1000 * 1000;
    if (session->control && now - session->control->started_ns > assembly_timeout_ns) {
      session->control.reset();
    }
    state->drain(*association, *session);
  }
  state->flush_xapp_controls(associations);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

e3ap_sctp_server::e3ap_sctp_server(std::unique_ptr<implementation> state_) : state(std::move(state_)) {}

e3ap_sctp_server_create_result e3ap_sctp_server::create(management_service*                     management,
                                                        std::shared_ptr<portable_data_store>    store,
                                                        e3ap_sctp_server_config                 config,
                                                        std::shared_ptr<e3_connection_registry> connections) noexcept
{
  if (!store) {
    return {nullptr, "E3AP SCTP server requires the portable data store"};
  }
  if (config.fragment_bytes == 0 || config.fragment_bytes > e3ap_maximum_fragment_bytes ||
      config.maximum_payload_bytes == 0 || config.maximum_payload_bytes > e3ap_maximum_payload_bytes ||
      config.maximum_subscriptions_per_dapp == 0 || config.maximum_subscriptions_per_dapp > 1024 ||
      config.lease_us == 0 || config.queue_depth_chunks == 0 || config.queue_depth_chunks > 1024 ||
      config.ran_identifier.empty() || config.ran_identifier.size() > 64 || config.setup_timeout.count() < 0 ||
      config.shared_region_rows == 0 || config.maximum_shared_region_bytes == 0 ||
      config.maximum_shared_region_bytes > 256ULL * 1024 * 1024) {
    return {nullptr, "invalid E3AP SCTP server bounds"};
  }
  try {
    if (config.streams.empty()) {
      config.streams = default_telemetry_streams();
    }
    config.transport.payload_protocol_id = e3ap_payload_protocol_id;
    config.transport.nof_streams         = std::max<uint16_t>(config.transport.nof_streams, 2);
    config.transport.maximum_message_bytes =
        std::max<std::size_t>(config.transport.maximum_message_bytes, e3ap_codec::maximum_pdu_bytes);

    auto state =
        std::make_unique<implementation>(management, std::move(store), std::move(config), std::move(connections));
    const uint64_t seed = monotonic_ns();
    std::memcpy(state->ran_instance.bytes.data(), "OCUDUE3A", 8);
    std::memcpy(state->ran_instance.bytes.data() + 8, &seed, sizeof(seed));
    auto server    = std::unique_ptr<e3ap_sctp_server>(new e3ap_sctp_server(std::move(state)));
    auto transport = sctp_association_server::create(*server, server->state->config.transport);
    if (!transport) {
      return {nullptr, std::move(transport.detail)};
    }
    server->state->transport = std::move(transport.server);
    return {std::move(server), {}};
  } catch (const std::exception& exception) {
    return {nullptr, exception.what()};
  } catch (...) {
    return {nullptr, "unknown E3AP SCTP server construction failure"};
  }
}

e3ap_sctp_server::~e3ap_sctp_server()
{
  stop();
}

void e3ap_sctp_server::stop() noexcept
{
  if (state != nullptr && state->transport != nullptr) {
    state->transport->stop();
  }
}

uint16_t e3ap_sctp_server::port() const noexcept
{
  return state != nullptr && state->transport != nullptr ? state->transport->port() : 0;
}

e3ap_sctp_server_metrics e3ap_sctp_server::metrics() const noexcept
{
  e3ap_sctp_server_metrics result;
  if (state == nullptr) {
    return result;
  }
  if (state->transport != nullptr) {
    result.transport = state->transport->metrics();
  }
  result.setups_completed          = state->setups_completed.load(std::memory_order_relaxed);
  result.setups_rejected           = state->setups_rejected.load(std::memory_order_relaxed);
  result.subscriptions_created     = state->subscriptions_created.load(std::memory_order_relaxed);
  result.subscriptions_removed     = state->subscriptions_removed.load(std::memory_order_relaxed);
  result.control_requests          = state->control_requests.load(std::memory_order_relaxed);
  result.control_failures          = state->control_failures.load(std::memory_order_relaxed);
  result.indications_sent          = state->indications_sent.load(std::memory_order_relaxed);
  result.indications_filtered      = state->indications_filtered.load(std::memory_order_relaxed);
  result.indications_dropped       = state->indications_dropped.load(std::memory_order_relaxed);
  result.protocol_errors           = state->protocol_errors.load(std::memory_order_relaxed);
  result.live_subscriptions        = state->live_subscriptions.load(std::memory_order_relaxed);
  result.indications_by_reference  = state->indications_by_reference.load(std::memory_order_relaxed);
  result.reports_received          = state->reports_received.load(std::memory_order_relaxed);
  result.xapp_controls_sent        = state->xapp_controls_sent.load(std::memory_order_relaxed);
  result.xapp_controls_dropped     = state->xapp_controls_dropped.load(std::memory_order_relaxed);
  result.libe3_setups_completed    = state->libe3_setups_completed.load(std::memory_order_relaxed);
  result.libe3_indications_dropped = state->libe3_indications_dropped.load(std::memory_order_relaxed);
  return result;
}

void e3ap_sctp_server::set_dapp_report_handler(std::function<void(const e3ap_dapp_report_event&)> handler) noexcept
{
  if (state == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(state->cross_thread_mutex);
  state->report_handler = std::move(handler);
}

std::vector<e3ap_dapp_report_event> e3ap_sctp_server::recent_reports() const noexcept
{
  std::vector<e3ap_dapp_report_event> result;
  if (state == nullptr) {
    return result;
  }
  std::lock_guard<std::mutex> lock(state->cross_thread_mutex);
  result.assign(state->recent_reports.begin(), state->recent_reports.end());
  return result;
}

bool e3ap_sctp_server::send_xapp_control_action(uint16_t             dapp_identifier,
                                                uint16_t             ran_function_identifier,
                                                std::vector<uint8_t> data) noexcept
{
  return send_xapp_control_action(dapp_identifier, ran_function_identifier, std::move(data), 0);
}

bool e3ap_sctp_server::send_xapp_control_action(uint16_t             dapp_identifier,
                                                uint16_t             ran_function_identifier,
                                                std::vector<uint8_t> data,
                                                uint64_t             expected_association_id) noexcept
{
  if (state == nullptr || data.empty() || data.size() > e3ap_maximum_fragment_bytes) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state->cross_thread_mutex);
  if (state->pending_xapp_controls.size() >= implementation::maximum_pending_xapp_controls) {
    return false;
  }
  const uint64_t association_id = state->dapp_associations[dapp_identifier];
  if (expected_association_id != 0 && expected_association_id != association_id) {
    return false;
  }
  try {
    state->pending_xapp_controls.push_back({dapp_identifier, ran_function_identifier, std::move(data), association_id});
  } catch (...) {
    return false;
  }
  state->pending_xapp_count.fetch_add(1, std::memory_order_relaxed);
  return true;
}
