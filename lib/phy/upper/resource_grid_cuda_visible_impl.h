// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Resource grid with CUDA managed backing memory for GPU-visible grids.

#pragma once

#include "../support/resource_grid_allocation_info.h"
#include "../support/resource_grid_reader_impl.h"
#include "../support/resource_grid_writer_impl.h"
#include "phy_acceleration_runtime_options.h"
#include "resource_grid_pinned_impl.h"
#include "ocudu/adt/tensor.h"
#include "ocudu/ocuduvec/conversion.h"
#include "ocudu/ocuduvec/copy.h"
#include "ocudu/ocuduvec/zero.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/resource_grid_dimensions.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/math/math_utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime.h>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <sched.h>
#include <vector>

namespace ocudu {

#ifdef ENABLE_CUDA
namespace dapp {
bool launch_owned_grid_snapshot_copy(const void* src_cbf16, void* dst_cbf16, uint64_t n_u32, void* stream) noexcept;
}
#endif

namespace detail {

template <unsigned NDIMS, typename Type, typename IndexType = unsigned>
class external_tensor : public tensor<NDIMS, Type, IndexType>
{
public:
  using dimensions_size_type = typename tensor<NDIMS, Type, IndexType>::dimensions_size_type;

  external_tensor(Type* data_, const dimensions_size_type& dimensions_) : data(data_) { resize(dimensions_); }

  void resize(const dimensions_size_type& dimensions_) override { dimensions = dimensions_; }

  const dimensions_size_type& get_dimensions_size() const override { return dimensions; }

protected:
  span<Type> get_data() override { return span<Type>(data, size()); }

  span<const Type> get_data() const override { return span<const Type>(data, size()); }

private:
  unsigned size() const { return std::accumulate(dimensions.begin(), dimensions.end(), 1U, std::multiplies<>()); }

  Type*                data = nullptr;
  dimensions_size_type dimensions;
};

} // namespace detail

/// \brief Resource grid whose host writer and GPU reader/writer share CUDA managed backing memory.
class cuda_visible_resource_grid : public resource_grid
{
public:
  cuda_visible_resource_grid(unsigned nof_ports_,
                             unsigned nof_symbols_,
                             unsigned nof_subc_,
                             int      nvcuda_device_id                  = 0,
                             bool     enable_concurrent_downlink_writes = false,
                             bool     uplink_no_host_migrate_           = false) :
    alloc_mask(nof_ports_, nof_symbols_, divide_ceil(nof_subc_, NOF_SUBCARRIERS_PER_RB)),
    uplink_no_host_migrate(uplink_no_host_migrate_),
    device_id(nvcuda_device_id),
    nof_ports(nof_ports_),
    nof_symbols(nof_symbols_),
    nof_subc(nof_subc_)
  {
    // Bind to the configured device so backing allocations and events land on the right GPU.
    (void)cudaSetDevice(device_id);

    const cuda_visible_managed_access_policy access =
        query_cuda_visible_managed_access_policy(device_id, enable_concurrent_downlink_writes);
    host_reads_device_memory_directly = access.host_reads_device_resident_directly;
    prefetch_enabled =
        phy_acceleration_env_flag_enabled("OCUDU_CUDA_VISIBLE_GRID_PREFETCH", access.prefetch_by_default);
    // Concurrent host/device writes to disjoint REs are only safe when prefetch
    // is off and the device is fully coherent (GH200-class). Forcing prefetch
    // on those parts disables the overlap on purpose.
    concurrent_downlink_writes = access.allow_concurrent_downlink_writes && !prefetch_enabled;
    prefer_device_memory =
        phy_acceleration_env_flag_enabled("OCUDU_CUDA_VISIBLE_GRID_PREFER_DEVICE", access.prefer_device_memory);

    static constexpr unsigned initial_ready_events = 32;
    grid_ready_events.reserve(initial_ready_events);
    pending_grid_ready_events.reserve(initial_ready_events);
    for (unsigned i_event = 0; i_event != initial_ready_events; ++i_event) {
      cudaEvent_t event = nullptr;
      if (cudaEventCreateWithFlags(&event, cudaEventDisableTiming) != cudaSuccess) {
        break;
      }
      if (grid_ready_event == nullptr) {
        grid_ready_event = event;
      }
      grid_ready_events.push_back(event);
    }
    cudaStream_t stream = nullptr;
    if (create_realtime_stream(&stream) == cudaSuccess) {
      maintenance_stream = stream;
    }

    managed_bytes = static_cast<size_t>(nof_ports) * nof_symbols * nof_subc * sizeof(cbf16_t);
    if ((grid_ready_event == nullptr) || managed_bytes == 0 ||
        cudaMallocManaged(&managed_data, managed_bytes) != cudaSuccess) {
      managed_data = nullptr;
      return;
    }
    advise_managed_memory();

    rg_buffer = std::make_unique<managed_tensor>(
        static_cast<cbf16_t*>(managed_data), managed_tensor::dimensions_size_type{nof_subc, nof_symbols, nof_ports});
    writer = std::make_unique<writer_impl>(*this, *rg_buffer, alloc_mask);
    reader = std::make_unique<reader_impl>(*this, *rg_buffer, alloc_mask);

    const uint64_t words = managed_bytes / sizeof(uint32_t);
    for (unsigned i = 0; i != owned_snapshot_ring; ++i) {
      cudaEvent_t ev = nullptr;
      if (cudaMalloc(&owned_snapshot[i], managed_bytes) != cudaSuccess ||
          cudaEventCreateWithFlags(&ev, cudaEventDisableTiming) != cudaSuccess) {
        if (owned_snapshot[i] != nullptr) {
          cudaFree(owned_snapshot[i]);
          owned_snapshot[i] = nullptr;
        }
        break;
      }
      owned_snapshot_ready[i] = ev;
      owned_snapshot_words    = words;
    }

    // Pinned staging buffer for host→owned H2D (Class-C). Always try when
    // snapshots exist. no_host_migrate still requires pinned for PUCCH.
    if (owned_snapshot_words != 0) {
      const bool pinned_ok = allocate_host_pinned();
      if (uplink_no_host_migrate && !pinned_ok && !host_reads_device_memory_directly) {
        uplink_no_host_migrate = false;
      }
    }

    // Set all the resource elements to zero. This cannot defer to set_all_zero(), because the allocation mask starts
    // out empty and that would skip the clear, leaving the uninitialised cudaMallocManaged() contents in the grid.
    for (unsigned port = 0; port != nof_ports; ++port) {
      ocuduvec::zero(rg_buffer->template get_view<static_cast<unsigned>(resource_grid_dimensions::port)>({port}));
    }
  }

