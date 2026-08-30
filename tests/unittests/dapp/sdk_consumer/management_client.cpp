// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/local_client.h"
#include <chrono>

int main()
{
  ocudu::dapp::e3::inventory_query query;
  query.header.transaction_id.bytes.back() = 1;
  ocudu::dapp::e3::local_management_client client("/tmp/ocudu-sdk-consumer-no-server.sock");
  return client.call(query, std::chrono::milliseconds{1}) ? 1 : 0;
}
