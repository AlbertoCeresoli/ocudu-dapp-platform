// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "pusch_test_cold_trace.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using ocudu::test_support::pusch_cold_trace;

class PuschTestColdTrace : public ::testing::Test
{
protected:
  void SetUp() override
  {
    char        name[] = "/tmp/ocudu-pusch-cold-XXXXXX";
    const auto* result = mkdtemp(name);
    ASSERT_NE(result, nullptr);
    directory = result;
    path      = directory / "trace.jsonl";
  }
  void TearDown() override
  {
    if (!directory.empty())
      std::filesystem::remove_all(directory);
  }
  std::vector<std::string> read() const
  {
    std::ifstream            stream(path);
    std::vector<std::string> rows;
    for (std::string line; std::getline(stream, line);)
      rows.push_back(line);
    return rows;
  }
  std::filesystem::path directory, path;
};

TEST_F(PuschTestColdTrace, FlushesEnteredPhaseBeforeWorkAndPreservesUniqueResults)
{
  pusch_cold_trace trace(path.string());
  auto             value = trace.measure("prepare", [&] {
    const auto rows = read();
    EXPECT_EQ(rows.size(), 1);
    if (!rows.empty()) {
      EXPECT_NE(rows[0].find("\"boundary\":\"begin\""), std::string::npos);
    }
    return std::make_unique<int>(7);
  });
  EXPECT_EQ(*value, 7);
  trace.record("process", "complete");
  const auto rows = read();
  ASSERT_EQ(rows.size(), 3);
  for (unsigned i = 0; i != rows.size(); ++i) {
    EXPECT_NE(rows[i].find("\"sequence\":" + std::to_string(i)), std::string::npos);
    EXPECT_NE(rows[i].find("\"epoch_s\":"), std::string::npos);
    EXPECT_NE(rows[i].find("\"elapsed_ns\":"), std::string::npos);
  }
  EXPECT_NE(rows[1].find("\"boundary\":\"end\""), std::string::npos);
}

TEST_F(PuschTestColdTrace, FailureDoesNotLookLikeSuccessfulCompletion)
{
  pusch_cold_trace trace(path.string());
  EXPECT_THROW(trace.measure("prepare", [] { throw std::runtime_error("injected"); }), std::runtime_error);
  const auto rows = read();
  ASSERT_EQ(rows.size(), 2);
  EXPECT_NE(rows[1].find("\"boundary\":\"failed\""), std::string::npos);
  EXPECT_EQ(rows[1].find("\"boundary\":\"end\""), std::string::npos);
}

TEST_F(PuschTestColdTrace, ExistingEvidenceIsNeverOverwritten)
{
  std::ofstream(path) << "preserve";
  EXPECT_THROW(pusch_cold_trace trace(path.string()), std::runtime_error);
  ASSERT_EQ(read(), std::vector<std::string>{"preserve"});
  EXPECT_THROW(pusch_cold_trace trace((directory / "missing" / "trace").string()), std::runtime_error);
}

TEST(PuschTestColdTraceDisabled, CallsWorkAndPreservesExceptionsWithoutOpeningFiles)
{
  pusch_cold_trace trace("");
  EXPECT_EQ(trace.measure("work", [] { return 42; }), 42);
  EXPECT_THROW(trace.measure("work", [] { throw std::runtime_error("injected"); }), std::runtime_error);
  trace.record("process", "complete");
}
