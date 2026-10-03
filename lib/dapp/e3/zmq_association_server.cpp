// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/zmq_association_server.h"

#ifdef OCUDU_DAPP_E3_HAVE_ZMQ

#include "ocudu/dapp/e3/e3ap_v2_codec.h"
#include "ocudu/dapp/e3/e3ap_protobuf_codec.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <variant>
#include <zmq.h>

using namespace ocudu::dapp::e3;

namespace {

using clock_type = std::chrono::steady_clock;

template <typename T, typename = void>
struct has_dapp_identifier : std::false_type {};
template <typename T>
struct has_dapp_identifier<T, std::void_t<decltype(std::declval<const T&>().dapp_identifier)>> : std::true_type {};

/// Identifier the PDU is addressed from (dApp-to-RAN); setup requests and message acks carry none.
std::optional<uint16_t> dapp_identifier_of(const e3ap_message& message) noexcept
{
  return std::visit(
      [](const auto& body) -> std::optional<uint16_t> {
        if constexpr (has_dapp_identifier<std::decay_t<decltype(body)>>::value) {
          return body.dapp_identifier;
        } else {
          return std::nullopt;
        }
      },
      message);
}

/// What routing needs from a PDU. Protobuf is read in place (e3ap_protobuf_codec::peek, no
/// allocation); APER has no cheap partial read, so it is decoded.
struct route {
  uint32_t                pdu_id{};
  bool                    setup_request{};
  bool                    setup_response{};
  bool                    message_ack{};
  std::optional<uint16_t> dapp_identifier;
  uint32_t                request_id{};
  bool                    positive{};
};

std::optional<route> route_of(e3ap_encoding encoding, ocudu::span<const uint8_t> bytes) noexcept
{
  route result;
  if (encoding == e3ap_encoding::protobuf) {
    const auto view = e3ap_protobuf_codec::peek(bytes);
    if (!view) {
      return std::nullopt;
    }
    result.pdu_id          = view->pdu_id;
    result.setup_request   = view->message == 3;
    result.setup_response  = view->message == 4;
    result.message_ack     = view->message == 13;
    result.dapp_identifier = view->dapp_identifier;
    result.request_id      = view->request_id;
    result.positive        = view->positive;
    return result;
  }
  // Every addition read: OCUDU-profile sessions share the sockets with libe3 ones.
  auto decoded = e3ap_v2_codec::decode(bytes, e3ap_profile::ocudu);
  if (!decoded) {
    return std::nullopt;
  }
  const auto& message    = decoded.value->message;
  result.pdu_id          = decoded.value->id;
  result.setup_request   = std::holds_alternative<e3ap_setup_request>(message);
  result.dapp_identifier = dapp_identifier_of(message);
  if (const auto* response = std::get_if<e3ap_setup_response>(&message)) {
    result.setup_response = true;
    result.request_id     = response->request_id;
    result.positive       = response->response_code == e3ap_response_code::positive;
  } else if (const auto* ack = std::get_if<e3ap_message_ack>(&message)) {
    result.message_ack = true;
    result.request_id  = ack->request_id;
    result.positive    = ack->response_code == e3ap_response_code::positive;
  }
  return result;
}

void free_vector(void* data, void* hint) noexcept
{
  static_cast<void>(data);
  delete static_cast<std::vector<uint8_t>*>(hint);
}

void close_socket(void*& socket) noexcept
{
  if (socket != nullptr) {
    zmq_close(socket);
    socket = nullptr;
  }
}

/// Creates the directory of an ipc:// endpoint (libe3 does the same for its own sockets).
void prepare_ipc(const std::string& endpoint)
{
  static const std::string prefix = "ipc://";
  if (endpoint.rfind(prefix, 0) != 0) {
    return;
  }
  const std::filesystem::path path(endpoint.substr(prefix.size()));
  if (path.has_parent_path()) {
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
  }
}

} // namespace

