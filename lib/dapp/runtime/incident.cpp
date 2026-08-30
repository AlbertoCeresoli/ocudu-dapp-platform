// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/incident.h"
#include <atomic>
#include <chrono>

using namespace ocudu::dapp;

namespace {

std::atomic<uint64_t> next_incident_id{1};

uint64_t stable_hash(const char* text) noexcept
{
  uint64_t hash = 1469598103934665603ULL;
  while (*text != '\0') {
    hash ^= static_cast<uint8_t>(*text++);
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool is_safe_message_character(unsigned char value) noexcept
{
  return value == ' ' || (value >= 0x21 && value <= 0x7e);
}

} // namespace

incident_record ocudu::dapp::make_incident(const incident_context& context,
                                           incident_cause          cause,
                                           incident_action         action,
                                           ocudu_dapp_status_v1    status,
                                           const char*             message,
                                           bool                    breaker_open) noexcept
{
  incident_record record{};
  record.incident_id  = next_incident_id.fetch_add(1, std::memory_order_relaxed);
  record.monotonic_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
  record.context       = context;
  record.cause         = cause;
  record.action        = action;
  record.native_status = static_cast<int32_t>(status);
  record.breaker_open  = breaker_open;
  if (cause == incident_cause::standard_exception) {
    record.exception_type_hash = stable_hash("std::exception");
  } else if (cause == incident_cause::unknown_exception) {
    record.exception_type_hash = stable_hash("unknown-exception");
  }

  if (message != nullptr) {
    std::size_t output = 0;
    for (std::size_t input = 0; message[input] != '\0' && output != record.sanitized_message.size(); ++input) {
      const auto value                   = static_cast<unsigned char>(message[input]);
      record.sanitized_message[output++] = is_safe_message_character(value) ? static_cast<char>(value) : '?';
    }
    record.message_size = static_cast<uint16_t>(output);
  }
  return record;
}
