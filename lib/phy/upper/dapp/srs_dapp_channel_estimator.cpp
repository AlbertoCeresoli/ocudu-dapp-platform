// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/phy/upper/dapp/srs_dapp_channel_estimator.h"
#include "ocudu/dapp/runtime/abi_validation.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/upper/sequence_generators/low_papr_sequence_generator.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_configuration.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_factory.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/ran/slot_point.h"
#include "ocudu/ran/srs/srs_constants.h"
#include "ocudu/ran/srs/srs_information.h"
#include "ocudu/ran/subcarrier_spacing.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <vector>
#ifdef ENABLE_CUDA
#include "srs_dapp_channel_estimator_cuda.h"
#include <cuda_runtime.h>
#endif

using namespace ocudu;

namespace {

static_assert(sizeof(cf_t) == 2 * sizeof(float), "CF32 dApp tensors require two adjacent floats");
static_assert(srs_constants::max_nof_rx_ports <= OCUDU_DAPP_SRS_MAX_RX_PORTS_V1);
static_assert(srs_constants::max_nof_tx_ports <= OCUDU_DAPP_SRS_MAX_TX_PORTS_V1);

constexpr unsigned max_nof_symbols = OCUDU_DAPP_SRS_MAX_SYMBOLS_V1;
constexpr unsigned max_nof_sets    = OCUDU_DAPP_SRS_MAX_PILOT_SETS_V1;
constexpr unsigned max_nof_rx      = srs_constants::max_nof_rx_ports;
constexpr unsigned max_nof_tx      = srs_constants::max_nof_tx_ports;

/// Memory of the tensors of one invocation.
struct tensor_memory {
  uint32_t domain;
  uint32_t backend;
  uint32_t device;
};

constexpr tensor_memory host_memory{OCUDU_DAPP_MEM_HOST_V1, OCUDU_DAPP_BACKEND_HOST_V1, 0};

template <typename T>
ocudu_dapp_tensor_view_v1 make_tensor(T*                              data,
                                      const tensor_memory&            memory,
                                      uint32_t                        element_type,
                                      uint32_t                        access,
                                      uint32_t                        layout_id,
                                      uint64_t                        allocation_id,
                                      uint64_t                        generation,
                                      std::initializer_list<uint64_t> dimensions) noexcept
{
  ocudu_dapp_tensor_view_v1 tensor{};
  tensor.struct_size    = sizeof(tensor);
  tensor.abi_major      = OCUDU_DAPP_ABI_MAJOR_V1;
  tensor.abi_minor      = OCUDU_DAPP_ABI_MINOR_V1;
  tensor.domain         = memory.domain;
  tensor.backend_id     = memory.backend;
  tensor.device_ordinal = memory.device;
  tensor.element_type   = element_type;
  tensor.rank           = dimensions.size();
  tensor.access         = access | (memory.domain == OCUDU_DAPP_MEM_HOST_V1 ? OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1 : 0);
  tensor.allocation_id  = allocation_id;
  tensor.allocation_generation = generation;
  tensor.native_handle         = reinterpret_cast<uintptr_t>(data);
  tensor.lease_token           = generation;
  tensor.layout_id             = layout_id;
  tensor.layout_version        = 1;

  uint64_t elements = 1;
  unsigned index    = 0;
  for (uint64_t dimension : dimensions) {
    tensor.dimensions[index++] = dimension;
    elements *= dimension;
  }
  uint64_t stride = sizeof(T);
  for (unsigned dimension = tensor.rank; dimension != 0; --dimension) {
    tensor.byte_strides[dimension - 1] = stride;
    stride *= tensor.dimensions[dimension - 1];
  }
  tensor.byte_length = elements * sizeof(T);
  return tensor;
}

uint64_t deadline_from_now(std::chrono::nanoseconds budget) noexcept
{
  const auto now =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (now < 0 || budget.count() < 0) {
    return 0;
  }
  const uint64_t value = static_cast<uint64_t>(now);
  const uint64_t delta = static_cast<uint64_t>(budget.count());
  return delta > std::numeric_limits<uint64_t>::max() - value ? std::numeric_limits<uint64_t>::max() : value + delta;
}

/// An SRS occasion as the module sees it.
struct srs_occasion {
  ocudu_dapp_srs_channel_estimator_metadata_v1 metadata{};
  std::array<srs_information, max_nof_tx>      ports{};
};

#ifdef ENABLE_CUDA
/// Device tensors and the stream of one lane, plus pinned staging for the host-side transfers.
class device_workspace
{
public:
  device_workspace(unsigned max_sequence_length)
  {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0 || cudaGetDevice(&device) != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      return;
    }
    const size_t rx_bytes        = sizeof(cf_t) * max_nof_sets * max_nof_rx * max_nof_symbols * max_sequence_length;
    const size_t reference_bytes = sizeof(cf_t) * max_nof_tx * max_sequence_length;
    ready                        = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess &&
            cudaMalloc(&rx_symbols, rx_bytes) == cudaSuccess &&
            cudaMalloc(&references, reference_bytes) == cudaSuccess &&
            cudaMalloc(&channel, sizeof(cf_t) * max_nof_rx * max_nof_tx) == cudaSuccess &&
            cudaMalloc(&measurements, sizeof(float) * OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1) == cudaSuccess &&
            cudaMallocHost(&host_references, reference_bytes) == cudaSuccess &&
            cudaMallocHost(&host_channel, sizeof(cf_t) * max_nof_rx * max_nof_tx) == cudaSuccess &&
            cudaMallocHost(&host_measurements, sizeof(float) * OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1) == cudaSuccess;
    if (!ready) {
      static_cast<void>(cudaGetLastError());
    }
  }

