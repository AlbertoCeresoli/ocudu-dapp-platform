// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// Helpers shared by the asn1c-backed APER codecs. Include after the generated
// asn1c headers; every function throws on malformed or oversize values so the
// codec entry points can convert failures into codec_result diagnostics.

extern "C" {
#include "BIT_STRING.h"
#include "OCTET_STRING.h"
#include "aper_decoder.h"
#include "aper_encoder.h"
#include "asn_SEQUENCE_OF.h"
#include "constraints.h"
}

#include "ocudu/adt/span.h"
#include "ocudu/dapp/management/types.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ocudu::dapp::e3::asn1_detail {

template <typename T>
T* make_asn()
{
  auto* value = static_cast<T*>(std::calloc(1, sizeof(T)));
  if (value == nullptr) {
    throw std::bad_alloc();
  }
  return value;
}

template <typename T, typename Sequence>
T& append_asn(Sequence& sequence)
{
  T* value = make_asn<T>();
  if (ASN_SEQUENCE_ADD(&sequence.list, value) != 0) {
    std::free(value);
    throw std::bad_alloc();
  }
  return *value;
}

inline void set_octets(OCTET_STRING_t& destination, const void* data, std::size_t size)
{
  if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      OCTET_STRING_fromBuf(&destination, size != 0 ? static_cast<const char*>(data) : "", static_cast<int>(size)) !=
          0) {
    throw std::bad_alloc();
  }
}

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

inline void set_text(OCTET_STRING_t& destination, std::string_view value)
{
  if (!valid_utf8(reinterpret_cast<const uint8_t*>(value.data()), value.size())) {
    throw std::runtime_error("invalid UTF-8 text");
  }
  set_octets(destination, value.data(), value.size());
}

inline void set_octets(OCTET_STRING_t& destination, const std::string& value)
{
  set_text(destination, value);
}

inline void set_octets(OCTET_STRING_t& destination, const std::vector<uint8_t>& value)
{
  set_octets(destination, value.data(), value.size());
}

inline void set_octets(OCTET_STRING_t& destination, ocudu::span<const uint8_t> value)
{
  set_octets(destination, value.data(), value.size());
}

inline std::string read_string(const OCTET_STRING_t& value, std::size_t maximum)
{
  if (value.size > maximum || (value.size != 0 && value.buf == nullptr)) {
    throw std::runtime_error("invalid bounded string");
  }
  if (!valid_utf8(value.buf, value.size)) {
    throw std::runtime_error("invalid UTF-8 text");
  }
  return value.size == 0 ? std::string{} : std::string(reinterpret_cast<const char*>(value.buf), value.size);
}

inline std::vector<uint8_t> read_bytes(const OCTET_STRING_t& value, std::size_t maximum)
{
  if (value.size > maximum || (value.size != 0 && value.buf == nullptr)) {
    throw std::runtime_error("invalid bounded byte string");
  }
  return value.size == 0 ? std::vector<uint8_t>{} : std::vector<uint8_t>(value.buf, value.buf + value.size);
}

inline void set_identifier(OCTET_STRING_t& destination, const management::identifier& value)
{
  set_octets(destination, value.bytes.data(), value.bytes.size());
}

inline management::identifier read_identifier(const OCTET_STRING_t& value)
{
  if (value.size != management::identifier{}.bytes.size() || value.buf == nullptr) {
    throw std::runtime_error("invalid identifier");
  }
  management::identifier result;
  std::memcpy(result.bytes.data(), value.buf, result.bytes.size());
  return result;
}

inline void set_u64(OCTET_STRING_t& destination, uint64_t value)
{
  uint8_t bytes[sizeof(value)];
  for (std::size_t index = 0; index != sizeof(value); ++index) {
    bytes[sizeof(value) - index - 1] = static_cast<uint8_t>(value >> (index * 8U));
  }
  set_octets(destination, bytes, sizeof(bytes));
}

inline uint64_t read_u64(const OCTET_STRING_t& value)
{
  if (value.size != sizeof(uint64_t) || value.buf == nullptr) {
    throw std::runtime_error("invalid fixed-width unsigned integer");
  }
  uint64_t result = 0;
  for (std::size_t index = 0; index != value.size; ++index) {
    result = (result << 8U) | value.buf[index];
  }
  return result;
}

template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
void set_u64(T& destination, uint64_t value)
{
  if constexpr (std::is_signed_v<T>) {
    if (value > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
      throw std::runtime_error("unsigned integer exceeds generated native type");
    }
  } else if (value > std::numeric_limits<T>::max()) {
    throw std::runtime_error("unsigned integer exceeds generated native type");
  }
  destination = static_cast<T>(value);
}

template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
uint64_t read_u64(const T& value)
{
  if constexpr (std::is_signed_v<T>) {
    if (value < 0) {
      throw std::runtime_error("negative unsigned integer");
    }
  }
  return static_cast<uint64_t>(value);
}

inline void set_i64(OCTET_STRING_t& destination, int64_t value)
{
  const uint64_t encoded = value >= 0 ? static_cast<uint64_t>(value)
                                      : std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(-(value + 1));
  set_u64(destination, encoded);
}

inline int64_t read_i64(const OCTET_STRING_t& value)
{
  const uint64_t encoded = read_u64(value);
  if ((encoded & (uint64_t{1} << 63U)) == 0) {
    return static_cast<int64_t>(encoded);
  }
  const uint64_t magnitude = (~encoded) + 1U;
  return magnitude == (uint64_t{1} << 63U) ? std::numeric_limits<int64_t>::min() : -static_cast<int64_t>(magnitude);
}

