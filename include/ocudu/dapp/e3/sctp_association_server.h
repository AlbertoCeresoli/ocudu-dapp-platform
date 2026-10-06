// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/adt/span.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ocudu::dapp::e3 {

/// Transport policy shared by every SCTP E3 carrier.
struct sctp_server_config {
  /// Literal IPv4/IPv6 address to bind. Loopback by default; anything else
  /// exposes the endpoint to the network and needs an external boundary.
  std::string bind_address{"127.0.0.1"};
  /// Zero selects an ephemeral port (tests); \ref sctp_association_server::port reports it.
  uint16_t port{};
  /// Optional exact peer addresses admitted after accept; empty admits all.
  /// This literal-address comparison is the only admission control: there is
  /// no TLS/DTLS or token authentication on the SCTP carriers.
  std::vector<std::string> allowed_peers;
  /// Concurrent associations; further accepts are rejected.
  uint32_t maximum_associations{16};
  /// Associations quiet for longer than this are closed unless the carrier
  /// retains an idle, fully drained association (e.g. a valid subscription).
  std::chrono::milliseconds idle_timeout{30000};
  /// How long a gracefully closing association may keep undelivered output
  /// before its slot is reclaimed anyway.
  std::chrono::milliseconds close_grace{2000};
  /// Cadence of \ref sctp_carrier::on_timer while any association exists.
  std::chrono::milliseconds timer_period{5};
  /// Largest complete inbound or outbound message.
  std::size_t maximum_message_bytes{2U * 1024U * 1024U};
  /// Bounded per-association outbound queue in messages.
  std::size_t maximum_queued_messages{512};
  /// Outbound/inbound SCTP streams requested per association.
  uint16_t nof_streams{2};
  /// Payload protocol identifier stamped on every outbound message.
  uint32_t payload_protocol_id{};
  /// Socket buffer size requested at accept; the kernel may clamp it.
  std::size_t socket_buffer_bytes{2U * 1024U * 1024U};
};

struct sctp_server_metrics {
  uint64_t accepted_associations{};
  uint64_t rejected_associations{};
  uint64_t closed_associations{};
  uint64_t idle_closures{};
  uint64_t oversize_closures{};
  uint64_t messages_received{};
  uint64_t messages_sent{};
  uint64_t send_queue_drops{};
  uint64_t transport_failures{};
  uint32_t live_associations{};
};

/// One accepted SCTP association as seen by a carrier. Only valid inside
/// carrier callbacks on the server thread.
class sctp_association
{
public:
  virtual ~sctp_association() = default;

  virtual uint64_t           id() const noexcept           = 0;
  virtual const std::string& peer_address() const noexcept = 0;
  /// Effective kernel send-buffer size; single messages must fit inside it.
  virtual std::size_t send_buffer_bytes() const noexcept = 0;

  /// Queues one complete message on the given SCTP stream. Returns false when
  /// the bounded queue is full or the message exceeds the transport bound.
  virtual bool send(std::vector<uint8_t> message, uint16_t stream) noexcept = 0;
  /// Messages that can still be queued before \ref send starts refusing.
  virtual std::size_t queue_capacity_remaining() const noexcept = 0;

  /// Requests an orderly close after the queued messages are flushed.
  virtual void close() noexcept = 0;

  /// Carrier-owned per-association state.
  virtual void  set_context(std::shared_ptr<void> context) noexcept = 0;
  virtual void* context() const noexcept                            = 0;
};

/// Message-level protocol handler plugged into the transport core. All
/// callbacks run on the server thread and must not block.
class sctp_carrier
{
public:
  virtual ~sctp_carrier() = default;

  virtual void on_opened(sctp_association& association) { static_cast<void>(association); }
  virtual void on_message(sctp_association& association, ocudu::span<const uint8_t> message) = 0;
  virtual void on_closed(sctp_association& association) { static_cast<void>(association); }
  /// Called at idle timeout only when the SCTP send queue is empty. A carrier
  /// may retain an association with valid application work even without data.
  /// Does not override transport failure, stalled-output timeout or close().
  /// ZMQ has no per-peer SCTP liveness and does not use this exemption.
  virtual bool retain_idle_association(sctp_association& association) noexcept
  {
    static_cast<void>(association);
    return false;
  }
  /// Periodic hook while associations exist; used for publication draining.
  virtual void on_timer(std::vector<sctp_association*>& associations) { static_cast<void>(associations); }
};

class sctp_association_server;

struct sctp_association_server_create_result {
  std::unique_ptr<sctp_association_server> server;
  std::string                              detail;
  explicit                                 operator bool() const noexcept { return server != nullptr; }
};

/// Single-threaded SCTP one-to-one association server with message framing by
/// SCTP boundaries (MSG_EOR), bounded queues, peer filtering, and idle close.
class sctp_association_server
{
public:
  static sctp_association_server_create_result create(sctp_carrier& carrier, sctp_server_config config) noexcept;

  ~sctp_association_server();
  sctp_association_server(const sctp_association_server&)            = delete;
  sctp_association_server& operator=(const sctp_association_server&) = delete;

  void                stop() noexcept;
  uint16_t            port() const noexcept;
  sctp_server_metrics metrics() const noexcept;

private:
  struct implementation;
  explicit sctp_association_server(std::unique_ptr<implementation> state_);
  std::unique_ptr<implementation> state;
};

} // namespace ocudu::dapp::e3
