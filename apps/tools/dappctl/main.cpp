// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/local_client.h"
#include "CLI/CLI11.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/random.h>
#include <type_traits>
#include <unistd.h>

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;
using namespace ocudu::dapp::management;

namespace {

constexpr std::size_t maximum_artifact_size = 1024 * 1024;

struct optional_generation {
  uint64_t     value{};
  CLI::Option* option{};

  std::optional<uint64_t> get() const
  {
    return option != nullptr && option->count() != 0 ? std::optional{value} : std::nullopt;
  }
};

struct instance_options {
  std::string         id;
  optional_generation expected_generation;
  bool                dry_run{};
};

identifier parse_identifier(std::string_view text)
{
  identifier  result;
  std::size_t index = 0;
  int         high  = -1;
  for (char character : text) {
    if (character == '-') {
      continue;
    }
    unsigned value = 0;
    if (character >= '0' && character <= '9') {
      value = static_cast<unsigned>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      value = static_cast<unsigned>(character - 'a' + 10);
    } else if (character >= 'A' && character <= 'F') {
      value = static_cast<unsigned>(character - 'A' + 10);
    } else {
      throw std::invalid_argument("identifier contains a non-hexadecimal character");
    }
    if (high < 0) {
      high = static_cast<int>(value);
    } else {
      if (index == result.bytes.size()) {
        throw std::invalid_argument("identifier must contain exactly 32 hexadecimal digits");
      }
      result.bytes[index++] = static_cast<uint8_t>((static_cast<unsigned>(high) << 4U) | value);
      high                  = -1;
    }
  }
  if (index != result.bytes.size() || high >= 0) {
    throw std::invalid_argument("identifier must contain exactly 32 hexadecimal digits");
  }
  return result;
}

identifier new_transaction_id() noexcept
{
  identifier  result;
  std::size_t offset = 0;
  while (offset != result.bytes.size()) {
    const ssize_t count = ::getrandom(result.bytes.data() + offset, result.bytes.size() - offset, 0);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    break;
  }
  if (offset != result.bytes.size()) {
    static std::atomic<uint64_t> sequence{0};
    uint64_t seed = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                    (static_cast<uint64_t>(::getpid()) << 32U) ^ sequence.fetch_add(1, std::memory_order_relaxed);
    for (uint8_t& byte : result.bytes) {
      seed ^= seed << 13U;
      seed ^= seed >> 7U;
      seed ^= seed << 17U;
      byte = static_cast<uint8_t>(seed);
    }
  }
  if (result.is_zero()) {
    result.bytes.back() = 1;
  }
  return result;
}

std::string hex_bytes(const uint8_t* bytes, std::size_t size, std::size_t maximum = 64)
{
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  const std::size_t shown = std::min(size, maximum);
  for (std::size_t index = 0; index != shown; ++index) {
    output << std::setw(2) << static_cast<unsigned>(bytes[index]);
  }
  if (shown != size) {
    output << "...(" << std::dec << size << " bytes)";
  }
  return output.str();
}

std::string identifier_text(const identifier& value)
{
  const std::string compact = hex_bytes(value.bytes.data(), value.bytes.size());
  return compact.substr(0, 8) + '-' + compact.substr(8, 4) + '-' + compact.substr(12, 4) + '-' + compact.substr(16, 4) +
         '-' + compact.substr(20);
}

/// ``primary``, ``conventional`` or an instance identifier.
pusch_route_target parse_route_target(std::string_view text)
{
  if (text == "primary") {
    return {};
  }
  if (text == "conventional") {
    return {pusch_route_target::kind::conventional, {}};
  }
  return {pusch_route_target::kind::instance, parse_identifier(text)};
}

/// ``RNTI[@CELL]=TARGET``; RNTI and cell accept 0x-prefixed hexadecimal or decimal.
pusch_route_entry parse_route_entry(const std::string& text)
{
  const auto separator = text.find('=');
  if (separator == std::string::npos) {
    throw std::invalid_argument("route entries are RNTI[@CELL]=TARGET");
  }
  const std::string   ue   = text.substr(0, separator);
  const auto          cell = ue.find('@');
  pusch_route_entry   entry;
  const unsigned long rnti = std::stoul(ue.substr(0, cell), nullptr, 0);
  if (rnti == 0 || rnti > 0xFFEF) {
    throw std::invalid_argument("RNTI must be in 1..0xFFEF");
  }
  entry.rnti = static_cast<uint16_t>(rnti);
  if (cell != std::string::npos) {
    const unsigned long index = std::stoul(ue.substr(cell + 1), nullptr, 0);
    if (index > 0xFFFE) {
      throw std::invalid_argument("cell index must be in 0..65534");
    }
    entry.cell_index = static_cast<uint16_t>(index);
  }
  entry.target = parse_route_target(std::string_view(text).substr(separator + 1));
  return entry;
}

std::string route_target_text(const pusch_route_target& target)
{
  switch (target.type) {
    case pusch_route_target::kind::primary:
      return "primary";
    case pusch_route_target::kind::conventional:
      return "conventional";
    case pusch_route_target::kind::instance:
      break;
  }
  return identifier_text(target.instance_id);
}

const char* route_hook_text(pusch_route_hook hook)
{
  switch (hook) {
    case pusch_route_hook::channel_estimator:
      return "channel-estimator";
    case pusch_route_hook::equalizer:
      return "equalizer";
    case pusch_route_hook::receiver:
      break;
  }
  return "receiver";
}

std::string safe_text(std::string_view text)
{
  std::ostringstream output;
  for (unsigned char character : text) {
    switch (character) {
      case '\\':
        output << "\\\\";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (character < 0x20U || character == 0x7fU) {
          output << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(character)
                 << std::dec;
        } else {
          output << static_cast<char>(character);
        }
    }
  }
  return output.str();
}

template <typename Integer>
Integer parse_integer(std::string_view text, const char* kind)
{
  Integer    value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    throw std::invalid_argument(std::string("invalid ") + kind + " value");
  }
  return value;
}

std::vector<uint8_t> parse_hex(std::string_view text)
{
  if ((text.size() & 1U) != 0U) {
    throw std::invalid_argument("hex byte value must contain an even number of digits");
  }
  std::vector<uint8_t> result;
  result.reserve(text.size() / 2);
  for (std::size_t index = 0; index != text.size(); index += 2) {
    unsigned   value{};
    const auto parsed = std::from_chars(text.data() + index, text.data() + index + 2, value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + index + 2) {
      throw std::invalid_argument("invalid hexadecimal byte value");
    }
    result.push_back(static_cast<uint8_t>(value));
  }
  return result;
}

config_entry parse_config_entry(std::string_view text)
{
  const std::size_t equals = text.find('=');
  const std::size_t colon  = equals == std::string_view::npos ? equals : text.find(':', equals + 1);
  if (equals == 0 || colon == std::string_view::npos || colon == equals + 1) {
    throw std::invalid_argument("config entry must have key=bool|i64|u64|f64|str|hex:value form");
  }
  config_entry result;
  result.key                  = text.substr(0, equals);
  const std::string_view kind = text.substr(equals + 1, colon - equals - 1);
  const std::string_view raw  = text.substr(colon + 1);
  if (kind == "bool") {
    if (raw == "true" || raw == "1") {
      result.entry_value = true;
    } else if (raw == "false" || raw == "0") {
      result.entry_value = false;
    } else {
      throw std::invalid_argument("boolean config value must be true, false, 1, or 0");
    }
  } else if (kind == "i64") {
    result.entry_value = parse_integer<int64_t>(raw, "signed integer");
  } else if (kind == "u64") {
    result.entry_value = parse_integer<uint64_t>(raw, "unsigned integer");
  } else if (kind == "f64") {
    std::size_t consumed = 0;
    double      value    = std::stod(std::string(raw), &consumed);
    if (consumed != raw.size() || !std::isfinite(value)) {
      throw std::invalid_argument("invalid floating-point config value");
    }
    result.entry_value = value;
  } else if (kind == "str") {
    result.entry_value = std::string(raw);
  } else if (kind == "hex") {
    result.entry_value = parse_hex(raw);
  } else {
    throw std::invalid_argument("unknown config value kind");
  }
  return result;
}

