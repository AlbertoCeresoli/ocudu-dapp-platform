// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/du/du_low/du_low_executor_mapper.h"
#include "ocudu/adt/mpmc_queue.h"
#include "ocudu/phy/upper/upper_phy_execution_configuration.h"
#include "ocudu/support/executors/executor_decoration_factory.h"
#include "ocudu/support/executors/inline_task_executor.h"
#include "ocudu/support/executors/strand_executor.h"
#include "ocudu/support/executors/task_fork_limiter.h"
#include "ocudu/support/ocudu_assert.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

using namespace ocudu;
using namespace odu;

namespace {

unsigned get_positive_env_unsigned(const char* env_name, unsigned default_value)
{
  const char* value = std::getenv(env_name);
  if (value == nullptr || value[0] == '\0') {
    return default_value;
  }

  char*         end    = nullptr;
  unsigned long parsed = std::strtoul(value, &end, 10);
  if ((end == value) || (*end != '\0') || (parsed == 0) || (parsed > std::numeric_limits<unsigned>::max())) {
    ocudulog::fetch_basic_logger("CONFIG").warning(
        "Ignoring invalid {}={}; using default value {}.", env_name, value, default_value);
    return default_value;
  }

  return static_cast<unsigned>(parsed);
}

/// Parses env as unsigned including 0. Returns nullopt when unset/invalid.
std::optional<unsigned> get_optional_env_unsigned(const char* env_name)
{
  const char* value = std::getenv(env_name);
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }

  char*         end    = nullptr;
  unsigned long parsed = std::strtoul(value, &end, 10);
  if ((end == value) || (*end != '\0') || (parsed > std::numeric_limits<unsigned>::max())) {
    ocudulog::fetch_basic_logger("CONFIG").warning("Ignoring invalid {}={}.", env_name, value);
    return std::nullopt;
  }

  return static_cast<unsigned>(parsed);
}

/// Spreads tasks across independent executors for multi-cell fairness (avoids one deep FIFO).
class round_robin_task_executor final : public task_executor
{
public:
  explicit round_robin_task_executor(std::vector<task_executor*> lanes_) : lanes(std::move(lanes_))
  {
    ocudu_assert(!lanes.empty(), "Round-robin executor needs at least one lane.");
  }

  [[nodiscard]] bool execute(unique_task task) override
  {
    const unsigned i = next.fetch_add(1U, std::memory_order_relaxed) % static_cast<unsigned>(lanes.size());
    return lanes[i]->execute(std::move(task));
  }

  [[nodiscard]] bool defer(unique_task task) override
  {
    const unsigned i = next.fetch_add(1U, std::memory_order_relaxed) % static_cast<unsigned>(lanes.size());
    return lanes[i]->defer(std::move(task));
  }

private:
  std::vector<task_executor*> lanes;
  std::atomic<unsigned>       next{0};
};

upper_phy_executor get_pdsch_base_executor(const du_low_executor_mapper_flexible_exec_config& flexible)
{
  const char* value = std::getenv("OCUDU_DU_LOW_PDSCH_EXECUTOR");
  if ((value == nullptr) || (value[0] == '\0') || (std::strcmp(value, "rt") == 0) ||
      (std::strcmp(value, "rt_high") == 0) || (std::strcmp(value, "rt-high") == 0)) {
    return flexible.rt_hi_prio_exec;
  }

  if ((std::strcmp(value, "high") == 0) || (std::strcmp(value, "non_rt_high") == 0) ||
      (std::strcmp(value, "non-rt-high") == 0)) {
    ocudulog::fetch_basic_logger("CONFIG").info("Routing PDSCH data processing to the non-RT high-priority executor.");
    return flexible.non_rt_hi_prio_exec;
  }

  if ((std::strcmp(value, "medium") == 0) || (std::strcmp(value, "non_rt_medium") == 0) ||
      (std::strcmp(value, "non-rt-medium") == 0)) {
    ocudulog::fetch_basic_logger("CONFIG").info(
        "Routing PDSCH data processing to the non-RT medium-priority executor.");
    return flexible.non_rt_medium_prio_exec;
  }

  ocudulog::fetch_basic_logger("CONFIG").warning(
      "Ignoring invalid OCUDU_DU_LOW_PDSCH_EXECUTOR={}; using the RT high-priority executor.", value);
  return flexible.rt_hi_prio_exec;
}

