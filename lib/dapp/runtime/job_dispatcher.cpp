// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/job_dispatcher.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>

using namespace ocudu::dapp;
using namespace ocudu::dapp::management;

job_dispatcher::job_dispatcher(std::size_t maximum_queued, std::size_t maximum_retained) :
  maximum_queue_depth(maximum_queued), maximum_retained_jobs(maximum_retained)
{
  if (maximum_queued == 0 || maximum_retained == 0) {
    throw std::invalid_argument("job dispatcher bounds must be nonzero");
  }
  std::random_device random;
  id_nonce = (static_cast<uint64_t>(random()) << 32U) ^ random() ^ monotonic_ns();
  if (id_nonce == 0) {
    id_nonce = 0x45334a4f42534a31ULL;
  }
  worker = std::thread([this] { worker_loop(); });
}

job_dispatcher::~job_dispatcher()
{
  stop();
}

job_submit_result job_dispatcher::submit(const identifier& transaction_id, uint64_t fingerprint, operation work)
{
  if (transaction_id.is_zero() || !work) {
    return {job_submit_code::invalid_request, std::nullopt, "transaction and operation are required"};
  }
  std::lock_guard<std::mutex> lock(mutex);
  if (stopping) {
    return {job_submit_code::stopping, std::nullopt, "dispatcher is stopping"};
  }
  if (auto transaction = transactions.find(transaction_id); transaction != transactions.end()) {
    auto existing = jobs.find(transaction->second);
    if (existing == jobs.end()) {
      return {job_submit_code::invalid_request, std::nullopt, "transaction index is inconsistent"};
    }
    if (existing->second.fingerprint != fingerprint) {
      return {job_submit_code::transaction_mismatch, transaction->second, "transaction identifies different work"};
    }
    return {job_submit_code::replayed, transaction->second, {}};
  }
  if (queue.size() == maximum_queue_depth) {
    return {job_submit_code::queue_full, std::nullopt, "bounded job queue is full"};
  }
  evict_completed_locked();
  if (jobs.size() >= maximum_retained_jobs) {
    return {job_submit_code::queue_full, std::nullopt, "retention bound is full of active jobs"};
  }

  const identifier job_id = next_id();
  entry            created;
  created.record.job_id                 = job_id;
  created.record.transaction_id         = transaction_id;
  created.record.state                  = job_state::accepted;
  created.record.submitted_monotonic_ns = monotonic_ns();
  created.fingerprint                   = fingerprint;
  created.work                          = std::move(work);
  jobs.emplace(job_id, std::move(created));
  transactions.emplace(transaction_id, job_id);
  queue.push_back(job_id);
  available.notify_one();
  changed.notify_all();
  return {job_submit_code::accepted, job_id, {}};
}

bool job_dispatcher::cancel(const identifier& job_id)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto                        found = jobs.find(job_id);
  if (found == jobs.end() || found->second.record.state != job_state::accepted) {
    return false;
  }
  auto queued = std::find(queue.begin(), queue.end(), job_id);
  if (queued == queue.end()) {
    return false;
  }
  queue.erase(queued);
  found->second.record.state                  = job_state::cancelled;
  found->second.record.progress_percent       = 100;
  found->second.record.completed_monotonic_ns = monotonic_ns();
  found->second.work                          = {};
  completion_order.push_back(job_id);
  changed.notify_all();
  return true;
}

std::optional<job_record> job_dispatcher::get(const identifier& job_id) const
{
  std::lock_guard<std::mutex> lock(mutex);
  auto                        found = jobs.find(job_id);
  return found != jobs.end() ? std::optional<job_record>(found->second.record) : std::nullopt;
}

std::vector<job_record> job_dispatcher::list() const
{
  std::lock_guard<std::mutex> lock(mutex);
  std::vector<job_record>     result;
  result.reserve(jobs.size());
  for (const auto& item : jobs) {
    result.push_back(item.second.record);
  }
  std::sort(result.begin(), result.end(), [](const job_record& left, const job_record& right) {
    return left.submitted_monotonic_ns < right.submitted_monotonic_ns;
  });
  return result;
}