  ~device_workspace()
  {
    if (stream != nullptr) {
      static_cast<void>(cudaStreamSynchronize(stream));
      static_cast<void>(cudaStreamDestroy(stream));
    }
    for (void* pointer : {rx_symbols, references, channel, measurements}) {
      static_cast<void>(cudaFree(pointer));
    }
    for (void* pointer : {host_references, host_channel, host_measurements}) {
      static_cast<void>(cudaFreeHost(pointer));
    }
  }

  device_workspace(const device_workspace&)            = delete;
  device_workspace& operator=(const device_workspace&) = delete;

  bool         ready = false;
  int          device{};
  cudaStream_t stream{};
  void*        rx_symbols{};
  void*        references{};
  void*        channel{};
  void*        measurements{};
  void*        host_references{};
  void*        host_channel{};
  void*        host_measurements{};
};
#endif

class srs_estimator_decorator final : public srs_estimator
{
public:
  srs_estimator_decorator(std::unique_ptr<srs_estimator>                     fallback_,
                          std::shared_ptr<srs_dapp_channel_estimator_target> target_,
                          uint32_t                                           lane_,
                          std::unique_ptr<low_papr_sequence_generator>       sequence_generator_,
                          const srs_dapp_channel_estimator_config&           config_) :
    fallback(std::move(fallback_)),
    target(std::move(target_)),
    lane(lane_),
    sequence_generator(std::move(sequence_generator_)),
    deadline(config_.deadline),
    completion_deadline(config_.completion_deadline),
    max_sequence_length(
        std::min<unsigned>(config_.max_nof_prb * NOF_SUBCARRIERS_PER_RB / 2, OCUDU_DAPP_SRS_MAX_SEQUENCE_LENGTH_V1)),
    rx_symbols(static_cast<size_t>(max_nof_sets) * max_nof_rx * max_nof_symbols * max_sequence_length),
    references(static_cast<size_t>(max_nof_tx) * max_sequence_length)
  {
  }

