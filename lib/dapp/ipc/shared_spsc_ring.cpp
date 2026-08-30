// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/ipc/shared_spsc_ring.h"
#include "ocudu/dapp/process/v1/shared_ring.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace ocudu::dapp::ipc;

namespace {

constexpr uint64_t align64(uint64_t value) noexcept
{
  return (value + OCUDU_DAPP_SHARED_RING_ALIGNMENT_V1 - 1U) & ~uint64_t{OCUDU_DAPP_SHARED_RING_ALIGNMENT_V1 - 1U};
}

using ring_header = ocudu_dapp_shared_ring_header_v1;
using slot_header = ocudu_dapp_shared_ring_slot_v1;

static_assert(sizeof(ring_header) <= OCUDU_DAPP_SHARED_RING_HEADER_BYTES_V1);
static_assert(offsetof(ring_header, write_index) == 40);
static_assert(offsetof(ring_header, read_index) == 48);
static_assert(offsetof(ring_header, epoch) == 56);
static_assert(sizeof(slot_header) == OCUDU_DAPP_SHARED_RING_ALIGNMENT_V1);

uint64_t atomic_load_relaxed(const uint64_t& value) noexcept
{
  return __atomic_load_n(&value, __ATOMIC_RELAXED);
}

uint64_t atomic_load_acquire(const uint64_t& value) noexcept
{
  return __atomic_load_n(&value, __ATOMIC_ACQUIRE);
}

void atomic_store_relaxed(uint64_t& value, uint64_t desired) noexcept
{
  __atomic_store_n(&value, desired, __ATOMIC_RELAXED);
}

void atomic_store_release(uint64_t& value, uint64_t desired) noexcept
{
  __atomic_store_n(&value, desired, __ATOMIC_RELEASE);
}

std::string error_text(const char* operation)
{
  return std::string(operation) + ": " + std::strerror(errno);
}

} // namespace

struct shared_spsc_ring::mapping {
  int               fd{-1};
  int               completion_fd{-1};
  void*             address{MAP_FAILED};
  std::size_t       size{};
  ring_header*      header{};
  uint64_t          producer_read_index{};
  uint64_t          consumer_read_index{};
  std::atomic_flag  producer_guard = ATOMIC_FLAG_INIT;
  std::atomic<bool> lease_active{false};

  ~mapping()
  {
    if (address != MAP_FAILED) {
      ::munmap(address, size);
    }
    if (fd >= 0) {
      ::close(fd);
    }
    if (completion_fd >= 0) {
      ::close(completion_fd);
    }
  }

  slot_header* slot(uint64_t index) const noexcept
  {
    auto* base = static_cast<uint8_t*>(address) + header->header_size;
    return reinterpret_cast<slot_header*>(base + (index % header->slots) * header->slot_stride);
  }
};

namespace {

struct flag_release {
  std::atomic_flag& flag;
  ~flag_release() { flag.clear(std::memory_order_release); }
};

} // namespace

