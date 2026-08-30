// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/abi/v1/base.h"
#include "ocudu/dapp/ipc/publication_route.h"

namespace ocudu::dapp {

/// Cumulative route-local accounting. Tensor copy counters exclude metadata,
/// headers and padding and advance only when a publication is committed.
struct class_c_process_endpoint_metrics {
  /// Bit N is set after a data publication from cell N commits. Tensorless plan routes do not contribute.
  uint32_t observed_cell_bitmap{};
  uint64_t publications{};
  uint64_t payload_bytes{};
  uint64_t backpressure_drops{};
  uint64_t invalid_drops{};
  uint64_t encode_failures{};
  uint64_t notification_failures{};
  uint64_t inactive_bypasses{};
  uint64_t concurrent_drops{};
  uint64_t snapshot_copy_operations{};
  uint64_t snapshot_copy_bytes{};
  uint64_t lease_operations{};
  uint64_t export_copy_operations{};
  uint64_t export_copy_bytes{};
  uint64_t export_slots_capacity{};
  uint64_t export_slots_in_use{};
  uint64_t export_slots_high_watermark{};
  uint64_t export_slot_reservation_failures{};
  uint64_t e3_payload_copy_operations{};
  uint64_t e3_payload_copy_bytes{};
};

/// Host-owned publication endpoint controlled as part of one isolated Class-C process generation.
class class_c_process_endpoint
{
public:
  virtual ~class_c_process_endpoint() = default;

  virtual uint32_t                         interface_id() const noexcept           = 0;
  virtual ipc::route_capabilities          export_capabilities() const noexcept    = 0;
  virtual void                             set_active(bool active) noexcept        = 0;
  virtual bool                             reset_after_consumer_restart() noexcept = 0;
  virtual class_c_process_endpoint_metrics snapshot_metrics() const noexcept { return {}; }

  /// Consumes one recently published source admission. Successful matches are single-use.
  virtual bool consume_result_source(uint64_t                        source_sequence,
                                     uint64_t                        source_plan_generation,
                                     uint32_t                        cell_id,
                                     const ocudu_dapp_radio_time_v1& created) noexcept
  {
    return false;
  }

  /// Consumes the quiet-intent result privilege for one admitted spectrum source.
  virtual bool consume_quiet_intent_source(uint64_t                        source_sequence,
                                           uint64_t                        source_plan_generation,
                                           uint32_t                        cell_id,
                                           const ocudu_dapp_radio_time_v1& created) noexcept
  {
    return false;
  }

  /// Consumes one SRS source admission with the exact resource/configuration identity.
  virtual bool consume_srs_result_source(uint64_t                        source_sequence,
                                         uint32_t                        srs_resource_id,
                                         uint32_t                        srs_config_id,
                                         uint32_t                        cell_id,
                                         const ocudu_dapp_radio_time_v1& created) noexcept
  {
    return false;
  }
};

} // namespace ocudu::dapp