  ~cuda_visible_resource_grid() override
  {
    if (maintenance_stream != nullptr) {
      cudaStreamSynchronize(static_cast<cudaStream_t>(maintenance_stream));
    }
    for (void* event : grid_ready_events) {
      cudaEventDestroy(static_cast<cudaEvent_t>(event));
    }
    grid_ready_events.clear();
    pending_grid_ready_events.clear();
    grid_ready_event = nullptr;
    if (maintenance_stream != nullptr) {
      cudaStreamDestroy(static_cast<cudaStream_t>(maintenance_stream));
      maintenance_stream = nullptr;
    }
    if (managed_data != nullptr) {
      cudaFree(managed_data);
      managed_data = nullptr;
    }
    for (unsigned i = 0; i != owned_snapshot_ring; ++i) {
      if (owned_snapshot_ready[i] != nullptr) {
        cudaEventDestroy(static_cast<cudaEvent_t>(owned_snapshot_ready[i]));
        owned_snapshot_ready[i] = nullptr;
      }
      if (owned_snapshot[i] != nullptr) {
        cudaFree(owned_snapshot[i]);
        owned_snapshot[i] = nullptr;
      }
      if (host_pinned_ready[i] != nullptr) {
        cudaEventDestroy(static_cast<cudaEvent_t>(host_pinned_ready[i]));
        host_pinned_ready[i] = nullptr;
      }
      if (host_pinned[i] != nullptr) {
        cudaFreeHost(host_pinned[i]);
        host_pinned[i] = nullptr;
      }
    }
  }

  bool is_valid() const { return managed_data != nullptr; }

  void set_all_zero() override
  {
    if (!reader) {
      return;
    }

    owned_snapshot_ready_ok = false;
    host_pinned_valid       = false;

    if (alloc_mask.is_empty()) {
      return;
    }

    if (clear_on_device()) {
      alloc_mask.reset_all();
      return;
    }

    // GB10: any host touch of the live UL VA can migrate PTEs home; the
    // next fade/PUSCH load Xid 31's FAULT_PTE @ 0x140000. Do not depend
    // on pages_device_resident — that flag can be stale. Extra UL RGs
    // are GPU-cleared above; if that failed, leave the buffer dirty.
    if (uplink_no_host_migrate && !host_reads_device_memory_directly) {
      alloc_mask.reset_all();
      return;
    }

    if (!pull_pages_home()) {
      // Cannot safely host-touch the VA (device reader still holds); leave the
      // buffer dirty rather than migrate pages under an in-flight kernel. The
      // mask reset matches the uplink_no_host_migrate branch above: allocated
      // REs are rewritten before use.
      alloc_mask.reset_all();
      return;
    }
    for (unsigned port = 0; port != nof_ports; ++port) {
      if (!alloc_mask.is_port_empty(port)) {
        ocuduvec::zero(rg_buffer->template get_view<static_cast<unsigned>(resource_grid_dimensions::port)>({port}));
      }
    }
    alloc_mask.reset_all();
    std::lock_guard<std::mutex> lock(grid_ready_mutex);
    pending_grid_ready_events.clear();
    grid_ready_recorded = false;
  }

  bool is_empty() const override
  {
    std::unique_lock<std::mutex> lock(grid_ready_mutex);
    wait_for_active_device_mapping_locked(lock);
    return alloc_mask.is_empty();
  }

  resource_grid_writer& get_writer() override { return *writer; }

  const resource_grid_reader& get_reader() const override { return *reader; }

private:
  using managed_tensor =
      detail::external_tensor<static_cast<unsigned>(resource_grid_dimensions::all), cbf16_t, resource_grid_dimensions>;

  class reader_impl : public resource_grid_reader_impl
  {
  public:
    reader_impl(const cuda_visible_resource_grid&    parent_,
                const storage_type&                  data_,
                const resource_grid_allocation_info& alloc_mask_) :
      resource_grid_reader_impl(data_, alloc_mask_), parent(parent_)
    {
    }

    bool supports_device_grid_reading() const override { return parent.managed_data != nullptr; }

    const void* get_device_grid_cbf16() const override { return parent.managed_data; }

    void* get_device_grid_ready_event() const override { return parent.grid_ready_event; }

    bool prepare_device_grid_reading(void* stream) const override { return parent.prepare_device_access(stream); }

    bool on_device_grid_reading_enqueued(void* stream) const override { return parent.record_device_access(stream); }

    void release_device_grid_reading() const override { parent.release_device_read_hold(); }

    bool capture_owned_device_snapshot(void* stream) const override
    {
      return parent.capture_owned_device_snapshot(stream);
    }

    void* begin_owned_device_snapshot() const override { return parent.begin_owned_device_snapshot(); }

    bool complete_owned_device_snapshot(void* stream) const override
    {
      return parent.complete_owned_device_snapshot(stream);
    }

    const void* get_owned_device_snapshot_cbf16() const override { return parent.owned_snapshot_view(); }

    void* get_owned_device_snapshot_ready_event() const override { return parent.owned_snapshot_event(); }

    bool stage_owned_snapshot_from_host() const override { return parent.stage_owned_snapshot_from_host(); }

    void* get_owned_host_mapped_device_ptr() const override { return parent.owned_host_mapped_ptr(); }

    bool synchronize_device_grid_reading() const override { return parent.synchronize_device_mapping(); }

    bool is_empty(unsigned port) const override { return resource_grid_reader_impl::is_empty(port); }

    bool is_empty() const override { return resource_grid_reader_impl::is_empty(); }

    span<cf_t> get(span<cf_t>                                 symbols,
                   unsigned                                   port,
                   unsigned                                   l,
                   unsigned                                   k_init,
                   const bounded_bitset<MAX_NOF_SUBCARRIERS>& mask) const override
    {
      if (parent.prepare_host_access()) {
        return resource_grid_reader_impl::get(symbols, port, l, k_init, mask);
      }
      span<const cbf16_t> symb = parent.host_snapshot_symbol(port, l);
      if (symb.empty() || k_init + mask.size() > symb.size()) {
        parent.log_host_access_failure("read");
        return symbols;
      }
      symb                    = symb.subspan(k_init, mask.size());
      const unsigned mask_count = mask.count();
      if (mask_count && mask.is_contiguous()) {
        ocuduvec::convert(symbols.first(mask_count), symb.subspan(mask.find_lowest(), mask_count));
        return symbols.last(symbols.size() - mask_count);
      }
      mask.for_each(0, mask.size(), [symb, &symbols](unsigned i_subc) {
        symbols.front() = to_cf(symb[i_subc]);
        symbols         = symbols.last(symbols.size() - 1);
      });
      return symbols;
    }