ring_open_result shared_spsc_ring::map_fd(int source_fd, int source_completion_fd, endpoint_role role)
{
  const int owned_fd            = ::fcntl(source_fd, F_DUPFD_CLOEXEC, 0);
  const int owned_completion_fd = ::fcntl(source_completion_fd, F_DUPFD_CLOEXEC, 0);
  if (owned_fd < 0 || owned_completion_fd < 0) {
    if (owned_fd >= 0) {
      ::close(owned_fd);
    }
    if (owned_completion_fd >= 0) {
      ::close(owned_completion_fd);
    }
    return {nullptr, error_text("dup ring capabilities")};
  }
  struct stat status {};
  if (::fstat(owned_fd, &status) != 0 || status.st_size < static_cast<off_t>(sizeof(ring_header)) ||
      static_cast<uint64_t>(status.st_size) > OCUDU_DAPP_SHARED_RING_MAX_BYTES_V1) {
    ::close(owned_fd);
    ::close(owned_completion_fd);
    return {nullptr, "invalid shared ring file size"};
  }
  const int access_mode = ::fcntl(owned_fd, F_GETFL) & O_ACCMODE;
  if ((role == endpoint_role::consumer && access_mode != O_RDONLY) ||
      (role == endpoint_role::producer && access_mode != O_RDWR)) {
    ::close(owned_fd);
    ::close(owned_completion_fd);
    return {nullptr, "shared ring data capability has wrong access mode"};
  }
  const int protections = role == endpoint_role::producer ? PROT_READ | PROT_WRITE : PROT_READ;
  void*     address     = ::mmap(nullptr, status.st_size, protections, MAP_SHARED, owned_fd, 0);
  if (address == MAP_FAILED) {
    const auto detail = error_text("map ring");
    ::close(owned_fd);
    ::close(owned_completion_fd);
    return {nullptr, detail};
  }
  auto mapped           = std::make_shared<shared_spsc_ring::mapping>();
  mapped->fd            = owned_fd;
  mapped->completion_fd = owned_completion_fd;
  mapped->address       = address;
  mapped->size          = status.st_size;
  mapped->header        = static_cast<ring_header*>(address);
  const auto& header    = *mapped->header;
  if (header.magic != OCUDU_DAPP_SHARED_RING_MAGIC_V1 || header.version != OCUDU_DAPP_SHARED_RING_VERSION_V1 ||
      header.header_size != OCUDU_DAPP_SHARED_RING_HEADER_BYTES_V1 || header.slots < 2 ||
      header.payload_capacity == 0 || header.slot_stride != align64(sizeof(slot_header) + header.payload_capacity) ||
      header.slot_stride > OCUDU_DAPP_SHARED_RING_MAX_BYTES_V1 ||
      header.slots > (OCUDU_DAPP_SHARED_RING_MAX_BYTES_V1 - header.header_size) / header.slot_stride) {
    return {nullptr, "shared ring header validation failed"};
  }
  const uint64_t expected = header.header_size + static_cast<uint64_t>(header.slots) * header.slot_stride;
  if (expected != header.mapped_bytes || expected != mapped->size) {
    return {nullptr, "shared ring header validation failed"};
  }
  mapped->producer_read_index = atomic_load_acquire(mapped->header->read_index);
  mapped->consumer_read_index = mapped->producer_read_index;
  return {std::unique_ptr<shared_spsc_ring>(new shared_spsc_ring(std::move(mapped), role)), {}};
}

ring_open_result shared_spsc_ring::create(uint32_t slots, uint32_t payload_capacity)
{
  static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr), "shared ring requires lock-free 64-bit atomics");
  if (slots < 2 || payload_capacity == 0) {
    return {nullptr, "ring requires at least two nonempty slots"};
  }
  const uint64_t header_size = OCUDU_DAPP_SHARED_RING_HEADER_BYTES_V1;
  const uint64_t stride      = align64(sizeof(slot_header) + payload_capacity);
  if (stride > OCUDU_DAPP_SHARED_RING_MAX_BYTES_V1 ||
      slots > (OCUDU_DAPP_SHARED_RING_MAX_BYTES_V1 - header_size) / stride) {
    return {nullptr, "requested shared ring is too large"};
  }
  const uint64_t bytes = header_size + static_cast<uint64_t>(slots) * stride;
  const int      fd    = ::memfd_create("ocudu-dapp-ring", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) {
    return {nullptr, error_text("create memfd ring")};
  }
  if (::ftruncate(fd, bytes) != 0 || ::fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
    const auto detail = error_text("size/seal memfd ring");
    ::close(fd);
    return {nullptr, detail};
  }
  void* address = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (address == MAP_FAILED) {
    const auto detail = error_text("map new ring");
    ::close(fd);
    return {nullptr, detail};
  }
  std::memset(address, 0, bytes);
  auto* header             = static_cast<ring_header*>(address);
  header->magic            = OCUDU_DAPP_SHARED_RING_MAGIC_V1;
  header->version          = OCUDU_DAPP_SHARED_RING_VERSION_V1;
  header->header_size      = header_size;
  header->mapped_bytes     = bytes;
  header->slots            = slots;
  header->payload_capacity = payload_capacity;
  header->slot_stride      = stride;
  atomic_store_relaxed(header->epoch, 1);
  ::munmap(address, bytes);
  const int completion_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (completion_fd < 0) {
    const auto detail = error_text("create ring completion eventfd");
    ::close(fd);
    return {nullptr, detail};
  }
  auto result = map_fd(fd, completion_fd, endpoint_role::producer);
  ::close(fd);
  ::close(completion_fd);
  return result;
}

