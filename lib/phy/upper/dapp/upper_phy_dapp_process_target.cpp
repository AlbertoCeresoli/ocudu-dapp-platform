// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/phy/upper/dapp/upper_phy_dapp_process_target.h"
#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/process/v1/control.h"
#include "ocudu/dapp/process/v1/publication_record.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

#ifdef ENABLE_CUDA
#include "class_c_cuda_export_pool.h"
#include "srs_channel_estimate_tensor_adapter.h"
#endif

using namespace ocudu;
using namespace ocudu::dapp::ipc;

namespace {

uint64_t monotonic_time_ns() noexcept
{
  const auto count =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return count > 0 ? static_cast<uint64_t>(count) : 1;
}

uint64_t align_record(uint64_t value) noexcept
{
  constexpr uint64_t alignment = OCUDU_DAPP_SHARED_RING_ALIGNMENT_V1;
  return value <= std::numeric_limits<uint64_t>::max() - (alignment - 1) ? (value + alignment - 1) & ~(alignment - 1)
                                                                         : 0;
}

constexpr std::size_t result_admission_capacity = 64;

uint64_t source_context(uint32_t cell_id, const ocudu_dapp_radio_time_v1& time) noexcept
{
  if (time.numerology > 4 || time.sfn >= 1024 || time.slot >= (10U << time.numerology) || time.reserved != 0) {
    return std::numeric_limits<uint64_t>::max();
  }
  return (static_cast<uint64_t>(cell_id) << 32U) | (static_cast<uint64_t>(time.sfn) << 16U) |
         (static_cast<uint64_t>(time.slot) << 4U) | time.numerology;
}

struct entry_guard {
  std::atomic_flag& flag;
  ~entry_guard() { flag.clear(std::memory_order_release); }
};

class process_target_impl final : public upper_phy_dapp_process_target
{
  struct result_admission {
    std::atomic_flag      access = ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> sequence{};
    std::atomic<uint64_t> context{};
    std::atomic<uint64_t> plan_generation{};
    std::atomic<uint32_t> srs_resource_id{};
    std::atomic<uint32_t> srs_config_id{};
    std::atomic<uint32_t> consumed_outputs{};
  };

  struct result_admission_commit {
    result_admission* admission{};
    uint64_t          sequence{};
    uint64_t          context{};
    uint64_t          plan_generation{};
    uint32_t          srs_resource_id{};
    uint32_t          srs_config_id{};
  };

public:
  process_target_impl(uint32_t                                     interface_id_,
                      uint32_t                                     route_flags_,
                      std::unique_ptr<publication_producer>        producer_,
                      std::unique_ptr<class_c_publication_encoder> encoder_
#ifdef ENABLE_CUDA
                      ,
                      std::unique_ptr<dapp::class_c_cuda_export_pool>            accelerator_,
                      std::unique_ptr<dapp::srs_channel_estimate_tensor_adapter> srs_accelerator_
#endif
                      ) :
    id(interface_id_),
    flags(route_flags_),
    producer(std::move(producer_)),
    encoder(std::move(encoder_))
#ifdef ENABLE_CUDA
    ,
    accelerator(std::move(accelerator_)),
    srs_accelerator(std::move(srs_accelerator_))
#endif
  {
  }

  uint32_t interface_id() const noexcept override { return id; }
  bool     active() const noexcept override { return enabled.load(std::memory_order_acquire); }
  void     set_active(bool value) noexcept override { enabled.store(value, std::memory_order_release); }

  route_capabilities export_capabilities() const noexcept override
  {
    auto capabilities = producer->export_capabilities();
#ifdef ENABLE_CUDA
    if (accelerator != nullptr) {
      capabilities.accelerator_export = accelerator->export_descriptor();
    }
#endif
    return capabilities;
  }

