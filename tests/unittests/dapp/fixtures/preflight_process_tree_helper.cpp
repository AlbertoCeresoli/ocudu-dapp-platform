// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#ifndef DAPP_PREFLIGHT_PROCESS_TREE_PID_FILE
#error "DAPP_PREFLIGHT_PROCESS_TREE_PID_FILE is required"
#endif

int main()
{
  const pid_t descendant = ::fork();
  if (descendant < 0) {
    return 2;
  }
  if (descendant == 0) {
    while (true) {
      ::pause();
    }
  }

  FILE* output = std::fopen(DAPP_PREFLIGHT_PROCESS_TREE_PID_FILE, "w");
  if (output == nullptr) {
    return 3;
  }
  std::fprintf(output, "%ld\n", static_cast<long>(descendant));
  if (std::fclose(output) != 0) {
    return 4;
  }
  while (true) {
    ::pause();
  }
}