template <typename T, std::enable_if_t<std::is_integral_v<T> && std::is_signed_v<T>, int> = 0>
void set_i64(T& destination, int64_t value)
{
  if (value < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
      value > static_cast<int64_t>(std::numeric_limits<T>::max())) {
    throw std::runtime_error("signed integer exceeds generated native type");
  }
  destination = static_cast<T>(value);
}

template <typename T, std::enable_if_t<std::is_integral_v<T> && std::is_signed_v<T>, int> = 0>
int64_t read_i64(const T& value)
{
  return static_cast<int64_t>(value);
}

inline void set_bits(BIT_STRING_t& destination, uint64_t value, std::size_t bytes)
{
  destination.buf = static_cast<uint8_t*>(std::calloc(bytes, 1));
  if (destination.buf == nullptr) {
    throw std::bad_alloc();
  }
  destination.size        = bytes;
  destination.bits_unused = 0;
  for (std::size_t index = 0; index != bytes; ++index) {
    destination.buf[bytes - index - 1] = static_cast<uint8_t>(value >> (index * 8));
  }
}

inline uint64_t read_bits(const BIT_STRING_t& value, std::size_t bytes)
{
  if (value.size != bytes || value.bits_unused != 0 || value.buf == nullptr) {
    throw std::runtime_error("invalid fixed-size bit string");
  }
  uint64_t result = 0;
  for (std::size_t index = 0; index != bytes; ++index) {
    result = (result << 8U) | value.buf[index];
  }
  return result;
}

/// Bounded-range integer read for constrained INTEGER fields.
inline uint64_t read_ranged(long value, uint64_t minimum, uint64_t maximum, const char* what)
{
  if (value < 0 || static_cast<uint64_t>(value) < minimum || static_cast<uint64_t>(value) > maximum) {
    throw std::runtime_error(std::string("out-of-range ") + what);
  }
  return static_cast<uint64_t>(value);
}

template <typename AsnType>
std::vector<uint8_t> encode_asn(asn_TYPE_descriptor_t& descriptor, const AsnType& value, std::size_t maximum_bytes)
{
  std::array<char, 256> error{};
  std::size_t           error_size = error.size();
  if (asn_check_constraints(&descriptor, &value, error.data(), &error_size) != 0) {
    throw std::runtime_error(std::string("ASN.1 constraint failed: ") + error.data());
  }
  struct encoding_sink {
    std::vector<uint8_t> bytes;
    std::size_t          maximum{};
    bool                 allocation_failed{};
    bool                 too_large{};
  } sink;
  sink.maximum = maximum_bytes;
  sink.bytes.reserve(4096);
  const auto consume = [](const void* data, std::size_t size, void* key) noexcept {
    auto& output = *static_cast<encoding_sink*>(key);
    if (size > output.maximum - output.bytes.size()) {
      output.too_large = true;
      return -1;
    }
    try {
      const auto* first = static_cast<const uint8_t*>(data);
      output.bytes.insert(output.bytes.end(), first, first + size);
      return 0;
    } catch (...) {
      output.allocation_failed = true;
      return -1;
    }
  };
  const auto encoded = aper_encode(&descriptor, nullptr, &value, consume, &sink);
  if (encoded.encoded < 0) {
    if (sink.too_large) {
      throw std::runtime_error("APER encoding exceeded its bound");
    }
    if (sink.allocation_failed) {
      throw std::bad_alloc();
    }
    const char* type = encoded.failed_type != nullptr ? encoded.failed_type->name : "unknown type";
    throw std::runtime_error(std::string("APER encoding failed at ") + type);
  }
  if ((static_cast<std::size_t>(encoded.encoded) + 7) / 8 != sink.bytes.size()) {
    throw std::runtime_error("APER encoder returned an inconsistent size");
  }
  return std::move(sink.bytes);
}

/// Owns one decoded asn1c structure and frees it with the type descriptor.
template <typename AsnType>
class asn_owner
{
public:
  asn_owner(asn_TYPE_descriptor_t& descriptor_, AsnType* value_ = nullptr) : descriptor(&descriptor_), value(value_) {}
  asn_owner(asn_owner&& other) noexcept : descriptor(other.descriptor), value(other.value) { other.value = nullptr; }
  asn_owner(const asn_owner&)            = delete;
  asn_owner& operator=(const asn_owner&) = delete;
  ~asn_owner()
  {
    if (value != nullptr) {
      ASN_STRUCT_FREE(*descriptor, value);
    }
  }

  AsnType*  get() const noexcept { return value; }
  AsnType** slot() noexcept { return &value; }

private:
  asn_TYPE_descriptor_t* descriptor;
  AsnType*               value;
};

template <typename AsnType>
asn_owner<AsnType> decode_asn(asn_TYPE_descriptor_t&     descriptor,
                              ocudu::span<const uint8_t> bytes,
                              std::size_t                maximum_bytes,
                              const char*                what)
{
  if (bytes.empty() || bytes.size() > maximum_bytes) {
    throw std::runtime_error(std::string("invalid APER size for ") + what);
  }
  asn_owner<AsnType> result(descriptor);
  asn_codec_ctx_t    context{};
  context.max_stack_size = 64 * 1024;
  const auto decoded =
      aper_decode_complete(&context, &descriptor, reinterpret_cast<void**>(result.slot()), bytes.data(), bytes.size());
  if (decoded.code != RC_OK || result.get() == nullptr) {
    throw std::runtime_error(std::string("malformed APER ") + what);
  }
  std::array<char, 256> error{};
  std::size_t           error_size = error.size();
  if (asn_check_constraints(&descriptor, result.get(), error.data(), &error_size) != 0) {
    throw std::runtime_error(std::string("invalid APER ") + what + ": " + error.data());
  }
  return result;
}

} // namespace ocudu::dapp::e3::asn1_detail
