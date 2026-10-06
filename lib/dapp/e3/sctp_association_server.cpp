// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/sctp_association_server.h"
#include "sctp_transport_internal.h"
#include "ocudu/support/io/unique_fd.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

using namespace ocudu::dapp::e3;
using namespace ocudu::dapp::e3::sctp_detail;

namespace {

constexpr std::size_t receive_chunk_bytes = 64 * 1024;

struct outbound_message {
  std::vector<uint8_t> bytes;
  uint16_t             stream{};
};

class association final : public sctp_association
{
public:
  association(uint64_t                  id_,
              ocudu::unique_fd          descriptor_,
              std::string               peer_,
              const sctp_server_config& config_,
              std::atomic<uint64_t>&    queue_drops_) :
    identity(id_),
    descriptor(std::move(descriptor_)),
    peer(std::move(peer_)),
    config(config_),
    last_activity_ns(monotonic_ns()),
    send_buffer(effective_send_buffer(descriptor.value())),
    queue_drops(queue_drops_)
  {
  }

  uint64_t           id() const noexcept override { return identity; }
  const std::string& peer_address() const noexcept override { return peer; }
  std::size_t        send_buffer_bytes() const noexcept override { return send_buffer; }

  bool send(std::vector<uint8_t> message, uint16_t stream) noexcept override
  {
    if (closing || message.empty() || message.size() > config.maximum_message_bytes ||
        outbound.size() >= config.maximum_queued_messages) {
      // Send-queue exhaustion is the primary symptom of a slow or wedged peer;
      // count it so operators can tell "peer is not draining" from "nothing to
      // send" even when every caller discards the return value.
      queue_drops.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    try {
      outbound.push_back({std::move(message), stream});
      return true;
    } catch (...) {
      queue_drops.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  }

  std::size_t queue_capacity_remaining() const noexcept override
  {
    return closing || outbound.size() >= config.maximum_queued_messages
               ? 0
               : config.maximum_queued_messages - outbound.size();
  }

  void close() noexcept override
  {
    if (!closing) {
      closing_since_ns = monotonic_ns();
    }
    closing = true;
  }

  /// Peer is gone or the transport failed: nothing queued can ever be sent,
  /// so drop it and let the reaper collect the slot immediately.
  void abort() noexcept
  {
    close();
    outbound.clear();
    head_offset = 0;
  }

  /// A graceful close that the peer never drains must not pin the slot forever.
  bool close_overdue(uint64_t now, uint64_t grace_ns) const noexcept
  {
    return closing && !outbound.empty() && now - closing_since_ns > grace_ns;
  }

  void  set_context(std::shared_ptr<void> context_) noexcept override { carrier_context = std::move(context_); }
  void* context() const noexcept override { return carrier_context.get(); }

  int  fd() const noexcept { return descriptor.value(); }
  bool wants_write() const noexcept { return !outbound.empty(); }
  bool finished() const noexcept { return closing && outbound.empty(); }
  bool is_closing() const noexcept { return closing; }
  bool idle(uint64_t now, uint64_t idle_ns) const noexcept { return idle_ns != 0 && now - last_activity_ns > idle_ns; }
  void touch() noexcept { last_activity_ns = monotonic_ns(); }

  /// Flushes as many queued messages as the socket accepts. Returns false on a
  /// transport failure that requires closing the association.
  bool flush(uint32_t ppid, uint64_t& sent_messages) noexcept
  {
    while (!outbound.empty()) {
      const auto&   head = outbound.front();
      const ssize_t sent = send_message(
          descriptor.value(), head.bytes.data() + head_offset, head.bytes.size() - head_offset, head.stream, ppid);
      if (sent == 0) {
        return true;
      }
      if (sent < 0) {
        // Transport failure: the remaining queue can never be delivered.
        outbound.clear();
        head_offset = 0;
        return false;
      }
      if (static_cast<std::size_t>(sent) < head.bytes.size() - head_offset) {
        // A short write would split one SCTP user message into two EOR-delimited
        // deliveries (SCTP_EXPLICIT_EOR is not enabled anywhere), so the peer
        // would reassemble a truncated PDU followed by a spurious one. Framing
        // is unrecoverable at this point: fail the association.
        outbound.clear();
        head_offset = 0;
        return false;
      }
      head_offset = 0;
      outbound.pop_front();
      ++sent_messages;
      touch();
    }
    return true;
  }

private:
  uint64_t                     identity;
  ocudu::unique_fd             descriptor;
  std::string                  peer;
  const sctp_server_config&    config;
  uint64_t                     last_activity_ns;
  std::size_t                  send_buffer;
  std::deque<outbound_message> outbound;
  /// Bytes of outbound.front() already accepted by the kernel (partial send).
  std::size_t            head_offset{};
  std::shared_ptr<void>  carrier_context;
  std::atomic<uint64_t>& queue_drops;
  bool                   closing{};
  uint64_t               closing_since_ns{};
};

} // namespace

struct sctp_association_server::implementation {
  implementation(sctp_carrier& carrier_, sctp_server_config config_) : carrier(carrier_), config(std::move(config_)) {}

