// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/shared_region.h"
#include "ocudu/dapp/e3/shared_region_layout.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace ocudu::dapp::e3;

namespace {

uint64_t realtime_ns() noexcept
{
  const auto value =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  return value > 0 ? static_cast<uint64_t>(value) : 0;
}

void store_release(uint64_t* target, uint64_t value) noexcept
{
  __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) noexcept
{
  return (value + alignment - 1) / alignment * alignment;
}

} // namespace

std::pair<std::unique_ptr<e3_shared_region>, std::string>
e3_shared_region::create(const e3_shared_region_config& config) noexcept
{
  if (config.name.size() < 2 || config.name.front() != '/' || config.name.size() > 63 || config.name == "/." ||
      config.name == "/.." ||
      config.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-", 1) !=
          std::string::npos) {
    return {nullptr, "shared region name must be a POSIX shm name of at most 63 characters starting with '/'"};
  }
  if (config.row_payload_bytes == 0 || config.row_payload_bytes > 256ULL * 1024 * 1024 || config.nof_rows == 0) {
    return {nullptr, "shared region needs a non-empty row and at least one row"};
  }
  const uint64_t row_bytes = align_up(OCUDU_E3_SHARED_ROW_HEADER_BYTES_V1 + config.row_payload_bytes, 64);
  const uint64_t bytes     = OCUDU_E3_SHARED_REGION_HEADER_BYTES_V1 + row_bytes * config.nof_rows;
  if (bytes > 256ULL * 1024 * 1024) {
    return {nullptr, "shared region exceeds 256 MiB"};
  }
  // Never unlink a colliding live region. Operator cleanup of stale names is
  // separate from admission, and O_EXCL preserves the current owner's mapping.
  const int descriptor = ::shm_open(config.name.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, config.mode);
  if (descriptor < 0) {
    return {nullptr, std::string("shm_open failed: ") + std::strerror(errno)};
  }
  // shm_open applies the umask; make the requested mode explicit.
  if (::fchmod(descriptor, config.mode) != 0 || ::ftruncate(descriptor, static_cast<off_t>(bytes)) != 0) {
    const std::string detail = std::string("ftruncate failed: ") + std::strerror(errno);
    ::close(descriptor);
    ::shm_unlink(config.name.c_str());
    return {nullptr, detail};
  }
  // Reserve tmpfs backing before touching pages: ENOSPC must reject the
  // subscription rather than SIGBUS the carrier/gNB on the first write.
  const int allocation_error = ::posix_fallocate(descriptor, 0, static_cast<off_t>(bytes));
  if (allocation_error != 0) {
    ::close(descriptor);
    ::shm_unlink(config.name.c_str());
    return {nullptr, std::string("shared region backing allocation failed: ") + std::strerror(allocation_error)};
  }
  // MAP_POPULATE pre-faults every page so the first rows do not pay page
  // faults on the carrier worker; huge pages are requested best-effort.
  void* base = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, descriptor, 0);
  if (base == MAP_FAILED) {
    const std::string detail = std::string("mmap failed: ") + std::strerror(errno);
    ::close(descriptor);
    ::shm_unlink(config.name.c_str());
    return {nullptr, detail};
  }
#ifdef MADV_HUGEPAGE
  ::madvise(base, bytes, MADV_HUGEPAGE);
#endif
  std::unique_ptr<e3_shared_region> region(new e3_shared_region(config, descriptor, base, bytes));
  region->row_bytes = row_bytes;
  auto* header      = reinterpret_cast<ocudu_e3_shared_region_header_v1*>(region->base);
  std::memset(header, 0, sizeof(*header));
  header->version          = OCUDU_E3_SHARED_REGION_VERSION_V1;
  header->header_bytes     = OCUDU_E3_SHARED_REGION_HEADER_BYTES_V1;
  header->row_header_bytes = OCUDU_E3_SHARED_ROW_HEADER_BYTES_V1;
  header->nof_rows         = config.nof_rows;
  header->row_bytes        = row_bytes;
  header->write_index      = OCUDU_E3_SHARED_REGION_NO_ROW;
  header->generation       = 0;
  for (uint16_t row = 0; row != config.nof_rows; ++row) {
    auto* row_header = reinterpret_cast<ocudu_e3_shared_row_header_v1*>(
        region->base + OCUDU_E3_SHARED_REGION_HEADER_BYTES_V1 + row_bytes * row);
    std::memset(row_header, 0, sizeof(*row_header));
  }
  // The magic is written last so a reader never sees a half-initialised header.
  store_release(&header->magic, OCUDU_E3_SHARED_REGION_MAGIC_V1);
  return {std::move(region), std::string{}};
}

e3_shared_region::e3_shared_region(e3_shared_region_config config_, int fd_, void* base_, uint64_t bytes_) noexcept :
  config(std::move(config_)), fd(fd_), base(static_cast<uint8_t*>(base_)), bytes(bytes_)
{
}

e3_shared_region::~e3_shared_region()
{
  if (base != nullptr) {
    ::munmap(base, bytes);
  }
  if (fd >= 0) {
    ::close(fd);
  }
  ::shm_unlink(config.name.c_str());
}

