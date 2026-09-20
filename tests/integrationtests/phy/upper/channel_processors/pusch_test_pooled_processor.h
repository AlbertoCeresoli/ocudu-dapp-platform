// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include "lib/phy/upper/channel_processors/pusch/pusch_processor_pool.h"
#include "ocudu/phy/upper/unique_rx_buffer.h"
#include <array>
#include <exception>
#include <stdexcept>

namespace ocudu {

/// Single-lane test owner using the real production wrapper and pool lease.
/// SCH delivery alone is not permission to reuse the grid, payload or notifier.
/// The caller keeps these alive until require_idle() succeeds. Never substitute
/// a non-owning sentinel for the processing activity needed by a receiver loan.
class pusch_test_pooled_processor final : public pusch_processor
{
public:
  explicit pusch_test_pooled_processor(std::unique_ptr<pusch_processor> processor)
  {
    std::array<std::unique_ptr<pusch_processor_wrapper>, 1> wrappers;
    wrappers[0] = std::make_unique<pusch_processor_wrapper>(std::move(processor));
    processors =
        std::make_unique<pusch_processor_wrapper::pool>(span<std::unique_ptr<pusch_processor_wrapper>>(wrappers));
  }

  ~pusch_test_pooled_processor() override
  {
    // Isolated test failure: do not destroy possibly borrowed storage or run
    // accelerator cleanup after a missing release. The process must end.
    if (!processors->get()) {
      std::terminate();
    }
  }

  void process(span<uint8_t>                    data,
               unique_rx_buffer                 rm_buffer,
               pusch_processor_result_notifier& notifier,
               const resource_grid_reader&      grid,
               const pdu_t&                     pdu) override
  {
    auto lease = processors->get();
    if (!lease) {
      throw std::runtime_error("Test PUSCH processor still has active consumers.");
    }
    auto& processor = *lease;
    processor.set_unique_ptr(std::move(lease));
    processor.process(data, std::move(rm_buffer), notifier, grid, pdu);
  }

  void require_idle()
  {
    if (!processors->get()) {
      throw std::runtime_error("Test PUSCH result arrived before all processing loans returned.");
    }
  }

private:
  std::unique_ptr<pusch_processor_wrapper::pool> processors;
};
} // namespace ocudu