  srs_estimator_result estimate(const resource_grid_reader& grid, const srs_estimator_configuration& config) override
  {
    if (!target->active()) {
      return fallback->estimate(grid, config);
    }
    srs_occasion occasion;
    if (const auto reason = describe(grid, config, occasion)) {
      target->report_fallback(lane, *reason);
      return fallback->estimate(grid, config);
    }
    std::optional<srs_estimator_result> result;
    try {
      generate_references(occasion);
      const uint32_t backend = target->backend_id();
      if (backend == OCUDU_DAPP_BACKEND_HOST_V1) {
        result = invoke_host(grid, config, occasion);
#ifdef ENABLE_CUDA
      } else if (backend == OCUDU_DAPP_BACKEND_CUDA_V1) {
        result = invoke_device(grid, config, occasion);
#endif
      } else {
        target->report_fallback(lane, dapp::class_a_fallback_reason::device);
      }
    } catch (...) {
      target->report_fallback(lane, dapp::class_a_fallback_reason::exception);
    }
    return result ? *std::move(result) : fallback->estimate(grid, config);
  }

private:
  /// Builds the module's view of the occasion, or says why the adapter cannot serve it.
  std::optional<dapp::class_a_fallback_reason>
  describe(const resource_grid_reader& grid, const srs_estimator_configuration& config, srs_occasion& occasion) const
  {
    const unsigned nof_rx      = config.ports.size();
    const unsigned nof_tx      = static_cast<unsigned>(config.resource.nof_antenna_ports);
    const unsigned nof_symbols = static_cast<unsigned>(config.resource.nof_symbols);
    const unsigned start       = config.resource.start_symbol.value();
    if (nof_rx == 0 || nof_rx > max_nof_rx || std::any_of(config.ports.begin(), config.ports.end(), [&](uint8_t port) {
          return port >= grid.get_nof_ports();
        })) {
      return dapp::class_a_fallback_reason::unsupported_rx_ports;
    }
    if (nof_tx == 0 || nof_tx > max_nof_tx) {
      return dapp::class_a_fallback_reason::unsupported_layers;
    }
    if (nof_symbols == 0 || start + nof_symbols > max_nof_symbols || start + nof_symbols > grid.get_nof_symbols() ||
        config.slot.numerology() > 4) {
      return dapp::class_a_fallback_reason::unsupported_allocation;
    }

    for (unsigned port = 0; port != nof_tx; ++port) {
      occasion.ports[port] = get_srs_information(config.resource, port);
    }
    const srs_information& port0       = occasion.ports[0];
    const bool             interleaved = nof_tx == 4 && port0.n_cs >= port0.n_cs_max / 2;
    const unsigned         nof_sets    = interleaved ? 2 : 1;
    if (port0.sequence_length == 0 || port0.sequence_length > max_sequence_length ||
        (port0.comb_size != 2 && port0.comb_size != 4)) {
      return dapp::class_a_fallback_reason::unsupported_allocation;
    }

    auto& srs                 = occasion.metadata;
    srs.struct_size           = sizeof(srs);
    srs.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
    srs.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
    srs.nof_rx_ports          = nof_rx;
    srs.nof_tx_ports          = nof_tx;
    srs.start_symbol          = start;
    srs.nof_symbols           = nof_symbols;
    srs.sequence_length       = port0.sequence_length;
    srs.comb_size             = port0.comb_size;
    srs.nof_pilot_sets        = nof_sets;
    srs.n_cs_max              = port0.n_cs_max;
    srs.sequence_group        = port0.sequence_group;
    srs.sequence_number       = port0.sequence_number;
    srs.subcarrier_spacing_hz = scs_to_khz(to_subcarrier_spacing(config.slot.numerology())) * 1000U;
    srs.numerology            = config.slot.numerology();
    srs.grid_nof_subcarriers  = grid.get_nof_subc();
    srs.flags                 = interleaved ? OCUDU_DAPP_SRS_FLAG_INTERLEAVED_PILOTS_V1 : 0;
    srs.rnti                  = config.context ? static_cast<uint32_t>(config.context->get_rnti()) : 0;
    srs.max_time_alignment_s =
        1.0 / static_cast<double>(port0.n_cs_max * srs.subcarrier_spacing_hz * static_cast<double>(port0.comb_size));
    for (unsigned set = 0; set != nof_sets; ++set) {
      srs.first_subcarrier[set] = occasion.ports[set].mapping_initial_subcarrier;
    }
    for (unsigned port = 0; port != nof_tx; ++port) {
      const srs_information& info = occasion.ports[port];
      srs.pilot_set[port]         = interleaved ? port % 2 : 0;
      srs.cyclic_shift[port]      = info.n_cs;
      // Every port shares the sequence and the comb; its subcarriers are those of its pilot set.
      if (info.sequence_length != port0.sequence_length || info.comb_size != port0.comb_size ||
          info.n_cs_max != port0.n_cs_max || info.sequence_group != port0.sequence_group ||
          info.sequence_number != port0.sequence_number ||
          info.mapping_initial_subcarrier != srs.first_subcarrier[srs.pilot_set[port]]) {
        return dapp::class_a_fallback_reason::unsupported_metadata;
      }
    }
    for (unsigned set = 0; set != nof_sets; ++set) {
      const uint64_t last =
          srs.first_subcarrier[set] + static_cast<uint64_t>(srs.comb_size) * (srs.sequence_length - 1);
      if (last >= srs.grid_nof_subcarriers) {
        return dapp::class_a_fallback_reason::unsupported_allocation;
      }
    }
    return std::nullopt;
  }