ring_open_result shared_spsc_ring::attach(int data_fd, int completion_fd, endpoint_role role)
{
  if (data_fd < 0 || completion_fd < 0 || role != endpoint_role::consumer) {
    return {nullptr, "invalid shared ring consumer capabilities"};
  }
  return map_fd(data_fd, completion_fd, role);
}

int shared_spsc_ring::export_fd() const noexcept
{
  if (role != endpoint_role::producer) {
    return ::fcntl(mapped->fd, F_DUPFD_CLOEXEC, 0);
  }
  char      path[64];
  const int length = std::snprintf(path, sizeof(path), "/proc/self/fd/%d", mapped->fd);
  return length > 0 && static_cast<std::size_t>(length) < sizeof(path) ? ::open(path, O_RDONLY | O_CLOEXEC) : -1;
}
int shared_spsc_ring::export_completion_fd() const noexcept
{
  return ::fcntl(mapped->completion_fd, F_DUPFD_CLOEXEC, 0);
}
uint32_t shared_spsc_ring::slot_count() const noexcept
{
  return mapped->header->slots;
}
uint32_t shared_spsc_ring::payload_capacity() const noexcept
{
  return mapped->header->payload_capacity;
}
uint64_t shared_spsc_ring::epoch() const noexcept
{
  return atomic_load_acquire(mapped->header->epoch);
}

bool shared_spsc_ring::try_publish(ocudu::span<const uint8_t> payload, uint32_t flags) noexcept
{
  const payload_fragment fragment{payload, 0};
  return try_publish(ocudu::span<const payload_fragment>(&fragment, 1), flags);
}

bool shared_spsc_ring::try_publish(ocudu::span<const payload_fragment> fragments, uint32_t flags) noexcept
{
  return try_publish_impl(fragments, flags, true);
}

bool shared_spsc_ring::try_publish_after_reclaim(ocudu::span<const payload_fragment> fragments, uint32_t flags) noexcept
{
  return try_publish_impl(fragments, flags, false);
}

bool shared_spsc_ring::try_publish_impl(ocudu::span<const payload_fragment> fragments,
                                        uint32_t                            flags,
                                        bool                                reclaim) noexcept
{
  uint64_t payload_size = 0;
  for (const payload_fragment& fragment : fragments) {
    const uint64_t capacity = mapped->header->payload_capacity;
    if (payload_size > capacity || fragment.bytes.size() > capacity - payload_size) {
      return false;
    }
    payload_size += fragment.bytes.size();
    if (fragment.trailing_zeros > capacity - payload_size) {
      return false;
    }
    payload_size += fragment.trailing_zeros;
  }
  if (role != endpoint_role::producer || payload_size > mapped->header->payload_capacity) {
    return false;
  }
  if (mapped->producer_guard.test_and_set(std::memory_order_acquire)) {
    return false;
  }
  const flag_release release{mapped->producer_guard};
  const uint64_t     write = atomic_load_relaxed(mapped->header->write_index);
  if (reclaim) {
    uint64_t completed = 0;
    uint64_t value     = 0;
    while (::read(mapped->completion_fd, &value, sizeof(value)) == sizeof(value)) {
      completed = value > std::numeric_limits<uint64_t>::max() - completed ? std::numeric_limits<uint64_t>::max()
                                                                           : completed + value;
    }
    const uint64_t available = write - mapped->producer_read_index;
    mapped->producer_read_index += std::min(completed, available);
    atomic_store_release(mapped->header->read_index, mapped->producer_read_index);
  }
  const uint64_t read = mapped->producer_read_index;
  if (write - read >= mapped->header->slots) {
    return false;
  }
  auto* slot     = mapped->slot(write);
  slot->size     = payload_size;
  slot->flags    = flags;
  slot->sequence = write;
  auto* output   = reinterpret_cast<uint8_t*>(slot + 1);
  for (const payload_fragment& fragment : fragments) {
    if (!fragment.bytes.empty()) {
      std::memcpy(output, fragment.bytes.data(), fragment.bytes.size());
      output += fragment.bytes.size();
    }
    if (fragment.trailing_zeros != 0) {
      std::memset(output, 0, fragment.trailing_zeros);
      output += fragment.trailing_zeros;
    }
  }
  atomic_store_release(mapped->header->write_index, write + 1);
  return true;
}

