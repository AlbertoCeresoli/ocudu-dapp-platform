// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/mpmc_queue.h"
#include "ocudu/ocudulog/logger.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/upper/channel_processors/pusch/formatters.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_processor.h"
#include "ocudu/phy/upper/unique_rx_buffer.h"
#include "ocudu/support/memory_pool/bounded_object_pool.h"
#include <atomic>
#include <limits>

namespace ocudu {

/// PUSCH processor wrapper. It appends its identifier into the free list when the processing is finished.
class pusch_processor_wrapper : public pusch_processor, private pusch_processor_result_notifier
{
public:
  /// Pool of PUSCH processor wrappers.
  using pool = bounded_unique_object_pool<pusch_processor_wrapper>;

  /// Creates a pusch processor wrapper from another pusch processor.
  explicit pusch_processor_wrapper(std::unique_ptr<pusch_processor> processor_) : processor(std::move(processor_))
  {
    ocudu_assert(processor, "Invalid pusch processor.");
  }

  /// Creates a PUSCH processor wrapper from another PUSCH processor wrapper.
  pusch_processor_wrapper(pusch_processor_wrapper&& other) :
    unique_pool_ptr(std::move(other.unique_pool_ptr)),
    upstream_activity(std::move(other.upstream_activity)),
    notifier(other.notifier),
    processor(std::move(other.processor))
  {
    ocudu_assert(other.activities.load(std::memory_order_relaxed) == 0, "Cannot move an active PUSCH processor.");
    other.notifier = nullptr;
  }

  // See pusch_processor interface for documentation.
  void process(span<uint8_t>                    data,
               unique_rx_buffer                 rm_buffer,
               pusch_processor_result_notifier& notifier_,
               const resource_grid_reader&      grid,
               const pdu_t&                     pdu) override
  {
    ocudu_assert(activities.load(std::memory_order_relaxed) == 0, "PUSCH processing activity is still in use.");
    // One implicit reference remains until terminal SCH notification, even if process() returns asynchronously.
    activities.store(1, std::memory_order_relaxed);
    auto processing = acquire_processing_activity();
    // Retain the original uplink slot/grid/task while ANY of this wrapper's consumers is still active.
    upstream_activity = notifier_.acquire_processing_activity();
    // Save original notifier.
    [[maybe_unused]] pusch_processor_result_notifier* prev_notifier = std::exchange(notifier, &notifier_);
    ocudu_assert(prev_notifier == nullptr, "PUSCH processor is in use.");

    // Process.
    processor->process(data, std::move(rm_buffer), *this, grid, pdu);
  }

  /// Sets the unique pointer that will be released upon the completion of the PUSCH processing.
  void set_unique_ptr(pool::ptr unique_token_) { unique_pool_ptr = std::move(unique_token_); }

private:
  pusch_processing_activity acquire_processing_activity() noexcept override
  {
    const auto previous = activities.fetch_add(1, std::memory_order_relaxed);
    ocudu_assert(previous != 0 && previous != std::numeric_limits<unsigned>::max(), "Invalid PUSCH activity count.");
    return {this, &release_activity};
  }

  static void release_activity(void* context) noexcept
  {
    auto& owner = *static_cast<pusch_processor_wrapper*>(context);
    const auto previous = owner.activities.fetch_sub(1, std::memory_order_acq_rel);
    ocudu_assert(previous != 0, "Unbalanced PUSCH activity release.");
    if (previous == 1) {
      // Empty both members before publishing the pool entry. Return the inner pool entry BEFORE releasing
      // the original slot: slot completion can permit destruction/reuse of the surrounding uplink processor.
      auto upstream = std::move(owner.upstream_activity);
      auto lease = std::move(owner.unique_pool_ptr);
      // Destruction order is lease then upstream. Do not access owner after lease destruction.
    }
  }

  // See pusch_processor_result_notifier for documentation.
  void on_uci(const pusch_processor_result_control& uci) override
  {
    auto processing = acquire_processing_activity();
    ocudu_assert(notifier != nullptr, "Invalid notifier.");
    notifier->on_uci(uci);
  }

  // See pusch_processor_result_notifier for documentation.
  void on_sch(const pusch_processor_result_data& sch) override
  {
    auto processing = acquire_processing_activity();
    // Notify the completion of the processing.
    pusch_processor_result_notifier* current_notifier = std::exchange(notifier, nullptr);
    ocudu_assert(current_notifier != nullptr, "Invalid notifier.");
    current_notifier->on_sch(sch);

    // Result delivery closes the implicit reference, not the whole processing lifetime. Caller activities
    // keep the wrapper occupied through decoder/diagnostic tails and asynchronous consumers.
    release_activity(this);
  }