  /// The transmitted sequence of every port, cyclic shift included (TS 38.211 Section 6.4.1.4.2).
  void generate_references(const srs_occasion& occasion)
  {
    const auto&    srs    = occasion.metadata;
    const unsigned length = srs.sequence_length;
    for (unsigned port = 0; port != srs.nof_tx_ports; ++port) {
      const srs_information& info = occasion.ports[port];
      sequence_generator->generate(span<cf_t>(references).subspan(static_cast<size_t>(port) * length, length),
                                   info.sequence_group,
                                   info.sequence_number,
                                   info.n_cs,
                                   info.n_cs_max);
    }
  }

  uint64_t next_generation() noexcept
  {
    uint64_t generation = ++invocation_sequence;
    return generation != 0 ? generation : ++invocation_sequence;
  }

  ocudu_dapp_invocation_v1 make_invocation(const srs_estimator_configuration& config,
                                           uint64_t                           generation,
                                           uint64_t                           execution_stream) const noexcept
  {
    ocudu_dapp_invocation_v1 invocation{};
    invocation.struct_size           = sizeof(invocation);
    invocation.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
    invocation.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
    invocation.invocation_id         = generation;
    invocation.cell_id               = config.cell_id;
    invocation.worker_id             = target->worker_id(lane);
    invocation.radio_time.sfn        = config.slot.sfn();
    invocation.radio_time.slot       = config.slot.slot_index();
    invocation.radio_time.numerology = config.slot.numerology();
    invocation.deadline_monotonic_ns = deadline_from_now(deadline);
    invocation.execution_stream      = execution_stream;
    invocation.input_generation      = generation;
    invocation.output_generation     = generation;
    return invocation;
  }

  /// Input and output descriptors of one invocation over the given memory.
  struct invocation_views {
    ocudu_dapp_srs_channel_estimator_input_v1  input{};
    ocudu_dapp_srs_channel_estimator_output_v1 output{};
  };

  invocation_views make_views(const srs_estimator_configuration& config,
                              const srs_occasion&                occasion,
                              const tensor_memory&               memory,
                              void*                              rx_data,
                              void*                              reference_data,
                              void*                              channel_data,
                              void*                              measurement_data,
                              uint64_t                           execution_stream) noexcept
  {
    const auto&    srs             = occasion.metadata;
    const uint64_t generation      = next_generation();
    const uint64_t allocation_base = (static_cast<uint64_t>(target->worker_id(lane)) + 1U) << 8U;

    invocation_views views;
    auto&            input            = views.input;
    input.invocation                  = make_invocation(config, generation, execution_stream);
    input.rx_symbols                  = make_tensor(static_cast<cf_t*>(rx_data),
                                   memory,
                                   OCUDU_DAPP_ELEMENT_CF32_V1,
                                   OCUDU_DAPP_ACCESS_READ_V1,
                                   OCUDU_DAPP_LAYOUT_SRS_RX_SET_PORT_SYMBOL_PILOT_CF32_V1,
                                   allocation_base + 1,
                                   generation,
                                                    {srs.nof_pilot_sets, srs.nof_rx_ports, srs.nof_symbols, srs.sequence_length});
    input.reference_symbols           = make_tensor(static_cast<cf_t*>(reference_data),
                                          memory,
                                          OCUDU_DAPP_ELEMENT_CF32_V1,
                                          OCUDU_DAPP_ACCESS_READ_V1,
                                          OCUDU_DAPP_LAYOUT_SRS_REFERENCE_TX_PILOT_CF32_V1,
                                          allocation_base + 2,
                                          generation,
                                                    {srs.nof_tx_ports, srs.sequence_length});
    input.srs.struct_size             = sizeof(input.srs);
    input.srs.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
    input.srs.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
    input.srs.metadata_schema_id      = OCUDU_DAPP_METADATA_SRS_CHANNEL_ESTIMATOR_V1;
    input.srs.metadata_schema_version = 1;
    input.srs.metadata                = {&occasion.metadata, sizeof(occasion.metadata), 0};

    auto& output          = views.output;
    output.channel_matrix = make_tensor(static_cast<cf_t*>(channel_data),
                                        memory,
                                        OCUDU_DAPP_ELEMENT_CF32_V1,
                                        OCUDU_DAPP_ACCESS_WRITE_V1,
                                        OCUDU_DAPP_LAYOUT_SRS_CHANNEL_MATRIX_RX_TX_CF32_V1,
                                        allocation_base + 3,
                                        generation,
                                        {srs.nof_rx_ports, srs.nof_tx_ports});
    output.measurements   = make_tensor(static_cast<float*>(measurement_data),
                                      memory,
                                      OCUDU_DAPP_ELEMENT_F32_V1,
                                      OCUDU_DAPP_ACCESS_WRITE_V1,
                                      OCUDU_DAPP_LAYOUT_SRS_MEASUREMENTS_F32_V1,
                                      allocation_base + 4,
                                      generation,
                                        {OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1});
    return views;
  }

