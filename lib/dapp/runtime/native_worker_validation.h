// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Host-side Class-A/B/C input validation helpers used by native_worker.
// Internal to lib/dapp — not part of the public runtime ABI.

#pragma once

#include "ocudu/dapp/abi/v1/module.h"
#include "ocudu/dapp/runtime/instance_runtime.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ocudu::dapp::native_worker_detail {

constexpr std::size_t max_worker_sources = 1024;

bool valid_invocation(const ocudu_dapp_invocation_v1& invocation, uint32_t worker_id) noexcept;
bool valid_input_tensor(const ocudu_dapp_tensor_view_v1& tensor) noexcept;
bool valid_output_tensor(const ocudu_dapp_tensor_view_v1& tensor) noexcept;
bool valid_ul_resource_plan(const ocudu_dapp_ul_resource_plan_v1& plan) noexcept;
bool spectrum_plan_generation(const ocudu_dapp_spectrum_input_v1& input, uint64_t& generation) noexcept;
bool srs_source_metadata(const ocudu_dapp_srs_isac_input_v1& input, uint32_t& resource_id,
                         uint32_t& config_id) noexcept;
bool valid_scheduler_interference(const ocudu_dapp_scheduler_input_v1& input) noexcept;
bool valid_scheduler_free_vrb(const ocudu_dapp_scheduler_input_v1& input) noexcept;
bool valid_interface_callback(const ocudu_dapp_interface_descriptor_v1& descriptor) noexcept;

std::optional<class_a_fallback_reason>
classify_unsupported_pusch(uint32_t                            interface_id,
                           const ocudu_dapp_pusch_profile_v1&  profile,
                           const ocudu_dapp_typed_metadata_v1& pusch_wrapper,
                           const ocudu_dapp_typed_metadata_v1& dmrs_wrapper) noexcept;

/// Checks an SRS channel-estimator invocation: a malformed request is invalid_input, a well-formed occasion
/// outside the module's declared profile is the matching unsupported_* reason.
std::optional<class_a_fallback_reason>
classify_srs_channel_estimator_input(const ocudu_dapp_srs_profile_v1&                  profile,
                                     const ocudu_dapp_srs_channel_estimator_input_v1&  input,
                                     const ocudu_dapp_srs_channel_estimator_output_v1& output) noexcept;

class_a_fallback_reason status_reason(ocudu_dapp_status_v1 status) noexcept;

uint64_t                         steady_now_ns() noexcept;
const ocudu_dapp_worker_api_v1*  get_worker_api(const ocudu_dapp_interface_descriptor_v1& descriptor) noexcept;

} // namespace ocudu::dapp::native_worker_detail
