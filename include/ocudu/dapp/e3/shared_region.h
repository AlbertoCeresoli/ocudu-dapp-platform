// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/dapp/e3/e3ap_protocol.h"
#include <cstdint>
#include <memory>
#include <string>
#include <sys/types.h>

namespace ocudu::dapp::e3 {

struct e3_shared_region_config {
  /// POSIX shared-memory name, starting with '/'.
  std::string name;
  /// Largest tensor one row can hold.
  uint64_t row_payload_bytes{};
  uint16_t nof_rows{4};
  /// File mode of the region; the DU's user owns it. 0640 lets a dApp in the
  /// DU's group map it read-only.
  mode_t mode{0640};
};

/// Where one written tensor landed.
struct e3_shared_region_write {
  uint16_t row_index{};
  uint64_t byte_offset{};
  uint64_t generation{};
  uint64_t byte_length{};
};

/// Writer side of a by-reference E3AP region (see shared_region_layout.h).
/// Single writer; rows are used round-robin. Reads never block the writer.
class e3_shared_region
{
public:
  static std::pair<std::unique_ptr<e3_shared_region>, std::string>
  create(const e3_shared_region_config& config) noexcept;

  ~e3_shared_region();
  e3_shared_region(const e3_shared_region&)            = delete;
  e3_shared_region& operator=(const e3_shared_region&) = delete;

  /// Copies one tensor into the next row and publishes it. Returns nothing when
  /// the tensor does not fit a row.
  std::optional<e3_shared_region_write> write(uint64_t                   sequence,
                                              uint64_t                   route_epoch,
                                              uint16_t                   telemetry_id,
                                              uint16_t                   cell_id,
                                              ocudu::span<const uint8_t> tensor) noexcept;

  /// Incremental write: \ref begin claims the next row for \c byte_length
  /// bytes (false when it does not fit), \ref append copies chunks straight
  /// into it as they arrive from the store, \ref commit publishes the row once
  /// every byte is in place, and \ref abort releases a partially written row.
  bool                                  begin(uint64_t sequence,
                                              uint64_t route_epoch,
                                              uint16_t telemetry_id,
                                              uint16_t cell_id,
                                              uint64_t byte_length) noexcept;
  bool                                  append(ocudu::span<const uint8_t> bytes) noexcept;
  std::optional<e3_shared_region_write> commit() noexcept;
  void                                  abort() noexcept;
  bool                                  writing() const noexcept { return in_progress; }

  /// The bytes of a committed row (valid until the row is reused).
  ocudu::span<const uint8_t> bytes_of(const e3_shared_region_write& written) const noexcept;

  /// Descriptor advertised in the subscription response.
  e3ap_shared_region descriptor() const noexcept;
  const std::string& name() const noexcept { return config.name; }

private:
  e3_shared_region(e3_shared_region_config config_, int fd_, void* base_, uint64_t bytes_) noexcept;

  e3_shared_region_config config;
  int                     fd{-1};
  uint8_t*                base{};
  uint64_t                bytes{};
  uint64_t                row_bytes{};
  uint16_t                next_row{};
  uint64_t                generation{};
  bool                    in_progress{};
  uint16_t                writing_row{};
  uint64_t                writing_length{};
  uint64_t                cursor{};
};

} // namespace ocudu::dapp::e3