/// Helper class to decorate executors with extra functionalities.
struct executor_decorator {
  template <typename Exec, typename Tracer = detail::null_event_tracer>
  task_executor& decorate(Exec&&                             exec,
                          bool                               is_sync,
                          bool                               tracing_enabled,
                          std::optional<unsigned>            throttle_thres,
                          executor_metrics_channel_registry* metrics_channel_registry,
                          const std::string&                 exec_name = "",
                          Tracer*                            tracer    = nullptr)
  {
    if (not is_sync and not tracing_enabled and not metrics_channel_registry and not throttle_thres) {
      // No decoration needed, return the original executor.
      return exec;
    }

    execution_decoration_config cfg;
    if (is_sync) {
      cfg.sync = execution_decoration_config::sync_option{};
    }
    if (throttle_thres.has_value()) {
      cfg.throttle = execution_decoration_config::throttle_option{*throttle_thres};
    }
    if (metrics_channel_registry != nullptr) {
      if constexpr (std::is_same_v<Tracer, file_event_tracer<true>>) {
        cfg.metrics.emplace(exec_name, *metrics_channel_registry, tracing_enabled, tracer);
      } else {
        cfg.metrics.emplace(exec_name, *metrics_channel_registry, tracing_enabled);
      }
    } else if (tracing_enabled) {
      cfg.trace = execution_decoration_config::trace_option{exec_name};
    }
    decorators.push_back(decorate_executor(std::forward<Exec>(exec), cfg));

    return *decorators.back();
  }

private:
  std::vector<std::unique_ptr<task_executor>> decorators;
};