    span<cbf16_t> get(span<cbf16_t>                              symbols,
                      unsigned                                   port,
                      unsigned                                   l,
                      unsigned                                   k_init,
                      const bounded_bitset<MAX_NOF_SUBCARRIERS>& mask) const override
    {
      if (parent.prepare_host_access()) {
        return resource_grid_reader_impl::get(symbols, port, l, k_init, mask);
      }
      span<const cbf16_t> symb = parent.host_snapshot_symbol(port, l);
      if (symb.empty() || k_init + mask.size() > symb.size()) {
        parent.log_host_access_failure("read");
        return symbols;
      }
      symb                      = symb.subspan(k_init, mask.size());
      const unsigned mask_count = mask.count();
      if (mask_count && mask.is_contiguous()) {
        ocuduvec::copy(symbols.first(mask_count), symb.subspan(mask.find_lowest(), mask_count));
        return symbols.last(symbols.size() - mask_count);
      }
      mask.for_each(0, mask.size(), [symb, &symbols](unsigned i_subc) {
        symbols.front() = symb[i_subc];
        symbols         = symbols.last(symbols.size() - 1);
      });
      return symbols;
    }

    void get(span<cf_t> symbols, unsigned port, unsigned l, unsigned k_init, unsigned stride = 1) const override
    {
      if (parent.prepare_host_access()) {
        resource_grid_reader_impl::get(symbols, port, l, k_init, stride);
        return;
      }
      span<const cbf16_t> rg_symbol = parent.host_snapshot_symbol(port, l);
      if (rg_symbol.empty() || stride == 0 ||
          k_init + stride * (symbols.empty() ? 0 : symbols.size() - 1) >= rg_symbol.size()) {
        parent.log_host_access_failure("read");
        return;
      }
      if (stride == 1) {
        ocuduvec::convert(symbols, rg_symbol.subspan(k_init, symbols.size()));
      } else {
        std::generate(symbols.begin(), symbols.end(), [&rg_symbol, stride, n = k_init]() mutable {
          cf_t temp = to_cf(rg_symbol[n]);
          n += stride;
          return temp;
        });
      }
    }

    void get(span<cbf16_t> symbols, unsigned port, unsigned l, unsigned k_init) const override
    {
      if (parent.prepare_host_access()) {
        resource_grid_reader_impl::get(symbols, port, l, k_init);
        return;
      }
      span<const cbf16_t> rg_symbol = parent.host_snapshot_symbol(port, l);
      if (rg_symbol.empty() || k_init + symbols.size() > rg_symbol.size()) {
        parent.log_host_access_failure("read");
        return;
      }
      ocuduvec::copy(symbols, rg_symbol.subspan(k_init, symbols.size()));
    }

    span<const cbf16_t> get_view(unsigned port, unsigned l) const override
    {
      if (parent.prepare_host_access()) {
        return resource_grid_reader_impl::get_view(port, l);
      }
      span<const cbf16_t> shadow = parent.host_snapshot_symbol(port, l);
      if (shadow.empty()) {
        parent.log_host_access_failure("read");
      }
      return shadow;
    }

    span<const cbf16_t> get_contiguous_view() const override
    {
      if (parent.prepare_host_access()) {
        return resource_grid_reader_impl::get_contiguous_view();
      }
      span<const cbf16_t> shadow = parent.host_snapshot_all();
      if (shadow.empty()) {
        parent.log_host_access_failure("read");
      }
      return shadow;
    }

  private:
    const cuda_visible_resource_grid& parent;
  };

  class writer_impl : public resource_grid_writer_impl
  {
  public:
    writer_impl(cuda_visible_resource_grid& parent_, storage_type& data_, resource_grid_allocation_info& alloc_mask_) :
      resource_grid_writer_impl(data_, alloc_mask_), parent(parent_)
    {
    }

    bool supports_device_grid_mapping() const override { return parent.managed_data != nullptr; }

    bool device_grid_mapping_aliases_host_grid() const override { return parent.managed_data != nullptr; }

    void* get_device_grid_bf16() override { return parent.managed_data; }

    span<const cf_t> put(unsigned                                   port,
                         unsigned                                   l,
                         unsigned                                   k_init,
                         const bounded_bitset<MAX_NOF_SUBCARRIERS>& mask,
                         span<const cf_t>                           symbols) override
    {
      if (!parent.prepare_host_write()) {
        parent.log_host_access_failure("write");
        return symbols;
      }
      return resource_grid_writer_impl::put(port, l, k_init, mask, symbols);
    }

    span<const cbf16_t> put(unsigned                                   port,
                            unsigned                                   l,
                            unsigned                                   k_init,
                            const bounded_bitset<MAX_NOF_SUBCARRIERS>& mask,
                            span<const cbf16_t>                        symbols) override
    {
      if (!parent.prepare_host_write()) {
        parent.log_host_access_failure("write");
        return symbols;
      }
      return resource_grid_writer_impl::put(port, l, k_init, mask, symbols);
    }

    void put(unsigned port, unsigned l, unsigned k_init, span<const cf_t> symbols) override
    {
      if (!parent.prepare_host_write()) {
        parent.log_host_access_failure("write");
        return;
      }
      resource_grid_writer_impl::put(port, l, k_init, symbols);
    }

    void put(unsigned port, unsigned l, unsigned k_init, unsigned stride, span<const cbf16_t> symbols) override
    {
      if (!parent.prepare_host_write()) {
        parent.log_host_access_failure("write");
        return;
      }
      resource_grid_writer_impl::put(port, l, k_init, stride, symbols);
    }

    span<cbf16_t> get_view(unsigned port, unsigned l) override
    {
      if (!parent.prepare_host_write()) {
        parent.log_host_access_failure("write");
        if (parent.materialize_owned_snapshot_to_host()) {
          return parent.host_shadow_symbol(port, l);
        }
        return {};
      }
      return resource_grid_writer_impl::get_view(port, l);
    }

    bool prepare_device_grid_mapping(void* stream) override { return parent.prepare_device_mapping(stream); }

    bool on_device_grid_mapping_enqueued(void* stream) override
    {
      if (!parent.record_device_mapping(stream)) {
        return false;
      }
      for (unsigned port = 0; port != parent.nof_ports; ++port) {
        mark_port_allocated(port);
      }
      return true;
    }

    bool cancel_device_grid_mapping() override { return parent.cancel_device_mapping(); }

    bool synchronize_device_grid_mapping() override { return parent.synchronize_device_mapping(); }

  private:
    cuda_visible_resource_grid& parent;
  };

