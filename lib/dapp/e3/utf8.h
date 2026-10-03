// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <cstddef>
#include <cstdint>

namespace ocudu::dapp::e3 {

/// Well-formed UTF-8 (RFC 3629: no overlongs, surrogates or code points above U+10FFFF).
inline bool valid_utf8(const uint8_t* data, std::size_t size) noexcept
{
  std::size_t index = 0;
  while (index != size) {
    const uint8_t first = data[index++];
    if (first < 0x80) {
      continue;
    }
    unsigned continuation = 0;
    uint8_t  lower        = 0x80;
    uint8_t  upper        = 0xbf;
    if (first >= 0xc2 && first <= 0xdf) {
      continuation = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuation = 2;
      lower        = first == 0xe0 ? 0xa0 : 0x80;
      upper        = first == 0xed ? 0x9f : 0xbf;
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuation = 3;
      lower        = first == 0xf0 ? 0x90 : 0x80;
      upper        = first == 0xf4 ? 0x8f : 0xbf;
    } else {
      return false;
    }
    if (size - index < continuation || data[index] < lower || data[index] > upper) {
      return false;
    }
    ++index;
    for (unsigned item = 1; item != continuation; ++item, ++index) {
      if (data[index] < 0x80 || data[index] > 0xbf) {
        return false;
      }
    }
  }
  return true;
}

} // namespace ocudu::dapp::e3
