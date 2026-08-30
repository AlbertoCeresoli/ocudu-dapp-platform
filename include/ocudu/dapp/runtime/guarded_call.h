// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "circuit_breaker.h"
#include "incident.h"
#include <array>
#include <cstddef>
#include <exception>
#include <utility>

namespace ocudu::dapp {

struct guarded_result {
  ocudu_dapp_status_v1 status{OCUDU_DAPP_INTERNAL_ERROR_V1};
  bool                 invoke_fallback{true};
  bool                 incident_recorded{false};
};

/// Host-side exception barrier for every transition into native module code.
template <typename Callable>
guarded_result guarded_call(Callable&&              callable,
                            circuit_breaker&        breaker,
                            incident_sink&          incidents,
                            const incident_context& context,
                            bool                    invoke_when_open = false) noexcept
{
  if (!invoke_when_open && !breaker.allows_invocation()) {
    return {OCUDU_DAPP_BYPASS_V1, true, false};
  }

  ocudu_dapp_status_v1 status          = OCUDU_DAPP_INTERNAL_ERROR_V1;
  incident_cause       cause           = incident_cause::returned_error;
  const char*          message         = nullptr;
  auto                 message_storage = std::array<char, incident_record::message_capacity + 1>{};
  try {
    status = std::forward<Callable>(callable)();
  } catch (const std::exception& exception) {
    status             = OCUDU_DAPP_EXCEPTION_V1;
    cause              = incident_cause::standard_exception;
    const char* source = exception.what();
    if (source != nullptr) {
      std::size_t length = 0;
      while (length != incident_record::message_capacity && source[length] != '\0') {
        message_storage[length] = source[length];
        ++length;
      }
      message_storage[length] = '\0';
      message                 = message_storage.data();
    }
  } catch (...) {
    status = OCUDU_DAPP_EXCEPTION_V1;
    cause  = incident_cause::unknown_exception;
  }

  if (status == OCUDU_DAPP_OK_V1) {
    breaker.observe(status);
    return {status, false, false};
  }
  if (status == OCUDU_DAPP_BYPASS_V1) {
    return {status, true, false};
  }

  if (status == OCUDU_DAPP_DEADLINE_V1) {
    cause = incident_cause::deadline_miss;
  } else if (status == OCUDU_DAPP_INVALID_INPUT_V1) {
    cause = incident_cause::invalid_input;
  } else if (status == OCUDU_DAPP_INVALID_OUTPUT_V1) {
    cause = incident_cause::invalid_output;
  } else if (status == OCUDU_DAPP_DEVICE_ERROR_V1) {
    cause = incident_cause::device_error;
  } else if (status == OCUDU_DAPP_RESOURCE_V1) {
    cause = incident_cause::resource_exhausted;
  }

  const bool opened = breaker.observe(status);
  const auto action = opened ? incident_action::breaker_opened : incident_action::invocation_fallback;
  auto       record = make_incident(context, cause, action, status, message, breaker.state() == breaker_state::open);
  return {status, true, incidents.try_push(record)};
}

} // namespace ocudu::dapp