  bool record_device_mapping(void* stream)
  {
    std::lock_guard<std::mutex> lock(grid_ready_mutex);
    void*                       event = get_next_grid_ready_event();
    if (event == nullptr) {
      device_mapping_active           = false;
      grid_ready_recorded             = false;
      device_mapping_replaces_pending = false;
      return false;
    }
    if (cudaEventRecord(static_cast<cudaEvent_t>(event), static_cast<cudaStream_t>(stream)) != cudaSuccess) {
      device_mapping_active           = false;
      grid_ready_recorded             = false;
      device_mapping_replaces_pending = false;
      return false;
    }
    if (device_mapping_replaces_pending) {
      pending_grid_ready_events.clear();
    }
    pending_grid_ready_events.push_back(event);
    grid_ready_event                = event;
    grid_ready_recorded             = !pending_grid_ready_events.empty();
    device_mapping_active           = false;
    device_mapping_replaces_pending = false;
    // A GPU producer just ran. Live-VA device reads are legal until a host
    // consumer pulls the pages home (see pull_pages_home).
    pages_device_resident = true;
    return true;
  }

  bool wait_for_device_mapping(void* stream) const
  {
    std::lock_guard<std::mutex> lock(grid_ready_mutex);
    return wait_for_device_mapping_locked(stream);
  }

  bool record_device_access(void* stream) const
  {
    std::lock_guard<std::mutex> lock(grid_ready_mutex);
    void*                       event = get_next_grid_ready_event();
    if (event == nullptr) {
      if (device_read_holds > 0) {
        --device_read_holds;
      }
      grid_ready_recorded = !pending_grid_ready_events.empty();
      return false;
    }
    if (cudaEventRecord(static_cast<cudaEvent_t>(event), static_cast<cudaStream_t>(stream)) != cudaSuccess) {
      if (device_read_holds > 0) {
        --device_read_holds;
      }
      grid_ready_recorded = !pending_grid_ready_events.empty();
      return false;
    }
    pending_grid_ready_events.push_back(event);
    grid_ready_event      = event;
    grid_ready_recorded   = !pending_grid_ready_events.empty();
    pages_device_resident = true;
    if (device_read_holds > 0) {
      --device_read_holds;
    }
    return true;
  }

  bool wait_for_device_mapping_locked(void* stream) const
  {
    if (!grid_ready_recorded || pending_grid_ready_events.empty()) {
      return managed_data != nullptr;
    }
    for (void* event : pending_grid_ready_events) {
      if (cudaStreamWaitEvent(static_cast<cudaStream_t>(stream), static_cast<cudaEvent_t>(event), 0) != cudaSuccess) {
        return false;
      }
    }
    return true;
  }

  void wait_for_active_device_mapping_locked(std::unique_lock<std::mutex>& lock) const
  {
    while (device_mapping_active) {
      lock.unlock();
      sched_yield();
      lock.lock();
    }
  }

  bool prepare_device_mapping(void* stream)
  {
    std::unique_lock<std::mutex> lock(grid_ready_mutex);
    if (managed_data == nullptr) {
      return false;
    }
    wait_for_active_device_mapping_locked(lock);
    if (!wait_for_device_mapping_locked(stream)) {
      return false;
    }
    const managed_prefetch_status status = prefetch_managed_memory(device_location(), stream);
    if (status == managed_prefetch_status::failed) {
      return false;
    }
    // Do not treat prefetch-to-device success as residency on GB10: it is a
    // no-op there. Residency is set when a GPU kernel is actually enqueued
    // (record_device_mapping / record_device_access / clear_on_device).
    if (prefer_device_memory) {
      pages_device_resident = true;
    }
    device_mapping_active           = true;
    device_mapping_replaces_pending = true;
    return true;
  }

  bool cancel_device_mapping()
  {
    std::lock_guard<std::mutex> lock(grid_ready_mutex);
    device_mapping_active           = false;
    device_mapping_replaces_pending = false;
    return true;
  }

  bool prepare_device_access(void* stream) const
  {
    std::unique_lock<std::mutex> lock(grid_ready_mutex);
    if (managed_data == nullptr) {
      return false;
    }
    wait_for_active_device_mapping_locked(lock);
    if (!wait_for_device_mapping_locked(stream)) {
      return false;
    }
    // Portable device-read gate:
    //   GH200 (CPU can load GPU-resident managed): always legal.
    //   GB10 / discrete-managed: legal only while a GPU producer last touched
    //   the allocation. Prefetch-to-device is a no-op and must not count —
    //   that lie is what Xid 31'd PUSCH at 0x140000 after a host consumer.
    //   Host copy is the fallback for that slot only.
    if (host_reads_device_memory_directly || pages_device_resident ||
        phy_acceleration_env_flag_enabled("OCUDU_CUDA_VISIBLE_GRID_FORCE_DEVICE_READ", false)) {
      ++device_read_holds;
      return true;
    }
    return false;
  }

  void release_device_read_hold() const
  {
    std::lock_guard<std::mutex> lock(grid_ready_mutex);
    if (device_read_holds > 0) {
      --device_read_holds;
    }
  }

  bool synchronize_device_mapping() { return prepare_host_access(); }

  bool synchronize_device_mapping() const { return prepare_host_access(); }

  bool prepare_host_write() const
  {
    // A downlink scheduler maps reference signals and PDSCH data to disjoint resource elements. On coherent
    // unified-memory systems, avoid making the next PDU's host-generated reference signal wait for the previous
    // PDSCH kernel. Grid clears complete before set_all_zero() returns and readers still wait for every producer.
    if (concurrent_downlink_writes) {
      std::lock_guard<std::mutex> lock(grid_ready_mutex);
      return managed_data != nullptr;
    }
    return pull_pages_home();
  }

  /// Synchronise the grid for host reads and writes of the *live* VA.
  ///
  /// GB10 cannot CPU-load GPU-resident managed pages, and it cannot safely
  /// cudaMemcpy the live managed VA either (D2H of that pointer Xid 31'd
  /// FAULT_PDE @ 0xe9a6_71c00000). Host access therefore waits for every
  /// device reader to enqueue, then prefetches the pages home. A later GPU
  /// producer (OFDM / channel hook) sets residency again.
  ///
  /// If a device reader still holds after a short wait, refuse the host
  /// access rather than memcpy or migrate under it.
  ///
  /// The failure here is almost never local. CUDA errors are sticky per context,
  /// so once any kernel anywhere in the process faults -- an out-of-range address
  /// in an unrelated stage, on an unrelated stream -- every subsequent CUDA call
  /// returns that error, and this function is usually just the first one to look.
  /// Returning a bare false made the resulting abort say only "failed to
  /// synchronize", which is why four of these went unexplained: the one fact that
  /// identifies the real fault was discarded at the point of detection.
  ///
  /// last_host_access_error carries it out to the fatal site instead. Only the
  /// first is kept, since every call after the first sticky failure reports the
  /// same code and the earliest is the closest to the cause.
  bool prepare_host_access() const { return pull_pages_home(); }

