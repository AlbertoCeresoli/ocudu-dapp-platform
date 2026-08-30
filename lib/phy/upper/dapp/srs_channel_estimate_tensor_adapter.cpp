// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "srs_channel_estimate_tensor_adapter.h"
#include "srs_channel_estimate_cuda.h"
#include "ocudu/adt/complex.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_configuration.h"
#include "ocudu/ran/srs/srs_constants.h"
#include "ocudu/ran/srs/srs_information.h"
#include <array>
#include <cuda_runtime_api.h>
#include <limits>
#include <new>

using namespace ocudu;
using namespace ocudu::dapp;

namespace {

constexpr uint32_t max_frequency_points = 272 * NOF_SUBCARRIERS_PER_RB / 2;
constexpr uint32_t max_sequence_values  = srs_constants::max_nof_tx_ports * max_frequency_points;
constexpr uint32_t max_channel_values =
    srs_constants::max_nof_rx_ports * srs_constants::max_nof_tx_ports * max_frequency_points;
constexpr uint32_t max_signature_values = 64;

class srs_channel_estimate_tensor_adapter_impl final : public srs_channel_estimate_tensor_adapter
{
public:
  static std::unique_ptr<srs_channel_estimate_tensor_adapter_impl> create() noexcept
  {
    try {
      auto generator_factory = create_low_papr_sequence_generator_sw_factory();
      auto generator         = generator_factory ? generator_factory->create() : nullptr;
      if (!generator) {
        return {};
      }
      std::unique_ptr<srs_channel_estimate_tensor_adapter_impl> result(
          new srs_channel_estimate_tensor_adapter_impl(std::move(generator)));
      if (cudaMalloc(&result->device_sequences, max_sequence_values * sizeof(cf_t)) != cudaSuccess ||
          cudaMalloc(&result->device_channel, max_channel_values * sizeof(cf_t)) != cudaSuccess) {
        static_cast<void>(cudaGetLastError());
        return {};
      }
      return result;
    } catch (...) {
      return {};
    }
  }

  ~srs_channel_estimate_tensor_adapter_impl() override
  {
    if (device_channel != nullptr) {
      static_cast<void>(cudaFree(device_channel));
    }
    if (device_sequences != nullptr) {
      static_cast<void>(cudaFree(device_sequences));
    }
  }

  srs_channel_estimate_tensor_result make(const resource_grid_reader&            grid,
                                          const srs_estimator_configuration&     config,
                                          const accelerator_resource_grid_lease& lease) noexcept override
  {
    try {
      return make_impl(grid, config, lease);
    } catch (...) {
      // Class-C: skip instead of INTERNAL_ERROR (immediate breaker open).
      // Do not release here — a launched kernel may still be reading.
      return {OCUDU_DAPP_BYPASS_V1, {}, false};
    }
  }

private:
  explicit srs_channel_estimate_tensor_adapter_impl(std::unique_ptr<low_papr_sequence_generator> generator_) :
    generator(std::move(generator_))
  {
  }