  bool reset_after_consumer_restart() noexcept override
  {
    if (active() || producer_entry.test_and_set(std::memory_order_acquire)) {
      return false;
    }
    const entry_guard release{producer_entry};
    if (!producer->reset_after_consumer_restart()) {
      return false;
    }
#ifdef ENABLE_CUDA
    if (accelerator != nullptr && !accelerator->reset_after_consumer_restart()) {
      return false;
    }
#endif
    for (auto& admission : result_admissions) {
      admission.sequence.store(0, std::memory_order_release);
    }
    return true;
  }

  bool consume_result_source(uint64_t                        source_sequence,
                             uint64_t                        source_plan_generation,
                             uint32_t                        cell_id,
                             const ocudu_dapp_radio_time_v1& created) noexcept override
  {
    if (source_sequence == 0) {
      return false;
    }
    return consume_spectrum_output(source_sequence, source_plan_generation, cell_id, created, 1U << 0U);
  }

  bool consume_quiet_intent_source(uint64_t                        source_sequence,
                                   uint64_t                        source_plan_generation,
                                   uint32_t                        cell_id,
                                   const ocudu_dapp_radio_time_v1& created) noexcept override
  {
    return consume_spectrum_output(source_sequence, source_plan_generation, cell_id, created, 1U << 1U);
  }

  bool consume_srs_result_source(uint64_t                        source_sequence,
                                 uint32_t                        srs_resource_id,
                                 uint32_t                        srs_config_id,
                                 uint32_t                        cell_id,
                                 const ocudu_dapp_radio_time_v1& created) noexcept override
  {
    if (source_sequence == 0) {
      return false;
    }
    auto& admission = result_admissions[source_sequence % result_admissions.size()];
    if (admission.access.test_and_set(std::memory_order_acquire)) {
      return false;
    }
    const entry_guard release{admission.access};
    const uint64_t    expected = source_context(cell_id, created);
    return expected != std::numeric_limits<uint64_t>::max() &&
           admission.sequence.load(std::memory_order_acquire) == source_sequence &&
           admission.context.load(std::memory_order_relaxed) == expected &&
           admission.srs_resource_id.load(std::memory_order_relaxed) == srs_resource_id &&
           admission.srs_config_id.load(std::memory_order_relaxed) == srs_config_id &&
           claim_output(admission, 1U << 0U);
  }

  dapp::class_c_process_endpoint_metrics snapshot_metrics() const noexcept override
  {
    dapp::class_c_process_endpoint_metrics result;
    result.observed_cell_bitmap     = observed_cell_bitmap.load(std::memory_order_relaxed);
    result.publications             = publications.load(std::memory_order_relaxed);
    result.payload_bytes            = bytes_published.load(std::memory_order_relaxed);
    result.backpressure_drops       = backpressure_drops.load(std::memory_order_relaxed);
    result.invalid_drops            = invalid_publications.load(std::memory_order_relaxed);
    result.encode_failures          = encode_failures.load(std::memory_order_relaxed);
    result.notification_failures    = notification_failures.load(std::memory_order_relaxed);
    result.inactive_bypasses        = inactive_bypasses.load(std::memory_order_relaxed);
    result.concurrent_drops         = concurrent_entry_drops.load(std::memory_order_relaxed);
    result.snapshot_copy_operations = snapshot_copy_operations.load(std::memory_order_relaxed);
    result.snapshot_copy_bytes      = snapshot_copy_bytes.load(std::memory_order_relaxed);
    result.export_copy_operations   = export_copy_operations.load(std::memory_order_relaxed);
    result.export_copy_bytes        = export_copy_bytes.load(std::memory_order_relaxed);
#ifdef ENABLE_CUDA
    if (accelerator != nullptr) {
      const auto pool                         = accelerator->snapshot_metrics();
      result.export_slots_capacity            = pool.capacity;
      result.export_slots_in_use              = pool.in_use;
      result.export_slots_high_watermark      = pool.high_watermark;
      result.export_slot_reservation_failures = pool.reservation_failures;
    }
#endif
    return result;
  }

