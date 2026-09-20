// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

// Private GNU/Itanium ABI linker wrappers, compiled only into the isolated
// pipeline test (and its CPU fake test). A symbol drift must fail the link.
// These names are NOT a public receiver ABI or production instrumentation.
#define PUSCH_TEST_INSPECT_SYMBOL                                                                                      \
  "_ZN5ocudu4dapp13native_worker16inspect_receiverERKNS0_22native_receiver_ticketER40ocudu_dapp_receiver_completion_"  \
  "result_v1"
#define PUSCH_TEST_RELEASE_SYMBOL "_ZN5ocudu4dapp13native_worker16release_receiverERKNS0_22native_receiver_ticketE"

namespace ocudu::test_support {
void enable_receiver_retirement_trace(bool enabled) noexcept;
}
