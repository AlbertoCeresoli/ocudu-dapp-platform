// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/artifact_verifier.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

using namespace ocudu::dapp;

namespace {

constexpr std::array<uint32_t, 64> round_constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

uint32_t rotate_right(uint32_t value, unsigned amount) noexcept
{
  return (value >> amount) | (value << (32U - amount));
}

class sha256
{
public:
  void update(const uint8_t* bytes, std::size_t count) noexcept
  {
    total_bytes += count;
    while (count != 0) {
      const auto copied = std::min(count, block.size() - block_size);
      std::memcpy(block.data() + block_size, bytes, copied);
      block_size += copied;
      bytes += copied;
      count -= copied;
      if (block_size == block.size()) {
        transform(block.data());
        block_size = 0;
      }
    }
  }

  artifact_digest finish() noexcept
  {
    const uint64_t bit_count = total_bytes * 8;
    block[block_size++]      = 0x80;
    if (block_size > 56) {
      std::fill(block.begin() + block_size, block.end(), 0);
      transform(block.data());
      block_size = 0;
    }
    std::fill(block.begin() + block_size, block.begin() + 56, 0);
    for (unsigned i = 0; i != 8; ++i) {
      block[63 - i] = static_cast<uint8_t>(bit_count >> (i * 8));
    }
    transform(block.data());

    artifact_digest result{};
    for (std::size_t i = 0; i != state.size(); ++i) {
      result[i * 4]     = static_cast<uint8_t>(state[i] >> 24);
      result[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
      result[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
      result[i * 4 + 3] = static_cast<uint8_t>(state[i]);
    }
    return result;
  }

private:
  void transform(const uint8_t* input) noexcept
  {
    std::array<uint32_t, 64> words{};
    for (unsigned i = 0; i != 16; ++i) {
      words[i] = (static_cast<uint32_t>(input[i * 4]) << 24) | (static_cast<uint32_t>(input[i * 4 + 1]) << 16) |
                 (static_cast<uint32_t>(input[i * 4 + 2]) << 8) | input[i * 4 + 3];
    }
    for (unsigned i = 16; i != 64; ++i) {
      const uint32_t s0 = rotate_right(words[i - 15], 7) ^ rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3);
      const uint32_t s1 = rotate_right(words[i - 2], 17) ^ rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10);
      words[i]          = words[i - 16] + s0 + words[i - 7] + s1;
    }

    auto working = state;
    for (unsigned i = 0; i != 64; ++i) {
      const uint32_t choose = (working[4] & working[5]) ^ (~working[4] & working[6]);
      const uint32_t sum1   = rotate_right(working[4], 6) ^ rotate_right(working[4], 11) ^ rotate_right(working[4], 25);
      const uint32_t temporary1 = working[7] + sum1 + choose + round_constants[i] + words[i];
      const uint32_t majority   = (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
      const uint32_t sum0 = rotate_right(working[0], 2) ^ rotate_right(working[0], 13) ^ rotate_right(working[0], 22);
      const uint32_t temporary2 = sum0 + majority;
      for (unsigned j = 7; j != 0; --j) {
        working[j] = working[j - 1];
      }
      working[4] += temporary1;
      working[0] = temporary1 + temporary2;
    }
    for (unsigned i = 0; i != state.size(); ++i) {
      state[i] += working[i];
    }
  }

  std::array<uint32_t, 8>
      state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<uint8_t, 64> block{};
  std::size_t             block_size{};
  uint64_t                total_bytes{};
};

artifact_error validate_elf(int fd) noexcept
{
  Elf64_Ehdr header{};
  if (::pread(fd, &header, sizeof(header), 0) != static_cast<ssize_t>(sizeof(header))) {
    return artifact_error::invalid_elf;
  }
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_type != ET_DYN) {
    return artifact_error::invalid_elf;
  }
#if defined(__aarch64__)
  if (header.e_machine != EM_AARCH64) {
    return artifact_error::incompatible_architecture;
  }
#elif defined(__x86_64__)
  if (header.e_machine != EM_X86_64) {
    return artifact_error::incompatible_architecture;
  }
#else
  return artifact_error::incompatible_architecture;
#endif
  if (header.e_phnum > 256 || header.e_phentsize != sizeof(Elf64_Phdr)) {
    return artifact_error::invalid_elf;
  }
  for (uint16_t i = 0; i != header.e_phnum; ++i) {
    Elf64_Phdr program{};
    const auto offset = static_cast<off_t>(header.e_phoff + static_cast<uint64_t>(i) * sizeof(program));
    if (::pread(fd, &program, sizeof(program), offset) != static_cast<ssize_t>(sizeof(program))) {
      return artifact_error::invalid_elf;
    }
    if (program.p_type == PT_GNU_STACK && (program.p_flags & PF_X) != 0) {
      return artifact_error::executable_stack;
    }
  }
  return artifact_error::none;
}

bool path_is_below(const std::filesystem::path& child, const std::filesystem::path& parent)
{
  auto child_it  = child.begin();
  auto parent_it = parent.begin();
  for (; parent_it != parent.end(); ++parent_it, ++child_it) {
    if (child_it == child.end() || *child_it != *parent_it) {
      return false;
    }
  }
  return child != parent;
}

} // namespace

approved_artifact::approved_artifact(int             descriptor,
                                     std::string     canonical_path,
                                     artifact_digest digest_,
                                     uint64_t        size_) noexcept :
  fd(descriptor), path(std::move(canonical_path)), content_digest(digest_), size(size_)
{
}

