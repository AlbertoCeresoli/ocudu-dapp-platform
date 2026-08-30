// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <optional>
#include <string>

namespace ocudu::dapp::e3 {

template <typename T>
struct codec_result {
  std::optional<T> value;
  std::string      detail;
  explicit         operator bool() const noexcept { return value.has_value(); }
};

} // namespace ocudu::dapp::e3
