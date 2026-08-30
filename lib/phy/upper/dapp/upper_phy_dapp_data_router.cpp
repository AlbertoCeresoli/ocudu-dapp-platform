// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/phy/upper/dapp/upper_phy_dapp_data_router.h"
#include "ocudu/adt/complex.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/upper/dapp/resource_grid_tensor_adapter.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_configuration.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/ran/slot_point.h"
#include "ocudu/ran/srs/srs_constants.h"
#include "ocudu/ran/srs/srs_information.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <tuple>

#ifdef ENABLE_CUDA
#include "srs_channel_estimate_tensor_adapter.h"
#include <cuda_runtime_api.h>
#include <sched.h>
#endif

using namespace ocudu;

namespace {

constexpr uint32_t max_route_fanout         = 64;
constexpr uint32_t max_srs_frequency_points = 272 * NOF_SUBCARRIERS_PER_RB / 2;
constexpr uint32_t any_producer             = std::numeric_limits<uint32_t>::max();
static_assert(sizeof(cf_t) == sizeof(float) * 2, "CF32 dApp tensor layout requires two floats");

std::unique_ptr<low_papr_sequence_generator> make_srs_generator()
{
  auto factory = create_low_papr_sequence_generator_sw_factory();
  return factory ? factory->create() : nullptr;
}

struct route_key {
  uint32_t interface_id;
  uint32_t cell_id;
  uint32_t producer_shard;
  uint32_t worker_lane;

  auto tie() const noexcept { return std::tie(interface_id, cell_id, producer_shard, worker_lane); }
  bool operator<(const route_key& other) const noexcept { return tie() < other.tie(); }
  bool operator==(const route_key& other) const noexcept { return tie() == other.tie(); }
};

struct srs_workspace {
  explicit srs_workspace(uint64_t allocation_id_) :
    allocation_id(allocation_id_),
    generator(make_srs_generator()),
    channel(srs_constants::max_nof_rx_ports * srs_constants::max_nof_tx_ports * max_srs_frequency_points),
    sequence(max_srs_frequency_points),
    received(max_srs_frequency_points)
  {
  }