/// One dApp session seen through the ZMQ sockets. Lives on the server thread only.
class zmq_virtual_association final : public sctp_association
{
public:
  zmq_virtual_association(uint64_t             id_,
                          void*                rep_,
                          void*                pub_,
                          std::size_t          limit_,
                          e3ap_encoding        encoding_,
                          sctp_server_metrics& metrics_) :
    association_id(id_),
    peer("zmq:" + std::to_string(id_)),
    rep(rep_),
    pub(pub_),
    limit(limit_),
    encoding(encoding_),
    metrics(metrics_)
  {
    last_activity = clock_type::now();
  }

  uint64_t           id() const noexcept override { return association_id; }
  const std::string& peer_address() const noexcept override { return peer; }
  std::size_t        send_buffer_bytes() const noexcept override { return limit; }
  std::size_t        queue_capacity_remaining() const noexcept override { return 512; }
  void               close() noexcept override { closing = true; }
  void  set_context(std::shared_ptr<void> context_) noexcept override { context_holder = std::move(context_); }
  void* context() const noexcept override { return context_holder.get(); }

  bool send(std::vector<uint8_t> message, uint16_t stream) noexcept override
  {
    static_cast<void>(stream);
    if (message.empty() || message.size() > limit) {
      ++metrics.send_queue_drops;
      return false;
    }
    if (track_ids) {
      remember_outbound_id(message);
    }
    if (awaiting_reply) {
      // The REQ that is waiting for its answer: the setup response (or the answer to whatever request
      // arrived on the setup socket). It also tells us the dApp identifier the carrier assigned.
      awaiting_reply = false;
      learn_identity(message);
      if (zmq_send(rep, message.data(), message.size(), 0) < 0) {
        ++metrics.transport_failures;
        return false;
      }
      ++metrics.messages_sent;
      return true;
    }
    // Zero-copy: the encoder's buffer is handed to ZMQ and freed when the PUB socket is done with it.
    auto*     owned = new (std::nothrow) std::vector<uint8_t>(std::move(message));
    zmq_msg_t frame;
    if (owned == nullptr || zmq_msg_init_data(&frame, owned->data(), owned->size(), free_vector, owned) != 0) {
      delete owned;
      ++metrics.send_queue_drops;
      return false;
    }
    if (zmq_msg_send(&frame, pub, ZMQ_DONTWAIT) < 0) {
      zmq_msg_close(&frame);
      ++metrics.send_queue_drops;
      return false;
    }
    ++metrics.messages_sent;
    return true;
  }

  /// True when this session recently sent a PDU with that id (routes an E3-MessageAck, which names
  /// no dApp).
  bool sent_recently(uint32_t pdu_id) const noexcept
  {
    return std::find(recent_ids.begin(), recent_ids.end(), pdu_id) != recent_ids.end();
  }

  const uint64_t    association_id;
  const std::string peer;
  bool              awaiting_reply{false};
  bool              closing{false};
  bool              opened{false};
  /// Set while more than one session is live: only then can an E3-MessageAck (which names no dApp)
  /// be ambiguous, so only then are sent PDU ids decoded and remembered.
  bool                    track_ids{false};
  std::optional<uint16_t> dapp_id;
  clock_type::time_point  last_activity;

private:
  void remember_outbound_id(const std::vector<uint8_t>& message) noexcept
  {
    const auto routed = route_of(encoding, message);
    if (!routed) {
      return;
    }
    if (recent_ids.size() == 64) {
      recent_ids.pop_front();
    }
    recent_ids.push_back(routed->pdu_id);
  }

  void learn_identity(const std::vector<uint8_t>& message) noexcept
  {
    const auto routed = route_of(encoding, message);
    if (!routed || !routed->setup_response) {
      return;
    }
    if (routed->positive && routed->dapp_identifier) {
      dapp_id = routed->dapp_identifier;
    } else {
      closing = true;
    }
  }

  void*                 rep;
  void*                 pub;
  const std::size_t     limit;
  const e3ap_encoding   encoding;
  sctp_server_metrics&  metrics;
  std::shared_ptr<void> context_holder;
  std::deque<uint32_t>  recent_ids;
};

struct zmq_association_server::implementation {
  implementation(sctp_carrier& carrier_, zmq_carrier_config config_) : carrier(carrier_), config(std::move(config_)) {}

