// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ocudu::dapp::detail {

using bounded_json = nlohmann::json;

enum class bounded_json_error { none, malformed, duplicate_key, excessive_depth, invalid_structure };

struct bounded_json_result {
  std::optional<bounded_json> document;
  bounded_json_error          error{bounded_json_error::none};
};

/// SAX-only structural pass used before DOM parsing. Keeping duplicate-key and
/// depth tracking outside the DOM callback avoids parser-controlled indices.
class bounded_json_sax final : public nlohmann::json_sax<bounded_json>
{
public:
  explicit bounded_json_sax(std::size_t maximum_depth_) : maximum_depth(maximum_depth_) {}

  bool null() override { return true; }
  bool boolean(bool) override { return true; }
  bool number_integer(number_integer_t) override { return true; }
  bool number_unsigned(number_unsigned_t) override { return true; }
  bool number_float(number_float_t, const string_t&) override { return true; }
  bool string(string_t&) override { return true; }
  bool binary(binary_t&) override { return true; }

  bool start_object(std::size_t) override { return push(true); }
  bool start_array(std::size_t) override { return push(false); }

  bool key(string_t& value) override
  {
    if (containers.empty() || !containers.back().object) {
      error = bounded_json_error::invalid_structure;
      return false;
    }
    if (!containers.back().keys.insert(value).second) {
      error = bounded_json_error::duplicate_key;
      return false;
    }
    return true;
  }

  bool end_object() override { return pop(true); }
  bool end_array() override { return pop(false); }

  bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override
  {
    if (error == bounded_json_error::none) {
      error = bounded_json_error::malformed;
    }
    return false;
  }

  bounded_json_error result(bool parser_succeeded) const noexcept
  {
    if (error != bounded_json_error::none) {
      return error;
    }
    return parser_succeeded && containers.empty() ? bounded_json_error::none : bounded_json_error::malformed;
  }

private:
  struct container {
    explicit container(bool object_) : object(object_) {}
    bool                  object;
    std::set<std::string> keys;
  };

  bool push(bool object)
  {
    if (containers.size() >= maximum_depth) {
      error = bounded_json_error::excessive_depth;
      return false;
    }
    containers.emplace_back(object);
    return true;
  }

  bool pop(bool object)
  {
    if (containers.empty() || containers.back().object != object) {
      error = bounded_json_error::invalid_structure;
      return false;
    }
    containers.pop_back();
    return true;
  }

  std::size_t            maximum_depth;
  std::vector<container> containers;
  bounded_json_error     error{bounded_json_error::none};
};

template <typename Iterator>
bounded_json_result parse_bounded_json(Iterator first, Iterator last, std::size_t maximum_depth)
{
  bounded_json_sax validator(maximum_depth);
  const bool       valid = bounded_json::sax_parse(first, last, &validator);
  const auto       error = validator.result(valid);
  if (error != bounded_json_error::none) {
    return {std::nullopt, error};
  }
  auto document = bounded_json::parse(first, last, nullptr, false);
  if (document.is_discarded()) {
    return {std::nullopt, bounded_json_error::malformed};
  }
  return {std::move(document), bounded_json_error::none};
}

} // namespace ocudu::dapp::detail
