// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Optional GPU channel-emulation hook for the split-8 RU boundary.
///
/// When the environment configures a channel-emulation plugin
/// (OCUDU_CHANNEL_EMULATOR_LIB + OCUDU_CHANNEL_EMULATOR_ARGS), every uplink
/// symbol notified by the lower PHY and every downlink slot grid handed to
/// the lower PHY is multiplied in place, in the frequency domain and on the
/// GPU, by a simulated 3GPP channel realization (UMa/UMi/RMa/TDL/CDL). The
/// plugin owns all channel math and CUDA resources; this hook only resolves
/// the grid's device pointer, fences the plugin's stream against the grid's
/// producers/consumers, and enqueues the apply. With the environment unset
/// the hook is a no-op costing one atomic load per call.

#pragma once

#include "ocudu/phy/support/prach_buffer_context.h"
#include "ocudu/phy/support/shared_resource_grid.h"
#include "ocudu/ran/slot_point.h"
#include <atomic>

namespace ocudu {

class prach_buffer;

/// Process-wide channel-emulation service. Thread-compatible: UL and DL
/// paths may call concurrently.
class channel_emulator_hook
{
public:
  /// Returns the singleton, initializing it from the environment on first use.
  static channel_emulator_hook& get();

  /// Returns true when a plugin is loaded and active.
  bool is_enabled() const { return enabled.load(std::memory_order_relaxed); }

  /// Applies the current channel realization to the uplink grid symbols
  /// received so far. \c last_symbol is the INCLUSIVE index of the last
  /// symbol the lower PHY demodulated (the RU notification convention); the
  /// hook tracks per-slot progress and multiplies only the not-yet-applied
  /// range. Enqueue-only; never blocks the caller.
  void apply_uplink(const shared_resource_grid& grid, slot_point slot, unsigned last_symbol);

  /// Applies the current channel realization to a complete downlink slot grid.
  void apply_downlink(const shared_resource_grid& grid, slot_point slot);

  /// Applies the current channel realization to a demodulated PRACH window
  /// (all ports/occasions/symbols), so random access experiences the same
  /// fading as every other signal. Enqueue-only.
  void apply_prach(prach_buffer& buffer, const prach_buffer_context& context);

private:
  channel_emulator_hook();
  ~channel_emulator_hook();

  void apply(const shared_resource_grid& grid,
             slot_point                  slot,
             unsigned                    first_symbol,
             unsigned                    end_symbol,
             int                         direction);

  struct impl;
  impl*                 pimpl = nullptr;
  std::atomic<bool>     enabled{false};
};

} // namespace ocudu