std::vector<uint8_t> read_artifact(const std::string& path)
{
  if (path.empty()) {
    return {};
  }
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    throw std::invalid_argument("cannot open model artifact: " + path);
  }
  const std::streamoff length = input.tellg();
  if (length < 0 || static_cast<uint64_t>(length) > maximum_artifact_size) {
    throw std::invalid_argument("model artifact exceeds the 1 MiB management bound");
  }
  std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
  input.seekg(0);
  if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), length)) {
    throw std::invalid_argument("cannot read model artifact: " + path);
  }
  return bytes;
}

ocudu_dapp_placement_v1 parse_placement(std::string_view value)
{
  if (value == "in-process" || value == "resident") {
    return OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1;
  }
  if (value == "process" || value == "out-of-process" || value == "supervised-process") {
    return OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1;
  }
  throw std::invalid_argument("placement must be in-process or process");
}

backend parse_backend(std::string_view value)
{
  if (value == "cpu") {
    return backend::cpu;
  }
  if (value == "cuda") {
    return backend::cuda;
  }
  if (value == "other" || value == "other-accelerator") {
    return backend::other_accelerator;
  }
  throw std::invalid_argument("backend must be cpu, cuda, or other");
}

ocudu::dapp::e3::lifecycle_action parse_lifecycle_action(std::string_view value)
{
  if (value == "prepare") {
    return ocudu::dapp::e3::lifecycle_action::prepare;
  }
  if (value == "arm") {
    return ocudu::dapp::e3::lifecycle_action::arm;
  }
  if (value == "shadow" || value == "enter-shadow") {
    return ocudu::dapp::e3::lifecycle_action::enter_shadow;
  }
  if (value == "activate" || value == "enable") {
    return ocudu::dapp::e3::lifecycle_action::activate;
  }
  if (value == "deactivate" || value == "disable") {
    return ocudu::dapp::e3::lifecycle_action::deactivate;
  }
  if (value == "retry" || value == "retry-quiescence") {
    return ocudu::dapp::e3::lifecycle_action::retry_quiescence;
  }
  if (value == "unload") {
    return ocudu::dapp::e3::lifecycle_action::unload;
  }
  if (value == "probe" || value == "breaker-probe" || value == "breakerProbe") {
    return ocudu::dapp::e3::lifecycle_action::breaker_probe;
  }
  throw std::invalid_argument("unknown lifecycle action");
}

ocudu_dapp_config_boundary_v1 parse_boundary(std::string_view value)
{
  if (value == "immediate") {
    return OCUDU_DAPP_CONFIG_BOUNDARY_IMMEDIATE_V1;
  }
  if (value == "invocation") {
    return OCUDU_DAPP_CONFIG_BOUNDARY_INVOCATION_V1;
  }
  if (value == "slot") {
    return OCUDU_DAPP_CONFIG_BOUNDARY_SLOT_V1;
  }
  if (value == "restart") {
    return OCUDU_DAPP_CONFIG_BOUNDARY_RESTART_V1;
  }
  throw std::invalid_argument("config boundary must be immediate, invocation, slot, or restart");
}

ocudu_dapp_model_action_v1 parse_model_action(std::string_view value)
{
  if (value == "stage") {
    return OCUDU_DAPP_MODEL_STAGE_V1;
  }
  if (value == "validate") {
    return OCUDU_DAPP_MODEL_VALIDATE_V1;
  }
  if (value == "warm") {
    return OCUDU_DAPP_MODEL_WARM_V1;
  }
  if (value == "activate") {
    return OCUDU_DAPP_MODEL_ACTIVATE_V1;
  }
  if (value == "rollback") {
    return OCUDU_DAPP_MODEL_ROLLBACK_V1;
  }
  if (value == "retire") {
    return OCUDU_DAPP_MODEL_RETIRE_V1;
  }
  throw std::invalid_argument("unknown model action");
}

incident_cause parse_incident_cause(std::string_view value)
{
  static constexpr std::array names{"returned-error",
                                    "standard-exception",
                                    "unknown-exception",
                                    "invalid-input",
                                    "invalid-output",
                                    "deadline-miss",
                                    "device-error",
                                    "resource-exhausted",
                                    "process-exit",
                                    "heartbeat-loss",
                                    "lease-expired",
                                    "quiescence-timeout",
                                    "security-violation",
                                    "policy-rejection"};
  const auto                  found = std::find(names.begin(), names.end(), value);
  if (found == names.end()) {
    throw std::invalid_argument("unknown incident cause");
  }
  return static_cast<incident_cause>(std::distance(names.begin(), found));
}

const char* name(backend value) noexcept
{
  switch (value) {
    case backend::cpu:
      return "cpu";
    case backend::cuda:
      return "cuda";
    case backend::other_accelerator:
      return "other-accelerator";
  }
  return "unknown";
}

const char* name(ocudu_dapp_placement_v1 value) noexcept
{
  switch (value) {
    case OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1:
      return "in-process";
    case OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1:
      return "supervised-process";
  }
  return "unknown";
}

const char* name(ocudu_dapp_class_v1 value) noexcept
{
  switch (value) {
    case OCUDU_DAPP_CLASS_A_INLINE_V1:
      return "A/resident-inline-L1";
    case OCUDU_DAPP_CLASS_B_DEADLINE_V1:
      return "B/deadline-bound-decision";
    case OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1:
      return "C/open-loop-analytics";
  }
  return "unknown";
}

const char* name(ocudu_dapp_threading_v1 value) noexcept
{
  switch (value) {
    case OCUDU_DAPP_THREADING_PER_WORKER_V1:
      return "per-worker";
    case OCUDU_DAPP_THREADING_REENTRANT_SHARED_V1:
      return "reentrant-shared";
    case OCUDU_DAPP_THREADING_SERIALIZED_V1:
      return "serialized";
  }
  return "unknown";
}

const char* name(binding_requirement value) noexcept
{
  switch (value) {
    case binding_requirement::required:
      return "required";
    case binding_requirement::optional:
      return "optional";
  }
  return "unknown";
}

const char* name(binding_state value) noexcept
{
  switch (value) {
    case binding_state::unbound:
      return "unbound";
    case binding_state::bound:
      return "bound";
    case binding_state::unavailable:
      return "unavailable";
  }
  return "unknown";
}

const char* name(ocudu_dapp_lifecycle_state_v1 value) noexcept
{
  static constexpr std::array names{"unknown",
                                    "discovered",
                                    "loaded",
                                    "prepared",
                                    "armed",
                                    "shadow",
                                    "active",
                                    "draining",
                                    "inactive",
                                    "unloaded",
                                    "degraded",
                                    "quarantined-resident",
                                    "error"};
  const auto                  index = static_cast<std::size_t>(value);
  return index < names.size() ? names[index] : "unknown";
}

