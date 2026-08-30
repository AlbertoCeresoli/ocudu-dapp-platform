// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/abi/v1/execution.h"
#include "ocudu/dapp/management/incidents.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace ocudu::dapp {

/// Host-authenticated source for a synchronous prepared-output transaction.
struct prepared_output_source {
  incident_context         identity;
  ocudu_dapp_invocation_v1 invocation;
  uint64_t                 input_plan_generation{};
  uint32_t                 input_srs_resource_id{};
  uint32_t                 input_srs_config_id{};
};

using prepared_output_commit = ocudu_dapp_status_v1 (*)(void*                         destination,
                                                        const void*                   normalized_object,
                                                        uint32_t                      object_size,
                                                        const prepared_output_source& source) noexcept;

/// Fixed-capacity staging transaction. Module output is committed only after its callback validates and meets deadline.
class prepared_output_transaction
{
public:
  static constexpr uint32_t maximum_records      = 4;
  static constexpr uint32_t maximum_record_bytes = 1024;

  prepared_output_transaction() noexcept = default;

  void                          begin(const prepared_output_source& source_) noexcept;
  ocudu_dapp_status_v1          stage(uint64_t               key,
                                      void*                  destination,
                                      prepared_output_commit commit,
                                      const void*            normalized_object,
                                      uint32_t               object_size) noexcept;
  ocudu_dapp_status_v1          commit() noexcept;
  void                          abort() noexcept;
  bool                          active() const noexcept { return is_active; }
  const prepared_output_source& source() const noexcept { return output_source; }

private:
  struct record {
    uint64_t               key{};
    void*                  destination{};
    prepared_output_commit commit{};
    uint32_t               size{};
    alignas(std::max_align_t) std::array<std::byte, maximum_record_bytes> bytes{};
  };

  prepared_output_source              output_source{};
  std::array<record, maximum_records> records{};
  uint32_t                            count{};
  bool                                is_active{};
};

/// Makes one transaction visible only on the current callback thread.
class prepared_output_scope
{
public:
  prepared_output_scope(prepared_output_transaction* transaction, const prepared_output_source& source) noexcept;
  ~prepared_output_scope();
  prepared_output_scope(const prepared_output_scope&)            = delete;
  prepared_output_scope& operator=(const prepared_output_scope&) = delete;

private:
  prepared_output_transaction* current{};
  prepared_output_transaction* previous{};
};

/// Returns null outside a guarded native invocation.
const prepared_output_source* current_prepared_output_source() noexcept;

/// Stages one normalized host-owned record in the current invocation transaction.
ocudu_dapp_status_v1 stage_prepared_output(uint64_t               key,
                                           void*                  destination,
                                           prepared_output_commit commit,
                                           const void*            normalized_object,
                                           uint32_t               object_size) noexcept;

} // namespace ocudu::dapp
