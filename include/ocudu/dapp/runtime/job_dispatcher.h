// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/management/types.h"
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ocudu::dapp {

enum class job_submit_code : uint8_t {
  accepted,
  replayed,
  transaction_mismatch,
  queue_full,
  stopping,
  invalid_request
};

struct job_submit_result {
  job_submit_code                       code{job_submit_code::invalid_request};
  std::optional<management::identifier> job_id;
  std::string                           detail;
  explicit                              operator bool() const noexcept
  {
    return code == job_submit_code::accepted || code == job_submit_code::replayed;
  }
};

class job_dispatcher
{
public:
  using operation = std::function<management::operation_result()>;

  explicit job_dispatcher(std::size_t maximum_queued = 64, std::size_t maximum_retained = 1024);
  ~job_dispatcher();

  job_dispatcher(const job_dispatcher&)            = delete;
  job_dispatcher& operator=(const job_dispatcher&) = delete;

  job_submit_result submit(const management::identifier& transaction_id, uint64_t fingerprint, operation work);
  bool              cancel(const management::identifier& job_id);

  std::optional<management::job_record> get(const management::identifier& job_id) const;
  std::vector<management::job_record>   list() const;
  std::optional<management::job_record> wait(const management::identifier&         job_id,
                                             std::chrono::steady_clock::time_point deadline) const;

  void stop();

private:
  struct entry {
    management::job_record record;
    uint64_t               fingerprint{};
    operation              work;
  };

  management::identifier next_id() noexcept;
  void                   worker_loop();
  static uint64_t        monotonic_ns() noexcept;
  static bool            terminal(management::job_state state) noexcept;
  void                   evict_completed_locked();

  const std::size_t                                                                               maximum_queue_depth;
  const std::size_t                                                                               maximum_retained_jobs;
  mutable std::mutex                                                                              mutex;
  mutable std::condition_variable                                                                 changed;
  std::condition_variable                                                                         available;
  std::unordered_map<management::identifier, entry, management::identifier_hash>                  jobs;
  std::unordered_map<management::identifier, management::identifier, management::identifier_hash> transactions;
  std::deque<management::identifier>                                                              queue;
  std::deque<management::identifier>                                                              completion_order;
  uint64_t                                                                                        id_nonce{};
  uint64_t                                                                                        id_sequence{1};
  bool                                                                                            stopping{};
  std::thread                                                                                     worker;
};

const char* to_string(job_submit_code code) noexcept;

} // namespace ocudu::dapp