  bool estimate(const resource_grid_reader&        grid,
                const srs_estimator_configuration& config,
                uint64_t                           generation,
                ocudu_dapp_tensor_view_v1&         output,
                srs_information&                   common_info) noexcept
  {
    try {
      return estimate_impl(grid, config, generation, output, common_info);
    } catch (...) {
      return false;
    }
  }

private:
  bool estimate_impl(const resource_grid_reader&        grid,
                     const srs_estimator_configuration& config,
                     uint64_t                           generation,
                     ocudu_dapp_tensor_view_v1&         output,
                     srs_information&                   common_info)
  {
    const uint32_t nof_rx_ports = config.ports.size();
    const uint32_t nof_tx_ports = static_cast<uint32_t>(config.resource.nof_antenna_ports);
    const uint32_t nof_symbols  = static_cast<uint32_t>(config.resource.nof_symbols);
    if (!generator || nof_rx_ports == 0 || nof_rx_ports > srs_constants::max_nof_rx_ports || nof_tx_ports == 0 ||
        nof_tx_ports > srs_constants::max_nof_tx_ports || nof_symbols == 0) {
      return false;
    }
    common_info                     = get_srs_information(config.resource, 0);
    const uint32_t frequency_points = common_info.sequence_length;
    if (frequency_points == 0 || frequency_points > max_srs_frequency_points) {
      return false;
    }

    for (uint32_t tx_port = 0; tx_port != nof_tx_ports; ++tx_port) {
      const srs_information info = get_srs_information(config.resource, tx_port);
      if (info.sequence_length != frequency_points) {
        return false;
      }
      const span<cf_t> sequence_view(sequence.data(), frequency_points);
      generator->generate(sequence_view, info.sequence_group, info.sequence_number, info.n_cs, info.n_cs_max);
      for (uint32_t rx_port_index = 0; rx_port_index != nof_rx_ports; ++rx_port_index) {
        cf_t* destination = channel.data() + (rx_port_index * nof_tx_ports + tx_port) * frequency_points;
        std::fill_n(destination, frequency_points, cf_t{});
        for (uint32_t symbol_offset = 0; symbol_offset != nof_symbols; ++symbol_offset) {
          const span<cf_t> received_view(received.data(), frequency_points);
          grid.get(received_view,
                   config.ports[rx_port_index],
                   config.resource.start_symbol.value() + symbol_offset,
                   info.mapping_initial_subcarrier,
                   info.comb_size);
          for (uint32_t frequency = 0; frequency != frequency_points; ++frequency) {
            destination[frequency] += received[frequency] * std::conj(sequence[frequency]);
          }
        }
        if (nof_symbols != 1) {
          const float scale = 1.0F / static_cast<float>(nof_symbols);
          for (uint32_t frequency = 0; frequency != frequency_points; ++frequency) {
            destination[frequency] *= scale;
          }
        }
      }
    }

    output                       = {};
    output.struct_size           = sizeof(output);
    output.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
    output.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
    output.domain                = OCUDU_DAPP_MEM_HOST_V1;
    output.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
    output.element_type          = OCUDU_DAPP_ELEMENT_CF32_V1;
    output.rank                  = 3;
    output.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
    output.allocation_id         = allocation_id;
    output.allocation_generation = generation;
    output.native_handle         = reinterpret_cast<uintptr_t>(channel.data());
    output.byte_length           = static_cast<uint64_t>(nof_rx_ports) * nof_tx_ports * frequency_points * sizeof(cf_t);
    output.dimensions[0]         = nof_rx_ports;
    output.dimensions[1]         = nof_tx_ports;
    output.dimensions[2]         = frequency_points;
    output.byte_strides[2]       = sizeof(cf_t);
    output.byte_strides[1]       = frequency_points * sizeof(cf_t);
    output.byte_strides[0]       = nof_tx_ports * output.byte_strides[1];
    output.layout_id             = OCUDU_DAPP_LAYOUT_SRS_CHANNEL_RX_TX_FREQUENCY_CF32_V1;
    output.layout_version        = 1;
    output.lease_token           = generation;
    return true;
  }

  uint64_t                                     allocation_id;
  std::unique_ptr<low_papr_sequence_generator> generator;
  std::vector<cf_t>                            channel;
  std::vector<cf_t>                            sequence;
  std::vector<cf_t>                            received;
};

struct srs_publish_key {
  uint32_t cell_id{};
  uint32_t producer_shard{};
  uint32_t worker_lane{};
  uint32_t sfn{};
  uint32_t slot_idx{};
  uint32_t rnti{};
  uint32_t resource{};

  auto tie() const noexcept { return std::tie(cell_id, producer_shard, worker_lane, sfn, slot_idx, rnti, resource); }
  bool operator==(const srs_publish_key& other) const noexcept { return tie() == other.tie(); }
};

struct route_group {
  route_key                                                       key;
  std::vector<std::shared_ptr<upper_phy_dapp_publication_target>> targets;
  uint64_t                                                        allocation_id{};
  uint64_t                                                        sequence{};
  std::unique_ptr<srs_workspace>                                  srs;
  std::optional<srs_publish_key>                                  last_srs_key;

  bool active() const noexcept
  {
    return std::any_of(targets.begin(), targets.end(), [](const auto& target) { return target->active(); });
  }

  /// Cheap variant for the per-symbol is_enabled gate; see
  /// upper_phy_dapp_publication_target::bound().
  bool bound() const noexcept
  {
    return std::any_of(targets.begin(), targets.end(), [](const auto& target) { return target->bound(); });
  }