uint64_t shared_spsc_ring::reclaim_completed() noexcept
{
  if (role != endpoint_role::producer || mapped->producer_guard.test_and_set(std::memory_order_acquire)) {
    return 0;
  }
  const flag_release release{mapped->producer_guard};
  uint64_t           completed = 0;
  uint64_t           value     = 0;
  while (::read(mapped->completion_fd, &value, sizeof(value)) == sizeof(value)) {
    completed = value > std::numeric_limits<uint64_t>::max() - completed ? std::numeric_limits<uint64_t>::max()
                                                                         : completed + value;
  }
  const uint64_t write     = atomic_load_relaxed(mapped->header->write_index);
  const uint64_t available = write - mapped->producer_read_index;
  const uint64_t reclaimed = std::min(completed, available);
  mapped->producer_read_index += reclaimed;
  atomic_store_release(mapped->header->read_index, mapped->producer_read_index);
  return reclaimed;
}

std::optional<shared_spsc_ring::read_lease> shared_spsc_ring::try_acquire() noexcept
{
  if (role != endpoint_role::consumer) {
    return std::nullopt;
  }
  bool expected = false;
  if (!mapped->lease_active.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
    return std::nullopt;
  }
  const uint64_t read  = mapped->consumer_read_index;
  const uint64_t write = atomic_load_acquire(mapped->header->write_index);
  if (read == write) {
    mapped->lease_active.store(false, std::memory_order_release);
    return std::nullopt;
  }
  auto* slot = mapped->slot(read);
  if (slot->size > mapped->header->payload_capacity || slot->sequence != read) {
    mapped->lease_active.store(false, std::memory_order_release);
    return std::nullopt;
  }
  return read_lease(mapped,
                    {reinterpret_cast<const uint8_t*>(slot + 1), slot->size},
                    slot->flags,
                    read,
                    atomic_load_acquire(mapped->header->epoch));
}

bool shared_spsc_ring::reset_after_consumer_restart() noexcept
{
  if (role != endpoint_role::producer) {
    return false;
  }
  uint64_t ignored = 0;
  while (::read(mapped->completion_fd, &ignored, sizeof(ignored)) == sizeof(ignored)) {
  }
  __atomic_fetch_add(&mapped->header->epoch, 1, __ATOMIC_ACQ_REL);
  mapped->producer_read_index = atomic_load_acquire(mapped->header->write_index);
  atomic_store_release(mapped->header->read_index, mapped->producer_read_index);
  return true;
}

shared_spsc_ring::read_lease::read_lease(std::shared_ptr<mapping>   owner_,
                                         ocudu::span<const uint8_t> bytes_,
                                         uint32_t                   flags_,
                                         uint64_t                   sequence_,
                                         uint64_t                   epoch_) noexcept :
  owner(std::move(owner_)), bytes(bytes_), entry_flags(flags_), entry_sequence(sequence_), acquired_epoch(epoch_)
{
}

shared_spsc_ring::read_lease::read_lease(read_lease&& other) noexcept
{
  *this = std::move(other);
}

shared_spsc_ring::read_lease& shared_spsc_ring::read_lease::operator=(read_lease&& other) noexcept
{
  if (this != &other) {
    reset();
    owner          = std::move(other.owner);
    bytes          = other.bytes;
    entry_flags    = other.entry_flags;
    entry_sequence = other.entry_sequence;
    acquired_epoch = other.acquired_epoch;
    other.bytes    = {};
  }
  return *this;
}

shared_spsc_ring::read_lease::~read_lease()
{
  reset();
}

void shared_spsc_ring::read_lease::reset() noexcept
{
  if (owner != nullptr && atomic_load_acquire(owner->header->epoch) == acquired_epoch) {
    const uint64_t increment = 1;
    const ssize_t  written   = ::write(owner->completion_fd, &increment, sizeof(increment));
    if (written == sizeof(increment)) {
      owner->consumer_read_index = entry_sequence + 1;
    }
  }
  if (owner != nullptr) {
    owner->lease_active.store(false, std::memory_order_release);
  }
  owner.reset();
  bytes = {};
}