  /// Processor identifier within the pool.
  pool::ptr unique_pool_ptr;
  pusch_processing_activity upstream_activity;
  std::atomic<unsigned> activities{};
  static_assert(std::atomic<unsigned>::is_always_lock_free, "PUSCH activity must not introduce a hidden lock.");
  /// Current PUSCH processor notifier.
  pusch_processor_result_notifier* notifier = nullptr;
  /// Wrapped PUSCH processor.
  std::unique_ptr<pusch_processor> processor;
};

/// \brief PUSCH processor pool
///
/// It contains PUSCH processors that are asynchronously executed. The processing of a PUSCH transmission is dropped if
/// there are no free PUSCH processors available.
///
class pusch_processor_pool : public pusch_processor
{
public:
  /// Alias for the common UCI processor pool. The processors of this pool operate synchronously from the thread that
  /// calls processing.
  using uci_processor_pool = bounded_unique_object_pool<pusch_processor>;

  /// Creates a PUSCH processor pool from a list of processors. Ownership is transferred to the pool.
  pusch_processor_pool(span<std::unique_ptr<pusch_processor_wrapper>> processors_,
                       std::shared_ptr<uci_processor_pool>            uci_processors_) :
    logger(ocudulog::fetch_basic_logger("PHY")), processors(processors_), uci_processors(uci_processors_)
  {
  }

  // See interface for documentation.
  void process(span<uint8_t>                    data,
               unique_rx_buffer                 rm_buffer,
               pusch_processor_result_notifier& notifier,
               const resource_grid_reader&      grid,
               const pdu_t&                     pdu) override
  {
    // Get a processor and process normally.
    auto unique_processor = processors.get();
    if (unique_processor) {
      // Save reference to the processor.
      pusch_processor& processor = *unique_processor;

      // Set unique pointer, it will be returned to the pool when the processing is completed.
      unique_processor->set_unique_ptr(std::move(unique_processor));

      // Process PUSCH.
      processor.process(data, std::move(rm_buffer), notifier, grid, pdu);
      return;
    }

    // No PUSCH processor is available and no UCI is present. Drop PUSCH reception.
    if ((pdu.uci.nof_harq_ack == 0) && (pdu.uci.nof_csi_part1 == 0)) {
      logger.warning(
          pdu.slot.sfn(), pdu.slot.slot_index(), "PUSCH processing queue is full. Dropping PUSCH {:s}.", pdu);

      // Report data-related discarded result if shared channel data is present.
      if (pdu.codeword.has_value()) {
        notifier.on_sch({.data = {.tb_crc_ok = false, .nof_codeblocks_total = 0, .ldpc_decoder_stats = {}}, .csi = {}});
      }

      return;
    }

    // Process UCI synchronously if UCI is present.
    uci_processor_pool::ptr uci_processor = uci_processors->get();
    if (uci_processor) {
      logger.warning(
          pdu.slot.sfn(), pdu.slot.slot_index(), "PUSCH processing queue is full. Processing UCI {:s}.", pdu);
      uci_processor->process(data, std::move(rm_buffer), notifier, grid, pdu);
    } else {
      logger.warning(
          pdu.slot.sfn(), pdu.slot.slot_index(), "PUSCH processing queue is full. Failed processing UCI {:s}.", pdu);

      // Report data-related discarded result if shared channel data is present.
      if (pdu.codeword.has_value()) {
        notifier.on_sch({.data = {.tb_crc_ok = false, .nof_codeblocks_total = 0, .ldpc_decoder_stats = {}}, .csi = {}});
      }

      pusch_processor_result_data discarded_results;
      notifier.on_uci({.harq_ack  = {.payload = uci_payload_type(pdu.uci.nof_harq_ack), .status = uci_status::invalid},
                       .csi_part1 = {},
                       .csi_part2 = {},
                       .csi       = {}});
    }
  }

private:
  /// Physical layer logger.
  ocudulog::basic_logger& logger;
  /// Actual PUSCH processor pool.
  pusch_processor_wrapper::pool processors;
  /// UCI processor pool, used only the normal processor pool runs out of processors or the PUSCH transmission only
  /// contains UCI.
  std::shared_ptr<uci_processor_pool> uci_processors;
};

} // namespace ocudu