  srs_channel_estimate_tensor_result make_impl(const resource_grid_reader&            grid,
                                               const srs_estimator_configuration&     config,
                                               const accelerator_resource_grid_lease& lease)
  {
    const uint32_t nof_rx_ports = config.ports.size();
    const uint32_t nof_tx_ports = static_cast<uint32_t>(config.resource.nof_antenna_ports);
    const uint32_t nof_symbols  = static_cast<uint32_t>(config.resource.nof_symbols);
    if (lease.allocation_id == 0 || lease.allocation_generation == 0 || lease.lease_token == 0 ||
        lease.backend_id != OCUDU_DAPP_BACKEND_CUDA_V1 || lease.execution_stream == nullptr ||
        lease.completion_token == 0 || device_sequences == nullptr || device_channel == nullptr || nof_rx_ports == 0 ||
        nof_rx_ports > srs_constants::max_nof_rx_ports || nof_tx_ports == 0 ||
        nof_tx_ports > srs_constants::max_nof_tx_ports || nof_symbols == 0 ||
        config.resource.start_symbol.value() + nof_symbols > grid.get_nof_symbols()) {
      return {OCUDU_DAPP_INVALID_INPUT_V1, {}, false};
    }

    srs_channel_estimate_cuda_config cuda_config{};
    cuda_config.nof_rx_ports         = nof_rx_ports;
    cuda_config.nof_tx_ports         = nof_tx_ports;
    cuda_config.nof_symbols          = nof_symbols;
    cuda_config.start_symbol         = config.resource.start_symbol.value();
    cuda_config.grid_nof_symbols     = grid.get_nof_symbols();
    cuda_config.grid_nof_subcarriers = grid.get_nof_subc();
    for (uint32_t rx = 0; rx != nof_rx_ports; ++rx) {
      if (config.ports[rx] >= grid.get_nof_ports()) {
        return {OCUDU_DAPP_INVALID_INPUT_V1, {}, false};
      }
      cuda_config.rx_ports[rx] = config.ports[rx];
    }

    std::array<srs_information, srs_constants::max_nof_tx_ports> information{};
    for (uint32_t tx = 0; tx != nof_tx_ports; ++tx) {
      information[tx] = get_srs_information(config.resource, tx);
      if (tx == 0) {
        cuda_config.frequency_points = information[tx].sequence_length;
        cuda_config.comb_size        = information[tx].comb_size;
      }
      const uint64_t last_subcarrier =
          information[tx].mapping_initial_subcarrier +
          static_cast<uint64_t>(information[tx].sequence_length - 1) * information[tx].comb_size;
      if (information[tx].sequence_length != cuda_config.frequency_points ||
          information[tx].comb_size != cuda_config.comb_size || cuda_config.frequency_points == 0 ||
          cuda_config.frequency_points > max_frequency_points || last_subcarrier >= grid.get_nof_subc()) {
        return {OCUDU_DAPP_UNSUPPORTED_V1, {}, false};
      }
      cuda_config.first_subcarrier[tx] = information[tx].mapping_initial_subcarrier;
    }
    const void* owned_grid  = grid.get_owned_device_snapshot_cbf16();
    void*       owned_ready = grid.get_owned_device_snapshot_ready_event();
    if (owned_grid == nullptr || owned_ready == nullptr) {
      if (!grid.stage_owned_snapshot_from_host()) {
        return {OCUDU_DAPP_BYPASS_V1, {}, false};
      }
      owned_grid  = grid.get_owned_device_snapshot_cbf16();
      owned_ready = grid.get_owned_device_snapshot_ready_event();
      if (owned_grid == nullptr || owned_ready == nullptr) {
        return {OCUDU_DAPP_BYPASS_V1, {}, false};
      }
    }
    if (cudaStreamWaitEvent(static_cast<cudaStream_t>(lease.execution_stream),
                            static_cast<cudaEvent_t>(owned_ready),
                            0) != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      return {OCUDU_DAPP_BYPASS_V1, {}, false};
    }

    const void* device_grid = owned_grid;
    if (!prepare_sequences(information, nof_tx_ports, cuda_config.frequency_points, lease.execution_stream)) {
      return {OCUDU_DAPP_BYPASS_V1, {}, false};
    }
    if (!launch_srs_channel_estimate_cuda(
            device_grid, device_sequences, device_channel, cuda_config, lease.execution_stream)) {
      return {OCUDU_DAPP_BYPASS_V1, {}, false};
    }

    ocudu_dapp_tensor_view_v1 tensor{};
    tensor.struct_size           = sizeof(tensor);
    tensor.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
    tensor.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
    tensor.domain                = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
    tensor.backend_id            = lease.backend_id;
    tensor.element_type          = OCUDU_DAPP_ELEMENT_CF32_V1;
    tensor.rank                  = 3;
    tensor.access                = OCUDU_DAPP_ACCESS_READ_V1;
    tensor.device_ordinal        = lease.device_ordinal;
    tensor.allocation_id         = lease.allocation_id;
    tensor.allocation_generation = lease.allocation_generation;
    tensor.native_handle         = reinterpret_cast<uintptr_t>(device_channel);
    tensor.dimensions[0]         = nof_rx_ports;
    tensor.dimensions[1]         = nof_tx_ports;
    tensor.dimensions[2]         = cuda_config.frequency_points;
    tensor.byte_strides[2]       = sizeof(cf_t);
    tensor.byte_strides[1]       = tensor.dimensions[2] * tensor.byte_strides[2];
    tensor.byte_strides[0]       = tensor.dimensions[1] * tensor.byte_strides[1];
    tensor.byte_length           = tensor.dimensions[0] * tensor.byte_strides[0];
    tensor.layout_id             = OCUDU_DAPP_LAYOUT_SRS_CHANNEL_RX_TX_FREQUENCY_CF32_V1;
    tensor.layout_version        = 1;
    tensor.ready_token           = reinterpret_cast<uintptr_t>(lease.execution_stream);
    tensor.completion_token      = lease.completion_token;
    tensor.lease_token           = lease.lease_token;
    return {OCUDU_DAPP_OK_V1, tensor, false};
  }

  bool prepare_sequences(const std::array<srs_information, srs_constants::max_nof_tx_ports>& information,
                         uint32_t                                                            nof_tx_ports,
                         uint32_t                                                            frequency_points,
                         void*                                                               stream)
  {
    std::array<int, max_signature_values> signature{};
    uint32_t                              signature_size = 0;
    auto                                  append         = [&](int value) {
      if (signature_size == signature.size()) {
        return false;
      }
      signature[signature_size++] = value;
      return true;
    };
    if (!append(nof_tx_ports) || !append(frequency_points)) {
      return false;
    }
    for (uint32_t tx = 0; tx != nof_tx_ports; ++tx) {
      const auto& info = information[tx];
      if (!append(info.sequence_group) || !append(info.sequence_number) || !append(info.n_cs) ||
          !append(info.n_cs_max) || !append(info.mapping_initial_subcarrier) || !append(info.comb_size)) {
        return false;
      }
    }
    if (signature_valid && signature_size == last_signature_size &&
        std::equal(signature.begin(), signature.begin() + signature_size, last_signature.begin())) {
      return true;
    }
    for (uint32_t tx = 0; tx != nof_tx_ports; ++tx) {
      const span<cf_t> values(host_sequences.data() + tx * frequency_points, frequency_points);
      const auto&      info = information[tx];
      generator->generate(values, info.sequence_group, info.sequence_number, info.n_cs, info.n_cs_max);
    }
    if (cudaMemcpyAsync(device_sequences,
                        host_sequences.data(),
                        static_cast<std::size_t>(nof_tx_ports) * frequency_points * sizeof(cf_t),
                        cudaMemcpyHostToDevice,
                        static_cast<cudaStream_t>(stream)) != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      return false;
    }
    last_signature      = signature;
    last_signature_size = signature_size;
    signature_valid     = true;
    return true;
  }

  std::unique_ptr<low_papr_sequence_generator> generator;
  std::array<cf_t, max_sequence_values>        host_sequences{};
  std::array<int, max_signature_values>        last_signature{};
  uint32_t                                     last_signature_size{};
  bool                                         signature_valid{};
  void*                                        device_sequences{};
  void*                                        device_channel{};
};

} // namespace

std::unique_ptr<srs_channel_estimate_tensor_adapter> ocudu::dapp::create_srs_channel_estimate_tensor_adapter() noexcept
{
  return srs_channel_estimate_tensor_adapter_impl::create();
}