  bool uses_accelerator_spectrum() const noexcept override
  {
#ifdef ENABLE_CUDA
    return id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 && accelerator != nullptr;
#else
    return false;
#endif
  }

  bool uses_accelerator_srs() const noexcept override
  {
#ifdef ENABLE_CUDA
    return id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1 && accelerator != nullptr && srs_accelerator != nullptr;
#else
    return false;
#endif
  }

  void publish_accelerator_spectrum(const ocudu_dapp_spectrum_input_v1& source,
                                    const resource_grid_reader&         grid,
                                    const dapp::resource_grid_lease&    lease) noexcept override
  {
#ifdef ENABLE_CUDA
    if (!uses_accelerator_spectrum()) {
      return;
    }
    if (producer_entry.test_and_set(std::memory_order_acquire)) {
      concurrent_entry_drops.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const entry_guard release{producer_entry};
    const uint64_t    completed = producer->reclaim_completed();
    if (!accelerator->reclaim(completed)) {
      invalid_publications.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (!active()) {
      inactive_bypasses.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    dapp::accelerator_resource_grid_lease accelerator_lease{};
    static_cast<dapp::resource_grid_lease&>(accelerator_lease) = lease;
    accelerator_lease.backend_id                               = OCUDU_DAPP_BACKEND_CUDA_V1;
    accelerator_lease.device_ordinal                           = accelerator->device_ordinal();
    accelerator_lease.execution_stream                         = accelerator->stream();
    accelerator_lease.completion_token                         = lease.lease_token;
    auto tensor = dapp::make_accelerator_resource_grid_tensor(grid, accelerator_lease);
    if (!tensor) {
      invalid_publications.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    auto input                        = source;
    input.invocation.execution_stream = reinterpret_cast<uintptr_t>(accelerator->stream());
    input.uplink_resource_grid        = tensor.tensor;
    publish_external(input, tensor.tensor);
    if (tensor.live_hold && !dapp::finish_accelerator_resource_grid_read(grid, accelerator->stream())) {
      invalid_publications.fetch_add(1, std::memory_order_relaxed);
    }
#else
    (void)source;
    (void)grid;
    (void)lease;
#endif
  }

  void publish_accelerator_srs(const ocudu_dapp_srs_isac_input_v1& source,
                               const resource_grid_reader&         grid,
                               const srs_estimator_configuration&  config,
                               const dapp::resource_grid_lease&    lease) noexcept override
  {
#ifdef ENABLE_CUDA
    if (!uses_accelerator_srs()) {
      return;
    }
    if (producer_entry.test_and_set(std::memory_order_acquire)) {
      concurrent_entry_drops.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const entry_guard release{producer_entry};
    const uint64_t    completed = producer->reclaim_completed();
    if (!accelerator->reclaim(completed)) {
      invalid_publications.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (!active()) {
      inactive_bypasses.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    dapp::accelerator_resource_grid_lease accelerator_lease{};
    static_cast<dapp::resource_grid_lease&>(accelerator_lease) = lease;
    accelerator_lease.backend_id                               = OCUDU_DAPP_BACKEND_CUDA_V1;
    accelerator_lease.device_ordinal                           = accelerator->device_ordinal();
    accelerator_lease.execution_stream                         = accelerator->stream();
    accelerator_lease.completion_token                         = lease.lease_token;
    auto tensor                                                = srs_accelerator->make(grid, config, accelerator_lease);
    if (tensor) {
      auto input                        = source;
      input.invocation.execution_stream = reinterpret_cast<uintptr_t>(accelerator->stream());
      input.channel_estimates           = tensor.tensor;
      publish_external(input, tensor.tensor);
    } else {
      invalid_publications.fetch_add(1, std::memory_order_relaxed);
    }
    if (tensor.grid_lease_started && !dapp::finish_accelerator_resource_grid_read(grid, accelerator->stream())) {
      invalid_publications.fetch_add(1, std::memory_order_relaxed);
    }
#else
    (void)source;
    (void)grid;
    (void)config;
    (void)lease;
#endif
  }

  void publish(const ocudu_dapp_spectrum_input_v1& input) noexcept override
  {
    publish(input, input.uplink_resource_grid);
  }

  void publish(const ocudu_dapp_srs_isac_input_v1& input) noexcept override { publish(input, input.channel_estimates); }

  void publish(const ocudu_dapp_ul_resource_plan_v1& plan) noexcept override
  {
    if (flags != OCUDU_DAPP_ROUTE_FLAG_UL_RESOURCE_PLAN_V1) {
      return;
    }
    if (producer_entry.test_and_set(std::memory_order_acquire)) {
      concurrent_entry_drops.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const entry_guard release{producer_entry};
    if (!active()) {
      inactive_bypasses.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const uint64_t observation_time = monotonic_time_ns();
    const uint64_t epoch            = producer->route_epoch();
    const auto     encoded          = encoder->encode(plan, epoch, observation_time);
    if (!encoded) {
      if (encoded.error == class_c_encode_error::invalid_input) {
        invalid_publications.fetch_add(1, std::memory_order_relaxed);
      } else {
        encode_failures.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }
    const uint64_t record_size = sizeof(ocudu_dapp_publication_record_header_v1) + encoded.metadata.size();
    if (record_size > producer->payload_capacity() || encoded.metadata.size() > std::numeric_limits<uint32_t>::max()) {
      invalid_publications.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    ocudu_dapp_publication_record_header_v1 header{};
    header.magic                = OCUDU_DAPP_PUBLICATION_RECORD_MAGIC_V1;
    header.version              = OCUDU_DAPP_PUBLICATION_RECORD_VERSION_V1;
    header.header_size          = sizeof(header);
    header.metadata_size        = encoded.metadata.size();
    header.publication_sequence = plan.plan_generation;
    header.route_epoch          = epoch;
    const std::array<payload_fragment, 2> fragments{{
        {{reinterpret_cast<const uint8_t*>(&header), sizeof(header)}, 0},
        {encoded.metadata, 0},
    }};
    const publish_result                  result = producer->publish(fragments);
    if (result == publish_result::published || result == publish_result::notification_failed) {
      publications.fetch_add(1, std::memory_order_relaxed);
      bytes_published.fetch_add(record_size, std::memory_order_relaxed);
      if (result == publish_result::notification_failed) {
        notification_failures.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (result == publish_result::full) {
      backpressure_drops.fetch_add(1, std::memory_order_relaxed);
    } else {
      invalid_publications.fetch_add(1, std::memory_order_relaxed);
    }
  }

private:
#ifdef ENABLE_CUDA
  template <typename Input>
  void publish_external(const Input& input, const ocudu_dapp_tensor_view_v1& tensor) noexcept
  {
    const uint64_t observation_time = monotonic_time_ns();
    const uint64_t epoch            = producer->route_epoch();
    const auto     encoded          = encoder->encode(input, epoch, observation_time);
    if (!encoded) {
      if (encoded.error == class_c_encode_error::invalid_input ||
          encoded.error == class_c_encode_error::unsupported_tensor) {
        invalid_publications.fetch_add(1, std::memory_order_relaxed);
      } else {
        encode_failures.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }
    const uint64_t record_size = sizeof(ocudu_dapp_publication_record_header_v1) + encoded.metadata.size();
    if (record_size > producer->payload_capacity() || encoded.metadata.size() > std::numeric_limits<uint32_t>::max()) {
      invalid_publications.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const uint32_t slot = accelerator->reserve();
    if (slot == std::numeric_limits<uint32_t>::max()) {
      backpressure_drops.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (!accelerator->snapshot(slot, tensor)) {
      accelerator->cancel(slot);
      invalid_publications.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    ocudu_dapp_publication_record_header_v1 header{};
    header.magic                = OCUDU_DAPP_PUBLICATION_RECORD_MAGIC_V1;
    header.version              = OCUDU_DAPP_PUBLICATION_RECORD_VERSION_V1;
    header.header_size          = sizeof(header);
    header.flags                = OCUDU_DAPP_PUBLICATION_RECORD_EXTERNAL_TENSOR_V1;
    header.metadata_size        = encoded.metadata.size();
    header.tensor_offset        = slot;
    header.tensor_size          = tensor.byte_length;
    header.publication_sequence = input.source_sequence;
    header.route_epoch          = epoch;
    const std::array<payload_fragment, 2> fragments{
        { {{reinterpret_cast<const uint8_t*>(&header), sizeof(header)}, 0},
          {encoded.metadata, 0},
        }};

    auto& admission = result_admissions[input.source_sequence % result_admissions.size()];
    if (admission.access.test_and_set(std::memory_order_acquire)) {
      accelerator->cancel(slot);
      // Contention with a concurrent supervisor consume_* call, not ring
      // pressure: count it separately so operators can tell the two apart.
      concurrent_entry_drops.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const entry_guard admission_release{admission.access};
    uint64_t          plan_generation = 0;
    uint32_t          srs_resource_id = 0;
    uint32_t          srs_config_id   = 0;
    if constexpr (std::is_same_v<Input, ocudu_dapp_spectrum_input_v1>) {
      const auto* metadata =
          static_cast<const ocudu_dapp_spectrum_slot_metadata_v1*>(input.slot_metadata.metadata.data);
      plan_generation = metadata != nullptr ? metadata->source_plan_generation : 0;
    } else {
      const auto* metadata = static_cast<const ocudu_dapp_srs_isac_metadata_v1*>(input.srs_metadata.metadata.data);
      if (metadata != nullptr) {
        srs_resource_id = metadata->srs_resource_id;
        srs_config_id   = metadata->srs_config_id;
      }
    }
    result_admission_commit admission_commit{&admission,
                                             input.source_sequence,
                                             source_context(input.invocation.cell_id, input.invocation.radio_time),
                                             plan_generation,
                                             srs_resource_id,
                                             srs_config_id};
    struct external_commit_context {
      result_admission_commit*        admission;
      dapp::class_c_cuda_export_pool* pool;
      uint32_t                        slot;
      bool                            committed{};
    } commit{&admission_commit, accelerator.get(), slot};

    const publish_result result = producer->publish_external(
        fragments,
        0,
        [](void* opaque) noexcept {
          auto& value     = *static_cast<external_commit_context*>(opaque);
          value.committed = value.pool->commit(value.slot);
          if (!value.committed) {
            return;
          }
          value.admission->admission->context.store(value.admission->context, std::memory_order_relaxed);
          value.admission->admission->plan_generation.store(value.admission->plan_generation,
                                                            std::memory_order_relaxed);
          value.admission->admission->srs_resource_id.store(value.admission->srs_resource_id,
                                                            std::memory_order_relaxed);
          value.admission->admission->srs_config_id.store(value.admission->srs_config_id, std::memory_order_relaxed);
          value.admission->admission->consumed_outputs.store(0, std::memory_order_relaxed);
          value.admission->admission->sequence.store(value.admission->sequence, std::memory_order_release);
        },
        &commit);
    if (result == publish_result::published || result == publish_result::notification_failed) {
      if (!commit.committed) {
        invalid_publications.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      publications.fetch_add(1, std::memory_order_relaxed);
      observe_cell(input.invocation.cell_id);
      bytes_published.fetch_add(record_size, std::memory_order_relaxed);
      export_copy_operations.fetch_add(1, std::memory_order_relaxed);
      export_copy_bytes.fetch_add(tensor.byte_length, std::memory_order_relaxed);
      if (result == publish_result::notification_failed) {
        notification_failures.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      accelerator->cancel(slot);
      if (result == publish_result::full) {
        backpressure_drops.fetch_add(1, std::memory_order_relaxed);
      } else {
        invalid_publications.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
#endif

  static bool claim_output(result_admission& admission, uint32_t output) noexcept
  {
    uint32_t consumed = admission.consumed_outputs.load(std::memory_order_relaxed);
    do {
      if ((consumed & output) != 0) {
        return false;
      }
    } while (!admission.consumed_outputs.compare_exchange_weak(
        consumed, consumed | output, std::memory_order_acq_rel, std::memory_order_relaxed));
    return true;
  }

  bool consume_spectrum_output(uint64_t                        source_sequence,
                               uint64_t                        source_plan_generation,
                               uint32_t                        cell_id,
                               const ocudu_dapp_radio_time_v1& created,
                               uint32_t                        output) noexcept
  {
    if (source_sequence == 0) {
      return false;
    }
    auto& admission = result_admissions[source_sequence % result_admissions.size()];
    if (admission.access.test_and_set(std::memory_order_acquire)) {
      return false;
    }
    const entry_guard release{admission.access};
    const uint64_t    expected = source_context(cell_id, created);
    return expected != std::numeric_limits<uint64_t>::max() &&
           admission.sequence.load(std::memory_order_acquire) == source_sequence &&
           admission.context.load(std::memory_order_relaxed) == expected &&
           admission.plan_generation.load(std::memory_order_relaxed) == source_plan_generation &&
           claim_output(admission, output);
  }

  template <typename Input>
  void publish(const Input& input, const ocudu_dapp_tensor_view_v1& tensor) noexcept
  {
    if (producer_entry.test_and_set(std::memory_order_acquire)) {
      concurrent_entry_drops.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const entry_guard release{producer_entry};
    if (!active()) {
      inactive_bypasses.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const uint64_t observation_time = monotonic_time_ns();
    const uint64_t epoch            = producer->route_epoch();
    const auto     encoded          = encoder->encode(input, epoch, observation_time);
    if (!encoded) {
      if (encoded.error == class_c_encode_error::invalid_input ||
          encoded.error == class_c_encode_error::unsupported_tensor) {
        invalid_publications.fetch_add(1, std::memory_order_relaxed);
      } else {
        encode_failures.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }
    if (tensor.byte_length > std::numeric_limits<std::size_t>::max() ||
        tensor.native_handle > std::numeric_limits<uintptr_t>::max() ||
        tensor.byte_offset > std::numeric_limits<uintptr_t>::max() - tensor.native_handle ||
        encoded.metadata.size() > std::numeric_limits<uint32_t>::max()) {
      invalid_publications.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const uint64_t metadata_end  = sizeof(ocudu_dapp_publication_record_header_v1) + encoded.metadata.size();
    const uint64_t tensor_offset = align_record(metadata_end);
    if (tensor_offset == 0 || tensor.byte_length > std::numeric_limits<uint64_t>::max() - tensor_offset ||
        tensor_offset + tensor.byte_length > producer->payload_capacity()) {
      invalid_publications.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    ocudu_dapp_publication_record_header_v1 header{};
    header.magic                = OCUDU_DAPP_PUBLICATION_RECORD_MAGIC_V1;
    header.version              = OCUDU_DAPP_PUBLICATION_RECORD_VERSION_V1;
    header.header_size          = sizeof(header);
    header.flags                = OCUDU_DAPP_PUBLICATION_RECORD_INLINE_TENSOR_V1;
    header.metadata_size        = encoded.metadata.size();
    header.tensor_offset        = tensor_offset;
    header.tensor_size          = tensor.byte_length;
    header.publication_sequence = input.source_sequence;
    header.route_epoch          = epoch;

    const auto* tensor_data =
        reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(tensor.native_handle + tensor.byte_offset));
    const std::array<payload_fragment, 3> fragments{{
        {{reinterpret_cast<const uint8_t*>(&header), sizeof(header)}, 0},
        {encoded.metadata, static_cast<uint32_t>(tensor_offset - metadata_end)},
        {{tensor_data, static_cast<std::size_t>(tensor.byte_length)}, 0},
    }};
    auto& admission = result_admissions[input.source_sequence % result_admissions.size()];
    if (admission.access.test_and_set(std::memory_order_acquire)) {
      // Admission-table contention, not ring pressure — see the CUDA path.
      concurrent_entry_drops.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const entry_guard admission_release{admission.access};
    uint64_t          plan_generation = 0;
    uint32_t          srs_resource_id = 0;
    uint32_t          srs_config_id   = 0;
    if constexpr (std::is_same_v<Input, ocudu_dapp_spectrum_input_v1>) {
      const auto* metadata =
          static_cast<const ocudu_dapp_spectrum_slot_metadata_v1*>(input.slot_metadata.metadata.data);
      plan_generation = metadata != nullptr ? metadata->source_plan_generation : 0;
    } else {
      const auto* metadata = static_cast<const ocudu_dapp_srs_isac_metadata_v1*>(input.srs_metadata.metadata.data);
      if (metadata != nullptr) {
        srs_resource_id = metadata->srs_resource_id;
        srs_config_id   = metadata->srs_config_id;
      }
    }
    result_admission_commit commit{&admission,
                                   input.source_sequence,
                                   source_context(input.invocation.cell_id, input.invocation.radio_time),
                                   plan_generation,
                                   srs_resource_id,
                                   srs_config_id};
    const publish_result    result = producer->publish(
        fragments,
        0,
        [](void* opaque) noexcept {
          auto& value = *static_cast<result_admission_commit*>(opaque);
          value.admission->context.store(value.context, std::memory_order_relaxed);
          value.admission->plan_generation.store(value.plan_generation, std::memory_order_relaxed);
          value.admission->srs_resource_id.store(value.srs_resource_id, std::memory_order_relaxed);
          value.admission->srs_config_id.store(value.srs_config_id, std::memory_order_relaxed);
          value.admission->consumed_outputs.store(0, std::memory_order_relaxed);
          value.admission->sequence.store(value.sequence, std::memory_order_release);
        },
        &commit);
    if (result == publish_result::published || result == publish_result::notification_failed) {
      publications.fetch_add(1, std::memory_order_relaxed);
      observe_cell(input.invocation.cell_id);
      bytes_published.fetch_add(tensor_offset + tensor.byte_length, std::memory_order_relaxed);
      snapshot_copy_operations.fetch_add(1, std::memory_order_relaxed);
      snapshot_copy_bytes.fetch_add(tensor.byte_length, std::memory_order_relaxed);
      if (result == publish_result::notification_failed) {
        notification_failures.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (result == publish_result::full) {
      backpressure_drops.fetch_add(1, std::memory_order_relaxed);
    } else {
      invalid_publications.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void observe_cell(uint32_t cell_id) noexcept
  {
    if (cell_id < OCUDU_DAPP_HOST_INPUT_CELL_BITMAP_BITS_V1) {
      observed_cell_bitmap.fetch_or(uint32_t{1} << cell_id, std::memory_order_relaxed);
    }
  }

  const uint32_t                                          id;
  const uint32_t                                          flags;
  std::unique_ptr<publication_producer>                   producer;
  std::unique_ptr<class_c_publication_encoder>            encoder;
  std::atomic<bool>                                       enabled{};
  std::atomic<uint32_t>                                   observed_cell_bitmap{};
  std::atomic<uint64_t>                                   publications{};
  std::atomic<uint64_t>                                   bytes_published{};
  std::atomic<uint64_t>                                   backpressure_drops{};
  std::atomic<uint64_t>                                   invalid_publications{};
  std::atomic<uint64_t>                                   encode_failures{};
  std::atomic<uint64_t>                                   notification_failures{};
  std::atomic<uint64_t>                                   inactive_bypasses{};
  std::atomic<uint64_t>                                   concurrent_entry_drops{};
  std::atomic<uint64_t>                                   snapshot_copy_operations{};
  std::atomic<uint64_t>                                   snapshot_copy_bytes{};
  std::atomic<uint64_t>                                   export_copy_operations{};
  std::atomic<uint64_t>                                   export_copy_bytes{};
  std::array<result_admission, result_admission_capacity> result_admissions{};
  std::atomic_flag                                        producer_entry = ATOMIC_FLAG_INIT;
#ifdef ENABLE_CUDA
  std::unique_ptr<dapp::class_c_cuda_export_pool>            accelerator;
  std::unique_ptr<dapp::srs_channel_estimate_tensor_adapter> srs_accelerator;
#endif
};

} // namespace

upper_phy_dapp_process_target_create_result
ocudu::create_upper_phy_dapp_process_target(upper_phy_dapp_process_target_config config) noexcept
{
  if (config.interface_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 &&
      config.interface_id != OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, "unsupported Class-C process interface", {}};
  }
  if (config.route_flags != 0 && (config.interface_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 ||
                                  config.route_flags != OCUDU_DAPP_ROUTE_FLAG_UL_RESOURCE_PLAN_V1)) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, "unsupported Class-C process route flags", {}};
  }
  if (config.backend_id != OCUDU_DAPP_BACKEND_HOST_V1 && config.backend_id != OCUDU_DAPP_BACKEND_CUDA_V1) {
    return {OCUDU_DAPP_UNSUPPORTED_V1, "unsupported Class-C process backend", {}};
  }
  if (config.backend_id == OCUDU_DAPP_BACKEND_CUDA_V1 && config.route_flags != 0) {
    return {OCUDU_DAPP_UNSUPPORTED_V1, "CUDA process export supports only tensor data routes", {}};
  }
  auto producer = publication_producer::create(config.slot_count, config.payload_capacity);
  if (!producer) {
    return {OCUDU_DAPP_RESOURCE_V1, std::move(producer.detail), {}};
  }
  auto encoder = class_c_publication_encoder::create(std::move(config.encoder));
  if (!encoder) {
    return {OCUDU_DAPP_UNSUPPORTED_V1, std::move(encoder.detail), {}};
  }
#ifdef ENABLE_CUDA
  std::unique_ptr<dapp::class_c_cuda_export_pool>            accelerator;
  std::unique_ptr<dapp::srs_channel_estimate_tensor_adapter> srs_accelerator;
  if (config.backend_id == OCUDU_DAPP_BACKEND_CUDA_V1) {
    auto created = dapp::class_c_cuda_export_pool::create(config.slot_count, config.payload_capacity);
    if (!created) {
      return {created.status, std::move(created.detail), {}};
    }
    accelerator = std::move(created.pool);
    if (config.interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) {
      srs_accelerator = dapp::create_srs_channel_estimate_tensor_adapter();
      if (srs_accelerator == nullptr) {
        return {OCUDU_DAPP_RESOURCE_V1, "unable to create CUDA SRS channel-estimate adapter", {}};
      }
    }
  }
#else
  if (config.backend_id == OCUDU_DAPP_BACKEND_CUDA_V1) {
    return {OCUDU_DAPP_UNSUPPORTED_V1, "CUDA process export is not available in this build", {}};
  }
#endif
  try {
    return {OCUDU_DAPP_OK_V1,
            {},
            std::make_shared<process_target_impl>(config.interface_id,
                                                  config.route_flags,
                                                  std::move(producer.producer),
                                                  std::move(encoder.encoder)
#ifdef ENABLE_CUDA
                                                      ,
                                                  std::move(accelerator),
                                                  std::move(srs_accelerator)
#endif
                                                      )};
  } catch (const std::bad_alloc&) {
    return {OCUDU_DAPP_RESOURCE_V1, "unable to allocate Class-C process target", {}};
  } catch (...) {
    return {OCUDU_DAPP_INTERNAL_ERROR_V1, "unknown Class-C process target construction failure", {}};
  }
}