  /// Invokes the module and checks it kept the host-owned output storage.
  bool call(invocation_views& views) noexcept
  {
    const auto expected_channel      = views.output.channel_matrix;
    const auto expected_measurements = views.output.measurements;
    const auto invoked               = target->invoke(lane, views.input, views.output);
    if (invoked.status != OCUDU_DAPP_OK_V1 || invoked.invoke_fallback) {
      return false;
    }
    if (!dapp::same_tensor_storage(expected_channel, views.output.channel_matrix) ||
        !dapp::same_tensor_storage(expected_measurements, views.output.measurements)) {
      target->report_invalid_output(lane, views.input.invocation);
      return false;
    }
    return true;
  }

  /// Host tensors for a host package, read from the host resource grid (host-L1 gNB).
  std::optional<srs_estimator_result>
  invoke_host(const resource_grid_reader& grid, const srs_estimator_configuration& config, const srs_occasion& occasion)
  {
    const auto& srs = occasion.metadata;
    read_host_grid(grid, config, srs);
    auto views = make_views(
        config, occasion, host_memory, rx_symbols.data(), references.data(), channel.data(), measurements.data(), 0);
    if (!call(views)) {
      return std::nullopt;
    }
    return to_result(srs, channel.data(), measurements.data(), views.input.invocation);
  }

  void read_host_grid(const resource_grid_reader&                         grid,
                      const srs_estimator_configuration&                  config,
                      const ocudu_dapp_srs_channel_estimator_metadata_v1& srs)
  {
    const unsigned length = srs.sequence_length;
    for (unsigned set = 0; set != srs.nof_pilot_sets; ++set) {
      for (unsigned rx = 0; rx != srs.nof_rx_ports; ++rx) {
        for (unsigned symbol = 0; symbol != srs.nof_symbols; ++symbol) {
          const size_t row = (static_cast<size_t>(set) * srs.nof_rx_ports + rx) * srs.nof_symbols + symbol;
          grid.get(span<cf_t>(rx_symbols).subspan(row * length, length),
                   config.ports[rx],
                   srs.start_symbol + symbol,
                   srs.first_subcarrier[set],
                   srs.comb_size);
        }
      }
    }
  }

#ifdef ENABLE_CUDA
  /// Enqueues the gather of the SRS resource elements from the owned device snapshot into the lane's device
  /// rx_symbols. Returns the workspace, or nullptr (reported) when the device or the snapshot is unavailable.
  device_workspace* gather_on_device(const resource_grid_reader&                         grid,
                                     const srs_estimator_configuration&                  config,
                                     const ocudu_dapp_srs_channel_estimator_metadata_v1& srs)
  {
    if (!workspace) {
      workspace = std::make_unique<device_workspace>(max_sequence_length);
    }
    device_workspace& device = *workspace;
    if (!device.ready || cudaSetDevice(device.device) != cudaSuccess) {
      target->report_fallback(lane, dapp::class_a_fallback_reason::device);
      return nullptr;
    }

    // Read the owned device snapshot of the grid, never the live managed grid, like the CUDA SRS estimator.
    bool held = grid.prepare_owned_device_snapshot_reading(device.stream);
    if (!held && grid.stage_owned_snapshot_from_host()) {
      held = grid.prepare_owned_device_snapshot_reading(device.stream);
    }
    const void* snapshot = held ? grid.get_owned_device_snapshot_cbf16() : nullptr;
    if (snapshot == nullptr) {
      if (held) {
        grid.release_owned_device_snapshot_reading();
      }
      target->report_fallback(lane, dapp::class_a_fallback_reason::device);
      return nullptr;
    }
    dapp::srs_rx_symbol_gather_config gather;
    gather.nof_pilot_sets       = srs.nof_pilot_sets;
    gather.nof_rx_ports         = srs.nof_rx_ports;
    gather.nof_symbols          = srs.nof_symbols;
    gather.start_symbol         = srs.start_symbol;
    gather.sequence_length      = srs.sequence_length;
    gather.comb_size            = srs.comb_size;
    gather.grid_nof_symbols     = grid.get_nof_symbols();
    gather.grid_nof_subcarriers = grid.get_nof_subc();
    std::copy(config.ports.begin(), config.ports.end(), std::begin(gather.rx_ports));
    std::copy(std::begin(srs.first_subcarrier), std::end(srs.first_subcarrier), std::begin(gather.first_subcarrier));
    if (!dapp::launch_srs_rx_symbol_gather(snapshot, device.rx_symbols, gather, device.stream)) {
      grid.release_owned_device_snapshot_reading();
      static_cast<void>(cudaGetLastError());
      target->report_fallback(lane, dapp::class_a_fallback_reason::device);
      return nullptr;
    }
    if (!grid.on_owned_device_snapshot_reading_enqueued(device.stream)) {
      // Without a read fence the snapshot cannot be recycled safely; drain the gather first.
      static_cast<void>(cudaStreamSynchronize(device.stream));
    }
    return &device;
  }