  bool pull_pages_home() const
  {
    std::unique_lock<std::mutex> lock(grid_ready_mutex);
    return pull_pages_home_locked(lock);
  }

  bool pull_pages_home_locked(std::unique_lock<std::mutex>& lock) const
  {
    wait_for_active_device_mapping_locked(lock);
    if (!wait_for_device_read_holds_locked(lock)) {
      return false;
    }
    if (!wait_for_pending_grid_events_locked()) {
      return false;
    }
    // UL on non-coherent UM: never CPU-touch or prefetch-home the live VA,
    // even when pages_device_resident is false (that flag is software and
    // can lag). Host readers must use the pinned snapshot. A live host
    // load page-faults the PTEs home and the next fade/PUSCH GPU load
    // Xid 31's FAULT_PTE @ 0x140000.
    if (uplink_no_host_migrate && !host_reads_device_memory_directly) {
      return false;
    }
    if (pages_device_resident && !host_reads_device_memory_directly) {
      if (prefetch_managed_memory(host_location(), maintenance_stream) == managed_prefetch_status::failed) {
        record_host_access_error(cudaPeekAtLastError(), "prefetch of managed grid to host");
        return false;
      }
      pages_device_resident = false;
    }
    return true;
  }

  bool wait_for_device_read_holds_locked(std::unique_lock<std::mutex>& lock) const
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    while (device_read_holds > 0) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      lock.unlock();
      sched_yield();
      lock.lock();
    }
    return true;
  }

  bool wait_for_pending_grid_events_locked() const
  {
    if (!grid_ready_recorded || pending_grid_ready_events.empty()) {
      return true;
    }
    for (void* event : pending_grid_ready_events) {
      const cudaError_t status = cuda_event_synchronize_yielding(static_cast<cudaEvent_t>(event));
      if (status != cudaSuccess) {
        record_host_access_error(status, "cudaEventQuery on grid-ready event");
        return false;
      }
    }
    pending_grid_ready_events.clear();
    grid_ready_recorded = false;
    return true;
  }

  static bool is_sticky_cuda_poison(cudaError_t status)
  {
    return status == cudaErrorIllegalAddress || status == cudaErrorLaunchFailure ||
           status == cudaErrorIllegalInstruction || status == cudaErrorMisalignedAddress ||
           status == cudaErrorHardwareStackError;
  }

  void record_host_access_error(cudaError_t status, const char* what) const
  {
    if (host_access_error == cudaSuccess) {
      host_access_error = status;
      host_access_stage = what;
    }
    if (is_sticky_cuda_poison(status)) {
      ensure_host_shadow();
    }
  }

  void ensure_host_shadow() const
  {
    if (cuda_context_poisoned) {
      return;
    }
    if (!allocate_host_shadow()) {
      return;
    }
    cuda_context_poisoned = true;
  }

  bool allocate_host_shadow() const
  {
    if (!host_shadow.empty()) {
      return true;
    }
    const size_t n = static_cast<size_t>(nof_ports) * nof_symbols * nof_subc;
    try {
      host_shadow.assign(n, cbf16_t{});
      return true;
    } catch (...) {
      host_shadow.clear();
      return false;
    }
  }

  bool use_host_shadow() const { return (cuda_context_poisoned || host_shadow_valid) && !host_shadow.empty(); }

  bool allocate_host_pinned() const
  {
    if (managed_bytes == 0) {
      return false;
    }
    for (unsigned i = 0; i != owned_snapshot_ring; ++i) {
      if (host_pinned[i] != nullptr && host_pinned_ready[i] != nullptr) {
        continue;
      }
      void*       pinned  = nullptr;
      void*       mapped  = nullptr;
      cudaEvent_t ev      = nullptr;
      if (cudaHostAlloc(&pinned, managed_bytes, cudaHostAllocMapped) != cudaSuccess ||
          cudaHostGetDevicePointer(&mapped, pinned, 0) != cudaSuccess ||
          cudaEventCreateWithFlags(&ev, cudaEventDisableTiming) != cudaSuccess) {
        if (pinned != nullptr) {
          cudaFreeHost(pinned);
        }
        if (ev != nullptr) {
          cudaEventDestroy(ev);
        }
        return false;
      }
      std::memset(pinned, 0, managed_bytes);
      host_pinned[i]       = pinned;
      host_pinned_dev[i]   = mapped;
      host_pinned_ready[i] = ev;
    }
    return true;
  }

  /// Host readers must get a full-sized span or they subspan() and SIGSEGV.
  /// Prefer the pinned D2H of the owned snapshot (maintenance stream, never
  /// the live VA, never a pageable vector). Zeros only if that copy is down.
  bool materialize_owned_snapshot_to_host() const
  {
    if (owned_snapshot_ready_ok) {
      const unsigned i = owned_snapshot_index;
      if (i < owned_snapshot_ring && host_pinned_enqueued[i] && host_pinned[i] != nullptr &&
          host_pinned_ready[i] != nullptr) {
        if (host_pinned_valid && host_pinned_index == i) {
          return true;
        }
        void* ready = owned_snapshot_ready[i] != nullptr ? owned_snapshot_ready[i] : host_pinned_ready[i];
        const cudaError_t st = cuda_event_synchronize_yielding(static_cast<cudaEvent_t>(ready));
        if (st == cudaSuccess) {
          host_pinned_index = i;
          host_pinned_valid = true;
          return true;
        }
        record_host_access_error(st, "wait for fade-kernel mapped-host snapshot");
      }
    }
    if (!allocate_host_shadow()) {
      return false;
    }
    host_shadow_valid = true;
    host_pinned_valid = false;
    return true;
  }

  span<const cbf16_t> host_snapshot_symbol(unsigned port, unsigned l) const
  {
    if (!materialize_owned_snapshot_to_host()) {
      return {};
    }
    if (host_pinned_valid && host_pinned_index < owned_snapshot_ring &&
        host_pinned[host_pinned_index] != nullptr) {
      auto* p = static_cast<const cbf16_t*>(host_pinned[host_pinned_index]);
      return span<const cbf16_t>(p + (static_cast<size_t>(port) * nof_symbols + l) * nof_subc, nof_subc);
    }
    return host_shadow_symbol(port, l);
  }

  span<const cbf16_t> host_snapshot_all() const
  {
    if (!materialize_owned_snapshot_to_host()) {
      return {};
    }
    if (host_pinned_valid && host_pinned_index < owned_snapshot_ring &&
        host_pinned[host_pinned_index] != nullptr) {
      auto* p = static_cast<const cbf16_t*>(host_pinned[host_pinned_index]);
      return span<const cbf16_t>(p, static_cast<size_t>(nof_ports) * nof_symbols * nof_subc);
    }
    return host_shadow_all();
  }

  span<cbf16_t> host_shadow_symbol(unsigned port, unsigned l) const
  {
    return span<cbf16_t>(host_shadow.data() + (static_cast<size_t>(port) * nof_symbols + l) * nof_subc, nof_subc);
  }

  span<const cbf16_t> host_shadow_all() const
  {
    return span<const cbf16_t>(host_shadow.data(), host_shadow.size());
  }

  /// What failed, and the CUDA error behind it. Never null.
  const char* host_access_failure_stage() const
  {
    return host_access_stage != nullptr ? host_access_stage : "unknown stage";
  }
  const char* host_access_failure_name() const { return cudaGetErrorName(host_access_error); }
  const char* host_access_failure_text() const { return cudaGetErrorString(host_access_error); }

  /// Host I/O used to abort the process here. A sticky Xid 31 on any earlier
  /// kernel makes every later cudaEventQuery fail; killing the gNB on the first
  /// DMRS put that notices is how we took 11 GB cores. Skip the access instead.
  void log_host_access_failure(const char* what) const
  {
    const unsigned k = host_access_failure_logs.fetch_add(1, std::memory_order_relaxed);
    if (k >= 32) {
      return;
    }
    fmt::print(stderr,
               "OCUDU WARNING: Failed to prepare CUDA-visible resource grid {} ({}): {} failed: {} ({}). CUDA "
               "errors are sticky per context -- the faulting kernel is probably an earlier one; check dmesg "
               "for an Xid.\n",
               what,
               k,
               host_access_failure_stage(),
               host_access_failure_name(),
               host_access_failure_text());
  }

  void* get_next_grid_ready_event() const
  {
    for (void* event : grid_ready_events) {
      if (std::find(pending_grid_ready_events.begin(), pending_grid_ready_events.end(), event) ==
          pending_grid_ready_events.end()) {
        return event;
      }
    }

    cudaEvent_t event = nullptr;
    if (cudaEventCreateWithFlags(&event, cudaEventDisableTiming) != cudaSuccess) {
      return nullptr;
    }
    grid_ready_events.push_back(event);
    return event;
  }

  bool clear_on_device()
  {
    if ((managed_data == nullptr) || (managed_bytes == 0) || (maintenance_stream == nullptr)) {
      return false;
    }

    std::unique_lock<std::mutex> lock(grid_ready_mutex);
    wait_for_active_device_mapping_locked(lock);
    // A reader between prepare_device_access and its fence record has no event
    // yet; wait the hold out first so the fence lands in pending events and
    // the memset below orders after it instead of zeroing under the read.
    if (!wait_for_device_read_holds_locked(lock)) {
      return false;
    }
    cudaStream_t stream = static_cast<cudaStream_t>(maintenance_stream);
    if (!wait_for_device_mapping_locked(stream)) {
      return false;
    }
    void* event = get_next_grid_ready_event();
    if (event == nullptr) {
      return false;
    }
    if (cudaMemsetAsync(managed_data, 0, managed_bytes, stream) != cudaSuccess) {
      return false;
    }
    if (cudaEventRecord(static_cast<cudaEvent_t>(event), stream) != cudaSuccess) {
      return false;
    }
    if (concurrent_downlink_writes &&
        (cuda_event_synchronize_yielding(static_cast<cudaEvent_t>(event)) != cudaSuccess)) {
      return false;
    }
    pending_grid_ready_events.clear();
    if (!concurrent_downlink_writes) {
      pending_grid_ready_events.push_back(event);
    }
    grid_ready_event                = event;
    grid_ready_recorded             = !pending_grid_ready_events.empty();
    device_mapping_active           = false;
    device_mapping_replaces_pending = false;
    pages_device_resident           = true;
    return true;
  }

  mutable cudaError_t           host_access_error{cudaSuccess};
  mutable const char*           host_access_stage{nullptr};
  mutable std::atomic<unsigned> host_access_failure_logs{0};
  mutable bool                  cuda_context_poisoned{false};
  mutable bool                  host_shadow_valid{false};
  mutable bool                  host_pinned_valid{false};
  mutable unsigned              host_pinned_index{~0u};
  mutable unsigned              host_shadow_from_index{~0u};
  mutable std::vector<cbf16_t>  host_shadow;

  static constexpr unsigned owned_snapshot_ring = 3;

  bool capture_owned_device_snapshot(void* stream) const
  {
    // GPU load of the live managed VA Xid 31's on GB10. Use the host stage.
    (void)stream;
    return stage_owned_snapshot_from_host();
  }

  /// After PUSCH has released the live VA: home pages, CPU-copy into pinned,
  /// H2D into owned cudaMalloc. Never cudaMemcpy the live pointer.
  bool stage_owned_snapshot_from_host() const
  {
    if (owned_snapshot_words == 0 || managed_data == nullptr) {
      return false;
    }
    if (owned_snapshot_ready_ok && owned_snapshot_index < owned_snapshot_ring &&
        host_pinned_enqueued[owned_snapshot_index]) {
      return true;
    }
    // Hold the grid mutex across pages-home AND the host memcpy: with the lock
    // dropped in between, a device producer can regain residency and launch on
    // the same VA, and the memcpy then faults the PTEs home mid-kernel (the
    // Xid 31 class this staging path exists to avoid).
    std::unique_lock<std::mutex> lock(grid_ready_mutex);
    if (!pull_pages_home_locked(lock)) {
      return false;
    }
    if (!allocate_host_pinned()) {
      return false;
    }
    const unsigned next = (owned_snapshot_index + 1U) % owned_snapshot_ring;
    if (owned_snapshot[next] == nullptr || owned_snapshot_ready[next] == nullptr || host_pinned[next] == nullptr ||
        maintenance_stream == nullptr) {
      return false;
    }
    if (owned_snapshot_ready[next] != nullptr) {
      (void)cuda_event_synchronize_yielding(static_cast<cudaEvent_t>(owned_snapshot_ready[next]));
    }
    std::memcpy(host_pinned[next], managed_data, managed_bytes);
#ifdef ENABLE_CUDA
    cudaStream_t ms = static_cast<cudaStream_t>(maintenance_stream);
    if (cudaMemcpyAsync(owned_snapshot[next],
                        host_pinned[next],
                        managed_bytes,
                        cudaMemcpyHostToDevice,
                        ms) != cudaSuccess ||
        cudaEventRecord(static_cast<cudaEvent_t>(owned_snapshot_ready[next]), ms) != cudaSuccess) {
      (void)cudaGetLastError();
      return false;
    }
    owned_snapshot_writing            = next;
    owned_snapshot_index              = next;
    owned_snapshot_ready_ok           = true;
    host_pinned_enqueued[next]        = true;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
      fmt::print(stderr,
                 "OCUDU: UL Class-C snapshot armed (host-home + pinned H2D, no live GPU load)\n");
    }
    return true;
