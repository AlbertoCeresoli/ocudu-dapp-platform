# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


add_executable(scheduler_policy_test scheduler_policy_test.cpp)
target_link_libraries(scheduler_policy_test
        ocudu_sched
        scheduler_test_suite
        sched_test_doubles
        ocudulog
        gtest
        ocudu_gtest_main)
gtest_discover_tests(scheduler_policy_test)

if (ENABLE_DAPP)
  add_executable(scheduler_dapp_policy_test
          quiet_reservation_test.cpp
          scheduler_dapp_context_test.cpp
          scheduler_dapp_policy_test.cpp)
  target_link_libraries(scheduler_dapp_policy_test
          ocudu_sched
          scheduler_test_suite
          scheduler_test_utils
          sched_test_doubles
          ocudulog
          gtest
          ocudu_gtest_main)
  gtest_discover_tests(scheduler_dapp_policy_test PROPERTIES LABELS "tsan")
endif ()