  std::optional<srs_estimator_result> invoke_device(const resource_grid_reader&        grid,
                                                    const srs_estimator_configuration& config,
                                                    const srs_occasion&                occasion)
  {
    const auto&       srs    = occasion.metadata;
    device_workspace* gather = gather_on_device(grid, config, srs);
    if (gather == nullptr) {
      return std::nullopt;
    }
    device_workspace& device = *gather;

    const size_t reference_bytes = sizeof(cf_t) * srs.nof_tx_ports * srs.sequence_length;
    std::copy_n(references.data(), srs.nof_tx_ports * srs.sequence_length, static_cast<cf_t*>(device.host_references));
    if (cudaMemcpyAsync(
            device.references, device.host_references, reference_bytes, cudaMemcpyHostToDevice, device.stream) !=
        cudaSuccess) {
      static_cast<void>(cudaStreamSynchronize(device.stream));
      target->report_fallback(lane, dapp::class_a_fallback_reason::device);
      return std::nullopt;
    }

    const tensor_memory memory{
        OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1, OCUDU_DAPP_BACKEND_CUDA_V1, static_cast<uint32_t>(device.device)};
    auto       views   = make_views(config,
                            occasion,
                            memory,
                            device.rx_symbols,
                            device.references,
                            device.channel,
                            device.measurements,
                            reinterpret_cast<uintptr_t>(device.stream));
    const auto started = std::chrono::steady_clock::now();
    const bool invoked = call(views);
    // The module enqueues on the lane stream; its buffers are reused next occasion, so always drain it.
    const bool copied = invoked &&
                        cudaMemcpyAsync(device.host_channel,
                                        device.channel,
                                        sizeof(cf_t) * srs.nof_rx_ports * srs.nof_tx_ports,
                                        cudaMemcpyDeviceToHost,
                                        device.stream) == cudaSuccess &&
                        cudaMemcpyAsync(device.host_measurements,
                                        device.measurements,
                                        sizeof(float) * OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1,
                                        cudaMemcpyDeviceToHost,
                                        device.stream) == cudaSuccess;
    const bool drained = cudaStreamSynchronize(device.stream) == cudaSuccess;
    if (!invoked) {
      return std::nullopt;
    }
    // The soft wall covers the module's queued work and the readback; a late result is still used.
    const auto observed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
    if (drained && observed > completion_deadline.count()) {
      target->report_completion_deadline(lane,
                                         views.input.invocation,
                                         static_cast<uint64_t>(observed),
                                         static_cast<uint64_t>(completion_deadline.count()));
    }
    if (!copied || !drained) {
      static_cast<void>(cudaGetLastError());
      target->report_fallback(lane, dapp::class_a_fallback_reason::device);
      return std::nullopt;
    }
    return to_result(srs,
                     static_cast<const cf_t*>(device.host_channel),
                     static_cast<const float*>(device.host_measurements),
                     views.input.invocation);
  }
#endif

