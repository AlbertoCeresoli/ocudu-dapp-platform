// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/management/types.h"
#include <cstring>
#include <type_traits>

using namespace ocudu::dapp::management;

namespace {

uint64_t append(uint64_t hash, const void* data, std::size_t size) noexcept
{
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (std::size_t i = 0; i != size; ++i) {
    hash = (hash ^ bytes[i]) * 1099511628211ULL;
  }
  return hash;
}

template <typename T>
uint64_t append(uint64_t hash, const T& value) noexcept
{
  return append(hash, &value, sizeof(value));
}

uint64_t append(uint64_t hash, const std::string& value) noexcept
{
  hash = append(hash, value.size());
  return append(hash, value.data(), value.size());
}

uint64_t append(uint64_t hash, const value& managed) noexcept
{
  hash = append(hash, managed.index());
  return std::visit(
      [hash](const auto& item) {
        using item_type = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<item_type, std::string> || std::is_same_v<item_type, std::vector<uint8_t>>) {
          return append(append(hash, item.size()), item.data(), item.size());
        } else {
          return append(hash, item);
        }
      },
      managed);
}

template <typename Request>
uint64_t common_fingerprint(const Request& request) noexcept
{
  uint64_t hash = append(1469598103934665603ULL, request.instance_id.bytes.data(), request.instance_id.bytes.size());
  hash          = append(hash, request.dry_run);
  hash          = append(hash, request.payload_fingerprint);
  const bool has_generation = request.expected_instance_generation.has_value();
  hash                      = append(hash, has_generation);
  return has_generation ? append(hash, *request.expected_instance_generation) : hash;
}

} // namespace

bool identifier::is_zero() const noexcept
{
  uint8_t combined = 0;
  for (uint8_t value : bytes) {
    combined |= value;
  }
  return combined == 0;
}

std::size_t identifier_hash::operator()(const identifier& value) const noexcept
{
  uint64_t first  = 0;
  uint64_t second = 0;
  std::memcpy(&first, value.bytes.data(), sizeof(first));
  std::memcpy(&second, value.bytes.data() + sizeof(first), sizeof(second));
  first ^= second + 0x9e3779b97f4a7c15ULL + (first << 6U) + (first >> 2U);
  return static_cast<std::size_t>(first);
}

uint64_t ocudu::dapp::management::canonical_fingerprint(const load_request& request) noexcept
{
  uint64_t hash = append(1469598103934665603ULL, request.package_id.bytes.data(), request.package_id.bytes.size());
  hash          = append(hash, request.placement);
  hash          = append(hash, request.selected_backend);
  hash          = append(hash, request.dry_run);
  hash          = append(hash, request.payload_fingerprint);
  const bool has_generation = request.expected_inventory_generation.has_value();
  hash                      = append(hash, has_generation);
  return has_generation ? append(hash, *request.expected_inventory_generation) : hash;
}

uint64_t ocudu::dapp::management::canonical_fingerprint(const catalog_admit_request& request) noexcept
{
  constexpr char domain[]   = "catalog.admit.v1";
  uint64_t       hash       = append(1469598103934665603ULL, domain, sizeof(domain));
  hash                      = append(hash, request.package_id.bytes.data(), request.package_id.bytes.size());
  hash                      = append(hash, request.dry_run);
  const bool has_generation = request.expected_inventory_generation.has_value();
  hash                      = append(hash, has_generation);
  return has_generation ? append(hash, *request.expected_inventory_generation) : hash;
}

uint64_t ocudu::dapp::management::canonical_fingerprint(const lifecycle_request& request) noexcept
{
  uint64_t hash = append(1469598103934665603ULL, request.instance_id.bytes.data(), request.instance_id.bytes.size());
  hash          = append(hash, request.action);
  hash          = append(hash, request.dry_run);
  hash          = append(hash, request.timeout_us);
  hash          = append(hash, request.payload_fingerprint);
  const bool has_generation = request.expected_generation.has_value();
  hash                      = append(hash, has_generation);
  return has_generation ? append(hash, *request.expected_generation) : hash;
}

uint64_t ocudu::dapp::management::canonical_fingerprint(const config_set_request& request) noexcept
{
  uint64_t hash = common_fingerprint(request);
  hash          = append(hash, request.expected_config_generation);
  hash          = append(hash, request.boundary);
  for (const auto& entry : request.entries) {
    hash = append(append(hash, entry.key), entry.entry_value);
  }
  return hash;
}

uint64_t ocudu::dapp::management::canonical_fingerprint(const model_request& request) noexcept
{
  uint64_t hash = common_fingerprint(request);
  hash          = append(hash, request.expected_model_generation);
  hash          = append(hash, request.action);
  hash          = append(hash, request.model_id);
  hash          = append(hash, request.model_version);
  return append(append(hash, request.artifact.size()), request.artifact.data(), request.artifact.size());
}

const char* ocudu::dapp::management::to_string(result_code code) noexcept
{
  static constexpr const char* names[] = {"ok",
                                          "dry run ok",
                                          "accepted",
                                          "not found",
                                          "generation conflict",
                                          "invalid request",
                                          "incompatible",
                                          "verification failed",
                                          "load failed",
                                          "lifecycle failed",
                                          "transaction mismatch",
                                          "resource exhausted"};
  const auto                   index   = static_cast<unsigned>(code);
  return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown management result";
}