  ~implementation()
  {
    stop();
    close_socket(rep);
    close_socket(pub);
    close_socket(sub);
    if (context != nullptr) {
      zmq_ctx_term(context);
    }
  }

  void open()
  {
    context = zmq_ctx_new();
    if (context == nullptr) {
      throw std::runtime_error("zmq_ctx_new failed");
    }
    const int linger  = 0;
    const int hwm     = 4096;
    auto      make_on = [&](int type, const std::string& endpoint, const char* what) {
      void* socket = zmq_socket(context, type);
      if (socket == nullptr) {
        throw std::runtime_error(std::string("zmq_socket failed for ") + what);
      }
      zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));
      if (type == ZMQ_PUB) {
        zmq_setsockopt(socket, ZMQ_SNDHWM, &hwm, sizeof(hwm));
      }
      if (type == ZMQ_SUB) {
        zmq_setsockopt(socket, ZMQ_SUBSCRIBE, "", 0);
        zmq_setsockopt(socket, ZMQ_RCVHWM, &hwm, sizeof(hwm));
      }
      const int64_t max_size = static_cast<int64_t>(config.maximum_message_bytes);
      zmq_setsockopt(socket, ZMQ_MAXMSGSIZE, &max_size, sizeof(max_size));
      prepare_ipc(endpoint);
      if (zmq_bind(socket, endpoint.c_str()) != 0) {
        const std::string detail = zmq_strerror(zmq_errno());
        zmq_close(socket);
        throw std::runtime_error(std::string("cannot bind the libe3 ZMQ ") + what + " socket to " + endpoint + ": " +
                                 detail);
      }
      return socket;
    };
    rep    = make_on(ZMQ_REP, config.setup_endpoint, "setup");
    pub    = make_on(ZMQ_PUB, config.publisher_endpoint, "publisher");
    sub    = make_on(ZMQ_SUB, config.subscriber_endpoint, "subscriber");
    worker = std::thread([this] { run(); });
  }

  void stop() noexcept
  {
    running.store(false, std::memory_order_relaxed);
    if (worker.joinable()) {
      worker.join();
    }
  }

  sctp_server_metrics metrics_copy() const noexcept
  {
    std::lock_guard<std::mutex> lock(metrics_mutex);
    return published;
  }

