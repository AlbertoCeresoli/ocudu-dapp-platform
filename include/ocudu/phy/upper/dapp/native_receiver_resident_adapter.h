// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include "ocudu/phy/upper/dapp/pusch_dapp_demodulator.h"
#include "ocudu/phy/upper/acceleration/phy_acceleration.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_resource_loan.h"
#include <array>
#include <atomic>
#include <memory>

namespace ocudu {

/// Move-only reclamation output. Destroy these owners off L1, after every consumer is fenced.
struct native_receiver_reclaimed {
  std::shared_ptr<dapp::native_worker> worker;
  pusch_resource_loan                  tensors;
};

/// Host PHY binding of native C completion to the resident decoder's acceptance callback.
/// Cold-allocated, stable address; caller retains it through all copied descriptors and explicit retirement.
/// Tensor owners must cover the actual exclusive allocations, metadata, streams and context. An arbitrary
/// lifetime holder is NOT proof of a pool lease. This class does not allocate or authenticate those resources.
class native_receiver_resident_adapter
{
public:
  static constexpr unsigned capacity = 4;
  native_receiver_resident_adapter() = default;
  ~native_receiver_resident_adapter();
  native_receiver_resident_adapter(const native_receiver_resident_adapter&)            = delete;
  native_receiver_resident_adapter& operator=(const native_receiver_resident_adapter&) = delete;

  /// Direct complete F16 codeword only; transformed/UCI-compacted outputs need a separate continuation loan.
  /// On success installs acceptance and moves tensors into a fixed record with the original native worker.
  /// A partial/uncertain enqueue retains tensors, closes admission and forbids fallback even on error.
  /// Caller must not consume that unpublished codeword; use control retirement after withdrawing consumers.
  dapp::native_invocation_result submit(pusch_dapp_demodulator_target&      target,
                                        uint32_t                            lane,
                                        const ocudu_dapp_receiver_input_v1& input,
                                        ocudu_dapp_receiver_output_v1&      output,
                                        resident_softbit_buffer&            codeword,
                                        pusch_resource_loan&                tensors) noexcept;

  /// After decoder RETURN and all diagnostic/other consumers finish/fence (or cancellation without consumption).
  /// OK moves owners to an empty output; it never runs their deleters. Not-ready/failure retains the record.
  ocudu_dapp_status_v1 release_after_consumers(const resident_softbit_buffer& codeword,
                                               native_receiver_reclaimed&     reclaimed) noexcept;
  void                 stop() noexcept { stopped.store(true, std::memory_order_release); }
  /// Control-only. Published records must first be released; quarantined submissions retire their original worker.
  /// No new work after this call. Backend destruction does not have a real-time wall-clock guarantee.
  ocudu_dapp_status_v1 retire(std::array<native_receiver_reclaimed, capacity>& reclaimed) noexcept;

private:
  struct record {
    native_receiver_resident_adapter*    owner{};
    dapp::native_receiver_ticket         ticket{};
    resident_softbit_buffer              expected{};
    std::shared_ptr<dapp::native_worker> worker;
    pusch_resource_loan                  tensors;
    bool                                 checked{}, quarantined{};
  };
  static bool                  accept(void*, const resident_softbit_buffer&) noexcept;
  static bool                  same_codeword(const resident_softbit_buffer&, const resident_softbit_buffer&) noexcept;
  static bool                  valid_codeword(const ocudu_dapp_receiver_input_v1&,
                                              const ocudu_dapp_receiver_output_v1&,
                                              const resident_softbit_buffer&) noexcept;
  static void                  reclaim(record&, native_receiver_reclaimed&) noexcept;
  std::array<record, capacity> records{};
  std::atomic_flag             gate = ATOMIC_FLAG_INIT;
  std::atomic<bool>            stopped{};
  uint64_t                     generation{};
};
} // namespace ocudu