class du_low_executor_mapper_impl : public du_low_executor_mapper
{
public:
  du_low_executor_mapper_impl(const du_low_executor_mapper_config& config)
  {
    if (std::holds_alternative<du_low_executor_mapper_single_exec_config>(config.executors)) {
      const auto& single = std::get<du_low_executor_mapper_single_exec_config>(config.executors);

      ocudu_assert(single.common_executor.executor != nullptr, "Invalid common executor.");
      ocudu_assert(single.common_executor.max_concurrency != 0, "Invalid common executor maximum concurrency.");

      phy_config.pdcch_executor   = single.common_executor;
      phy_config.pdsch_executor   = single.common_executor;
      phy_config.ssb_executor     = single.common_executor;
      phy_config.csi_rs_executor  = single.common_executor;
      phy_config.prs_executor     = single.common_executor;
      phy_config.dl_grid_executor = single.common_executor;
      phy_config.prach_executor   = single.common_executor;
      phy_config.pusch_executor   = single.common_executor;
      phy_config.pucch_executor   = single.common_executor;
      phy_config.srs_executor     = single.common_executor;

      phy_config.pusch_ch_estimator_executor = create_inline_task_executor();

      // The PDSCH processor and the PUSCH decoder shall work synchronously.
      phy_config.pdsch_codeblock_executor = {};
      phy_config.pusch_decoder_executor   = {};
    } else if (std::holds_alternative<du_low_executor_mapper_flexible_exec_config>(config.executors)) {
      const unsigned max_pucch_batch_size = 16;
      const unsigned max_pusch_batch_size = 1;
      // Default batch size of 1 improves multi-sector RT interleaving versus larger batches.
      // Override with OCUDU_DU_LOW_PDSCH_EXECUTOR_MAX_BATCH when a larger batch is preferred.
      const unsigned max_pdsch_batch_size = get_positive_env_unsigned("OCUDU_DU_LOW_PDSCH_EXECUTOR_MAX_BATCH", 1);
      static constexpr std::array<unsigned, 1> pdsch_queue_sizes{default_queue_size};
      static constexpr std::array<unsigned, 1> pucch_queue_sizes{default_queue_size};
      static constexpr std::array<unsigned, 3> pusch_srs_queue_sizes{
          default_queue_size, default_queue_size, default_queue_size};

      const auto& flexible = std::get<du_low_executor_mapper_flexible_exec_config>(config.executors);

      report_error_if_not(
          flexible.max_pusch_and_srs_concurrency <= flexible.non_rt_medium_prio_exec.max_concurrency,
          "Maximum PUSCH and SRS concurrency (i.e., {}) exceeds the number of main pool threads (i.e., {}).",
          flexible.max_pusch_and_srs_concurrency,
          flexible.non_rt_medium_prio_exec.max_concurrency);

      upper_phy_executor pdsch_base_executor = get_pdsch_base_executor(flexible);

      // Optional round-robin multi-lane dispatch: OCUDU_DU_LOW_PDSCH_FAIR_LANES=N.
      // Spreads PDSCH tasks across N single-concurrency lanes (no per-cell affinity).
      unsigned rr_fair_lanes = 0;
      if (const auto env_lanes = get_optional_env_unsigned("OCUDU_DU_LOW_PDSCH_FAIR_LANES")) {
        rr_fair_lanes = *env_lanes;
      }
      if (rr_fair_lanes > flexible.max_pdsch_concurrency) {
        rr_fair_lanes = flexible.max_pdsch_concurrency;
      }

      // Per-cell PDSCH fairness (opt-in): OCUDU_DU_LOW_PDSCH_CELL_FAIR=N creates one
      // lane per max_pdsch slot with N tasks in flight each; cells bind by cell_id % lanes.
      // N=1 ("on"/"auto") serializes each cell; N>1 relaxes the per-cell serialization tax
      // while keeping cross-cell isolation.
      unsigned cell_fair_lanes    = 0;
      unsigned cell_fair_inflight = 1;
      {
        const char* cell_fair = std::getenv("OCUDU_DU_LOW_PDSCH_CELL_FAIR");
        if (cell_fair != nullptr && (std::strcmp(cell_fair, "on") == 0 || std::strcmp(cell_fair, "auto") == 0)) {
          cell_fair_lanes = flexible.max_pdsch_concurrency;
        } else if (cell_fair != nullptr && cell_fair[0] != '\0' && std::strcmp(cell_fair, "0") != 0) {
          char*         end    = nullptr;
          unsigned long parsed = std::strtoul(cell_fair, &end, 10);
          if ((end != cell_fair) && (*end == '\0') && (parsed > 0) &&
              (parsed <= std::numeric_limits<unsigned>::max())) {
            cell_fair_lanes    = flexible.max_pdsch_concurrency;
            cell_fair_inflight = static_cast<unsigned>(parsed);
          } else {
            ocudulog::fetch_basic_logger("CONFIG").warning(
                "Ignoring invalid OCUDU_DU_LOW_PDSCH_CELL_FAIR={}; expected 0, on, auto or a positive in-flight count.",
                cell_fair);
          }
        }
      }

      upper_phy_executor pdsch_exec;
      if (cell_fair_lanes > 1) {
        // True per-cell affinity: cell_id % N selects a dedicated bounded-flight lane.
        phy_config.pdsch_cell_executors.clear();
        phy_config.pdsch_cell_executors.reserve(cell_fair_lanes);
        for (unsigned i = 0; i != cell_fair_lanes; ++i) {
          auto lane = create_task_fork_limiter(
              pdsch_base_executor, cell_fair_inflight, pdsch_queue_sizes, max_pdsch_batch_size);
          phy_config.pdsch_cell_executors.push_back(lane[0].executor);
        }
        pdsch_exec = upper_phy_executor{.executor        = phy_config.pdsch_cell_executors.front(),
                                        .max_concurrency = cell_fair_lanes * cell_fair_inflight};
        ocudulog::fetch_basic_logger("CONFIG").info(
            "PDSCH per-cell fair lanes enabled: lanes={} ({} in-flight/cell-group, batch={}). "
            "Disable with OCUDU_DU_LOW_PDSCH_CELL_FAIR=0.",
            cell_fair_lanes,
            cell_fair_inflight,
            max_pdsch_batch_size);
      } else if (rr_fair_lanes > 1) {
        std::vector<task_executor*> lane_ptrs;
        lane_ptrs.reserve(rr_fair_lanes);
        for (unsigned i = 0; i != rr_fair_lanes; ++i) {
          auto lane = create_task_fork_limiter(pdsch_base_executor, 1, pdsch_queue_sizes, max_pdsch_batch_size);
          lane_ptrs.push_back(lane[0].executor);
        }
        executors.emplace_back(std::make_unique<round_robin_task_executor>(std::move(lane_ptrs)));
        pdsch_exec = upper_phy_executor{.executor = executors.back().get(), .max_concurrency = rr_fair_lanes};
        ocudulog::fetch_basic_logger("CONFIG").info(
            "PDSCH RR multi-lane dispatch enabled: lanes={} (each concurrency=1, batch={}).",
            rr_fair_lanes,
            max_pdsch_batch_size);
      } else {
        auto pdsch_executors = create_task_fork_limiter(
            pdsch_base_executor, flexible.max_pdsch_concurrency, pdsch_queue_sizes, max_pdsch_batch_size);
        pdsch_exec = pdsch_executors[0];
      }

      phy_config.pdcch_executor           = flexible.rt_hi_prio_exec;
      phy_config.pdsch_executor           = pdsch_exec;
      phy_config.ssb_executor             = flexible.rt_hi_prio_exec;
      phy_config.csi_rs_executor          = flexible.rt_hi_prio_exec;
      phy_config.prs_executor             = flexible.rt_hi_prio_exec;
      phy_config.dl_grid_executor         = flexible.non_rt_hi_prio_exec;
      phy_config.pdsch_codeblock_executor = pdsch_exec;
      phy_config.prach_executor           = flexible.non_rt_hi_prio_exec;
      phy_config.pucch_executor =
          create_task_fork_limiter(
              flexible.non_rt_hi_prio_exec, flexible.max_pucch_concurrency, pucch_queue_sizes, max_pucch_batch_size)
              .front();

      // Optional PUSCH/SRS routing. Default remains non-rt-medium (shared with PDSCH when PDSCH is
      // also on medium). Explicit OCUDU_DU_LOW_PUSCH_EXECUTOR=high|low|rt|medium overrides.
      // OCUDU_DU_LOW_PUSCH_AUTO_SEPARATE=1 moves PUSCH off medium only when PDSCH is also on medium;
      // left opt-in because enabling it by default raised multi-sector RU late in A/B tests.
      const bool         pdsch_on_medium = (pdsch_base_executor.executor == flexible.non_rt_medium_prio_exec.executor);
      upper_phy_executor pusch_base      = flexible.non_rt_medium_prio_exec;
      if (const char* pusch_route = std::getenv("OCUDU_DU_LOW_PUSCH_EXECUTOR")) {
        if ((std::strcmp(pusch_route, "low") == 0) || (std::strcmp(pusch_route, "non_rt_low") == 0) ||
            (std::strcmp(pusch_route, "non-rt-low") == 0)) {
          pusch_base = flexible.non_rt_low_prio_exec;
          ocudulog::fetch_basic_logger("CONFIG").info("Routing PUSCH/SRS to non-RT low-priority executor.");
        } else if ((std::strcmp(pusch_route, "high") == 0) || (std::strcmp(pusch_route, "non_rt_high") == 0) ||
                   (std::strcmp(pusch_route, "non-rt-high") == 0)) {
          pusch_base = flexible.non_rt_hi_prio_exec;
          ocudulog::fetch_basic_logger("CONFIG").info("Routing PUSCH/SRS to non-RT high-priority executor.");
        } else if ((std::strcmp(pusch_route, "rt") == 0) || (std::strcmp(pusch_route, "rt_high") == 0) ||
                   (std::strcmp(pusch_route, "rt-high") == 0)) {
          pusch_base = flexible.rt_hi_prio_exec;
          ocudulog::fetch_basic_logger("CONFIG").info("Routing PUSCH/SRS to RT high-priority executor.");
        } else if ((std::strcmp(pusch_route, "medium") == 0) || (std::strcmp(pusch_route, "non_rt_medium") == 0) ||
                   (std::strcmp(pusch_route, "non-rt-medium") == 0)) {
          pusch_base = flexible.non_rt_medium_prio_exec;
          ocudulog::fetch_basic_logger("CONFIG").info("Routing PUSCH/SRS to non-RT medium-priority executor.");
        } else {
          ocudulog::fetch_basic_logger("CONFIG").warning(
              "Ignoring invalid OCUDU_DU_LOW_PUSCH_EXECUTOR={}; keeping default non-rt-medium.", pusch_route);
        }
      } else if (pdsch_on_medium) {
        const char* auto_sep = std::getenv("OCUDU_DU_LOW_PUSCH_AUTO_SEPARATE");
        if (auto_sep != nullptr && auto_sep[0] != '\0' && std::strcmp(auto_sep, "0") != 0) {
          if ((std::strcmp(auto_sep, "low") == 0) || (std::strcmp(auto_sep, "non-rt-low") == 0)) {
            pusch_base = flexible.non_rt_low_prio_exec;
            ocudulog::fetch_basic_logger("CONFIG").info(
                "Auto-separating PUSCH/SRS to non-RT low-priority executor (prefer DL on medium).");
          } else {
            pusch_base = flexible.non_rt_hi_prio_exec;
            ocudulog::fetch_basic_logger("CONFIG").info("Auto-separating PUSCH/SRS to non-RT high-priority executor "
                                                        "(OCUDU_DU_LOW_PUSCH_AUTO_SEPARATE={}).",
                                                        auto_sep);
          }
        }
      }
      // Optional UL concurrency cap: OCUDU_DU_LOW_PUSCH_CONCURRENCY_CAP=N (default: no cap).
      unsigned pusch_conc = flexible.max_pusch_and_srs_concurrency;
      if (const auto cap = get_optional_env_unsigned("OCUDU_DU_LOW_PUSCH_CONCURRENCY_CAP")) {
        if (*cap > 0 && *cap < pusch_conc) {
          ocudulog::fetch_basic_logger("CONFIG").info(
              "Capping PUSCH/SRS concurrency {} → {} (OCUDU_DU_LOW_PUSCH_CONCURRENCY_CAP).", pusch_conc, *cap);
          pusch_conc = *cap;
        }
      }
      auto pusch_srs_execs =
          create_task_fork_limiter(pusch_base, pusch_conc, pusch_srs_queue_sizes, max_pusch_batch_size);
      phy_config.pusch_ch_estimator_executor = pusch_srs_execs[0];
      phy_config.pusch_executor              = pusch_srs_execs[1];
      phy_config.pusch_decoder_executor      = pusch_srs_execs[2];
      phy_config.srs_executor                = pusch_srs_execs[2];
    }

    ocudu_assert(phy_config.pdcch_executor.is_valid(), "Invalid PDCCH executor.");
    ocudu_assert(phy_config.pdsch_executor.is_valid(), "Invalid PDSCH executor.");
    ocudu_assert(phy_config.ssb_executor.is_valid(), "Invalid SSB executor.");
    ocudu_assert(phy_config.csi_rs_executor.is_valid(), "Invalid NZP-CSI-RS executor.");
    ocudu_assert(phy_config.prs_executor.is_valid(), "Invalid PRS executor.");
    ocudu_assert(phy_config.dl_grid_executor.is_valid(), "Invalid DL grid pool executor.");
    ocudu_assert(phy_config.pucch_executor.is_valid(), "Invalid PUCCH executor.");
    ocudu_assert(phy_config.pusch_executor.is_valid(), "Invalid PUSCH executor.");
    ocudu_assert(phy_config.pusch_ch_estimator_executor.is_valid(), "Invalid PUSCH channel estimator executor.");
    ocudu_assert(phy_config.prach_executor.is_valid(), "Invalid PRACH executor.");
    ocudu_assert(phy_config.srs_executor.is_valid(), "Invalid SRS executor.");

    if (config.exec_metrics_channel_registry || config.executor_tracing_enable) {
      bool                               executor_tracing_enable  = config.executor_tracing_enable;
      executor_metrics_channel_registry* metrics_channel_registry = config.exec_metrics_channel_registry;

      phy_config.pdcch_executor = wrap_executor_with_metric_tracing(
          phy_config.pdcch_executor, "pdcch_exec", executor_tracing_enable, metrics_channel_registry);
      phy_config.pdsch_executor = wrap_executor_with_metric_tracing(
          phy_config.pdsch_executor, "pdsch_exec", executor_tracing_enable, metrics_channel_registry);
      phy_config.ssb_executor = wrap_executor_with_metric_tracing(
          phy_config.ssb_executor, "ssb_exec", executor_tracing_enable, metrics_channel_registry);
      phy_config.csi_rs_executor = wrap_executor_with_metric_tracing(
          phy_config.csi_rs_executor, "csi_rs_exec", executor_tracing_enable, metrics_channel_registry);
      phy_config.prs_executor = wrap_executor_with_metric_tracing(
          phy_config.prs_executor, "prs_exec", executor_tracing_enable, metrics_channel_registry);
      if (phy_config.pdsch_codeblock_executor.is_valid()) {
        phy_config.pdsch_codeblock_executor = wrap_executor_with_metric_tracing(phy_config.pdsch_codeblock_executor,
                                                                                "pdsch_codeblock_exec",
                                                                                config.executor_tracing_enable,
                                                                                config.exec_metrics_channel_registry);
      }
      phy_config.prach_executor = wrap_executor_with_metric_tracing(
          phy_config.prach_executor, "prach_exec", executor_tracing_enable, metrics_channel_registry);
      phy_config.pusch_executor = wrap_executor_with_metric_tracing(
          phy_config.pusch_executor, "pusch_exec", executor_tracing_enable, metrics_channel_registry);
      phy_config.pusch_ch_estimator_executor = wrap_executor_with_metric_tracing(phy_config.pusch_ch_estimator_executor,
                                                                                 "pusch_ch_est_exec",
                                                                                 config.executor_tracing_enable,
                                                                                 config.exec_metrics_channel_registry);
      if (phy_config.pusch_decoder_executor.is_valid()) {
        phy_config.pusch_decoder_executor = wrap_executor_with_metric_tracing(phy_config.pusch_decoder_executor,
                                                                              "pusch_dec_exec",
                                                                              config.executor_tracing_enable,
                                                                              config.exec_metrics_channel_registry);
      }
      phy_config.pucch_executor = wrap_executor_with_metric_tracing(
          phy_config.pucch_executor, "pucch_exec", executor_tracing_enable, metrics_channel_registry);
      phy_config.srs_executor = wrap_executor_with_metric_tracing(
          phy_config.srs_executor, "srs_exec", executor_tracing_enable, metrics_channel_registry);
    }
  }