private:
  bool receive(void* socket, std::vector<uint8_t>& buffer) noexcept
  {
    zmq_msg_t message;
    zmq_msg_init(&message);
    const int size = zmq_msg_recv(&message, socket, ZMQ_DONTWAIT);
    if (size < 0) {
      zmq_msg_close(&message);
      return false;
    }
    const auto* data = static_cast<const uint8_t*>(zmq_msg_data(&message));
    buffer.assign(data, data + size);
    zmq_msg_close(&message);
    ++metrics.messages_received;
    return true;
  }

  zmq_virtual_association* find_by_dapp(uint16_t dapp_id) noexcept
  {
    for (auto& [id, association] : associations) {
      if (association->dapp_id == dapp_id && !association->closing) {
        return association.get();
      }
    }
    return nullptr;
  }

  zmq_virtual_association* find_by_sent_id(uint32_t pdu_id) noexcept
  {
    zmq_virtual_association* match = nullptr;
    std::size_t              live  = 0;
    zmq_virtual_association* only  = nullptr;
    for (auto& [id, association] : associations) {
      if (association->closing || !association->dapp_id) {
        continue;
      }
      ++live;
      only = association.get();
      if (association->sent_recently(pdu_id)) {
        if (match != nullptr) {
          return nullptr; // ambiguous: two sessions used the same id recently
        }
        match = association.get();
      }
    }
    return match != nullptr ? match : (live == 1 ? only : nullptr);
  }

  void deliver(zmq_virtual_association& association, const std::vector<uint8_t>& bytes) noexcept
  {
    association.last_activity = clock_type::now();
    if (!association.opened) {
      association.opened = true;
      carrier.on_opened(association);
    }
    carrier.on_message(association, ocudu::span<const uint8_t>(bytes.data(), bytes.size()));
  }

  void handle_setup_socket() noexcept
  {
    std::vector<uint8_t> bytes;
    if (!receive(rep, bytes)) {
      return;
    }
    // REQ/REP is lockstep: exactly one reply must leave for every request, whatever happens to it.
    const auto               decoded = route_of(config.encoding, bytes);
    zmq_virtual_association* target  = nullptr;
    if (decoded && !decoded->setup_request && decoded->dapp_identifier) {
      target = find_by_dapp(*decoded->dapp_identifier);
    }
    if (target == nullptr && decoded && decoded->setup_request) {
      if (live_count() < config.maximum_associations) {
        auto association = std::make_unique<zmq_virtual_association>(
            next_association_id++, rep, pub, config.maximum_message_bytes, config.encoding, metrics);
        target                     = association.get();
        associations[target->id()] = std::move(association);
        ++metrics.accepted_associations;
      } else {
        ++metrics.rejected_associations;
      }
    }
    if (target == nullptr) {
      if (!decoded) {
        ++metrics.transport_failures;
      }
      zmq_send(rep, "", 0, 0);
      return;
    }
    target->awaiting_reply = true;
    deliver(*target, bytes);
    if (target->awaiting_reply) {
      // The carrier answered nothing (malformed or refused request): unblock the REQ and drop the session.
      target->awaiting_reply = false;
      zmq_send(rep, "", 0, 0);
      if (!target->dapp_id) {
        target->closing = true;
      }
    }
  }

  void handle_subscriber_socket() noexcept
  {
    std::vector<uint8_t> bytes;
    for (int budget = 0; budget != 256 && receive(sub, bytes); ++budget) {
      if (zmq_virtual_association* only = single_session()) {
        // One session: no routing decision to make, and the carrier decodes (and validates the
        // dApp identifier) anyway.
        deliver(*only, bytes);
        continue;
      }
      const auto routed = route_of(config.encoding, bytes);
      if (!routed) {
        ++metrics.transport_failures;
        continue;
      }
      zmq_virtual_association* target = nullptr;
      if (routed->dapp_identifier) {
        target = find_by_dapp(*routed->dapp_identifier);
      } else if (routed->message_ack) {
        target = find_by_sent_id(routed->request_id);
      }
      if (target == nullptr) {
        // Another RAN's dApp, a session that is gone, or an ack we cannot attribute.
        ++unroutable;
        continue;
      }
      deliver(*target, bytes);
    }
  }

  zmq_virtual_association* single_session() noexcept
  {
    zmq_virtual_association* only = nullptr;
    for (auto& [id, association] : associations) {
      if (association->closing || !association->dapp_id) {
        continue;
      }
      if (only != nullptr) {
        return nullptr;
      }
      only = association.get();
    }
    return only;
  }

  std::size_t live_count() const noexcept
  {
    return static_cast<std::size_t>(std::count_if(
        associations.begin(), associations.end(), [](const auto& entry) { return !entry.second->closing; }));
  }

  void reap() noexcept
  {
    const auto now = clock_type::now();
    for (auto it = associations.begin(); it != associations.end();) {
      auto& association = *it->second;
      if (!association.closing && config.idle_timeout.count() > 0 &&
          now - association.last_activity > config.idle_timeout) {
        association.closing = true;
        ++metrics.idle_closures;
      }
      if (association.closing) {
        if (association.opened) {
          carrier.on_closed(association);
        }
        ++metrics.closed_associations;
        it = associations.erase(it);
      } else {
        ++it;
      }
    }
  }

  void run() noexcept
  {
    auto next_timer = clock_type::now();
    while (running.load(std::memory_order_relaxed)) {
      zmq_pollitem_t items[] = {{rep, 0, ZMQ_POLLIN, 0}, {sub, 0, ZMQ_POLLIN, 0}};
      const auto     wait_ms = std::max<long>(
          0, std::chrono::duration_cast<std::chrono::milliseconds>(next_timer - clock_type::now()).count());
      if (zmq_poll(items, 2, std::min<long>(wait_ms, 50)) < 0 && zmq_errno() != EINTR) {
        ++metrics.transport_failures;
      }
      if ((items[0].revents & ZMQ_POLLIN) != 0) {
        handle_setup_socket();
      }
      if ((items[1].revents & ZMQ_POLLIN) != 0) {
        handle_subscriber_socket();
      }
      if (clock_type::now() >= next_timer) {
        next_timer += config.timer_period;
        if (clock_type::now() > next_timer) {
          next_timer = clock_type::now() + config.timer_period;
        }
        timer_sessions.clear();
        for (auto& [id, association] : associations) {
          if (!association->closing && association->opened && association->dapp_id) {
            timer_sessions.push_back(association.get());
          }
        }
        for (auto* association : timer_sessions) {
          static_cast<zmq_virtual_association*>(association)->track_ids = timer_sessions.size() > 1;
        }
        if (!timer_sessions.empty()) {
          carrier.on_timer(timer_sessions);
        }
        metrics.live_associations = static_cast<uint32_t>(live_count());
        std::lock_guard<std::mutex> lock(metrics_mutex);
        published = metrics;
      }
      reap();
    }
    for (auto& [id, association] : associations) {
      if (association->opened) {
        carrier.on_closed(*association);
      }
    }
    associations.clear();
  }