#else
    return false;
#endif
  }

  void* begin_owned_device_snapshot() const
  {
    if (owned_snapshot_words == 0) {
      return nullptr;
    }
    // Fade of live + later host-migrate of the same VA is Xid 31 @ 0x140000.
    // Only fork when PUCCH will not home the live pages.
    if (!uplink_no_host_migrate && !host_reads_device_memory_directly) {
      return nullptr;
    }
    // Write the buffer that is not currently advertised so mid-slot Class-C
    // still sees the previous completed snapshot (never the live VA).
    const unsigned next = (owned_snapshot_index + 1U) % owned_snapshot_ring;
    if (owned_snapshot[next] == nullptr || owned_snapshot_ready[next] == nullptr) {
      return nullptr;
    }
    if (host_pinned_ready[next] != nullptr) {
      (void)cuda_event_synchronize_yielding(static_cast<cudaEvent_t>(host_pinned_ready[next]));
    }
    owned_snapshot_writing = next;
    return owned_snapshot[next];
  }

  bool complete_owned_device_snapshot(void* stream) const
  {
    const unsigned w = owned_snapshot_writing;
    if (stream == nullptr || w >= owned_snapshot_ring || owned_snapshot[w] == nullptr ||
        owned_snapshot_ready[w] == nullptr) {
      return false;
    }
#ifdef ENABLE_CUDA
    if (cudaEventRecord(static_cast<cudaEvent_t>(owned_snapshot_ready[w]),
                        static_cast<cudaStream_t>(stream)) != cudaSuccess) {
      return false;
    }
    owned_snapshot_index    = w;
    owned_snapshot_ready_ok = true;

    // Fade kernel writes mapped-pinned host from registers on this stream.
    // The snapshot-ready event is also the host-ready event — no D2H.
    if (host_pinned[w] != nullptr && host_pinned_dev[w] != nullptr) {
      host_pinned_enqueued[w] = true;
    }
    return true;
#else
    (void)stream;
    return false;
#endif
  }

  const void* owned_snapshot_view() const
  {
    return owned_snapshot_ready_ok ? owned_snapshot[owned_snapshot_index] : nullptr;
  }

  void* owned_snapshot_event() const
  {
    return owned_snapshot_ready_ok ? owned_snapshot_ready[owned_snapshot_index] : nullptr;
  }

  void* owned_host_mapped_ptr() const
  {
    const unsigned w = owned_snapshot_writing;
    if (w >= owned_snapshot_ring) {
      return nullptr;
    }
    return host_pinned_dev[w];
  }

  mutable void*    owned_snapshot[owned_snapshot_ring]{};
  mutable void*    owned_snapshot_ready[owned_snapshot_ring]{};
  mutable void*    host_pinned[owned_snapshot_ring]{};
  mutable void*    host_pinned_dev[owned_snapshot_ring]{};
  mutable void*    host_pinned_ready[owned_snapshot_ring]{};
  mutable bool     host_pinned_enqueued[owned_snapshot_ring]{};
  mutable unsigned owned_snapshot_index{0};
  mutable unsigned owned_snapshot_writing{0};
  mutable bool     owned_snapshot_ready_ok{false};
  uint64_t         owned_snapshot_words{0};

  /// True when a GPU kernel last produced this allocation (map / apply / clear).
  ///
  /// Not "prefetch claimed the pages are on the device" — that is a no-op on
  /// GB10. Host pull_pages_home clears this after waiting for device_read_holds.
  /// Device reads of the live VA are allowed only while this is set (or the
  /// part is fully coherent). Never cudaMemcpy the live managed VA on GB10.
  mutable bool     pages_device_resident{false};
  mutable unsigned device_read_holds{0};

  /// Whether this device lets the CPU read device-resident managed pages.
  ///
  /// True only when cudaDevAttrDirectManagedMemAccessFromHost is 1 (GH200-class).
  /// GB10, discrete GPUs, and typical Jetson report 0: a host load of a
  /// GPU-resident managed page is not supported and the pages have to come
  /// home first. Queried per device so one binary stays correct on all four.
  bool host_reads_device_memory_directly{false};

  static cudaError_t cuda_event_synchronize_yielding(cudaEvent_t event)
  {
    cudaError_t status;
    while ((status = cudaEventQuery(event)) == cudaErrorNotReady) {
      sched_yield();
    }
    return status;
  }

  static cudaError_t create_realtime_stream(cudaStream_t* stream)
  {
    int         least_priority    = 0;
    int         greatest_priority = 0;
    cudaError_t status            = cudaDeviceGetStreamPriorityRange(&least_priority, &greatest_priority);
    if (status == cudaSuccess) {
      status = cudaStreamCreateWithPriority(stream, cudaStreamNonBlocking, greatest_priority);
      if (status == cudaSuccess) {
        return cudaSuccess;
      }
      (void)cudaGetLastError();
    }

    return cudaStreamCreateWithFlags(stream, cudaStreamNonBlocking);
  }

  static bool query_cuda_device_flag(int device, cudaDeviceAttr attribute)
  {
    int value = 0;
    return (cudaDeviceGetAttribute(&value, attribute, device) == cudaSuccess) && (value != 0);
  }

  static cuda_visible_managed_access_policy query_cuda_visible_managed_access_policy(int  device,
                                                                                    bool enable_concurrent_downlink)
  {
    return make_cuda_visible_managed_access_policy(
        query_cuda_device_flag(device, cudaDevAttrConcurrentManagedAccess),
        query_cuda_device_flag(device, cudaDevAttrPageableMemoryAccessUsesHostPageTables),
        query_cuda_device_flag(device, cudaDevAttrDirectManagedMemAccessFromHost),
        enable_concurrent_downlink);
  }

  void advise_managed_memory() const
  {
    if (managed_data == nullptr) {
      return;
    }
    // Best-effort hints only. The public resource-grid contract must not depend on managed-memory advice support.
    (void)cudaMemAdvise(managed_data, managed_bytes, cudaMemAdviseSetAccessedBy, device_location());
    if (prefer_device_memory) {
      (void)cudaMemAdvise(managed_data, managed_bytes, cudaMemAdviseSetPreferredLocation, device_location());
      // On coherent systems, CUDA requires this hint for the CPU to access GPU-resident managed pages directly
      // instead of faulting and migrating them back to host memory.
      (void)cudaMemAdvise(managed_data, managed_bytes, cudaMemAdviseSetAccessedBy, host_location());
    }
  }

