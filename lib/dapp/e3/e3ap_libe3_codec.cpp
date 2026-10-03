// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/e3ap_libe3_codec.h"
#include "ocudu/dapp/e3/e3ap_v2_codec.h"

using namespace ocudu::dapp::e3;

codec_result<std::vector<uint8_t>> e3ap_libe3_codec::encode(const e3ap_pdu& pdu) noexcept
{
  return e3ap_v2_codec::encode(pdu, e3ap_profile::libe3);
}

codec_result<e3ap_pdu> e3ap_libe3_codec::decode(ocudu::span<const uint8_t> bytes) noexcept
{
  return e3ap_v2_codec::decode(bytes, e3ap_profile::libe3);
}

bool e3ap_libe3_codec::representable(const e3ap_message& message, std::string* detail) noexcept
{
  return e3ap_v2_codec::representable(message, e3ap_profile::libe3, detail);
}