  /// Maps the module outputs to the estimator result; a non-finite coefficient or an out-of-range measurement
  /// is an invalid output.
  std::optional<srs_estimator_result> to_result(const ocudu_dapp_srs_channel_estimator_metadata_v1& srs,
                                                const cf_t*                                         coefficients,
                                                const float*                                        values,
                                                const ocudu_dapp_invocation_v1&                     invocation) noexcept
  {
    srs_estimator_result result;
    result.channel_matrix = srs_channel_matrix(srs.nof_rx_ports, srs.nof_tx_ports);
    for (unsigned rx = 0; rx != srs.nof_rx_ports; ++rx) {
      for (unsigned tx = 0; tx != srs.nof_tx_ports; ++tx) {
        const cf_t value = coefficients[static_cast<size_t>(rx) * srs.nof_tx_ports + tx];
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
          target->report_invalid_output(lane, invocation);
          return std::nullopt;
        }
        result.channel_matrix.set_coefficient(value, rx, tx);
      }
    }

    // A measurement is either a number or NaN (not provided).
    const auto measurement = [&](unsigned index, bool& valid) -> std::optional<float> {
      const float value = values[index];
      if (std::isnan(value)) {
        return std::nullopt;
      }
      valid &= std::isfinite(value);
      return value;
    };
    bool       valid       = true;
    const auto noise       = measurement(OCUDU_DAPP_SRS_MEASUREMENT_NOISE_VARIANCE_V1, valid);
    result.epre_dB         = measurement(OCUDU_DAPP_SRS_MEASUREMENT_EPRE_DB_V1, valid);
    result.rsrp_dB         = measurement(OCUDU_DAPP_SRS_MEASUREMENT_RSRP_DB_V1, valid);
    const auto ta          = measurement(OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_S_V1, valid);
    const auto resolution  = measurement(OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_RESOLUTION_S_V1, valid);
    const auto ta_min      = measurement(OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_MIN_S_V1, valid);
    const auto ta_max      = measurement(OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_MAX_S_V1, valid);
    const bool ta_complete = ta && resolution && ta_min && ta_max;
    const bool ta_absent   = !ta && !resolution && !ta_min && !ta_max;
    if (!valid || (noise && *noise < 0.0F) || !(ta_complete || ta_absent) ||
        (ta_complete && (*resolution < 0.0F || *ta_min > *ta_max))) {
      target->report_invalid_output(lane, invocation);
      return std::nullopt;
    }
    result.noise_variance = noise;
    // Without a measurement the time alignment reads as the host estimator's "no measurement" value.
    result.time_alignment = ta_complete
                                ? time_alignment_measurement{*ta, *resolution, *ta_min, *ta_max}
                                : time_alignment_measurement{
                                      0.0, 0.0, std::numeric_limits<double>::min(), std::numeric_limits<double>::max()};
    return result;
  }

  std::unique_ptr<srs_estimator>                         fallback;
  std::shared_ptr<srs_dapp_channel_estimator_target>     target;
  const uint32_t                                         lane;
  std::unique_ptr<low_papr_sequence_generator>           sequence_generator;
  const std::chrono::nanoseconds                         deadline;
  // Read by the CUDA backend only.
  [[maybe_unused]] const std::chrono::nanoseconds        completion_deadline;
  const unsigned                                         max_sequence_length;
  std::vector<cf_t>                                      rx_symbols;
  std::vector<cf_t>                                      references;
  std::array<cf_t, max_nof_rx * max_nof_tx>              channel{};
  std::array<float, OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1> measurements{};
  uint64_t                                               invocation_sequence{};
#ifdef ENABLE_CUDA
  std::unique_ptr<device_workspace> workspace;
#endif
};

class srs_estimator_factory_decorator final : public srs_estimator_factory
{
public:
  srs_estimator_factory_decorator(std::shared_ptr<srs_estimator_factory>               fallback_,
                                  std::shared_ptr<srs_dapp_channel_estimator_target>   target_,
                                  std::shared_ptr<low_papr_sequence_generator_factory> sequences_,
                                  srs_dapp_channel_estimator_config                    config_) :
    fallback(std::move(fallback_)), target(std::move(target_)), sequences(std::move(sequences_)), config(config_)
  {
  }