public:
  sctp_carrier&      carrier;
  zmq_carrier_config config;
  void*              context{};
  void*              rep{};
  void*              pub{};
  void*              sub{};
  std::thread        worker;
  std::atomic<bool>  running{true};
  uint64_t           unroutable{0};

private:
  uint64_t                                                     next_association_id{1};
  std::vector<sctp_association*>                               timer_sessions;
  std::map<uint64_t, std::unique_ptr<zmq_virtual_association>> associations;
  sctp_server_metrics                                          metrics;
  mutable std::mutex                                           metrics_mutex;
  sctp_server_metrics                                          published;
};

bool zmq_association_server::available() noexcept
{
  return true;
}

zmq_association_server_create_result zmq_association_server::create(sctp_carrier&      carrier,
                                                                    zmq_carrier_config config) noexcept
{
  if (config.setup_endpoint.empty() || config.publisher_endpoint.empty() || config.subscriber_endpoint.empty() ||
      config.maximum_associations == 0 || config.timer_period.count() <= 0 || config.maximum_message_bytes == 0) {
    return {nullptr, "invalid libe3 ZMQ carrier configuration"};
  }
  try {
    auto state = std::make_unique<implementation>(carrier, std::move(config));
    state->open();
    return {std::unique_ptr<zmq_association_server>(new zmq_association_server(std::move(state))), {}};
  } catch (const std::exception& exception) {
    return {nullptr, exception.what()};
  } catch (...) {
    return {nullptr, "unknown libe3 ZMQ carrier failure"};
  }
}

#else // OCUDU_DAPP_E3_HAVE_ZMQ

using namespace ocudu::dapp::e3;

struct zmq_association_server::implementation {};

bool zmq_association_server::available() noexcept
{
  return false;
}

zmq_association_server_create_result zmq_association_server::create(sctp_carrier&      carrier,
                                                                    zmq_carrier_config config) noexcept
{
  static_cast<void>(carrier);
  static_cast<void>(config);
  return {nullptr, "this build has no ZeroMQ: the libe3 ZMQ carrier is unavailable"};
}

#endif // OCUDU_DAPP_E3_HAVE_ZMQ

zmq_association_server::zmq_association_server(std::unique_ptr<implementation> state_) : state(std::move(state_)) {}

zmq_association_server::~zmq_association_server() = default;

void zmq_association_server::stop() noexcept
{
#ifdef OCUDU_DAPP_E3_HAVE_ZMQ
  if (state != nullptr) {
    state->stop();
  }
#endif
}

sctp_server_metrics zmq_association_server::metrics() const noexcept
{
#ifdef OCUDU_DAPP_E3_HAVE_ZMQ
  if (state != nullptr) {
    return state->metrics_copy();
  }
#endif
  return {};
}