const char* name(ocudu_dapp_value_kind_v1 value) noexcept
{
  static constexpr std::array names{"unknown", "bool", "i64", "u64", "f64", "string", "bytes"};
  const auto                  index = static_cast<std::size_t>(value);
  return index < names.size() ? names[index] : "unknown";
}

const char* name(job_state value) noexcept
{
  static constexpr std::array names{"accepted", "running", "waiting-boundary", "succeeded", "failed", "cancelled"};
  const auto                  index = static_cast<std::size_t>(value);
  return index < names.size() ? names[index] : "unknown";
}

const char* name(incident_phase value) noexcept
{
  static constexpr std::array names{"query",
                                    "create",
                                    "configure",
                                    "prepare",
                                    "warmup",
                                    "invoke",
                                    "completion",
                                    "report",
                                    "stop",
                                    "quiesce",
                                    "unload"};
  const auto                  index = static_cast<std::size_t>(value);
  return index < names.size() ? names[index] : "unknown";
}

const char* name(incident_cause value) noexcept
{
  static constexpr std::array names{"returned-error",
                                    "standard-exception",
                                    "unknown-exception",
                                    "invalid-input",
                                    "invalid-output",
                                    "deadline-miss",
                                    "device-error",
                                    "resource-exhausted",
                                    "process-exit",
                                    "heartbeat-loss",
                                    "lease-expired",
                                    "quiescence-timeout",
                                    "security-violation",
                                    "policy-rejection"};
  const auto                  index = static_cast<std::size_t>(value);
  return index < names.size() ? names[index] : "unknown";
}

const char* name(incident_action value) noexcept
{
  static constexpr std::array names{"observed",
                                    "invocation-fallback",
                                    "breaker-opened",
                                    "disabled",
                                    "model-rollback",
                                    "quarantined",
                                    "process-restarted",
                                    "du-restart-required"};
  const auto                  index = static_cast<std::size_t>(value);
  return index < names.size() ? names[index] : "unknown";
}

std::string value_text(const value& input)
{
  return std::visit(
      [](const auto& item) -> std::string {
        using item_type = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<item_type, bool>) {
          return item ? "true" : "false";
        } else if constexpr (std::is_same_v<item_type, std::string>) {
          return '"' + safe_text(item) + '"';
        } else if constexpr (std::is_same_v<item_type, std::vector<uint8_t>>) {
          return "hex:" + hex_bytes(item.data(), item.size());
        } else {
          std::ostringstream output;
          output << item;
          return output.str();
        }
      },
      input);
}

void print_interface(const interface_record& interface, std::string_view indent)
{
  std::cout << indent << "interface " << interface.interface_id << " v" << interface.interface_major << '.'
            << interface.interface_minor << " class=" << name(interface.dapp_class)
            << " threading=" << name(interface.threading) << " binding=" << name(interface.requirement)
            << " binding-state=" << name(interface.state) << " placement-mask=0x" << std::hex
            << interface.placement_mask << " capabilities=0x" << interface.capability_bits << std::dec << '\n';
  if (interface.pusch) {
    std::cout << indent << "  pusch-profile max-prb=" << interface.pusch->max_nof_prb
              << " max-rx-ports=" << interface.pusch->max_nof_rx_ports
              << " max-layers=" << interface.pusch->max_nof_layers << " modulation-mask=0x" << std::hex
              << interface.pusch->modulation_mask << " dmrs-type-mask=0x" << interface.pusch->dmrs_type_mask
              << " flags=0x" << interface.pusch->flags << std::dec << '\n';
  }
}

void print_operation(const operation_result& result, std::string_view indent)
{
  std::cout << indent << "operation status=" << to_string(result.code) << " state=" << name(result.state)
            << " inventory-generation=" << result.inventory_generation
            << " instance-generation=" << result.instance_generation
            << " resource-generation=" << result.resource_generation << '\n';
  if (result.instance_id) {
    std::cout << indent << "instance-id=" << identifier_text(*result.instance_id) << '\n';
  }
  if (!result.detail.empty()) {
    std::cout << indent << "detail=" << safe_text(result.detail) << '\n';
  }
}

identifier identifier_from_words(const uint64_t words[2]) noexcept
{
  identifier result;
  std::memcpy(result.bytes.data(), words, result.bytes.size());
  return result;
}

