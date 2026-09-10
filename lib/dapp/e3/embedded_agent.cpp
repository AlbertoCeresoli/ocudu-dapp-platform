// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/embedded_agent.h"

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;
using namespace ocudu::dapp::management;

job_submit_result embedded_agent::submit(catalog_admit_request request)
{
  const auto fingerprint = canonical_fingerprint(request);
  const auto transaction = request.transaction_id;
  return jobs.submit(
      transaction, fingerprint, [this, request = std::move(request)] { return instance_owner.admit_package(request); });
}

job_submit_result embedded_agent::submit(load_request request)
{
  if (request.create_args != nullptr) {
    return {job_submit_code::invalid_request, std::nullopt, "E3 load cannot carry a native pointer"};
  }
  const auto fingerprint = canonical_fingerprint(request);
  const auto transaction = request.transaction_id;
  return jobs.submit(
      transaction, fingerprint, [this, request = std::move(request)] { return instance_owner.load(request); });
}

job_submit_result embedded_agent::submit(lifecycle_request request)
{
  if (request.primary_payload != nullptr || request.secondary_payload != nullptr) {
    return {job_submit_code::invalid_request, std::nullopt, "E3 lifecycle cannot carry a native pointer"};
  }
  const auto fingerprint = canonical_fingerprint(request);
  const auto transaction = request.transaction_id;
  return jobs.submit(
      transaction, fingerprint, [this, request = std::move(request)] { return instance_owner.control(request); });
}

job_submit_result embedded_agent::submit(config_set_request request)
{
  const auto fingerprint = canonical_fingerprint(request);
  const auto transaction = request.transaction_id;
  return jobs.submit(
      transaction, fingerprint, [this, request = std::move(request)] { return instance_owner.set_config(request); });
}

job_submit_result embedded_agent::submit(model_request request)
{
  const auto fingerprint = canonical_fingerprint(request);
  const auto transaction = request.transaction_id;
  return jobs.submit(
      transaction, fingerprint, [this, request = std::move(request)] { return instance_owner.manage_model(request); });
}
