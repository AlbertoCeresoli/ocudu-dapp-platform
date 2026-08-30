// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "zmq_diag_publisher.h"
#include <cstring>
#include <zmq.h>

using namespace ocudu;

namespace {

flatbuffers::FlatBufferBuilder& tls_flatbuffer_builder()
{
  thread_local static flatbuffers::FlatBufferBuilder builder(256 * 1024);
  return builder;
}

} // namespace

zmq_diag_publisher::zmq_diag_publisher(const std::string& endpoint)
{
  if (endpoint.empty()) {
    return;
  }

  ctx_ = zmq_ctx_new();
  if (!ctx_) {
    return;
  }

  sock_ = zmq_socket(ctx_, ZMQ_PUB);
  if (!sock_) {
    zmq_ctx_destroy(ctx_);
    ctx_ = nullptr;
    return;
  }

  int sndhwm = 8;
  zmq_setsockopt(sock_, ZMQ_SNDHWM, &sndhwm, sizeof(sndhwm));
  int linger = 0;
  zmq_setsockopt(sock_, ZMQ_LINGER, &linger, sizeof(linger));
  int sndbuf = 4 * 1024 * 1024;
  zmq_setsockopt(sock_, ZMQ_SNDBUF, &sndbuf, sizeof(sndbuf));

  if (zmq_bind(sock_, endpoint.c_str()) != 0) {
    zmq_close(sock_);
    zmq_ctx_destroy(ctx_);
    sock_ = nullptr;
    ctx_  = nullptr;
    return;
  }

  enabled_.store(true, std::memory_order_release);
}

zmq_diag_publisher::~zmq_diag_publisher()
{
  enabled_.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(send_mtx_);
  if (sock_) {
    zmq_close(sock_);
    sock_ = nullptr;
  }
  if (ctx_) {
    zmq_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }
}

zmq_diag_publisher_stats zmq_diag_publisher::get_stats() const
{
  return {.messages_attempted = messages_attempted_.load(std::memory_order_relaxed),
          .messages_sent      = messages_sent_.load(std::memory_order_relaxed),
          .transport_drops    = transport_drops_.load(std::memory_order_relaxed),
          .oversized_drops    = oversized_drops_.load(std::memory_order_relaxed),
          .rate_policy_skips  = rate_policy_skips_.load(std::memory_order_relaxed)};
}

bool zmq_diag_publisher::should_collect(phy_diagnostic_stream stream, uint16_t rnti)
{
  return should_emit(to_zmq_diag_topic(stream), rnti);
}

bool zmq_diag_publisher::is_collect_due(phy_diagnostic_stream stream, uint16_t rnti)
{
  return is_emit_due(to_zmq_diag_topic(stream), rnti);
}

void zmq_diag_publisher::on_pusch_crc(const pusch_crc_diagnostic& event)
{
  if (!is_enabled()) {
    return;
  }

  auto&             builder = tls_flatbuffer_builder();
  const std::size_t bytes   = serialize_zmq_diag_pusch_crc(builder, event);
  if (bytes == 0) {
    oversized_drops_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  send_payload(zmq_diag_topic::pusch_crc, builder.GetBufferPointer(), bytes);
}

void zmq_diag_publisher::on_pusch_symbols(const pusch_symbols_diagnostic& event)
{
  if (!is_enabled()) {
    return;
  }

  // Publish metadata even when no equalized IQ samples are available.
  auto&             builder = tls_flatbuffer_builder();
  const std::size_t bytes   = serialize_zmq_diag_pusch_symbols(builder, event);
  if (bytes == 0) {
    oversized_drops_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  send_payload(zmq_diag_topic::pusch_sym, builder.GetBufferPointer(), bytes);
}

void zmq_diag_publisher::on_srs_channel(const srs_channel_diagnostic& event)
{
  if (!is_enabled() || event.h_coefficients.empty()) {
    return;
  }

  auto&             builder = tls_flatbuffer_builder();
  const std::size_t bytes   = serialize_zmq_diag_srs_channel(builder, event);
  if (bytes == 0) {
    oversized_drops_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  send_payload(zmq_diag_topic::srs_chan, builder.GetBufferPointer(), bytes);
}

void zmq_diag_publisher::on_pusch_dmrs_channel(const pusch_dmrs_channel_diagnostic& event)
{
  if (!is_enabled() || event.h_estimates.empty()) {
    return;
  }

  auto&             builder = tls_flatbuffer_builder();
  const std::size_t bytes   = serialize_zmq_diag_pusch_dmrs_channel(builder, event);
  if (bytes == 0) {
    oversized_drops_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  send_payload(zmq_diag_topic::pusch_dmrs_chan, builder.GetBufferPointer(), bytes);
}

bool zmq_diag_publisher::should_emit(zmq_diag_topic topic, uint16_t rnti)
{
  if (!is_enabled()) {
    return false;
  }
  const uint64_t now           = now_ns();
  const uint32_t key           = (static_cast<uint32_t>(topic) << 16) | rnti;
  const uint64_t min_period_ns = zmq_diag_min_period_ns(topic);

  std::lock_guard<std::mutex> lock(rate_mtx_);
  auto&                       last = last_emit_ns_[key];
  if ((min_period_ns != 0) && (now - last < min_period_ns)) {
    rate_policy_skips_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  last = now;
  return true;
}

bool zmq_diag_publisher::is_emit_due(zmq_diag_topic topic, uint16_t rnti)
{
  if (!is_enabled()) {
    return false;
  }
  const uint64_t now           = now_ns();
  const uint32_t key           = (static_cast<uint32_t>(topic) << 16) | rnti;
  const uint64_t min_period_ns = zmq_diag_min_period_ns(topic);

  std::lock_guard<std::mutex> lock(rate_mtx_);
  if (min_period_ns == 0) {
    return true;
  }
  auto it = last_emit_ns_.find(key);
  if (it == last_emit_ns_.end()) {
    return true;
  }
  return (now - it->second) >= min_period_ns;
}

void zmq_diag_publisher::send_payload(zmq_diag_topic topic, const uint8_t* payload, std::size_t payload_len)
{
  messages_attempted_.fetch_add(1, std::memory_order_relaxed);

  std::lock_guard<std::mutex> lock(send_mtx_);
  if (!sock_) {
    transport_drops_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  const char*       topic_name = zmq_diag_topic_name(topic);
  const std::size_t topic_len  = std::strlen(topic_name);
  if (zmq_send(sock_, topic_name, topic_len, ZMQ_SNDMORE | ZMQ_DONTWAIT) < 0) {
    transport_drops_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (zmq_send(sock_, payload, payload_len, ZMQ_DONTWAIT) < 0) {
    transport_drops_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  messages_sent_.fetch_add(1, std::memory_order_relaxed);
}