void print_response(const management_response& response)
{
  std::cout << "transaction-id=" << identifier_text(response.transaction_id) << '\n';
  std::cout << "status=" << to_string(response.code) << '\n';
  if (response.job_id) {
    std::cout << "job-id=" << identifier_text(*response.job_id) << '\n';
  }
  if (!response.error_text.empty()) {
    std::cout << "error=" << safe_text(response.error_text) << '\n';
  }
  if (response.result) {
    print_operation(*response.result, "");
  }
  std::visit(
      [](const auto& payload) {
        using payload_type = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<payload_type, std::monostate>) {
          return;
        } else if constexpr (std::is_same_v<payload_type, pusch_routing_result>) {
          std::cout << "pusch-routes generation=" << payload.generation
                    << " slots-per-hook=" << unsigned(payload.slots_per_hook)
                    << " default=" << route_target_text(payload.default_target)
                    << " unavailable-lookups=" << payload.unavailable_lookups << '\n';
          for (const pusch_route_binding& binding : payload.bindings) {
            std::cout << "binding instance=" << identifier_text(binding.instance_id)
                      << " hook=" << route_hook_text(binding.hook) << " slot=" << unsigned(binding.slot) << '\n';
          }
          for (const pusch_route_entry& entry : payload.entries) {
            std::cout << "route rnti=0x" << std::hex << entry.rnti << std::dec
                      << " cell=" << (entry.cell_index ? std::to_string(*entry.cell_index) : std::string("any"))
                      << " target=" << route_target_text(entry.target) << '\n';
          }
        } else if constexpr (std::is_same_v<payload_type, cell_info_result>) {
          std::cout << "ran-control ue-release=" << std::boolalpha << payload.capabilities.ue_release
                    << " handover=" << payload.capabilities.handover << " tx-gain=" << payload.capabilities.tx_gain
                    << " rx-gain=" << payload.capabilities.rx_gain << std::noboolalpha << '\n';
          for (const cell_descriptor& cell : payload.cells) {
            std::cout << "cell " << cell.cell_id << " pci=" << cell.pci << " plmn=" << cell.plmn << " tac=" << cell.tac
                      << " nci=" << cell.nr_cell_identity << " band=n" << cell.band << " dl-arfcn=" << cell.dl_arfcn
                      << " ul-arfcn=" << cell.ul_arfcn << " dl-hz=" << cell.dl_center_hz
                      << " ul-hz=" << cell.ul_center_hz << " bw-mhz=" << cell.bandwidth_mhz << " prbs=" << cell.nof_prbs
                      << " scs-khz=" << cell.scs_khz << " tx-ant=" << unsigned(cell.nof_tx_antennas)
                      << " rx-ant=" << unsigned(cell.nof_rx_antennas) << " barred=" << std::boolalpha << cell.barred
                      << std::noboolalpha;
            if (cell.tdd) {
              std::cout << " tdd=" << cell.tdd->period_slots << "slots/" << unsigned(cell.tdd->dl_slots) << "D"
                        << unsigned(cell.tdd->dl_symbols) << "d" << unsigned(cell.tdd->ul_slots) << "U"
                        << unsigned(cell.tdd->ul_symbols) << "u";
            }
            if (cell.prach) {
              std::cout << " prach=idx" << unsigned(cell.prach->config_index) << "/root"
                        << cell.prach->root_sequence_index << "/zcz" << cell.prach->zero_correlation_zone << "/prb"
                        << cell.prach->prb_start;
            }
            if (cell.tx_gain_db) {
              std::cout << " tx-gain-db=" << *cell.tx_gain_db;
            }
            if (cell.rx_gain_db) {
              std::cout << " rx-gain-db=" << *cell.rx_gain_db;
            }
            std::cout << '\n';
          }
        } else if constexpr (std::is_same_v<payload_type, inventory_result>) {
          const auto& inventory = payload.inventory;
          std::cout << "inventory-generation=" << inventory.generation << '\n';
          for (const package_record& package : inventory.packages) {
            std::cout << "package " << identifier_text(package.package_id) << " name=" << safe_text(package.global_name)
                      << " vendor=" << safe_text(package.vendor) << " version=" << safe_text(package.version)
                      << " compatible=" << std::boolalpha << package.compatible << std::noboolalpha
                      << " digest=" << hex_bytes(package.content_digest.data(), package.content_digest.size())
                      << " manifest=v" << package.manifest_schema_version << " signature="
                      << (package.signature_status == package_signature_status::verified ? "verified" : "unsigned");
            if (package.signature_status == package_signature_status::verified) {
              std::cout << " key=" << hex_bytes(package.signing_key_id.data(), package.signing_key_id.size())
                        << " sequence=" << package.signature_sequence << " policy-epoch=" << package.trust_policy_epoch;
            }
            if (package.sbom_verified) {
              std::cout << " sbom=verified sbom-digest="
                        << hex_bytes(package.sbom_digest.data(), package.sbom_digest.size());
            }
            std::cout << '\n';
            if (!package.incompatibility.empty()) {
              std::cout << "  incompatibility=" << safe_text(package.incompatibility) << '\n';
            }
            std::cout << "  backends=";
            for (std::size_t index = 0; index != package.backends.size(); ++index) {
              std::cout << (index == 0 ? "" : ",") << name(package.backends[index]);
            }
            std::cout << '\n';
            for (const interface_record& interface : package.interfaces) {
              print_interface(interface, "  ");
            }
          }
          for (const instance_record& instance : inventory.instances) {
            std::cout << "instance " << identifier_text(instance.instance_id)
                      << " package=" << identifier_text(instance.package_id)
                      << " name=" << safe_text(instance.global_name) << " state=" << name(instance.state)
                      << " placement=" << name(instance.placement) << " backend=" << name(instance.selected_backend)
                      << " generation=" << instance.generation << " last-incident=" << instance.last_incident_id
                      << " management-capabilities=0x" << std::hex << instance.management_capabilities << std::dec
                      << '\n';
            if (!instance.last_error.empty()) {
              std::cout << "  last-error=" << safe_text(instance.last_error) << '\n';
            }
            for (const interface_record& interface : instance.interfaces) {
              print_interface(interface, "  ");
            }
          }
          for (const e3_connection_record& connection : payload.connections) {
            std::cout << "e3-connection association=" << connection.association_id
                      << " peer=" << safe_text(connection.peer_address) << " setup=" << std::boolalpha
                      << connection.setup_complete << std::noboolalpha << " dapp-id=" << connection.dapp_id.value_or(0)
                      << " dapp-name=" << safe_text(connection.dapp_name) << " controls=" << connection.control_requests
                      << " subscriptions=" << connection.subscriptions.size() << '\n';
            for (const auto& subscription : connection.subscriptions) {
              std::cout << "  subscription " << subscription.subscription_id
                        << " ran-function=" << subscription.ran_function_id << " telemetry=";
              for (std::size_t index = 0; index != subscription.telemetry_identifiers.size(); ++index) {
                std::cout << (index == 0 ? "" : ",") << subscription.telemetry_identifiers[index];
              }
              std::cout << '\n';
            }
          }
        } else if constexpr (std::is_same_v<payload_type, config_result>) {
          if (payload.generation) {
            std::cout << "config-generation=" << *payload.generation << '\n';
          }
          for (const config_descriptor& descriptor : payload.descriptors) {
            std::cout << "config-schema " << safe_text(descriptor.key) << " kind=" << name(descriptor.value_kind)
                      << " flags=0x" << std::hex << descriptor.flags << std::dec
                      << " description=" << safe_text(descriptor.description) << '\n';
            if (descriptor.default_value) {
              std::cout << "  default=" << value_text(*descriptor.default_value) << '\n';
            }
            if (descriptor.minimum_value) {
              std::cout << "  minimum=" << value_text(*descriptor.minimum_value) << '\n';
            }
            if (descriptor.maximum_value) {
              std::cout << "  maximum=" << value_text(*descriptor.maximum_value) << '\n';
            }
          }
          for (const config_entry& entry : payload.entries) {
            std::cout << "config " << safe_text(entry.key) << '=' << value_text(entry.entry_value) << '\n';
          }
        } else if constexpr (std::is_same_v<payload_type, metric_result>) {
          for (const metric_descriptor& descriptor : payload.descriptors) {
            std::cout << "metric-schema " << safe_text(descriptor.name) << " kind=" << name(descriptor.value_kind)
                      << " unit=" << safe_text(descriptor.unit) << " flags=0x" << std::hex << descriptor.flags
                      << std::dec << " description=" << safe_text(descriptor.description) << '\n';
          }
          for (const metric_sample& sample : payload.samples) {
            std::cout << "metric " << safe_text(sample.name) << '=' << value_text(sample.sample_value)
                      << " timestamp-ns=" << sample.monotonic_timestamp_ns << '\n';
          }
        } else if constexpr (std::is_same_v<payload_type, incident_result>) {
          if (payload.cleared_count) {
            std::cout << "cleared-count=" << *payload.cleared_count << '\n';
          }
          for (const incident_record& incident : payload.incidents) {
            std::cout << "incident " << incident.incident_id << " timestamp-ns=" << incident.monotonic_ns
                      << " instance=" << identifier_text(identifier_from_words(incident.context.instance_id))
                      << " package=" << identifier_text(identifier_from_words(incident.context.package_id))
                      << " phase=" << name(incident.context.phase) << " cause=" << name(incident.cause)
                      << " action=" << name(incident.action) << " status=" << incident.native_status
                      << " recurrence=" << incident.recurrence_count << " hook=" << incident.context.hook_id
                      << " worker=" << incident.context.worker_id << " latency-ns=" << incident.observed_latency_ns
                      << " deadline-ns=" << incident.deadline_ns << " breaker-open=" << std::boolalpha
                      << incident.breaker_open << " fallback-succeeded=" << incident.fallback_succeeded
                      << std::noboolalpha << '\n';
            const std::size_t size = std::min<std::size_t>(incident.message_size, incident.sanitized_message.size());
            if (size != 0) {
              std::cout << "  message=" << safe_text({incident.sanitized_message.data(), size}) << '\n';
            }
          }
        } else if constexpr (std::is_same_v<payload_type, incident_health_summary>) {
          std::cout << "health total=" << payload.total << " standard-exceptions=" << payload.standard_exceptions
                    << " unknown-exceptions=" << payload.unknown_exceptions
                    << " deadline-misses=" << payload.deadline_misses << " invalid-inputs=" << payload.invalid_inputs
                    << " invalid-outputs=" << payload.invalid_outputs << " device-errors=" << payload.device_errors
                    << " resource-exhaustions=" << payload.resource_exhaustions
                    << " quiescence-timeouts=" << payload.quiescence_timeouts
                    << " breaker-opened=" << payload.breaker_opened << " fallbacks=" << payload.fallbacks
                    << " quarantined=" << payload.quarantined << " history-evictions=" << payload.history_evictions
                    << " source-ring-drops=" << payload.source_ring_drops
                    << " last-incident=" << payload.last_incident_id << '\n';
        } else if constexpr (std::is_same_v<payload_type, job_result>) {
          for (const job_record& job : payload.jobs) {
            std::cout << "job " << identifier_text(job.job_id) << " transaction=" << identifier_text(job.transaction_id)
                      << " state=" << name(job.state) << " progress=" << static_cast<unsigned>(job.progress_percent)
                      << "% submitted-ns=" << job.submitted_monotonic_ns << " started-ns=" << job.started_monotonic_ns
                      << " completed-ns=" << job.completed_monotonic_ns << '\n';
            if (!job.detail.empty()) {
              std::cout << "  detail=" << safe_text(job.detail) << '\n';
            }
            if (job.result) {
              print_operation(*job.result, "  ");
            }
          }
        } else if constexpr (std::is_same_v<payload_type, quiet_reservation_result>) {
          std::cout << "quiet-reservations=" << payload.reservations.size() << " truncated=" << std::boolalpha
                    << payload.truncated << std::noboolalpha << '\n';
          for (const quiet_reservation_record& record : payload.reservations) {
            const auto& value = record.reservation;
            std::cout << "quiet-reservation " << identifier_text(identifier_from_words(value.reservation_id))
                      << " request=" << identifier_text(identifier_from_words(value.request_id))
                      << " source=" << identifier_text(identifier_from_words(value.request_source_instance_id))
                      << " source-generation=" << value.request_source_generation
                      << " source-sequence=" << value.request_source_sequence << " cell=" << record.cell_id
                      << " start=" << value.start.sfn << ':' << value.start.slot << ':' << value.start.numerology
                      << " duration=" << value.duration_slots << " repeat-period=" << value.repeat_period_slots
                      << " repeats=" << value.repeat_count << " prbs=" << value.first_prb << '+' << value.nof_prbs
                      << " level=" << static_cast<unsigned>(value.achieved_level)
                      << " status=" << static_cast<unsigned>(value.status) << " reason=" << value.reason
                      << " confidence-q16=" << value.confidence_q16 << " expiry-ns=" << value.expiry_monotonic_ns
                      << '\n';
          }
        } else if constexpr (std::is_same_v<payload_type, data_subscription_result>) {
          for (const auto& subscription : payload.subscriptions) {
            const auto& request = subscription.request;
            std::cout << "data-subscription " << identifier_text(request.subscription_id)
                      << " interface=" << request.interface_id;
            if (request.cell_id) {
              std::cout << " cell=" << *request.cell_id;
            }
            std::cout << " sample-every=" << request.sample_every_n
                      << " queue-depth-chunks=" << request.queue_depth_chunks
                      << " maximum-chunk-bytes=" << request.maximum_chunk_bytes << " lease-us=" << request.lease_us
                      << " drop-policy="
                      << (request.drop_policy == portable_drop_policy::drop_newest   ? "drop-newest"
                          : request.drop_policy == portable_drop_policy::drop_oldest ? "drop-oldest"
                                                                                     : "coalesce-latest")
                      << " active=" << std::boolalpha << subscription.active << std::noboolalpha
                      << " lease-expiry-ns=" << subscription.lease_expiry_ns
                      << " queued-chunks=" << subscription.queued_chunks
                      << " published-objects=" << subscription.published_objects
                      << " published-tensor-bytes=" << subscription.published_tensor_bytes
                      << " dropped-objects=" << subscription.dropped_objects
                      << " busy-drops=" << subscription.busy_drops
                      << " sampled-objects=" << subscription.sampled_objects << '\n';
          }
        } else if constexpr (std::is_same_v<payload_type, data_publication_result>) {
          for (const auto& chunk : payload.chunks) {
            std::cout << "data-publication " << identifier_text(chunk.subscription_id)
                      << " interface=" << chunk.interface_id << " cell=" << chunk.cell_id
                      << " sequence=" << chunk.publication_sequence << " route-epoch=" << chunk.route_epoch
                      << " chunk=" << chunk.chunk_index << '/' << chunk.chunk_count
                      << " tensor-size=" << chunk.tensor_size << " metadata-bytes=" << chunk.metadata.size()
                      << " tensor-chunk-bytes=" << chunk.tensor_chunk.size()
                      << " dropped-objects=" << chunk.dropped_objects << '\n';
          }
          if (payload.subscription) {
            std::cout << "data-poll-state queued-chunks=" << payload.subscription->queued_chunks
                      << " lease-expiry-ns=" << payload.subscription->lease_expiry_ns
                      << " dropped-objects=" << payload.subscription->dropped_objects << '\n';
          }
        }
      },
      response.payload);
}