  // See interface for documentation.
  const upper_phy_execution_configuration& get_upper_phy_execution_config() const override { return phy_config; }

private:
  /// Default queue size for DU low executors.
  static constexpr unsigned default_queue_size = 2048;

  /// Creates a strand on the top of a given executor.
  std::vector<upper_phy_executor>
  create_strand(upper_phy_executor base_executor, span<const unsigned> queue_sizes, unsigned max_batch)
  {
    ocudu_assert(base_executor.is_valid(), "Invalid executor.");
    ocudu_assert(!queue_sizes.empty(), "Queue sizes must not be empty.");

    // Only one priority level is requested.
    if ((queue_sizes.size() == 1)) {
      // Skip creating a strand if the base executor maximum concurrency is one.
      if (base_executor.max_concurrency == 1) {
        return {base_executor};
      }

      // Create strand without priority queues.
      executors.emplace_back(make_task_strand_ptr<concurrent_queue_policy::lockfree_mpmc>(
          *base_executor.executor, queue_sizes[0], max_batch));
      return {{executors.back().get(), 1}};
    }

    // Instantiate a priority task strand if several priority levels are requested.
    std::vector<concurrent_queue_params> qparams;
    qparams.reserve(queue_sizes.size());
    for (auto qsize : queue_sizes) {
      qparams.emplace_back(concurrent_queue_params{concurrent_queue_policy::lockfree_mpmc, qsize});
    }
    auto prio_strand_ptr = make_priority_task_strand_ptr(*base_executor.executor, qparams);

    // Save executors.
    auto exec_list = prio_strand_ptr->get_executors();
    adaptors.emplace_back(std::move(prio_strand_ptr));

    std::vector<upper_phy_executor> ret;
    for (task_executor& exec : exec_list) {
      ret.emplace_back(upper_phy_executor{.executor = &exec, .max_concurrency = 1});
    }
    return ret;
  }