#if CUDART_VERSION >= 13000
  cudaMemLocation device_location() const { return cudaMemLocation{cudaMemLocationTypeDevice, device_id}; }

  static cudaMemLocation host_location() { return cudaMemLocation{cudaMemLocationTypeHost, 0}; }

  static bool is_host_location(cudaMemLocation destination)
  {
    return (destination.type == cudaMemLocationTypeHost) || (destination.type == cudaMemLocationTypeHostNuma) ||
           (destination.type == cudaMemLocationTypeHostNumaCurrent);
  }
#else
  int device_location() const { return device_id; }

  static int host_location() { return cudaCpuDeviceId; }

  static bool is_host_location(int destination) { return destination == cudaCpuDeviceId; }
#endif

  enum class managed_prefetch_status { disabled, done, unsupported, failed };

  managed_prefetch_status prefetch_managed_memory(decltype(host_location()) destination, void* stream) const
  {
    if (!prefetch_enabled || (managed_data == nullptr)) {
      return managed_prefetch_status::disabled;
    }
#if CUDART_VERSION >= 13000
    cudaError_t status =
        cudaMemPrefetchAsync(managed_data, managed_bytes, destination, 0, static_cast<cudaStream_t>(stream));
#else
    cudaError_t status =
        cudaMemPrefetchAsync(managed_data, managed_bytes, destination, static_cast<cudaStream_t>(stream));
#endif
    if (status == cudaErrorNotSupported) {
      return managed_prefetch_status::unsupported;
    }
    if (status != cudaSuccess) {
      return managed_prefetch_status::failed;
    }
    if (is_host_location(destination)) {
      return cudaStreamSynchronize(static_cast<cudaStream_t>(stream)) == cudaSuccess ? managed_prefetch_status::done
                                                                                    : managed_prefetch_status::failed;
    }
    return managed_prefetch_status::done;
  }

  void*                      managed_data       = nullptr;
  size_t                     managed_bytes      = 0;
  mutable void*              grid_ready_event   = nullptr;
  void*                      maintenance_stream = nullptr;
  mutable std::vector<void*> grid_ready_events;
  mutable std::vector<void*> pending_grid_ready_events;
  mutable std::mutex         grid_ready_mutex;
  mutable bool               grid_ready_recorded             = false;
  mutable bool               device_mapping_active           = false;
  mutable bool               device_mapping_replaces_pending = false;
  /// Per-port, per-symbol allocation tracking, shared with the reader and writer.
  resource_grid_allocation_info alloc_mask;
  bool     prefetch_enabled           = phy_acceleration_env_flag_enabled("OCUDU_CUDA_VISIBLE_GRID_PREFETCH", false);
  bool     concurrent_downlink_writes = false;
  bool     uplink_no_host_migrate     = false;
  bool     prefer_device_memory       = false;
  int      device_id                  = 0;
  unsigned nof_ports                  = 0;
  unsigned nof_symbols                = 0;
  unsigned nof_subc                   = 0;
  std::unique_ptr<managed_tensor>            rg_buffer;
  std::unique_ptr<resource_grid_writer_impl> writer;
  std::unique_ptr<reader_impl>               reader;
};

