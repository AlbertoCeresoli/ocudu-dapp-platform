// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Unit tests for PUSCH acceleration runtime decision helpers.

#include "pusch_acceleration_runtime_options.h"
#include <cstdlib>
#include <gtest/gtest.h>
#include <optional>
#include <string>

using namespace ocudu;

namespace {

class scoped_env_var
{
public:
  explicit scoped_env_var(const char* name_) : name(name_)
  {
    const char* value = std::getenv(name.c_str());
    if (value != nullptr) {
      original_value = value;
    }
  }

  ~scoped_env_var()
  {
    if (original_value.has_value()) {
      (void)setenv(name.c_str(), original_value->c_str(), 1);
    } else {
      (void)unsetenv(name.c_str());
    }
  }

  bool set(const char* value) { return setenv(name.c_str(), value, 1) == 0; }

  bool unset() { return unsetenv(name.c_str()) == 0; }

private:
  std::string                name;
  std::optional<std::string> original_value;
};

} // namespace

TEST(pusch_acceleration_runtime_options_test, device_uci_feature_flag_defaults_off_and_accepts_true_values)
{
  scoped_env_var device_uci("OCUDU_PUSCH_ENABLE_ACCELERATED_UCI");

  ASSERT_TRUE(device_uci.unset());
  EXPECT_FALSE(pusch_acceleration_device_uci_enabled());

  ASSERT_TRUE(device_uci.set("yes"));
  EXPECT_TRUE(pusch_acceleration_device_uci_enabled());

  ASSERT_TRUE(device_uci.set("off"));
  EXPECT_FALSE(pusch_acceleration_device_uci_enabled());
}