approved_artifact::approved_artifact(approved_artifact&& other) noexcept :
  fd(other.fd), path(std::move(other.path)), content_digest(other.content_digest), size(other.size)
{
  other.fd = -1;
}

approved_artifact& approved_artifact::operator=(approved_artifact&& other) noexcept
{
  if (this != &other) {
    if (fd >= 0) {
      ::close(fd);
    }
    fd             = other.fd;
    path           = std::move(other.path);
    content_digest = other.content_digest;
    size           = other.size;
    other.fd       = -1;
  }
  return *this;
}

approved_artifact::~approved_artifact()
{
  if (fd >= 0) {
    ::close(fd);
  }
}

artifact_verifier::artifact_verifier(std::string catalog_root_, uint64_t maximum_artifact_bytes) :
  maximum_bytes(maximum_artifact_bytes)
{
  std::error_code error;
  auto            canonical = std::filesystem::canonical(catalog_root_, error);
  if (error || !std::filesystem::is_directory(canonical)) {
    throw std::invalid_argument("dApp catalog root is not an existing directory");
  }
  root = canonical.string();
}

artifact_verification_result artifact_verifier::verify(const std::string&     candidate,
                                                       const artifact_digest& expected_digest) const
{
  return inspect_impl(candidate, &expected_digest);
}

artifact_inspection_result artifact_verifier::inspect(const std::string& candidate) const
{
  auto result = inspect_impl(candidate, nullptr);
  if (!result) {
    return {result.error, {}, 0, {}, std::move(result.detail)};
  }
  return {artifact_error::none,
          result.artifact->digest(),
          result.artifact->size_bytes(),
          result.artifact->canonical_path(),
          {}};
}

artifact_verification_result artifact_verifier::inspect_impl(const std::string&     candidate,
                                                             const artifact_digest* expected_digest) const
{
  std::error_code error;
  const auto      canonical = std::filesystem::canonical(candidate, error);
  if (error || !path_is_below(canonical, root)) {
    return {artifact_error::path_outside_catalog, nullptr, "candidate does not resolve below catalog root"};
  }
  struct stat link_status {};
  if (::lstat(candidate.c_str(), &link_status) != 0 || S_ISLNK(link_status.st_mode)) {
    return {artifact_error::open_failed, nullptr, "symbolic links are not admitted"};
  }

  const int fd = ::open(canonical.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return {artifact_error::open_failed, nullptr, std::strerror(errno)};
  }
  struct fd_guard {
    int value;
    ~fd_guard()
    {
      if (value >= 0)
        ::close(value);
    }
  } guard{fd};

  struct stat status {};
  if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
    return {artifact_error::not_regular_file, nullptr, "artifact is not a regular file"};
  }
  const uid_t effective_uid = ::geteuid();
  if (status.st_uid != effective_uid && status.st_uid != 0) {
    return {artifact_error::insecure_owner, nullptr, "artifact owner is neither root nor the DU user"};
  }
  if ((status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    return {artifact_error::insecure_permissions, nullptr, "group/other writable artifact rejected"};
  }
  if (status.st_size < 0 || static_cast<uint64_t>(status.st_size) > maximum_bytes) {
    return {artifact_error::file_too_large, nullptr, "artifact exceeds configured byte limit"};
  }
  if (const auto elf_error = validate_elf(fd); elf_error != artifact_error::none) {
    return {elf_error, nullptr, "artifact is not a compatible 64-bit shared ELF object"};
  }

  sha256                     hasher;
  std::array<uint8_t, 65536> buffer{};
  uint64_t                   offset = 0;
  while (offset != static_cast<uint64_t>(status.st_size)) {
    const auto wanted = static_cast<std::size_t>(std::min<uint64_t>(buffer.size(), status.st_size - offset));
    const auto count  = ::pread(fd, buffer.data(), wanted, static_cast<off_t>(offset));
    if (count <= 0) {
      return {artifact_error::read_failed, nullptr, "failed while hashing artifact"};
    }
    hasher.update(buffer.data(), static_cast<std::size_t>(count));
    offset += static_cast<uint64_t>(count);
  }
  const auto digest = hasher.finish();
  if (expected_digest != nullptr && digest != *expected_digest) {
    return {artifact_error::digest_mismatch, nullptr, "artifact SHA-256 differs from approved manifest"};
  }

  guard.value = -1;
  return {artifact_error::none,
          std::unique_ptr<approved_artifact>(
              new approved_artifact(fd, canonical.string(), digest, static_cast<uint64_t>(status.st_size))),
          {}};
}

const char* ocudu::dapp::to_string(artifact_error error) noexcept
{
  static constexpr const char* names[] = {"none",
                                          "invalid catalog root",
                                          "path outside catalog",
                                          "open failed",
                                          "not a regular file",
                                          "insecure owner",
                                          "insecure permissions",
                                          "file too large",
                                          "read failed",
                                          "invalid ELF",
                                          "incompatible architecture",
                                          "executable stack",
                                          "digest mismatch"};
  const auto                   index   = static_cast<unsigned>(error);
  return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown artifact error";
}

std::string ocudu::dapp::to_hex(const artifact_digest& digest)
{
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (uint8_t byte : digest) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
}

artifact_digest ocudu::dapp::sha256_bytes(const uint8_t* data, std::size_t size) noexcept
{
  sha256 hasher;
  if (size != 0) {
    hasher.update(data, size);
  }
  return hasher.finish();
}
