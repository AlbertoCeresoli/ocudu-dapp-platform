// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include <cstdint>
#include <memory>
#include <string>

namespace ocudu::dapp::ipc {

struct class_c_identity {
  uint64_t low{};
  uint64_t high{};
  explicit operator bool() const noexcept { return low != 0 || high != 0; }
};

enum class class_c_host_object_profile : uint8_t { posix_shared, embedded_payload };

struct class_c_encoder_config {
  class_c_identity            producer_instance;
  class_c_identity            ran_node;
  std::string                 clock_domain_id{"steady-clock"};
  uint64_t                    lease_duration_ns{100'000'000};
  uint32_t                    metadata_capacity{8192};
  class_c_host_object_profile host_object_profile{class_c_host_object_profile::posix_shared};
  /// Carrier centre of the observed cell, written to ObservationContext.reference_frequency_hz
  /// so out-of-process consumers can place PRBs in absolute frequency. Zero when unknown.
  uint64_t reference_frequency_hz{0};
};

enum class class_c_encode_error : uint8_t {
  none,
  invalid_config,
  invalid_input,
  unsupported_tensor,
  metadata_too_large,
  internal_error
};

struct class_c_encode_result {
  class_c_encode_error       error{class_c_encode_error::internal_error};
  ocudu::span<const uint8_t> metadata;
  explicit operator bool() const noexcept { return error == class_c_encode_error::none && !metadata.empty(); }
};

class class_c_publication_encoder;

struct class_c_encoder_create_result {
  std::unique_ptr<class_c_publication_encoder> encoder;
  std::string                                  detail;
  explicit                                     operator bool() const noexcept { return encoder != nullptr; }
};

/// Reusable, pre-sized FlatBuffer encoder for one Class-C producer route.
class class_c_publication_encoder
{
public:
  static class_c_encoder_create_result create(class_c_encoder_config config) noexcept;

  ~class_c_publication_encoder();
  class_c_publication_encoder(class_c_publication_encoder&&) noexcept;
  class_c_publication_encoder& operator=(class_c_publication_encoder&&) noexcept;
  class_c_publication_encoder(const class_c_publication_encoder&)            = delete;
  class_c_publication_encoder& operator=(const class_c_publication_encoder&) = delete;

  class_c_encode_result
  encode(const ocudu_dapp_spectrum_input_v1& input, uint64_t route_epoch, uint64_t observation_time_ns) noexcept;
  class_c_encode_result
  encode(const ocudu_dapp_srs_isac_input_v1& input, uint64_t route_epoch, uint64_t observation_time_ns) noexcept;
  class_c_encode_result
  encode(const ocudu_dapp_ul_resource_plan_v1& plan, uint64_t route_epoch, uint64_t observation_time_ns) noexcept;

private:
  struct impl;
  explicit class_c_publication_encoder(std::unique_ptr<impl> state_);
  std::unique_ptr<impl> state;
};

const char* to_string(class_c_encode_error error) noexcept;

} // namespace ocudu::dapp::ipc
