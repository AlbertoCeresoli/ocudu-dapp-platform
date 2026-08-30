# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

if (ENABLE_E3 AND ENABLE_E3_ASN1_CODEC)
  add_subdirectory(dappctl)
endif ()

if (ENABLE_DAPP)
  add_subdirectory(dapp_preflight)
  add_subdirectory(pusch_replay)
  if (NOT DISABLE_MBEDTLS)
    add_subdirectory(dapp_package)
  endif ()
endif ()
