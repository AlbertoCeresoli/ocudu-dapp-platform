// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/prepared_output.h"
#include <algorithm>
#include <cstring>

using namespace ocudu::dapp;

namespace {

thread_local prepared_output_transaction* active_transaction{};

} // namespace

void prepared_output_transaction::begin(const prepared_output_source& source_) noexcept
{
  abort();
  output_source = source_;
  is_active     = true;
}

ocudu_dapp_status_v1 prepared_output_transaction::stage(uint64_t               key,
                                                        void*                  destination,
                                                        prepared_output_commit commit_callback,
                                                        const void*            normalized_object,
                                                        uint32_t               object_size) noexcept
{
  if (!is_active || key == 0 || destination == nullptr || commit_callback == nullptr || normalized_object == nullptr ||
      object_size == 0 || object_size > maximum_record_bytes) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  if (std::any_of(records.begin(), records.begin() + count, [key](const record& item) { return item.key == key; })) {
    return OCUDU_DAPP_CONFLICT_V1;
  }
  if (count == records.size()) {
    return OCUDU_DAPP_RESOURCE_V1;
  }
  record& output     = records[count++];
  output.key         = key;
  output.destination = destination;
  output.commit      = commit_callback;
  output.size        = object_size;
  std::memcpy(output.bytes.data(), normalized_object, object_size);
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 prepared_output_transaction::commit() noexcept
{
  if (!is_active) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  is_active = false;
  for (uint32_t index = 0; index != count; ++index) {
    const record& item   = records[index];
    const auto    status = item.commit(item.destination, item.bytes.data(), item.size, output_source);
    if (status != OCUDU_DAPP_OK_V1) {
      count = 0;
      return status;
    }
  }
  count = 0;
  return OCUDU_DAPP_OK_V1;
}

void prepared_output_transaction::abort() noexcept
{
  count     = 0;
  is_active = false;
}

prepared_output_scope::prepared_output_scope(prepared_output_transaction*  transaction,
                                             const prepared_output_source& source) noexcept :
  current(transaction), previous(active_transaction)
{
  if (current != nullptr) {
    current->begin(source);
    active_transaction = current;
  }
}

prepared_output_scope::~prepared_output_scope()
{
  if (current != nullptr) {
    active_transaction = previous;
    if (current->active()) {
      current->abort();
    }
  }
}

const prepared_output_source* ocudu::dapp::current_prepared_output_source() noexcept
{
  return active_transaction != nullptr && active_transaction->active() ? &active_transaction->source() : nullptr;
}

ocudu_dapp_status_v1 ocudu::dapp::stage_prepared_output(uint64_t               key,
                                                        void*                  destination,
                                                        prepared_output_commit commit,
                                                        const void*            normalized_object,
                                                        uint32_t               object_size) noexcept
{
  return active_transaction != nullptr
             ? active_transaction->stage(key, destination, commit, normalized_object, object_size)
             : OCUDU_DAPP_CONFLICT_V1;
}