std::optional<job_record> job_dispatcher::wait(const identifier&                     job_id,
                                               std::chrono::steady_clock::time_point deadline) const
{
  std::unique_lock<std::mutex> lock(mutex);
  if (jobs.find(job_id) == jobs.end()) {
    return std::nullopt;
  }
  changed.wait_until(lock, deadline, [&] {
    auto found = jobs.find(job_id);
    return found == jobs.end() || terminal(found->second.record.state);
  });
  auto found = jobs.find(job_id);
  return found != jobs.end() ? std::optional<job_record>(found->second.record) : std::nullopt;
}

void job_dispatcher::stop()
{
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!stopping) {
      stopping = true;
      while (!queue.empty()) {
        auto found = jobs.find(queue.front());
        queue.pop_front();
        if (found != jobs.end() && found->second.record.state == job_state::accepted) {
          found->second.record.state                  = job_state::cancelled;
          found->second.record.progress_percent       = 100;
          found->second.record.completed_monotonic_ns = monotonic_ns();
          found->second.work                          = {};
          completion_order.push_back(found->first);
        }
      }
    }
  }
  available.notify_all();
  changed.notify_all();
  if (worker.joinable()) {
    worker.join();
  }
}

identifier job_dispatcher::next_id() noexcept
{
  identifier     result;
  const uint64_t sequence = id_sequence++;
  std::memcpy(result.bytes.data(), &id_nonce, sizeof(id_nonce));
  std::memcpy(result.bytes.data() + sizeof(id_nonce), &sequence, sizeof(sequence));
  return result;
}

void job_dispatcher::worker_loop()
{
  while (true) {
    identifier job_id;
    operation  work;
    {
      std::unique_lock<std::mutex> lock(mutex);
      available.wait(lock, [this] { return stopping || !queue.empty(); });
      if (stopping && queue.empty()) {
        return;
      }
      job_id = queue.front();
      queue.pop_front();
      auto found = jobs.find(job_id);
      if (found == jobs.end() || found->second.record.state != job_state::accepted) {
        continue;
      }
      found->second.record.state                = job_state::running;
      found->second.record.progress_percent     = 1;
      found->second.record.started_monotonic_ns = monotonic_ns();
      work                                      = std::move(found->second.work);
      changed.notify_all();
    }

    operation_result outcome;
    try {
      outcome = work();
    } catch (const std::exception& exception) {
      outcome.code   = result_code::lifecycle_failed;
      outcome.detail = exception.what();
    } catch (...) {
      outcome.code   = result_code::lifecycle_failed;
      outcome.detail = "unknown exception escaped management job";
    }

    {
      std::lock_guard<std::mutex> lock(mutex);
      auto                        found = jobs.find(job_id);
      if (found == jobs.end()) {
        continue;
      }
      found->second.record.result                 = outcome;
      found->second.record.state                  = outcome ? job_state::succeeded : job_state::failed;
      found->second.record.progress_percent       = 100;
      found->second.record.completed_monotonic_ns = monotonic_ns();
      found->second.record.detail                 = outcome.detail;
      completion_order.push_back(job_id);
      evict_completed_locked();
      changed.notify_all();
    }
  }
}

uint64_t job_dispatcher::monotonic_ns() noexcept
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool job_dispatcher::terminal(job_state state) noexcept
{
  return state == job_state::succeeded || state == job_state::failed || state == job_state::cancelled;
}

void job_dispatcher::evict_completed_locked()
{
  while (jobs.size() >= maximum_retained_jobs && !completion_order.empty()) {
    const identifier oldest = completion_order.front();
    completion_order.pop_front();
    auto found = jobs.find(oldest);
    if (found == jobs.end() || !terminal(found->second.record.state)) {
      continue;
    }
    transactions.erase(found->second.record.transaction_id);
    jobs.erase(found);
  }
}

const char* ocudu::dapp::to_string(job_submit_code code) noexcept
{
  static constexpr const char* names[] = {
      "accepted", "replayed", "transaction mismatch", "queue full", "stopping", "invalid request"};
  const auto index = static_cast<unsigned>(code);
  return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown job submit result";
}