  std::unique_ptr<srs_estimator> create() override { return wrap(fallback->create()); }

  std::unique_ptr<srs_estimator> create(ocudulog::basic_logger& logger) override
  {
    return wrap(fallback->create(logger));
  }

  std::unique_ptr<srs_estimator_configuration_validator> create_validator() override
  {
    return fallback->create_validator();
  }

private:
  std::unique_ptr<srs_estimator> wrap(std::unique_ptr<srs_estimator> estimator)
  {
    if (!estimator) {
      return {};
    }
    // An estimator beyond the hook's lanes keeps the host estimator.
    const uint32_t lane = next_lane.fetch_add(1, std::memory_order_relaxed);
    if (lane >= config.nof_lanes) {
      return estimator;
    }
    auto generator = sequences->create();
    if (!generator) {
      return {};
    }
    try {
      return std::make_unique<srs_estimator_decorator>(
          std::move(estimator), target, lane, std::move(generator), config);
    } catch (...) {
      return {};
    }
  }

  std::shared_ptr<srs_estimator_factory>               fallback;
  std::shared_ptr<srs_dapp_channel_estimator_target>   target;
  std::shared_ptr<low_papr_sequence_generator_factory> sequences;
  srs_dapp_channel_estimator_config                    config;
  std::atomic<uint32_t>                                next_lane{};
};

class native_srs_channel_estimator_target final : public srs_dapp_channel_estimator_target
{
public:
  explicit native_srs_channel_estimator_target(std::shared_ptr<dapp::native_hook_point> point_) :
    point(std::move(point_))
  {
  }

  bool     active() const noexcept override { return point->active(); }
  uint32_t lane_count() const noexcept override { return point->lane_count(); }
  uint32_t worker_id(uint32_t lane) const noexcept override { return point->worker_id(lane); }
  uint32_t backend_id() const noexcept override { return point->backend_id(); }

  dapp::native_invocation_result invoke(uint32_t                                         lane,
                                        const ocudu_dapp_srs_channel_estimator_input_v1& input,
                                        ocudu_dapp_srs_channel_estimator_output_v1&      output) noexcept override
  {
    return point->invoke(lane, input, output);
  }

  void report_invalid_output(uint32_t lane, const ocudu_dapp_invocation_v1& invocation) noexcept override
  {
    point->report_failure(lane, invocation, OCUDU_DAPP_INVALID_OUTPUT_V1);
  }

  void report_fallback(uint32_t lane, dapp::class_a_fallback_reason reason) noexcept override
  {
    point->record_class_a_fallback(lane, reason);
  }

  void report_completion_deadline(uint32_t                        lane,
                                  const ocudu_dapp_invocation_v1& invocation,
                                  uint64_t                        observed_latency_ns,
                                  uint64_t                        deadline_ns) noexcept override
  {
    point->report_completion_deadline(lane, invocation, observed_latency_ns, deadline_ns);
  }

private:
  std::shared_ptr<dapp::native_hook_point> point;
};

} // namespace

std::shared_ptr<srs_estimator_factory>
ocudu::create_srs_dapp_channel_estimator_factory(std::shared_ptr<srs_estimator_factory>               fallback,
                                                 std::shared_ptr<srs_dapp_channel_estimator_target>   target,
                                                 std::shared_ptr<low_papr_sequence_generator_factory> low_papr_factory,
                                                 srs_dapp_channel_estimator_config                    config) noexcept
{
  if (!fallback || !target || !low_papr_factory || config.nof_lanes == 0 || config.max_nof_prb == 0 ||
      config.max_nof_prb > MAX_NOF_PRBS || config.deadline.count() <= 0 || config.completion_deadline.count() <= 0 ||
      target->lane_count() < config.nof_lanes) {
    return {};
  }
  try {
    return std::make_shared<srs_estimator_factory_decorator>(
        std::move(fallback), std::move(target), std::move(low_papr_factory), config);
  } catch (...) {
    return {};
  }
}

std::shared_ptr<srs_dapp_channel_estimator_target>
ocudu::create_native_srs_dapp_channel_estimator_target(std::shared_ptr<dapp::native_hook_point> point) noexcept
{
  if (!point || point->interface_id() != OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1) {
    return {};
  }
  try {
    return std::make_shared<native_srs_channel_estimator_target>(std::move(point));
  } catch (...) {
    return {};
  }
}