  bool initialize(std::string& detail)
  {
    if (config.maximum_associations == 0 || config.maximum_associations > 1024 || config.maximum_message_bytes == 0 ||
        config.maximum_queued_messages == 0 || config.nof_streams == 0 || config.timer_period.count() <= 0) {
      detail = "invalid SCTP server bounds";
      return false;
    }
    sockaddr_storage address{};
    socklen_t        length = 0;
    if (!make_address(config.bind_address, config.port, address, length, detail)) {
      return false;
    }
    listener = ocudu::unique_fd(::socket(address.ss_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_SCTP));
    if (!listener.is_open()) {
      detail = error_text("create SCTP socket");
      return false;
    }
    const int reuse = 1;
    ::setsockopt(listener.value(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    request_streams(listener.value(), config.nof_streams);
    // Buffers must be requested before listen(): the INIT-ACK advertises the
    // receive window from the listener's buffer and accepted sockets inherit
    // both sizes. Delayed SACK is disabled here for the same inheritance
    // reason; see disable_delayed_sack().
    request_socket_buffers(listener.value(), config.socket_buffer_bytes);
    disable_delayed_sack(listener.value());
    if (::bind(listener.value(), reinterpret_cast<const sockaddr*>(&address), length) != 0) {
      detail = error_text("bind SCTP socket");
      return false;
    }
    if (::listen(listener.value(), static_cast<int>(std::min<uint32_t>(config.maximum_associations, 128))) != 0) {
      detail = error_text("listen on SCTP socket");
      return false;
    }
    const auto port = bound_port(listener.value());
    if (!port) {
      detail = error_text("read SCTP listener port");
      return false;
    }
    listen_port = *port;
    stop_event  = ocudu::unique_fd(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (!stop_event.is_open()) {
      detail = error_text("create SCTP stop event");
      return false;
    }
    return true;
  }

  void start()
  {
    worker = std::thread([this] { run(); });
  }

  void stop() noexcept
  {
    if (!stopping.exchange(true, std::memory_order_acq_rel)) {
      uint64_t signal = 1;
      ssize_t  written;
      do {
        written = ::write(stop_event.value(), &signal, sizeof(signal));
      } while (written < 0 && errno == EINTR);
      if (written != static_cast<ssize_t>(sizeof(signal)) && !(written < 0 && errno == EAGAIN)) {
        static_cast<void>(::shutdown(listener.value(), SHUT_RDWR));
      }
    }
    if (worker.joinable()) {
      worker.join();
    }
  }

  sctp_server_metrics snapshot() const noexcept
  {
    return {accepted.load(std::memory_order_relaxed),
            rejected.load(std::memory_order_relaxed),
            closed.load(std::memory_order_relaxed),
            idle_closed.load(std::memory_order_relaxed),
            oversize_closed.load(std::memory_order_relaxed),
            received_messages.load(std::memory_order_relaxed),
            sent.load(std::memory_order_relaxed),
            queue_drops.load(std::memory_order_relaxed),
            failures.load(std::memory_order_relaxed),
            live.load(std::memory_order_relaxed)};
  }

private:
  void run() noexcept
  {
    std::vector<pollfd> descriptors;
    while (!stopping.load(std::memory_order_acquire)) {
      descriptors.clear();
      descriptors.push_back({listener.value(), POLLIN, 0});
      descriptors.push_back({stop_event.value(), POLLIN, 0});
      for (const auto& item : associations) {
        short events = POLLIN;
        if (item->wants_write()) {
          events |= POLLOUT;
        }
        descriptors.push_back({item->fd(), events, 0});
      }
      const int timeout_ms =
          associations.empty() ? 1000 : static_cast<int>(std::max<int64_t>(1, config.timer_period.count()));
      const int ready = ::poll(descriptors.data(), descriptors.size(), timeout_ms);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        failures.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      if ((descriptors[1].revents & POLLIN) != 0) {
        break;
      }
      // Service only the associations that were polled: accepting first would
      // append entries with no pollfd behind them and read stale revents.
      const std::size_t polled = descriptors.size() - 2;
      for (std::size_t index = 0; index != polled && index != associations.size(); ++index) {
        service(*associations[index], descriptors[index + 2].revents);
      }
      if ((descriptors[0].revents & POLLIN) != 0) {
        accept_pending();
      }
      if (!associations.empty()) {
        try {
          std::vector<sctp_association*> live_list;
          live_list.reserve(associations.size());
          for (auto& item : associations) {
            if (!item->is_closing()) {
              live_list.push_back(item.get());
            }
          }
          carrier.on_timer(live_list);
        } catch (...) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        for (auto& item : associations) {
          flush(*item);
        }
      }
      reap();
    }
    for (auto& item : associations) {
      notify_closed(*item);
    }
    associations.clear();
    live.store(0, std::memory_order_relaxed);
  }

  void accept_pending() noexcept
  {
    while (true) {
      sockaddr_storage peer{};
      socklen_t        length = sizeof(peer);
      ocudu::unique_fd accepted_fd(
          ::accept4(listener.value(), reinterpret_cast<sockaddr*>(&peer), &length, SOCK_CLOEXEC | SOCK_NONBLOCK));
      if (!accepted_fd.is_open()) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        return;
      }
      // The peer text is published to E3 clients (connection inventory) where
      // the schema requires a non-empty address.
      std::string peer_text = address_text(peer);
      if (peer_text.empty()) {
        peer_text = "unknown";
      }
      const bool admitted =
          config.allowed_peers.empty() ||
          std::find(config.allowed_peers.begin(), config.allowed_peers.end(), peer_text) != config.allowed_peers.end();
      if (!admitted || associations.size() >= config.maximum_associations) {
        rejected.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      request_socket_buffers(accepted_fd.value(), config.socket_buffer_bytes);
      disable_delayed_sack(accepted_fd.value());
      set_nodelay(accepted_fd.value());
      enable_receive_info(accepted_fd.value());
      try {
        auto item =
            std::make_unique<association>(++next_identity, std::move(accepted_fd), peer_text, config, queue_drops);
        accepted.fetch_add(1, std::memory_order_relaxed);
        live.store(static_cast<uint32_t>(associations.size() + 1), std::memory_order_relaxed);
        associations.push_back(std::move(item));
        carrier.on_opened(*associations.back());
      } catch (...) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  void service(association& item, short revents) noexcept
  {
    if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (revents & POLLIN) == 0) {
      item.abort();
      return;
    }
    if ((revents & POLLIN) != 0) {
      // Drain everything the kernel has queued; each delivery is bounded.
      for (unsigned iteration = 0; iteration != 64; ++iteration) {
        std::size_t  received = 0;
        receive_info info;
        auto         status = receive_part(item.fd(), scratch(), receive_chunk_bytes, received, info);
        if (status == receive_status::would_block) {
          break;
        }
        if (status == receive_status::notification) {
          continue;
        }
        if (status == receive_status::closed || status == receive_status::error) {
          if (status == receive_status::error) {
            failures.fetch_add(1, std::memory_order_relaxed);
          }
          // The peer shut down or the association failed: queued output is undeliverable.
          item.abort();
          break;
        }
        // A wrong non-zero payload protocol identifier is cross-protocol
        // confusion, not this carrier's traffic. Unstamped deliveries (0) stay
        // accepted for client compatibility.
        if (info.valid && info.ppid != 0 && info.ppid != config.payload_protocol_id) {
          failures.fetch_add(1, std::memory_order_relaxed);
          item.abort();
          break;
        }
        item.touch();
        if (!append(item, received, status == receive_status::complete)) {
          break;
        }
      }
    }
    if ((revents & POLLOUT) != 0 || item.wants_write()) {
      flush_or_fail(item);
    }
    if (!item.is_closing() && item.idle(monotonic_ns(), idle_ns()) &&
        (item.wants_write() || !carrier.retain_idle_association(item))) {
      idle_closed.fetch_add(1, std::memory_order_relaxed);
      item.close();
    }
  }

  bool append(association& item, std::size_t received, bool complete) noexcept
  {
    auto& partial = partials[item.id()];
    if (partial.size() + received > config.maximum_message_bytes) {
      partial.clear();
      oversize_closed.fetch_add(1, std::memory_order_relaxed);
      item.close();
      return false;
    }
    try {
      partial.insert(partial.end(), scratch(), scratch() + received);
    } catch (...) {
      partial.clear();
      failures.fetch_add(1, std::memory_order_relaxed);
      item.close();
      return false;
    }
    if (!complete) {
      return true;
    }
    std::vector<uint8_t> message;
    message.swap(partial);
    received_messages.fetch_add(1, std::memory_order_relaxed);
    try {
      carrier.on_message(item, message);
    } catch (...) {
      failures.fetch_add(1, std::memory_order_relaxed);
      item.close();
      return false;
    }
    return !item.is_closing();
  }

  void flush_or_fail(association& item) noexcept
  {
    if (!flush(item)) {
      failures.fetch_add(1, std::memory_order_relaxed);
      item.close();
    }
  }

  bool flush(association& item) noexcept
  {
    uint64_t   sent_now = 0;
    const bool ok       = item.flush(config.payload_protocol_id, sent_now);
    sent.fetch_add(sent_now, std::memory_order_relaxed);
    if (!ok) {
      item.close();
    }
    return ok;
  }

  void reap() noexcept
  {
    const uint64_t now = monotonic_ns();
    const uint64_t grace_ns =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(config.close_grace).count());
    for (auto iterator = associations.begin(); iterator != associations.end();) {
      auto& item = **iterator;
      if (item.close_overdue(now, grace_ns)) {
        item.abort();
      }
      if (item.finished() || (item.is_closing() && !item.wants_write())) {
        notify_closed(item);
        partials.erase(item.id());
        iterator = associations.erase(iterator);
        closed.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      ++iterator;
    }
    live.store(static_cast<uint32_t>(associations.size()), std::memory_order_relaxed);
  }

  void notify_closed(association& item) noexcept
  {
    try {
      carrier.on_closed(item);
    } catch (...) {
      failures.fetch_add(1, std::memory_order_relaxed);
    }
  }

  uint64_t idle_ns() const noexcept
  {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(config.idle_timeout).count());
  }

  uint8_t* scratch() noexcept
  {
    if (scratch_storage.size() != receive_chunk_bytes) {
      scratch_storage.resize(receive_chunk_bytes);
    }
    return scratch_storage.data();
  }

public:
  uint16_t listen_port{};

private:
  sctp_carrier&                                      carrier;
  sctp_server_config                                 config;
  ocudu::unique_fd                                   listener;
  ocudu::unique_fd                                   stop_event;
  std::thread                                        worker;
  std::atomic<bool>                                  stopping{false};
  std::vector<std::unique_ptr<association>>          associations;
  std::unordered_map<uint64_t, std::vector<uint8_t>> partials;
  std::vector<uint8_t>                               scratch_storage;
  uint64_t                                           next_identity{};
  std::atomic<uint64_t>                              accepted{0};
  std::atomic<uint64_t>                              rejected{0};
  std::atomic<uint64_t>                              closed{0};
  std::atomic<uint64_t>                              idle_closed{0};
  std::atomic<uint64_t>                              oversize_closed{0};
  std::atomic<uint64_t>                              received_messages{0};
  std::atomic<uint64_t>                              sent{0};
  std::atomic<uint64_t>                              queue_drops{0};
  std::atomic<uint64_t>                              failures{0};
  std::atomic<uint32_t>                              live{0};
};

sctp_association_server::sctp_association_server(std::unique_ptr<implementation> state_) : state(std::move(state_)) {}

sctp_association_server_create_result sctp_association_server::create(sctp_carrier&      carrier,
                                                                      sctp_server_config config) noexcept
{
  try {
    auto        state = std::make_unique<implementation>(carrier, std::move(config));
    std::string detail;
    if (!state->initialize(detail)) {
      return {nullptr, std::move(detail)};
    }
    auto result = std::unique_ptr<sctp_association_server>(new sctp_association_server(std::move(state)));
    result->state->start();
    return {std::move(result), {}};
  } catch (const std::exception& exception) {
    return {nullptr, exception.what()};
  } catch (...) {
    return {nullptr, "unknown SCTP server construction failure"};
  }
}

sctp_association_server::~sctp_association_server()
{
  stop();
}

void sctp_association_server::stop() noexcept
{
  if (state != nullptr) {
    state->stop();
  }
}

uint16_t sctp_association_server::port() const noexcept
{
  return state != nullptr ? state->listen_port : 0;
}

sctp_server_metrics sctp_association_server::metrics() const noexcept
{
  return state != nullptr ? state->snapshot() : sctp_server_metrics{};
}