  uint64_t next_sequence() noexcept
  {
    if (++sequence == 0) {
      sequence = 1;
    }
    return sequence;
  }
};

ocudu_dapp_invocation_v1 make_invocation(route_key key, slot_point slot, uint64_t sequence) noexcept
{
  ocudu_dapp_invocation_v1 invocation{};
  invocation.struct_size           = sizeof(invocation);
  invocation.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  invocation.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  invocation.invocation_id         = sequence;
  invocation.cell_id               = key.cell_id;
  invocation.radio_time.sfn        = slot.sfn();
  invocation.radio_time.slot       = slot.slot_index();
  invocation.radio_time.numerology = slot.numerology();
  invocation.input_generation      = sequence;
  return invocation;
}

template <typename T>
ocudu_dapp_typed_metadata_v1 make_metadata(uint32_t schema, const T& value) noexcept
{
  ocudu_dapp_typed_metadata_v1 metadata{};
  metadata.struct_size             = sizeof(metadata);
  metadata.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
  metadata.metadata_schema_id      = schema;
  metadata.metadata_schema_version = 1;
  metadata.metadata                = {&value, sizeof(value), 0};
  return metadata;
}

route_key make_key(uint32_t interface_id, upper_phy_dapp_producer_context producer) noexcept
{
  return {interface_id, producer.cell_id, producer.producer_shard, producer.worker_lane};
}

route_key make_key(const upper_phy_dapp_route_binding& binding) noexcept
{
  if (binding.all_srs_producers_on_worker_lane) {
    return {binding.target->interface_id(), any_producer, any_producer, binding.producer.worker_lane};
  }
  return make_key(binding.target->interface_id(), binding.producer);
}

int64_t seconds_to_nanoseconds(double seconds) noexcept
{
  const long double nanoseconds = static_cast<long double>(seconds) * 1.0e9L;
  if (std::isnan(nanoseconds)) {
    return 0;
  }
  if (nanoseconds >= static_cast<long double>(std::numeric_limits<int64_t>::max())) {
    return std::numeric_limits<int64_t>::max();
  }
  if (nanoseconds <= static_cast<long double>(std::numeric_limits<int64_t>::min())) {
    return std::numeric_limits<int64_t>::min();
  }
  return static_cast<int64_t>(std::round(nanoseconds));
}

float finite_or_zero(std::optional<float> value) noexcept
{
  return value.has_value() && std::isfinite(*value) ? *value : 0.0F;
}

float finite_nonnegative_or_zero(std::optional<float> value) noexcept
{
  return value.has_value() && std::isfinite(*value) && *value >= 0.0F ? *value : 0.0F;
}

bool get_common_srs_information(const resource_grid_reader&        grid,
                                const srs_estimator_configuration& config,
                                srs_information&                   common) noexcept
{
  try {
    const uint32_t nof_rx_ports = config.ports.size();
    const uint32_t nof_tx_ports = static_cast<uint32_t>(config.resource.nof_antenna_ports);
    const uint32_t nof_symbols  = static_cast<uint32_t>(config.resource.nof_symbols);
    if (nof_rx_ports == 0 || nof_rx_ports > srs_constants::max_nof_rx_ports || nof_tx_ports == 0 ||
        nof_tx_ports > srs_constants::max_nof_tx_ports || nof_symbols == 0 ||
        config.resource.start_symbol.value() + nof_symbols > grid.get_nof_symbols()) {
      return false;
    }
    for (uint32_t rx = 0; rx != nof_rx_ports; ++rx) {
      if (config.ports[rx] >= grid.get_nof_ports()) {
        return false;
      }
    }
    common = get_srs_information(config.resource, 0);
    if (common.sequence_length == 0 || common.sequence_length > max_srs_frequency_points) {
      return false;
    }
    for (uint32_t tx = 0; tx != nof_tx_ports; ++tx) {
      const auto     information     = get_srs_information(config.resource, tx);
      const uint64_t last_subcarrier = information.mapping_initial_subcarrier +
                                       static_cast<uint64_t>(information.sequence_length - 1) * information.comb_size;
      if (information.sequence_length != common.sequence_length || information.comb_size != common.comb_size ||
          last_subcarrier >= grid.get_nof_subc()) {
        return false;
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

class upper_phy_dapp_data_router_impl final : public upper_phy_dapp_data_router
{
public:
  explicit upper_phy_dapp_data_router_impl(std::vector<route_group> groups_) : groups(std::move(groups_)) {}

  bool is_enabled(uint32_t interface_id, upper_phy_dapp_producer_context context) const noexcept override
  {
    const route_group* group = find(interface_id, context);
    return group != nullptr && group->bound();
  }

  void publish_spectrum(upper_phy_dapp_producer_context context,
                        const resource_grid_reader&     grid,
                        slot_point                      slot,
                        upper_phy_dapp_expected_ul_plan plan) noexcept override
  {
    route_group* group = find(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, context);
    if (group == nullptr || !group->active()) {
      return;
    }
    const uint64_t sequence = group->next_sequence();

    ocudu_dapp_spectrum_slot_metadata_v1 slot_metadata{};
    slot_metadata.struct_size     = sizeof(slot_metadata);
    slot_metadata.abi_major       = OCUDU_DAPP_ABI_MAJOR_V1;
    slot_metadata.abi_minor       = OCUDU_DAPP_ABI_MINOR_V1;
    slot_metadata.bandwidth_hz    = static_cast<uint64_t>(grid.get_nof_subc()) * scs_to_khz(slot.scs()) * 1000U;
    slot_metadata.rx_ports        = grid.get_nof_ports();
    slot_metadata.symbols         = grid.get_nof_symbols();
    slot_metadata.subcarriers     = grid.get_nof_subc();
    slot_metadata.amplitude_scale = 1.0F;

    ocudu_dapp_tensor_view_v1 expected_use{};
    const uint32_t            nof_prbs = grid.get_nof_subc() / NOF_SUBCARRIERS_PER_RB;
    const bool valid_plan = plan.complete && plan.nof_symbols == grid.get_nof_symbols() && plan.nof_prbs == nof_prbs &&
                            nof_prbs != 0 && nof_prbs <= OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 &&
                            grid.get_nof_subc() % NOF_SUBCARRIERS_PER_RB == 0 &&
                            plan.cells.size() == static_cast<std::size_t>(plan.nof_symbols) * plan.nof_prbs;
    if (valid_plan) {
      slot_metadata.flags |= OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1;
      slot_metadata.source_plan_generation = sequence;
      expected_use.struct_size             = sizeof(expected_use);
      expected_use.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
      expected_use.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
      expected_use.domain                  = OCUDU_DAPP_MEM_HOST_V1;
      expected_use.backend_id              = OCUDU_DAPP_BACKEND_HOST_V1;
      expected_use.element_type            = OCUDU_DAPP_ELEMENT_U8_V1;
      expected_use.rank                    = 2;
      expected_use.access                  = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
      expected_use.allocation_id           = group->allocation_id + upper_phy_dapp_max_route_bindings;
      expected_use.allocation_generation   = sequence;
      expected_use.native_handle           = reinterpret_cast<uintptr_t>(plan.cells.data());
      expected_use.byte_length             = plan.cells.size();
      expected_use.dimensions[0]           = plan.nof_symbols;
      expected_use.dimensions[1]           = plan.nof_prbs;
      expected_use.byte_strides[0]         = plan.nof_prbs;
      expected_use.byte_strides[1]         = sizeof(uint8_t);
      expected_use.layout_id               = OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1;
      expected_use.layout_version          = 1;
      expected_use.lease_token             = sequence;
    }

    ocudu_dapp_spectrum_input_v1 input{};
    input.invocation        = make_invocation(make_key(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, context), slot, sequence);
    input.scheduled_re_mask = expected_use;
    input.slot_metadata     = make_metadata(OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1, slot_metadata);
    input.source_sequence   = sequence;
    std::optional<dapp::resource_grid_tensor_result> host_tensor;
    const dapp::resource_grid_lease                  lease{group->allocation_id, sequence, sequence};
    for (const auto& target : group->targets) {
      if (!target->active()) {
        continue;
      }
      if (target->uses_accelerator_spectrum()) {
        // GPU-native only: owned cudaMalloc snapshot. Never prefetch-home the
        // live UL grid for Class-C — that migrates pages and Xid 31's the
        // next fade/PUSCH GPU load.
        target->publish_accelerator_spectrum(input, grid, lease);
        continue;
      }
      if (!host_tensor) {
        host_tensor = dapp::make_host_resource_grid_tensor(grid, lease);
      }
      if (*host_tensor) {
        input.uplink_resource_grid = host_tensor->tensor;
        target->publish(input);
      }
    }
  }

  void publish_srs_isac(upper_phy_dapp_producer_context    context,
                        const resource_grid_reader&        grid,
                        const srs_estimator_configuration& config,
                        const srs_estimator_result&        result) noexcept override
  {
    route_group* group = find(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, context);
    if (group == nullptr || !group->active() || !group->srs) {
      return;
    }
    // Missing a frame is fine. rnti=0 mixes UEs into one Doppler ring.
    if (!config.context.has_value() || to_value(config.context->get_rnti()) == 0) {
      return;
    }
    srs_information information{};
    if (!get_common_srs_information(grid, config, information)) {
      return;
    }
    // The upper-PHY may surface the same completed observation through more
    // than one callback. Deduplicate within the producer lane without a global
    // hot-path lock. Include the actual route identity so wildcard lane routes
    // cannot suppress another cell or producer shard.
    const srs_publish_key once_key{context.cell_id,
                                   context.producer_shard,
                                   context.worker_lane,
                                   config.slot.sfn(),
                                   config.slot.slot_index(),
                                   to_value(config.context->get_rnti()),
                                   static_cast<uint32_t>(config.resource.configuration_index.value())};
    if (group->last_srs_key.has_value() && once_key == *group->last_srs_key) {
      return;
    }
    group->last_srs_key     = once_key;
    const uint64_t sequence = group->next_sequence();

    ocudu_dapp_srs_isac_metadata_v1 metadata{};
    metadata.struct_size           = sizeof(metadata);
    metadata.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
    metadata.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
    metadata.srs_resource_id       = config.resource.configuration_index.value();
    metadata.srs_config_id         = config.resource.configuration_index.value();
    metadata.rx_ports              = config.ports.size();
    metadata.tx_ports              = static_cast<uint32_t>(config.resource.nof_antenna_ports);
    metadata.comb_size             = information.comb_size;
    metadata.comb_offset           = config.resource.comb_offset.value();
    metadata.first_subcarrier      = information.mapping_initial_subcarrier;
    metadata.subcarrier_spacing_hz = scs_to_khz(config.slot.scs()) * 1000U;
    metadata.frequency_points      = information.sequence_length;
    metadata.noise_variance        = finite_nonnegative_or_zero(result.noise_variance);
    metadata.rsrp_db               = finite_or_zero(result.rsrp_dB);
    metadata.epre_db               = finite_or_zero(result.epre_dB);
    metadata.timing_advance_ns     = seconds_to_nanoseconds(result.time_alignment.time_alignment);
    // Per-UE attribution for multi-UE ISAC stream demux; 0 = unknown (see ABI header).
    metadata.rnti = config.context.has_value() ? to_value(config.context->get_rnti()) : 0U;

    ocudu_dapp_srs_isac_input_v1 input{};
    input.invocation      = make_invocation(make_key(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, context), config.slot, sequence);
    input.srs_metadata    = make_metadata(OCUDU_DAPP_METADATA_SRS_ISAC_V1, metadata);
    input.source_sequence = sequence;
    const dapp::resource_grid_lease          lease{group->allocation_id, sequence, sequence};
    std::optional<ocudu_dapp_tensor_view_v1> host_tensor;
    bool                                     host_attempted = false;
    for (const auto& target : group->targets) {
      if (!target->active()) {
        continue;
      }
      if (target->uses_accelerator_srs()) {
        target->publish_accelerator_srs(input, grid, config, lease);
        continue;
      }
      if (!host_attempted) {
        host_attempted = true;
        ocudu_dapp_tensor_view_v1 tensor{};
        srs_information           ignored{};
        if (group->srs->estimate(grid, config, sequence, tensor, ignored)) {
          host_tensor = tensor;
        }
      }
      if (host_tensor) {
        input.channel_estimates = *host_tensor;
        target->publish(input);
      }
    }
  }

private:
  route_group* find(route_key key) noexcept
  {
    auto found = std::lower_bound(
        groups.begin(), groups.end(), key, [](const route_group& group, route_key value) { return group.key < value; });
    return found != groups.end() && found->key == key ? &*found : nullptr;
  }

  const route_group* find(route_key key) const noexcept
  {
    auto found = std::lower_bound(
        groups.begin(), groups.end(), key, [](const route_group& group, route_key value) { return group.key < value; });
    return found != groups.end() && found->key == key ? &*found : nullptr;
  }

  route_group* find(uint32_t interface_id, upper_phy_dapp_producer_context context) noexcept
  {
    if (auto* exact = find(make_key(interface_id, context)); exact != nullptr) {
      return exact;
    }
    return interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1
               ? find(route_key{interface_id, any_producer, any_producer, context.worker_lane})
               : nullptr;
  }

  const route_group* find(uint32_t interface_id, upper_phy_dapp_producer_context context) const noexcept
  {
    if (const auto* exact = find(make_key(interface_id, context)); exact != nullptr) {
      return exact;
    }
    return interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1
               ? find(route_key{interface_id, any_producer, any_producer, context.worker_lane})
               : nullptr;
  }

  std::vector<route_group> groups;
};

class native_publication_target final : public upper_phy_dapp_publication_target
{
public:
  native_publication_target(std::shared_ptr<dapp::native_hook_point> point_, uint32_t lane_)
#ifdef ENABLE_CUDA
    :
    point(std::move(point_)), lane(lane_)
#else
    :
    point(std::move(point_)), lane(lane_)
#endif
  {
#ifdef ENABLE_CUDA
    if (point->interface_id() == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 ||
        point->interface_id() == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) {
      accelerator = cuda_execution_lane::create();
    }
    if (point->interface_id() == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1 && accelerator != nullptr) {
      srs_accelerator = dapp::create_srs_channel_estimate_tensor_adapter();
    }
    if (accelerator != nullptr) {
      static_cast<void>(point->set_completion_barrier(lane, accelerator));
    }
#endif
  }

  uint32_t interface_id() const noexcept override { return point->interface_id(); }
  bool     active() const noexcept override { return point->active(); }
  // Relaxed-atomic gate: avoids the lane-0 seq_cst reader-guard RMW pair that
  // active() pays, which ping-pongs one cache line across all cells' PHY
  // threads when checked per UL symbol.
  bool bound() const noexcept override { return point->bound(); }

  bool uses_accelerator_spectrum() const noexcept override
  {
    return interface_id() == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 && point->backend_id() == OCUDU_DAPP_BACKEND_CUDA_V1;
  }

  bool uses_accelerator_srs() const noexcept override
  {
    return interface_id() == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1 && point->backend_id() == OCUDU_DAPP_BACKEND_CUDA_V1;
  }

  void publish_accelerator_spectrum(const ocudu_dapp_spectrum_input_v1& source,
                                    const resource_grid_reader&         grid,
                                    const dapp::resource_grid_lease&    lease) noexcept override
  {
    if (!uses_accelerator_spectrum()) {
      return;
    }
    auto input                 = source;
    input.invocation.worker_id = point->worker_id(lane);
#ifdef ENABLE_CUDA
    if (accelerator != nullptr) {
      dapp::accelerator_resource_grid_lease accelerator_lease{};
      static_cast<dapp::resource_grid_lease&>(accelerator_lease) = lease;
      accelerator_lease.backend_id                               = OCUDU_DAPP_BACKEND_CUDA_V1;
      accelerator_lease.device_ordinal                           = accelerator->device_ordinal();
      accelerator_lease.execution_stream                         = accelerator->stream();
      accelerator_lease.completion_token                         = accelerator->completion_token();
      auto tensor = dapp::make_accelerator_resource_grid_tensor(grid, accelerator_lease);
      if (tensor) {
        input.invocation.execution_stream = reinterpret_cast<uintptr_t>(accelerator->stream());
        input.uplink_resource_grid        = tensor.tensor;
        point->publish(lane, input);
        if (tensor.live_hold && !dapp::finish_accelerator_resource_grid_read(grid, accelerator->stream())) {
          // Bounded: an unbounded cudaStreamSynchronize here runs on the UL-PHY
          // thread exactly when the stream is degraded. The grid's own read-hold
          // waiters are bounded too, so give up after the same 5 ms budget.
          static_cast<void>(accelerator->wait_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(5)));
        }
        return;
      }
      if (tensor.status != OCUDU_DAPP_BYPASS_V1 && tensor.status != OCUDU_DAPP_UNSUPPORTED_V1) {
        point->report_failure(lane, input.invocation, tensor.status);
      }
      return;
    }
#endif
    point->report_failure(lane, input.invocation, OCUDU_DAPP_RESOURCE_V1);
  }

  void publish(const ocudu_dapp_spectrum_input_v1& source) noexcept override
  {
    if (interface_id() != OCUDU_DAPP_INTERFACE_SPECTRUM_V1) {
      return;
    }
    auto input                 = source;
    input.invocation.worker_id = point->worker_id(lane);
    point->publish(lane, input);
  }

  void publish_accelerator_srs(const ocudu_dapp_srs_isac_input_v1& source,
                               const resource_grid_reader&         grid,
                               const srs_estimator_configuration&  config,
                               const dapp::resource_grid_lease&    lease) noexcept override
  {
    if (!uses_accelerator_srs()) {
      return;
    }
    auto input                 = source;
    input.invocation.worker_id = point->worker_id(lane);
#ifdef ENABLE_CUDA
    if (accelerator != nullptr && srs_accelerator != nullptr) {
      dapp::accelerator_resource_grid_lease accelerator_lease{};
      static_cast<dapp::resource_grid_lease&>(accelerator_lease) = lease;
      accelerator_lease.backend_id                               = OCUDU_DAPP_BACKEND_CUDA_V1;
      accelerator_lease.device_ordinal                           = accelerator->device_ordinal();
      accelerator_lease.execution_stream                         = accelerator->stream();
      accelerator_lease.completion_token                         = accelerator->completion_token();
      auto tensor = srs_accelerator->make(grid, config, accelerator_lease);
      if (tensor) {
        input.invocation.execution_stream = reinterpret_cast<uintptr_t>(accelerator->stream());
        input.channel_estimates           = tensor.tensor;
        point->publish(lane, input);
      }
      if (tensor.grid_lease_started && !dapp::finish_accelerator_resource_grid_read(grid, accelerator->stream())) {
        // Bounded for the same reason as the spectrum path above.
        static_cast<void>(accelerator->wait_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(5)));
      }
      if (!tensor && tensor.status != OCUDU_DAPP_BYPASS_V1 && tensor.status != OCUDU_DAPP_UNSUPPORTED_V1) {
        point->report_failure(lane, input.invocation, tensor.status);
      }
      return;
    }
#endif
    point->report_failure(lane, input.invocation, OCUDU_DAPP_RESOURCE_V1);
  }

  void publish(const ocudu_dapp_ul_resource_plan_v1& plan) noexcept override
  {
    if (interface_id() == OCUDU_DAPP_INTERFACE_SPECTRUM_V1) {
      point->publish(lane, plan);
    }
  }

  void publish(const ocudu_dapp_srs_isac_input_v1& source) noexcept override
  {
    if (interface_id() != OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) {
      return;
    }
    auto input                 = source;
    input.invocation.worker_id = point->worker_id(lane);
    point->publish(lane, input);
  }

private:
#ifdef ENABLE_CUDA
  class cuda_execution_lane final : public dapp::native_completion_barrier
  {
  public:
    static std::shared_ptr<cuda_execution_lane> create() noexcept
    {
      int          device = 0;
      cudaStream_t stream = nullptr;
      if (cudaGetDevice(&device) != cudaSuccess ||
          cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess) {
        static_cast<void>(cudaGetLastError());
        return {};
      }
      try {
        return std::shared_ptr<cuda_execution_lane>(new cuda_execution_lane(stream, device));
      } catch (...) {
        static_cast<void>(cudaStreamDestroy(stream));
        return {};
      }
    }

    ~cuda_execution_lane() override
    {
      static_cast<void>(cudaStreamSynchronize(cuda_stream));
      static_cast<void>(cudaStreamDestroy(cuda_stream));
    }

    bool wait_until(std::chrono::steady_clock::time_point deadline) noexcept override
    {
      if (deadline == std::chrono::steady_clock::time_point::max()) {
        return cudaStreamSynchronize(cuda_stream) == cudaSuccess;
      }
      for (;;) {
        const cudaError_t status = cudaStreamQuery(cuda_stream);
        if (status == cudaSuccess) {
          return true;
        }
        if (status != cudaErrorNotReady || std::chrono::steady_clock::now() >= deadline) {
          if (status != cudaErrorNotReady) {
            static_cast<void>(cudaGetLastError());
          }
          return false;
        }
        sched_yield();
      }
    }

    void*    stream() const noexcept { return cuda_stream; }
    uint32_t device_ordinal() const noexcept { return static_cast<uint32_t>(device); }
    uint64_t completion_token() const noexcept { return reinterpret_cast<uintptr_t>(cuda_stream); }

  private:
    cuda_execution_lane(cudaStream_t stream_, int device_) noexcept : cuda_stream(stream_), device(device_) {}

    cudaStream_t cuda_stream;
    int          device;
  };
#endif

  std::shared_ptr<dapp::native_hook_point> point;
  uint32_t                                 lane;
#ifdef ENABLE_CUDA
  std::shared_ptr<cuda_execution_lane>                       accelerator;
  std::unique_ptr<dapp::srs_channel_estimate_tensor_adapter> srs_accelerator;
#endif
};

} // namespace

upper_phy_dapp_data_router_create_result
ocudu::create_upper_phy_dapp_data_router(std::vector<upper_phy_dapp_route_binding> bindings) noexcept
{
  if (bindings.empty() || bindings.size() > upper_phy_dapp_max_route_bindings) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, "invalid Class-C route binding count", {}};
  }
  try {
    for (const auto& binding : bindings) {
      if (!binding.target ||
          (binding.target->interface_id() != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 &&
           binding.target->interface_id() != OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) ||
          (binding.all_srs_producers_on_worker_lane &&
           binding.target->interface_id() != OCUDU_DAPP_INTERFACE_SRS_ISAC_V1)) {
        return {OCUDU_DAPP_INVALID_INPUT_V1, "invalid Class-C route target", {}};
      }
    }
    std::sort(bindings.begin(), bindings.end(), [](const auto& lhs, const auto& rhs) {
      return make_key(lhs) < make_key(rhs);
    });
    std::vector<route_group> groups;
    groups.reserve(bindings.size());
    for (auto& binding : bindings) {
      const route_key key = make_key(binding);
      if (groups.empty() || !(groups.back().key == key)) {
        route_group group{};
        group.key           = key;
        group.allocation_id = groups.size() + 1;
        if (key.interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) {
          group.srs = std::make_unique<srs_workspace>(group.allocation_id);
        }
        groups.push_back(std::move(group));
      }
      if (groups.back().targets.size() == max_route_fanout) {
        return {OCUDU_DAPP_RESOURCE_V1, "Class-C route fanout exceeds bound", {}};
      }
      groups.back().targets.push_back(std::move(binding.target));
    }
    return {OCUDU_DAPP_OK_V1, {}, std::make_shared<upper_phy_dapp_data_router_impl>(std::move(groups))};
  } catch (const std::bad_alloc&) {
    return {OCUDU_DAPP_RESOURCE_V1, "unable to allocate Class-C route table", {}};
  } catch (const std::exception&) {
    return {OCUDU_DAPP_INTERNAL_ERROR_V1, "unable to construct Class-C route table", {}};
  } catch (...) {
    return {OCUDU_DAPP_INTERNAL_ERROR_V1, "unknown Class-C route construction failure", {}};
  }
}

std::shared_ptr<upper_phy_dapp_publication_target>
ocudu::create_native_upper_phy_dapp_target(std::shared_ptr<dapp::native_hook_point> point, uint32_t lane) noexcept
{
  if (!point || lane >= point->lane_count() ||
      (point->interface_id() != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 &&
       point->interface_id() != OCUDU_DAPP_INTERFACE_SRS_ISAC_V1)) {
    return {};
  }
  try {
    return std::make_shared<native_publication_target>(std::move(point), lane);
  } catch (...) {
    return {};
  }
}
