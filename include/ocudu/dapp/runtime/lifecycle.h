// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/abi/v1/base.h"

namespace ocudu::dapp {

bool is_legal_lifecycle_transition(ocudu_dapp_lifecycle_state_v1 from, ocudu_dapp_lifecycle_state_v1 to) noexcept;

/// Non-throwing lifecycle state holder. The owner serializes management-plane calls.
class lifecycle
{
public:
  explicit lifecycle(ocudu_dapp_lifecycle_state_v1 initial = OCUDU_DAPP_STATE_DISCOVERED_V1) noexcept : value(initial)
  {
  }

  ocudu_dapp_lifecycle_state_v1 state() const noexcept { return value; }
  bool                          transition_to(ocudu_dapp_lifecycle_state_v1 next) noexcept;
  void                          fail() noexcept { value = OCUDU_DAPP_STATE_ERROR_V1; }

private:
  ocudu_dapp_lifecycle_state_v1 value;
};

} // namespace ocudu::dapp
