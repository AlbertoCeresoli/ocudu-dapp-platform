// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/lifecycle.h"

using namespace ocudu::dapp;

bool ocudu::dapp::is_legal_lifecycle_transition(ocudu_dapp_lifecycle_state_v1 from,
                                                ocudu_dapp_lifecycle_state_v1 to) noexcept
{
  if (to == OCUDU_DAPP_STATE_ERROR_V1) {
    return from != OCUDU_DAPP_STATE_UNLOADED_V1;
  }
  switch (from) {
    case OCUDU_DAPP_STATE_DISCOVERED_V1:
      return to == OCUDU_DAPP_STATE_LOADED_V1;
    case OCUDU_DAPP_STATE_LOADED_V1:
      return to == OCUDU_DAPP_STATE_PREPARED_V1 || to == OCUDU_DAPP_STATE_UNLOADED_V1;
    case OCUDU_DAPP_STATE_PREPARED_V1:
      return to == OCUDU_DAPP_STATE_ARMED_V1 || to == OCUDU_DAPP_STATE_INACTIVE_V1;
    case OCUDU_DAPP_STATE_ARMED_V1:
      return to == OCUDU_DAPP_STATE_SHADOW_V1 || to == OCUDU_DAPP_STATE_INACTIVE_V1;
    case OCUDU_DAPP_STATE_SHADOW_V1:
      return to == OCUDU_DAPP_STATE_ACTIVE_V1 || to == OCUDU_DAPP_STATE_DRAINING_V1;
    case OCUDU_DAPP_STATE_ACTIVE_V1:
      return to == OCUDU_DAPP_STATE_DRAINING_V1 || to == OCUDU_DAPP_STATE_DEGRADED_V1;
    case OCUDU_DAPP_STATE_DRAINING_V1:
      return to == OCUDU_DAPP_STATE_INACTIVE_V1 || to == OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1;
    case OCUDU_DAPP_STATE_INACTIVE_V1:
      return to == OCUDU_DAPP_STATE_UNLOADED_V1 || to == OCUDU_DAPP_STATE_ARMED_V1 ||
             to == OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1;
    case OCUDU_DAPP_STATE_DEGRADED_V1:
      return to == OCUDU_DAPP_STATE_DRAINING_V1 || to == OCUDU_DAPP_STATE_INACTIVE_V1;
    case OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1:
      return to == OCUDU_DAPP_STATE_INACTIVE_V1;
    case OCUDU_DAPP_STATE_ERROR_V1:
      return to == OCUDU_DAPP_STATE_DRAINING_V1 || to == OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1;
    case OCUDU_DAPP_STATE_UNLOADED_V1:
      return false;
  }
  return false;
}

bool lifecycle::transition_to(ocudu_dapp_lifecycle_state_v1 next) noexcept
{
  if (!is_legal_lifecycle_transition(value, next)) {
    return false;
  }
  value = next;
  return true;
}