std::optional<e3_shared_region_write> e3_shared_region::write(uint64_t                   sequence,
                                                              uint64_t                   route_epoch,
                                                              uint16_t                   telemetry_id,
                                                              uint16_t                   cell_id,
                                                              ocudu::span<const uint8_t> tensor) noexcept
{
  if (!begin(sequence, route_epoch, telemetry_id, cell_id, tensor.size()) || !append(tensor)) {
    abort();
    return std::nullopt;
  }
  return commit();
}

bool e3_shared_region::begin(uint64_t sequence,
                             uint64_t route_epoch,
                             uint16_t telemetry_id,
                             uint16_t cell_id,
                             uint64_t byte_length) noexcept
{
  if (in_progress) {
    abort();
  }
  if (byte_length > config.row_payload_bytes) {
    return false;
  }
  writing_row               = next_row;
  next_row                  = static_cast<uint16_t>((next_row + 1) % config.nof_rows);
  writing_length            = byte_length;
  cursor                    = 0;
  in_progress               = true;
  const uint64_t row_offset = OCUDU_E3_SHARED_REGION_HEADER_BYTES_V1 + row_bytes * writing_row;
  auto*          row_header = reinterpret_cast<ocudu_e3_shared_row_header_v1*>(base + row_offset);
  // Seqlock: odd while writing.
  store_release(&row_header->generation, generation + 1);
  std::atomic_thread_fence(std::memory_order_release);
  row_header->sequence     = sequence;
  row_header->byte_length  = byte_length;
  row_header->telemetry_id = telemetry_id;
  row_header->cell_id      = cell_id;
  row_header->realtime_ns  = realtime_ns();
  row_header->route_epoch  = route_epoch;
  return true;
}

bool e3_shared_region::append(ocudu::span<const uint8_t> chunk) noexcept
{
  if (!in_progress || chunk.size() > writing_length - cursor) {
    return false;
  }
  if (!chunk.empty()) {
    const uint64_t row_offset = OCUDU_E3_SHARED_REGION_HEADER_BYTES_V1 + row_bytes * writing_row;
    std::memcpy(base + row_offset + OCUDU_E3_SHARED_ROW_HEADER_BYTES_V1 + cursor, chunk.data(), chunk.size());
    cursor += chunk.size();
  }
  return true;
}

std::optional<e3_shared_region_write> e3_shared_region::commit() noexcept
{
  if (!in_progress || cursor != writing_length) {
    abort();
    return std::nullopt;
  }
  const uint64_t row_offset = OCUDU_E3_SHARED_REGION_HEADER_BYTES_V1 + row_bytes * writing_row;
  auto*          row_header = reinterpret_cast<ocudu_e3_shared_row_header_v1*>(base + row_offset);
  const uint64_t done       = generation + 2;
  std::atomic_thread_fence(std::memory_order_release);
  store_release(&row_header->generation, done);
  generation   = done;
  in_progress  = false;
  auto* header = reinterpret_cast<ocudu_e3_shared_region_header_v1*>(base);
  store_release(&header->generation, done);
  store_release(&header->write_index, writing_row);
  return e3_shared_region_write{writing_row, row_offset + OCUDU_E3_SHARED_ROW_HEADER_BYTES_V1, done, writing_length};
}

void e3_shared_region::abort() noexcept
{
  if (!in_progress) {
    return;
  }
  const uint64_t row_offset = OCUDU_E3_SHARED_REGION_HEADER_BYTES_V1 + row_bytes * writing_row;
  auto*          row_header = reinterpret_cast<ocudu_e3_shared_row_header_v1*>(base + row_offset);
  row_header->byte_length   = 0;
  // Advance to an even generation the descriptor of no object ever named.
  const uint64_t done = generation + 2;
  std::atomic_thread_fence(std::memory_order_release);
  store_release(&row_header->generation, done);
  generation  = done;
  in_progress = false;
}

ocudu::span<const uint8_t> e3_shared_region::bytes_of(const e3_shared_region_write& written) const noexcept
{
  if (written.row_index >= config.nof_rows || written.byte_offset > bytes ||
      written.byte_length > bytes - written.byte_offset || written.byte_length > config.row_payload_bytes ||
      written.generation == 0 || written.generation % 2 != 0) {
    return {};
  }
  const uint64_t row_offset = OCUDU_E3_SHARED_REGION_HEADER_BYTES_V1 + row_bytes * written.row_index;
  const auto*    row        = reinterpret_cast<const ocudu_e3_shared_row_header_v1*>(base + row_offset);
  if (written.byte_offset != row_offset + OCUDU_E3_SHARED_ROW_HEADER_BYTES_V1 ||
      __atomic_load_n(&row->generation, __ATOMIC_ACQUIRE) != written.generation ||
      row->byte_length != written.byte_length) {
    return {};
  }
  return {base + written.byte_offset, static_cast<std::size_t>(written.byte_length)};
}

e3ap_shared_region e3_shared_region::descriptor() const noexcept
{
  e3ap_shared_region result;
  result.memory_domain  = e3ap_memory_domain::posix_shared;
  result.region_name    = config.name;
  result.region_bytes   = bytes;
  result.header_bytes   = OCUDU_E3_SHARED_REGION_HEADER_BYTES_V1;
  result.row_bytes      = row_bytes;
  result.nof_rows       = config.nof_rows;
  result.layout_version = OCUDU_E3_SHARED_REGION_VERSION_V1;
  return result;
}