  /// Creates a task fork limiter on the top of a given executor.
  std::vector<upper_phy_executor> create_task_fork_limiter(upper_phy_executor   base_executor,
                                                           unsigned             max_nof_threads,
                                                           span<const unsigned> queue_sizes,
                                                           unsigned             max_batch)
  {
    ocudu_assert(base_executor.is_valid(), "Invalid executor.");
    ocudu_assert(!queue_sizes.empty(), "Queue sizes must not be empty.");

    // Limit maximum number of threads to the base executor maximum concurrency.
    if ((max_nof_threads == 0) || (max_nof_threads > base_executor.max_concurrency)) {
      max_nof_threads = base_executor.max_concurrency;
    }

    // If the resultant maximum number of threads is one, create a strand.
    if (max_nof_threads <= 1) {
      return create_strand(base_executor, queue_sizes, max_batch);
    }

    // Skip creation of the fork limiter if there is only one queue and the maximum number of threads is equal to the
    // base executor maximum concurrency.
    if ((queue_sizes.size() == 1) && (max_nof_threads == base_executor.max_concurrency)) {
      return {base_executor};
    }

    // Create fork limiter.
    auto fork_limiter = std::make_unique<task_fork_limiter<task_executor&>>(
        *base_executor.executor, max_nof_threads, queue_sizes, max_batch);
    auto execs = fork_limiter->get_executors();
    adaptors.emplace_back(std::move(fork_limiter));

    std::vector<upper_phy_executor> ret;
    for (task_executor& exec : execs) {
      ret.emplace_back(upper_phy_executor{.executor = &exec, .max_concurrency = max_nof_threads});
    }
    return ret;
  }

