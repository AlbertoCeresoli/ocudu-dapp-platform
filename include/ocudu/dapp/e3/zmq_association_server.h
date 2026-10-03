// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/e3/sctp_association_server.h"
#include "ocudu/dapp/e3/e3ap_protobuf_codec.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace ocudu::dapp::e3 {

/// RAN side of libe3's ZMQ link layer (libe3 >= 0.2, \c E3LinkLayer::ZMQ, role RAN). The RAN binds three
/// sockets and every dApp connects to them:
///  - setup:      REP. A dApp's REQ carries its E3-SetupRequest; the setup response goes back on it.
///  - publisher:  PUB. Every RAN-to-dApp PDU after setup (indications, acks, xApp controls) is
///                broadcast here; each PDU names its dApp and libe3 peers filter on that.
///  - subscriber: SUB (subscribed to everything). dApp-to-RAN PDUs after setup arrive here and are
///                routed by the dApp identifier they carry.
/// Endpoints are ZMQ URIs: "ipc:///path" or "tcp://*:port" (libe3 defaults: tcp 9990/9991/9999, ipc
/// /tmp/dapps/{setup,e3_socket,dapp_socket}).
/// E3-PDU encoding of a libe3 link. libe3 fixes it in its configuration (E3Config.encoding), never on
/// the wire, so a ZMQ carrier serves one encoding.
enum class e3ap_encoding : uint8_t {
  /// ASN.1 APER (libe3 \c EncodingFormat::ASN1, the default).
  aper,
  /// Protocol Buffers (libe3 \c EncodingFormat::PROTOBUF; OCUDU-profile peers too).
  protobuf
};

struct zmq_carrier_config {
  std::string setup_endpoint{"ipc:///tmp/dapps/setup"};
  std::string publisher_endpoint{"ipc:///tmp/dapps/e3_socket"};
  std::string subscriber_endpoint{"ipc:///tmp/dapps/dapp_socket"};
  /// Concurrent dApp sessions; further setups are answered with an empty reply and dropped.
  uint32_t maximum_associations{16};
  /// ZMQ has no connection liveness: a session that sent nothing for this long is closed (0 = never;
  /// a released dApp is closed by its E3-ReleaseMessage either way).
  std::chrono::milliseconds idle_timeout{0};
  /// Cadence of \ref sctp_carrier::on_timer.
  std::chrono::milliseconds timer_period{5};
  /// Largest single PDU accepted or sent.
  std::size_t maximum_message_bytes{64U * 1024U};
  /// Encoding every dApp on these sockets uses.
  e3ap_encoding encoding{e3ap_encoding::aper};
};

class zmq_association_server;

struct zmq_association_server_create_result {
  std::unique_ptr<zmq_association_server> server;
  std::string                             detail;
  explicit                                operator bool() const noexcept { return server != nullptr; }
};

/// Drives an \ref sctp_carrier (one virtual association per dApp session) from libe3's ZMQ sockets on
/// its own thread, so the same E3AP procedure code serves SCTP and ZMQ peers. Carries the libe3 E3AP
/// module in the configured encoding (APER, or Protobuf with either profile).
class zmq_association_server
{
public:
  /// False when this build has no ZeroMQ.
  static bool                                 available() noexcept;
  static zmq_association_server_create_result create(sctp_carrier& carrier, zmq_carrier_config config) noexcept;

  ~zmq_association_server();
  zmq_association_server(const zmq_association_server&)            = delete;
  zmq_association_server& operator=(const zmq_association_server&) = delete;

  void                stop() noexcept;
  sctp_server_metrics metrics() const noexcept;

private:
  struct implementation;
  explicit zmq_association_server(std::unique_ptr<implementation> state_);
  std::unique_ptr<implementation> state;
};

} // namespace ocudu::dapp::e3