request_header instance_header(identifier transaction, const instance_options& options)
{
  request_header header;
  header.transaction_id               = transaction;
  header.instance_id                  = parse_identifier(options.id);
  header.expected_instance_generation = options.expected_generation.get();
  header.dry_run                      = options.dry_run;
  return header;
}

void add_instance_option(CLI::App& command, instance_options& options, bool mutation)
{
  command.add_option("--instance", options.id, "16-byte dApp instance ID (hex)")->required();
  if (mutation) {
    options.expected_generation.option = command.add_option(
        "--expected-generation", options.expected_generation.value, "Reject a stale instance generation");
    options.expected_generation.option->check(CLI::PositiveNumber);
    command.add_flag("--dry-run", options.dry_run, "Validate without applying the operation");
  }
}

} // namespace

int main(int argc, char** argv)
{
  CLI::App app{"Control and inspect the embedded OCUDU E3 dApp agent over the local "
               "credentialed socket (for the E3AP SCTP carrier use ocudu_dappctl.py --sctp)"};
  app.require_subcommand(1);

  const char* socket_environment = std::getenv("OCUDU_DAPP_E3_SOCKET");
  std::string socket_path = socket_environment != nullptr ? socket_environment : "/run/ocudu/dapp-management.sock";
  uint32_t    timeout_ms  = 1000;
  uint32_t    server_uid{};
  std::string transaction_text;
  auto*       server_uid_option = app.add_option("--server-uid", server_uid, "Expected effective UID of the DU");
  app.add_option("--socket", socket_path, "Credentialed Unix seqpacket endpoint")->envname("OCUDU_DAPP_E3_SOCKET");
  app.add_option("--timeout-ms", timeout_ms, "Bounded request/response timeout")->check(CLI::PositiveNumber);
  app.add_option("--transaction", transaction_text, "Explicit transaction ID for an idempotent retry");

  bool  omit_packages{};
  bool  omit_instances{};
  auto* inventory_command = app.add_subcommand("inventory", "List registered packages and live instances");
  inventory_command->add_flag("--omit-packages", omit_packages);
  inventory_command->add_flag("--omit-instances", omit_instances);

  std::string         catalog_package;
  optional_generation catalog_generation;
  bool                catalog_dry_run{};
  auto*               catalog_command = app.add_subcommand("catalog", "Manage locally staged signed catalog packages");
  catalog_command->require_subcommand(1);
  auto* admit_command = catalog_command->add_subcommand("admit", "Verify and admit a bundle without loading it");
  admit_command->add_option("--package", catalog_package, "16-byte staged package ID (hex)")->required();
  catalog_generation.option =
      admit_command->add_option("--expected-inventory-generation", catalog_generation.value, "Reject stale inventory");
  catalog_generation.option->check(CLI::PositiveNumber);
  admit_command->add_flag("--dry-run", catalog_dry_run);

  struct {
    std::string         package;
    std::string         placement{"in-process"};
    std::string         backend_name{"cpu"};
    optional_generation expected_inventory;
    bool                dry_run{};
  } load;
  auto* load_command = app.add_subcommand("load", "Load a verified package as a new instance");
  load_command->add_option("--package", load.package, "16-byte package ID (hex)")->required();
  load_command->add_option("--placement", load.placement, "in-process or process");
  load_command->add_option("--backend", load.backend_name, "cpu, cuda, or other");
  load.expected_inventory.option = load_command->add_option(
      "--expected-inventory-generation", load.expected_inventory.value, "Reject a stale package inventory");
  load.expected_inventory.option->check(CLI::PositiveNumber);
  load_command->add_flag("--dry-run", load.dry_run);

  struct {
    instance_options instance;
    std::string      action;
    uint64_t         timeout_us{1'000'000};
  } lifecycle;
  auto* lifecycle_command = app.add_subcommand("lifecycle", "Prepare, enable, disable, or unload an instance");
  lifecycle_command
      ->add_option("action",
                   lifecycle.action,
                   "prepare|arm|shadow|activate|deactivate|retry|unload (enable/disable aliases accepted)")
      ->required();
  add_instance_option(*lifecycle_command, lifecycle.instance, true);
  lifecycle_command->add_option("--timeout-us", lifecycle.timeout_us, "Drain/quiescence timeout");

  auto* config_command = app.add_subcommand("config", "Query or update typed dApp configuration");
  config_command->require_subcommand(1);
  instance_options config_get;
  auto*            config_get_command = config_command->add_subcommand("get", "Read effective configuration");
  add_instance_option(*config_get_command, config_get, false);
  instance_options config_schema;
  auto*            config_schema_command = config_command->add_subcommand("schema", "Describe configuration keys");
  add_instance_option(*config_schema_command, config_schema, false);
  struct {
    instance_options         instance;
    uint64_t                 config_generation{};
    std::string              boundary{"invocation"};
    std::vector<std::string> entries;
  } config_set_options;
  auto* config_set_command = config_command->add_subcommand("set", "Apply a typed configuration transaction");
  add_instance_option(*config_set_command, config_set_options.instance, true);
  config_set_command
      ->add_option("--config-generation", config_set_options.config_generation, "Expected effective config generation")
      ->required()
      ->check(CLI::PositiveNumber);
  config_set_command->add_option("--boundary", config_set_options.boundary, "immediate|invocation|slot|restart");
  config_set_command->add_option("--set", config_set_options.entries, "Repeat key=bool|i64|u64|f64|str|hex:value")
      ->required();

  auto* metrics_command = app.add_subcommand("metrics", "Query dApp metric metadata or samples");
  metrics_command->require_subcommand(1);
  instance_options metrics_get;
  auto*            metrics_get_command = metrics_command->add_subcommand("get", "Read a bounded metric snapshot");
  add_instance_option(*metrics_get_command, metrics_get, false);
  instance_options metrics_schema;
  auto*            metrics_schema_command = metrics_command->add_subcommand("schema", "Describe dApp metrics");
  add_instance_option(*metrics_schema_command, metrics_schema, false);

  struct {
    std::string         instance;
    std::string         cause;
    optional_generation after;
    std::size_t         limit{100};
  } incidents_list;
  auto* incidents_command = app.add_subcommand("incidents", "Query and acknowledge safety incidents");
  incidents_command->require_subcommand(1);
  auto* incidents_list_command = incidents_command->add_subcommand("list", "List bounded incident history");
  incidents_list_command->add_option("--instance", incidents_list.instance, "Optional instance ID filter");
  incidents_list_command->add_option("--cause", incidents_list.cause, "Optional incident-cause filter");
  incidents_list.after.option = incidents_list_command->add_option(
      "--after", incidents_list.after.value, "Only IDs greater than this incident ID");
  incidents_list_command->add_option("--limit", incidents_list.limit)->check(CLI::Range(1, 1024));
  uint64_t clear_through{};
  auto*    incidents_clear_command = incidents_command->add_subcommand("clear", "Acknowledge and clear history");
  incidents_clear_command->add_option("--through", clear_through, "Highest acknowledged incident ID")->required();

  auto* health_command = app.add_subcommand("health", "Read cumulative safety and fallback counters");

  auto* jobs_command = app.add_subcommand("jobs", "Inspect or cancel asynchronous management work");
  jobs_command->require_subcommand(1);
  auto*       jobs_list_command = jobs_command->add_subcommand("list", "List bounded job state");
  std::string job_get_id;
  auto*       jobs_get_command = jobs_command->add_subcommand("get", "Read one job");
  jobs_get_command->add_option("--job", job_get_id, "16-byte job ID (hex)")->required();
  std::string job_cancel_id;
  auto*       jobs_cancel_command = jobs_command->add_subcommand("cancel", "Request cancellation of queued work");
  jobs_cancel_command->add_option("--job", job_cancel_id, "16-byte job ID (hex)")->required();

  struct {
    std::string  instance;
    uint32_t     cell{};
    CLI::Option* cell_option{};
    std::size_t  limit{100};
  } quiet_reservations;
  auto* quiet_reservations_command =
      app.add_subcommand("quiet-reservations", "List authoritative bounded quiet reservations");
  quiet_reservations_command->add_option("--instance", quiet_reservations.instance, "Optional source instance ID");
  quiet_reservations.cell_option =
      quiet_reservations_command->add_option("--cell", quiet_reservations.cell, "Optional DU cell index");
  quiet_reservations.cell_option->check(CLI::Range(0, 65535));
  quiet_reservations_command->add_option("--limit", quiet_reservations.limit)->check(CLI::Range(1, 1024));

  auto* data_command = app.add_subcommand("data", "Manage the bounded portable Class-C E3 payload profile");
  data_command->require_subcommand(1);
  struct {
    std::string  subscription;
    uint32_t     interface_id{};
    uint32_t     cell{};
    CLI::Option* cell_option{};
    uint32_t     sample_every{1};
    uint32_t     queue_depth{64};
    uint32_t     chunk_bytes{64 * 1024};
    uint64_t     lease_us{1'000'000};
    std::string  drop_policy{"drop-newest"};
  } data_subscribe;
  auto* data_subscribe_command = data_command->add_subcommand("subscribe", "Create or renew one subscription");
  data_subscribe_command->add_option("--subscription", data_subscribe.subscription, "16-byte subscription ID (hex)")
      ->required();
  data_subscribe_command->add_option("--interface", data_subscribe.interface_id, "Spectrum or SRS interface ID")
      ->required()
      ->check(CLI::Range(1U, std::numeric_limits<uint32_t>::max()));
  data_subscribe.cell_option =
      data_subscribe_command->add_option("--cell", data_subscribe.cell, "Optional cell filter");
  data_subscribe.cell_option->check(CLI::Range(0U, 65535U));
  data_subscribe_command->add_option("--sample-every", data_subscribe.sample_every)->check(CLI::Range(1U, 65535U));
  data_subscribe_command->add_option("--queue-depth", data_subscribe.queue_depth)->check(CLI::Range(1U, 1024U));
  data_subscribe_command->add_option("--chunk-bytes", data_subscribe.chunk_bytes)->check(CLI::Range(1024U, 65536U));
  data_subscribe_command->add_option("--lease-us", data_subscribe.lease_us)->check(CLI::Range(1ULL, 60000000ULL));
  data_subscribe_command->add_option(
      "--drop-policy", data_subscribe.drop_policy, "drop-newest|drop-oldest|coalesce-latest");
  auto*       data_list_command = data_command->add_subcommand("list", "List subscriptions and delivery counters");
  std::string data_remove_id;
  auto*       data_remove_command = data_command->add_subcommand("remove", "Remove one subscription");
  data_remove_command->add_option("--subscription", data_remove_id, "16-byte subscription ID (hex)")->required();
  std::string data_poll_id;
  uint32_t    data_poll_limit{8};
  auto*       data_poll_command = data_command->add_subcommand("poll", "Poll bounded APER publication chunks");
  data_poll_command->add_option("--subscription", data_poll_id, "16-byte subscription ID (hex)")->required();
  data_poll_command->add_option("--limit", data_poll_limit)->check(CLI::Range(1U, 8U));

  uint16_t ue_release_pci{};
  uint16_t ue_release_rnti{};
  bool     ue_release_dry_run{};
  auto*    ue_release_command = app.add_subcommand("ue-release", "Queue an RRC release-to-idle through E3");
  ue_release_command->add_option("--pci", ue_release_pci, "Serving physical cell ID")
      ->required()
      ->check(CLI::Range(0, 1007));
  ue_release_command->add_option("--rnti", ue_release_rnti, "UE C-RNTI")->required()->check(CLI::Range(1, 65535));
  ue_release_command->add_flag("--dry-run", ue_release_dry_run, "Validate without dispatching the release");

  uint16_t handover_pci{}, handover_rnti{}, handover_target{};
  bool     handover_dry_run{};
  auto*    handover_command = app.add_subcommand("ue-handover", "Queue a handover of a UE to another cell through E3");
  handover_command->add_option("--pci", handover_pci, "Serving physical cell ID")
      ->required()
      ->check(CLI::Range(0, 1007));
  handover_command->add_option("--rnti", handover_rnti, "UE C-RNTI")->required()->check(CLI::Range(1, 65535));
  handover_command->add_option("--target-pci", handover_target, "Target physical cell ID")
      ->required()
      ->check(CLI::Range(0, 1007));
  handover_command->add_flag("--dry-run", handover_dry_run, "Validate without dispatching the handover");

  std::string gain_direction{"rx"};
  uint16_t    gain_port{};
  double      gain_db{};
  bool        gain_dry_run{};
  auto*       gain_command = app.add_subcommand("rf-gain", "Set a radio-unit TX or RX gain through E3");
  gain_command->add_option("--direction", gain_direction, "tx or rx")->check(CLI::IsMember({"tx", "rx"}));
  gain_command->add_option("--port", gain_port, "Radio-unit port index")->check(CLI::Range(0, 63));
  gain_command->add_option("--gain-db", gain_db, "Gain in dB")->required();
  gain_command->add_flag("--dry-run", gain_dry_run, "Validate without touching the radio");

  auto* cell_info_command = app.add_subcommand("cell-info", "Describe the served cells and RAN control capabilities");

  std::string              route_action;
  std::vector<std::string> route_entries;
  std::string              route_default;
  bool                     route_dry_run{};
  auto*                    route_command = app.add_subcommand(
      "pusch-route", "Show or change the per-UE route of Class-A PUSCH grants between dApp instances");
  route_command->add_option("action", route_action, "show|set|replace|clear")
      ->required()
      ->check(CLI::IsMember({"show", "set", "replace", "clear"}));
  route_command->add_option("--entry", route_entries, "RNTI[@CELL]=TARGET (primary, conventional or instance ID)");
  route_command->add_option("--default", route_default, "Route of UEs without an entry (replace only)");
  route_command->add_flag("--dry-run", route_dry_run, "Validate without changing the table");

  struct {
    instance_options instance;
    std::string      action;
    std::string      model_id;
    std::string      version;
    uint64_t         model_generation{};
    std::string      artifact;
  } model;
  auto* model_command = app.add_subcommand("model", "Stage, validate, warm, activate, roll back, or retire a model");
  model_command->add_option("action", model.action, "stage|validate|warm|activate|rollback|retire")->required();
  add_instance_option(*model_command, model.instance, true);
  model_command->add_option("--model", model.model_id, "Stable model ID")->required();
  model_command->add_option("--version", model.version, "Model version");
  model_command->add_option("--model-generation", model.model_generation, "Expected model generation")->required();
  model_command->get_option("--model-generation")->check(CLI::PositiveNumber);
  model_command->add_option("--artifact", model.artifact, "Bounded artifact file (maximum 1 MiB)");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& error) {
    return app.exit(error);
  }

  try {
    const identifier transaction = transaction_text.empty() ? new_transaction_id() : parse_identifier(transaction_text);
    request_header   common;
    common.transaction_id = transaction;
    management_request request;

    if (*inventory_command) {
      request = inventory_query{common, !omit_packages, !omit_instances};
    } else if (*admit_command) {
      common.expected_inventory_generation = catalog_generation.get();
      common.dry_run                       = catalog_dry_run;
      request                              = catalog_admit{common, parse_identifier(catalog_package)};
    } else if (*load_command) {
      common.expected_inventory_generation = load.expected_inventory.get();
      common.dry_run                       = load.dry_run;
      lifecycle_control operation;
      operation.header           = common;
      operation.action           = ocudu::dapp::e3::lifecycle_action::load;
      operation.package_id       = parse_identifier(load.package);
      operation.placement        = parse_placement(load.placement);
      operation.selected_backend = parse_backend(load.backend_name);
      request                    = std::move(operation);
    } else if (*lifecycle_command) {
      if (lifecycle.timeout_us > 3'600'000'000ULL) {
        throw std::invalid_argument("lifecycle timeout exceeds the E3 service-model bound");
      }
      lifecycle_control operation;
      operation.header     = instance_header(transaction, lifecycle.instance);
      operation.action     = parse_lifecycle_action(lifecycle.action);
      operation.timeout_us = lifecycle.timeout_us;
      request              = std::move(operation);
    } else if (*config_get_command || *config_schema_command) {
      const instance_options& options = *config_get_command ? config_get : config_schema;
      config_query            operation;
      operation.header = instance_header(transaction, options);
      operation.kind   = *config_get_command ? query_kind::effective : query_kind::schema;
      request          = std::move(operation);
    } else if (*config_set_command) {
      config_set operation;
      operation.header                     = instance_header(transaction, config_set_options.instance);
      operation.expected_config_generation = config_set_options.config_generation;
      operation.boundary                   = parse_boundary(config_set_options.boundary);
      for (const std::string& entry : config_set_options.entries) {
        operation.entries.push_back(parse_config_entry(entry));
      }
      request = std::move(operation);
    } else if (*metrics_get_command || *metrics_schema_command) {
      const instance_options& options = *metrics_get_command ? metrics_get : metrics_schema;
      metric_query            operation;
      operation.header = instance_header(transaction, options);
      operation.kind   = *metrics_get_command ? query_kind::effective : query_kind::schema;
      request          = std::move(operation);
    } else if (*incidents_list_command) {
      incident_query operation;
      operation.header.transaction_id = transaction;
      if (!incidents_list.instance.empty()) {
        operation.header.instance_id = parse_identifier(incidents_list.instance);
      }
      if (!incidents_list.cause.empty()) {
        operation.cause = parse_incident_cause(incidents_list.cause);
      }
      operation.after_incident_id = incidents_list.after.get();
      operation.maximum_records   = incidents_list.limit;
      request                     = std::move(operation);
    } else if (*incidents_clear_command) {
      incident_query operation;
      operation.header.transaction_id = transaction;
      operation.action                = incident_query_action::clear_through;
      operation.after_incident_id     = clear_through;
      request                         = std::move(operation);
    } else if (*health_command) {
      request = health_query{common};
    } else if (*jobs_list_command || *jobs_get_command || *jobs_cancel_command) {
      job_query operation;
      operation.header = common;
      if (*jobs_get_command) {
        operation.action = job_query_action::get;
        operation.job_id = parse_identifier(job_get_id);
      } else if (*jobs_cancel_command) {
        operation.action = job_query_action::cancel;
        operation.job_id = parse_identifier(job_cancel_id);
      } else {
        operation.action = job_query_action::list;
      }
      request = std::move(operation);
    } else if (*quiet_reservations_command) {
      quiet_reservation_query operation;
      operation.header = common;
      if (!quiet_reservations.instance.empty()) {
        operation.header.instance_id = parse_identifier(quiet_reservations.instance);
      }
      if (quiet_reservations.cell_option->count() != 0) {
        operation.cell_id = quiet_reservations.cell;
      }
      operation.maximum_records = quiet_reservations.limit;
      request                   = std::move(operation);
    } else if (*data_subscribe_command) {
      data_subscription_control operation;
      operation.header          = common;
      operation.action          = data_subscription_action::create;
      operation.subscription_id = parse_identifier(data_subscribe.subscription);
      operation.interface_id    = data_subscribe.interface_id;
      if (data_subscribe.cell_option->count() != 0) {
        operation.cell_id = data_subscribe.cell;
      }
      operation.sample_every_n      = data_subscribe.sample_every;
      operation.queue_depth_chunks  = data_subscribe.queue_depth;
      operation.maximum_chunk_bytes = data_subscribe.chunk_bytes;
      operation.lease_us            = data_subscribe.lease_us;
      if (data_subscribe.drop_policy == "drop-newest") {
        operation.drop_policy = portable_drop_policy::drop_newest;
      } else if (data_subscribe.drop_policy == "drop-oldest") {
        operation.drop_policy = portable_drop_policy::drop_oldest;
      } else if (data_subscribe.drop_policy == "coalesce-latest") {
        operation.drop_policy = portable_drop_policy::coalesce_latest;
      } else {
        throw std::invalid_argument("data drop policy must be drop-newest, drop-oldest, or coalesce-latest");
      }
      request = std::move(operation);
    } else if (*data_list_command) {
      data_subscription_control operation;
      operation.header = common;
      operation.action = data_subscription_action::list;
      request          = std::move(operation);
    } else if (*data_remove_command) {
      data_subscription_control operation;
      operation.header          = common;
      operation.action          = data_subscription_action::remove;
      operation.subscription_id = parse_identifier(data_remove_id);
      request                   = std::move(operation);
    } else if (*data_poll_command) {
      data_poll_query operation;
      operation.header          = common;
      operation.subscription_id = parse_identifier(data_poll_id);
      operation.maximum_chunks  = data_poll_limit;
      request                   = std::move(operation);
    } else if (*ue_release_command) {
      ran_control operation;
      operation.header.transaction_id = transaction;
      operation.header.dry_run        = ue_release_dry_run;
      operation.serving_pci           = ue_release_pci;
      operation.rnti                  = ue_release_rnti;
      request                         = std::move(operation);
    } else if (*handover_command) {
      ran_control operation;
      operation.header.transaction_id = transaction;
      operation.header.dry_run        = handover_dry_run;
      operation.action                = ran_control_action::handover_ue;
      operation.serving_pci           = handover_pci;
      operation.rnti                  = handover_rnti;
      operation.target_pci            = handover_target;
      request                         = std::move(operation);
    } else if (*gain_command) {
      ran_control operation;
      operation.header.transaction_id = transaction;
      operation.header.dry_run        = gain_dry_run;
      operation.action  = gain_direction == "tx" ? ran_control_action::set_tx_gain : ran_control_action::set_rx_gain;
      operation.port_id = static_cast<uint8_t>(gain_port);
      operation.gain_db = gain_db;
      request           = std::move(operation);
    } else if (*cell_info_command) {
      cell_info_query operation;
      operation.header.transaction_id = transaction;
      request                         = std::move(operation);
    } else if (*route_command) {
      pusch_routing operation;
      operation.header.transaction_id = transaction;
      operation.header.dry_run        = route_dry_run;
      operation.action                = route_action == "show"      ? pusch_routing_action::query
                                        : route_action == "set"     ? pusch_routing_action::update
                                        : route_action == "replace" ? pusch_routing_action::replace
                                                                    : pusch_routing_action::clear;
      const bool takes_entries =
          operation.action == pusch_routing_action::update || operation.action == pusch_routing_action::replace;
      if ((!takes_entries && !route_entries.empty()) ||
          (!route_default.empty() && operation.action != pusch_routing_action::replace) ||
          (operation.action == pusch_routing_action::update && route_entries.empty())) {
        throw std::invalid_argument(
            "show and clear take no entries, set needs --entry, and only replace takes --default");
      }
      for (const auto& text : route_entries) {
        operation.entries.push_back(parse_route_entry(text));
      }
      if (!route_default.empty()) {
        operation.default_target = parse_route_target(route_default);
      }
      request = std::move(operation);
    } else if (*model_command) {
      model_control operation;
      operation.header                    = instance_header(transaction, model.instance);
      operation.expected_model_generation = model.model_generation;
      operation.action                    = parse_model_action(model.action);
      operation.model_id                  = model.model_id;
      operation.model_version             = model.version;
      operation.artifact                  = read_artifact(model.artifact);
      request                             = std::move(operation);
    } else {
      throw std::invalid_argument("no management operation selected");
    }

    const std::optional<uid_t> expected_uid =
        server_uid_option->count() != 0 ? std::optional<uid_t>{static_cast<uid_t>(server_uid)} : std::nullopt;
    local_management_client client(socket_path, expected_uid);
    auto                    result = client.call(request, std::chrono::milliseconds{timeout_ms});
    if (!result) {
      std::cerr << "transport-error=" << safe_text(result.detail) << '\n';
      return 1;
    }
    print_response(*result.value);
    // Exit contract (shared with ocudu_dappctl.py): 0 = accepted,
    // 1 = transport failure, 2 = invalid input, 3 = agent refused.
    switch (result.value->code) {
      case result_code::ok:
      case result_code::dry_run_ok:
      case result_code::accepted:
        return 0;
      default:
        return 3;
    }
  } catch (const std::exception& error) {
    std::cerr << "input-error=" << safe_text(error.what()) << '\n';
    return 2;
  }
}