  /// Creates an inline task executor.
  upper_phy_executor create_inline_task_executor()
  {
    executors.emplace_back(std::make_unique<inline_task_executor>());
    return {executors.back().get(), 1};
  }

  /// Wraps an executor with a metric decorator.
  upper_phy_executor wrap_executor_with_metric_tracing(upper_phy_executor                 base_executor,
                                                       std::string                        exec_name,
                                                       bool                               tracing_enabled,
                                                       executor_metrics_channel_registry* metrics_channel_registry)
  {
    ocudu_assert(base_executor.is_valid(), "Invalid executor.");
    auto* decorated_executor = &decorator.decorate(
        *base_executor.executor, false, tracing_enabled, std::nullopt, metrics_channel_registry, exec_name);
    return {decorated_executor, base_executor.max_concurrency};
  }

  /// Upper physical layer executor configuration.
  upper_phy_execution_configuration phy_config;
  /// List of internal executor instances.
  std::vector<std::unique_ptr<task_executor>> executors;
  /// Strands and task fork limiters used by the mapper.
  std::vector<std::variant<std::unique_ptr<priority_task_strand<task_executor&>>,
                           std::unique_ptr<task_fork_limiter<task_executor&>>>>
                     adaptors;
  executor_decorator decorator;
};

} // namespace

std::unique_ptr<du_low_executor_mapper>
ocudu::odu::create_du_low_executor_mapper(const du_low_executor_mapper_config& config)
{
  return std::make_unique<du_low_executor_mapper_impl>(config);
}