/// \brief Resource-grid factory that uses CUDA managed memory only on integrated/unified CUDA devices.
class resource_grid_cuda_visible_factory : public resource_grid_factory
{
public:
  enum class direction { generic, downlink, uplink };

  explicit resource_grid_cuda_visible_factory(direction           grid_direction_           = direction::generic,
                                              int                 cuda_device_id_           = 0,
                                              std::optional<bool> use_managed_grid_override = std::nullopt) :
    grid_direction(grid_direction_),
    device_id(cuda_device_id_),
    use_managed_grid(use_managed_grid_override.value_or(should_use_managed_grid(grid_direction_, cuda_device_id_))),
    managed_grid_required(use_managed_grid_override.value_or(false))
  {
  }

  std::unique_ptr<resource_grid> create(unsigned nof_ports, unsigned nof_symbols, unsigned nof_subc) override
  {
    if (use_managed_grid) {
      // UL fade is skipped (Xid 31 @ 0x140000). PUCCH must host-read the
      // live grid — do not set uplink_no_host_migrate or CSI is zeros and
      // UEs RLF.
      auto grid = std::make_unique<cuda_visible_resource_grid>(nof_ports,
                                                              nof_symbols,
                                                              nof_subc,
                                                              device_id,
                                                              grid_direction == direction::downlink,
                                                              false);
      if (grid->is_valid()) {
        return grid;
      }
      report_fatal_error_if_not(
          !managed_grid_required, "Failed to allocate a required CUDA managed resource grid on device {}", device_id);
    }
    return fallback_factory->create(nof_ports, nof_symbols, nof_subc);
  }

private:
  static const char* get_direction_env_name(direction grid_direction)
  {
    switch (grid_direction) {
      case direction::downlink:
        return "OCUDU_DL_CUDA_VISIBLE_GRID";
      case direction::uplink:
        return "OCUDU_UL_CUDA_VISIBLE_GRID";
      case direction::generic:
      default:
        return nullptr;
    }
  }

  static bool mode_uses_managed(const char* mode, int device_id)
  {
    if (phy_acceleration_env_mode_is_managed(mode)) {
      return true;
    }
    if ((mode != nullptr) && ((std::strcmp(mode, "pinned") == 0) || (std::strcmp(mode, "off") == 0) ||
                              (std::strcmp(mode, "disabled") == 0))) {
      return false;
    }
    // Unknown values, including "auto", keep the automatic platform policy.
    return should_use_managed_grid_auto(device_id);
  }

  static bool should_use_managed_grid(direction grid_direction, int device_id)
  {
    const char* mode = phy_acceleration_cuda_visible_grid_mode(get_direction_env_name(grid_direction));
    if ((mode == nullptr) && (grid_direction == direction::generic)) {
      mode = std::getenv("OCUDU_UL_CUDA_VISIBLE_GRID");
    }
    if (mode != nullptr) {
      return mode_uses_managed(mode, device_id);
    }

    return should_use_managed_grid_auto(device_id);
  }

  static bool should_use_managed_grid_auto(int device_id)
  {
    int integrated = 0;
    if (cudaDeviceGetAttribute(&integrated, cudaDevAttrIntegrated, device_id) != cudaSuccess) {
      return false;
    }
    return integrated != 0;
  }

  direction                              grid_direction;
  int                                    device_id;
  bool                                   use_managed_grid;
  bool                                   managed_grid_required;
  std::shared_ptr<resource_grid_factory> fallback_factory = std::make_shared<resource_grid_pinned_factory>();
};

} // namespace ocudu
