// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/incident_store.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

using namespace ocudu::dapp;

incident_store::incident_store(std::size_t capacity, uint64_t deduplication_window_ns) :
  maximum_records(capacity), dedup_window_ns(deduplication_window_ns)
{
  if (capacity == 0) {
    throw std::invalid_argument("incident history capacity must be nonzero");
  }
}

void incident_store::ingest(const incident_record& incident)
{
  std::lock_guard<std::mutex> lock(mutex);
  increment_health(incident);

  if (!records.empty() && same_recurrence(records.back(), incident) &&
      incident.monotonic_ns >= records.back().monotonic_ns &&
      incident.monotonic_ns - records.back().monotonic_ns <= dedup_window_ns) {
    auto& aggregate              = records.back();
    aggregate.monotonic_ns       = incident.monotonic_ns;
    aggregate.incident_id        = incident.incident_id;
    aggregate.context            = incident.context;
    aggregate.native_status      = incident.native_status;
    aggregate.action             = incident.action;
    aggregate.breaker_open       = incident.breaker_open;
    aggregate.fallback_succeeded = incident.fallback_succeeded;
    const uint64_t recurrence    = static_cast<uint64_t>(aggregate.recurrence_count) + incident.recurrence_count;
    aggregate.recurrence_count =
        static_cast<uint32_t>(std::min<uint64_t>(recurrence, std::numeric_limits<uint32_t>::max()));
    cumulative.last_incident_id = std::max(cumulative.last_incident_id, incident.incident_id);
    return;
  }

  if (records.size() == maximum_records) {
    records.pop_front();
    ++cumulative.history_evictions;
  }
  records.push_back(incident);
  cumulative.last_incident_id = std::max(cumulative.last_incident_id, incident.incident_id);
}

std::vector<incident_record> incident_store::query(const incident_filter& filter) const
{
  std::lock_guard<std::mutex>  lock(mutex);
  std::vector<incident_record> result;
  if (filter.maximum_records == 0) {
    return result;
  }
  result.reserve(std::min(filter.maximum_records, records.size()));
  for (const auto& record : records) {
    if (filter.after_incident_id && record.incident_id <= *filter.after_incident_id) {
      continue;
    }
    if (filter.instance_id && (record.context.instance_id[0] != (*filter.instance_id)[0] ||
                               record.context.instance_id[1] != (*filter.instance_id)[1])) {
      continue;
    }
    if (filter.cause && record.cause != *filter.cause) {
      continue;
    }
    if (filter.phase && record.context.phase != *filter.phase) {
      continue;
    }
    result.push_back(record);
    if (result.size() == filter.maximum_records) {
      break;
    }
  }
  return result;
}

std::optional<incident_record> incident_store::get(uint64_t incident_id) const
{
  std::lock_guard<std::mutex> lock(mutex);
  auto found = std::find_if(records.begin(), records.end(), [incident_id](const incident_record& record) {
    return record.incident_id == incident_id;
  });
  return found != records.end() ? std::optional<incident_record>(*found) : std::nullopt;
}

std::size_t incident_store::clear_through(uint64_t acknowledged_incident_id)
{
  std::lock_guard<std::mutex> lock(mutex);
  std::size_t                 removed = 0;
  while (!records.empty() && records.front().incident_id <= acknowledged_incident_id) {
    records.pop_front();
    ++removed;
  }
  return removed;
}

incident_health_summary incident_store::health() const
{
  std::lock_guard<std::mutex> lock(mutex);
  return cumulative;
}

std::size_t incident_store::size() const
{
  std::lock_guard<std::mutex> lock(mutex);
  return records.size();
}

void incident_store::account_source_drops(uint64_t dropped_delta)
{
  std::lock_guard<std::mutex> lock(mutex);
  cumulative.source_ring_drops += dropped_delta;
}

bool incident_store::same_recurrence(const incident_record& left, const incident_record& right) const noexcept
{
  return left.context.instance_id[0] == right.context.instance_id[0] &&
         left.context.instance_id[1] == right.context.instance_id[1] && left.context.hook_id == right.context.hook_id &&
         left.context.phase == right.context.phase && left.cause == right.cause &&
         left.exception_type_hash == right.exception_type_hash && left.native_status == right.native_status;
}

void incident_store::increment_health(const incident_record& incident) noexcept
{
  ++cumulative.total;
  switch (incident.cause) {
    case incident_cause::standard_exception:
      ++cumulative.standard_exceptions;
      break;
    case incident_cause::unknown_exception:
      ++cumulative.unknown_exceptions;
      break;
    case incident_cause::deadline_miss:
      ++cumulative.deadline_misses;
      break;
    case incident_cause::invalid_input:
      ++cumulative.invalid_inputs;
      break;
    case incident_cause::invalid_output:
      ++cumulative.invalid_outputs;
      break;
    case incident_cause::device_error:
      ++cumulative.device_errors;
      break;
    case incident_cause::resource_exhausted:
      ++cumulative.resource_exhaustions;
      break;
    case incident_cause::quiescence_timeout:
      ++cumulative.quiescence_timeouts;
      break;
    default:
      break;
  }
  if (incident.action == incident_action::breaker_opened) {
    ++cumulative.breaker_opened;
  }
  if (incident.action == incident_action::invocation_fallback || incident.action == incident_action::breaker_opened) {
    ++cumulative.fallbacks;
  }
  if (incident.action == incident_action::quarantined) {
    ++cumulative.quarantined;
  }
}
