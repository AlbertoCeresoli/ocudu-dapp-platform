// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include <memory>
#include <string>

namespace ocudu::test_support {
/// Standalone test only. Copies perturb timing; captured runs are not latency qualifications.
/// One serialized SISO, four-PRB, full-slot type-1 grant at a time. No live telemetry hook.
class pusch_test_receiver_capture
{
public:
  pusch_test_receiver_capture(const std::string& prefix, unsigned qm, unsigned mcs, bool neural);
  ~pusch_test_receiver_capture();
  pusch_test_receiver_capture(const pusch_test_receiver_capture&) = delete;
  pusch_test_receiver_capture& operator=(const pusch_test_receiver_capture&) = delete;
  void begin(unsigned frame);
  /// Call only after the real resident decoder AND processor resource lease have completed.
  void finish(span<const uint8_t> payload, span<const cbf16_t> tx, span<const cbf16_t> rx);

private:
  struct implementation;
  std::unique_ptr<implementation> impl;
};
} // namespace ocudu::test_support
