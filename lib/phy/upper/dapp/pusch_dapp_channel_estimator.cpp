// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/phy/upper/dapp/pusch_dapp_channel_estimator.h"
#include "../signal_processors/dmrs_helper.h"
#include "ocudu/dapp/runtime/abi_validation.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/signal_processors/pusch/factories.h"
#include "ocudu/ran/pusch/pusch_constants.h"
#include "ocudu/support/math/math_utils.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <vector>

using namespace ocudu;

namespace {

constexpr float    inverse_sqrt_two   = 0.70710678118654752440F;
constexpr unsigned max_dmrs_re_per_rb = NOF_SUBCARRIERS_PER_RB / 2;
static_assert(sizeof(cf_t) == 2 * sizeof(float), "CF32 dApp tensors require two adjacent floats");

template <typename T>
ocudu_dapp_tensor_view_v1 make_host_tensor(T*                              data,
                                           uint32_t                        element_type,
                                           uint32_t                        access,
                                           uint64_t                        allocation_id,
                                           uint64_t                        generation,
                                           std::initializer_list<uint64_t> dimensions) noexcept
{
  ocudu_dapp_tensor_view_v1 tensor{};
  tensor.struct_size           = sizeof(tensor);
  tensor.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  tensor.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  tensor.domain                = OCUDU_DAPP_MEM_HOST_V1;
  tensor.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
  tensor.element_type          = element_type;
  tensor.rank                  = dimensions.size();
  tensor.access                = access | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  tensor.allocation_id         = allocation_id;
  tensor.allocation_generation = generation;
  tensor.native_handle         = reinterpret_cast<uintptr_t>(data);
  tensor.lease_token           = generation;

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

uint64_t deadline_from_now(std::chrono::nanoseconds budget) noexcept
{
  const auto now =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  const auto limit = std::numeric_limits<uint64_t>::max();
  if (now < 0 || budget.count() < 0) {
    return 0;
  }
  const uint64_t value = static_cast<uint64_t>(now);
  const uint64_t delta = static_cast<uint64_t>(budget.count());
  return delta > limit - value ? limit : value + delta;
}

class channel_estimator_decorator final : public dmrs_pusch_estimator, private dmrs_pusch_estimator_results
{
public:
  channel_estimator_decorator(std::unique_ptr<dmrs_pusch_estimator>                fallback_,
                              std::shared_ptr<pusch_dapp_channel_estimator_target> target_,
                              uint32_t                                             lane_,
                              std::unique_ptr<pseudo_random_generator>             prg_,
                              std::unique_ptr<low_papr_sequence_generator>         low_papr_,
                              const pusch_dapp_channel_estimator_config&           config_) :
    fallback(std::move(fallback_)),
    target(std::move(target_)),
    lane(lane_),
    prg(std::move(prg_)),
    low_papr(std::move(low_papr_)),
    limits(config_),
    grid_storage(static_cast<size_t>(limits.max_nof_rx_ports) * MAX_NSYMB_PER_SLOT * limits.max_nof_prb *
                 NOF_SUBCARRIERS_PER_RB),
    output_storage(static_cast<size_t>(limits.max_nof_rx_ports) * MAX_NSYMB_PER_SLOT * limits.max_nof_prb *
                   NOF_SUBCARRIERS_PER_RB),
    committed_storage(output_storage.size()),
    noise_storage(limits.max_nof_rx_ports),
    committed_noise(noise_storage.size()),
    path_rsrp(noise_storage.size()),
    references(static_cast<size_t>(MAX_NSYMB_PER_SLOT) * limits.max_nof_prb * max_dmrs_re_per_rb),
    coordinates(static_cast<size_t>(MAX_NSYMB_PER_SLOT) * limits.max_nof_prb * max_dmrs_re_per_rb * 2),
    pilot_sequence(static_cast<size_t>(limits.max_nof_prb) * max_dmrs_re_per_rb)
  {
  }

  void estimate(dmrs_pusch_estimator_notifier& notifier,
                const resource_grid_reader&    grid,
                const configuration&           config) override
  {
    // One route per grant; the stock-receiver and other-hook views are inactive.
    auto& hook = target->route(target->resolve_route(config.cell_id, to_value(config.rnti)));
    if (!hook.active()) {
      fallback->estimate(notifier, grid, config);
      return;
    }
    if (const auto reason = preflight(grid, config)) {
      hook.report_fallback(lane, *reason);
      fallback->estimate(notifier, grid, config);
      return;
    }
    if (!invoke(hook, grid, config)) {
      fallback->estimate(notifier, grid, config);
      return;
    }
    notifier.on_estimation_complete(*this);
  }

private:
  std::optional<dapp::class_a_fallback_reason> preflight(const resource_grid_reader& grid,
                                                         const configuration&        config) const noexcept
  {
    const unsigned nof_layers = config.get_nof_tx_layers();
    if (nof_layers != 1 || nof_layers > limits.max_nof_layers) {
      return dapp::class_a_fallback_reason::unsupported_layers;
    }
    if (config.rx_ports.empty() || config.rx_ports.size() > limits.max_nof_rx_ports ||
        std::any_of(config.rx_ports.begin(), config.rx_ports.end(), [&](uint8_t port) {
          return port >= grid.get_nof_ports();
        })) {
      return dapp::class_a_fallback_reason::unsupported_rx_ports;
    }
    const int first_rb = config.rb_mask.find_lowest();
    const int last_rb  = config.rb_mask.find_highest();
    if (config.nof_symbols == 0 || config.first_symbol + config.nof_symbols > MAX_NSYMB_PER_SLOT ||
        config.first_symbol + config.nof_symbols > grid.get_nof_symbols() || first_rb < 0 || last_rb < first_rb ||
        static_cast<unsigned>(last_rb + 1) > limits.max_nof_prb ||
        static_cast<unsigned>(last_rb + 1) * NOF_SUBCARRIERS_PER_RB > grid.get_nof_subc()) {
      return dapp::class_a_fallback_reason::unsupported_allocation;
    }
    if (!std::isfinite(config.scaling) || !(config.scaling > 0.0F)) {
      return dapp::class_a_fallback_reason::unsupported_dmrs;
    }
    return std::nullopt;
  }

  bool invoke(pusch_dapp_channel_estimator_target& hook,
              const resource_grid_reader&          grid,
              const configuration&                 config) noexcept
  {
    try {
      return invoke_impl(hook, grid, config);
    } catch (...) {
      hook.report_fallback(lane, dapp::class_a_fallback_reason::exception);
      return false;
    }
  }

  bool
  invoke_impl(pusch_dapp_channel_estimator_target& hook, const resource_grid_reader& grid, const configuration& config)
  {
    const uint64_t deadline   = deadline_from_now(limits.deadline);
    const unsigned nof_ports  = config.rx_ports.size();
    const unsigned nof_layers = config.get_nof_tx_layers();
    const int      first_rb_i = config.rb_mask.find_lowest();
    const int      last_rb_i  = config.rb_mask.find_highest();
    first_subcarrier          = static_cast<unsigned>(first_rb_i) * NOF_SUBCARRIERS_PER_RB;
    nof_subcarriers           = static_cast<unsigned>(last_rb_i - first_rb_i + 1) * NOF_SUBCARRIERS_PER_RB;
    nof_rx_ports              = nof_ports;
    nof_tx_layers             = nof_layers;
    first_symbol              = config.first_symbol;
    nof_symbols               = config.nof_symbols;

    for (unsigned port = 0; port != nof_rx_ports; ++port) {
      for (unsigned symbol = 0; symbol != nof_symbols; ++symbol) {
        const size_t offset = (static_cast<size_t>(port) * nof_symbols + symbol) * nof_subcarriers;
        grid.get(span<cf_t>(grid_storage).subspan(offset, nof_subcarriers),
                 config.rx_ports[port],
                 first_symbol + symbol,
                 first_subcarrier);
      }
    }

    unsigned nof_pilots = 0;
    if (!generate_pilots(config, nof_pilots)) {
      return false;
    }

    uint64_t generation = ++invocation_sequence;
    if (generation == 0) {
      generation = ++invocation_sequence;
    }
    const uint64_t allocation_base = (static_cast<uint64_t>(hook.worker_id(lane)) + 1U) << 8U;

    ocudu_dapp_pusch_metadata_v1 pusch{};
    pusch.struct_size          = sizeof(pusch);
    pusch.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
    pusch.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
    pusch.nof_rx_ports         = nof_rx_ports;
    pusch.nof_layers           = nof_tx_layers;
    pusch.modulation           = OCUDU_DAPP_MODULATION_UNSPECIFIED_V1;
    pusch.start_symbol         = first_symbol;
    pusch.nof_symbols          = nof_symbols;
    pusch.start_subcarrier     = first_subcarrier;
    pusch.nof_subcarriers      = nof_subcarriers;
    pusch.noise_variance_floor = limits.noise_variance_floor;
    pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_SUBCARRIER_SPACING_HZ_V1] =
        static_cast<uint64_t>(scs_to_khz(config.slot.scs())) * 1000U;
    pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_GRID_NOF_SUBCARRIERS_V1] = grid.get_nof_subc();
    pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_RNTI_V1]                 = to_value(config.rnti);

    ocudu_dapp_dmrs_metadata_v1 dmrs{};
    dmrs.struct_size = sizeof(dmrs);
    dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
    dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
    dmrs.nof_pilots  = nof_pilots;
    dmrs.flags =
        OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
    dmrs.dmrs_type         = static_cast<uint32_t>(config.get_dmrs_type());
    dmrs.reference_symbols = make_host_tensor(references.data(),
                                              OCUDU_DAPP_ELEMENT_CF32_V1,
                                              OCUDU_DAPP_ACCESS_READ_V1,
                                              allocation_base + 2,
                                              generation,
                                              {nof_tx_layers, nof_pilots});
    dmrs.coordinates       = make_host_tensor(coordinates.data(),
                                        OCUDU_DAPP_ELEMENT_U16_V1,
                                        OCUDU_DAPP_ACCESS_READ_V1,
                                        allocation_base + 3,
                                        generation,
                                              {nof_pilots, 2});

    ocudu_dapp_channel_estimator_input_v1 input{};
    input.invocation.struct_size           = sizeof(input.invocation);
    input.invocation.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
    input.invocation.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
    input.invocation.invocation_id         = generation;
    input.invocation.cell_id               = config.cell_id;
    input.invocation.worker_id             = hook.worker_id(lane);
    input.invocation.radio_time.sfn        = config.slot.sfn();
    input.invocation.radio_time.slot       = config.slot.slot_index();
    input.invocation.radio_time.numerology = config.slot.numerology();
    input.invocation.deadline_monotonic_ns = deadline;
    input.invocation.input_generation      = generation;
    input.invocation.output_generation     = generation;
    input.rx_grid                          = make_host_tensor(grid_storage.data(),
                                     OCUDU_DAPP_ELEMENT_CF32_V1,
                                     OCUDU_DAPP_ACCESS_READ_V1,
                                     allocation_base + 1,
                                     generation,
                                                              {nof_rx_ports, nof_symbols, nof_subcarriers});
    input.pusch                            = make_metadata(OCUDU_DAPP_METADATA_PUSCH_V1, pusch);
    input.dmrs                             = make_metadata(OCUDU_DAPP_METADATA_DMRS_V1, dmrs);

    ocudu_dapp_channel_estimator_output_v1 output{};
    output.channel_estimates    = make_host_tensor(output_storage.data(),
                                                OCUDU_DAPP_ELEMENT_CF32_V1,
                                                OCUDU_DAPP_ACCESS_WRITE_V1,
                                                allocation_base + 4,
                                                generation,
                                                   {nof_rx_ports, nof_tx_layers, nof_symbols, nof_subcarriers});
    output.noise_variances      = make_host_tensor(noise_storage.data(),
                                              OCUDU_DAPP_ELEMENT_F32_V1,
                                              OCUDU_DAPP_ACCESS_WRITE_V1,
                                              allocation_base + 5,
                                              generation,
                                                   {nof_rx_ports, nof_tx_layers});
    const auto expected_channel = output.channel_estimates;
    const auto expected_noise   = output.noise_variances;
    const auto invoked          = hook.invoke(lane, input, output);
    if (invoked.status != OCUDU_DAPP_OK_V1 || invoked.invoke_fallback) {
      return false;
    }
    if (!dapp::same_tensor_storage(expected_channel, output.channel_estimates) ||
        !dapp::same_tensor_storage(expected_noise, output.noise_variances) || !commit_outputs()) {
      hook.report_invalid_output(lane, input.invocation);
      return false;
    }
    return true;
  }

  bool generate_pilots(const configuration& config, unsigned& nof_pilots)
  {
    const dmrs_pxsch_parameters parameters        = get_pxsch_dmrs_params(config.get_dmrs_type(), 0);
    const unsigned              pilots_per_symbol = config.rb_mask.count() * parameters.re_pattern.count();
    unsigned                    dmrs_symbols      = 0;
    for (unsigned symbol = config.first_symbol; symbol != config.first_symbol + config.nof_symbols; ++symbol) {
      dmrs_symbols += config.symbols_mask.test(symbol);
    }
    if (pilots_per_symbol == 0 || dmrs_symbols == 0 ||
        static_cast<size_t>(pilots_per_symbol) * dmrs_symbols > references.size()) {
      return false;
    }

    nof_pilots = 0;
    for (unsigned symbol = config.first_symbol; symbol != config.first_symbol + config.nof_symbols; ++symbol) {
      if (!config.symbols_mask.test(symbol)) {
        continue;
      }
      span<cf_t> sequence(pilot_sequence.data(), pilots_per_symbol);
      if (std::holds_alternative<low_papr_sequence_configuration>(config.sequence_config)) {
        const auto& sequence_config = std::get<low_papr_sequence_configuration>(config.sequence_config);
        low_papr->generate(sequence, sequence_config.n_rs_id % 30, 0, 0, 1);
      } else {
        const auto&    sequence_config = std::get<pseudo_random_sequence_configuration>(config.sequence_config);
        const unsigned nsymb           = get_nsymb_per_slot(cyclic_prefix::NORMAL);
        const unsigned c_init =
            ((nsymb * config.slot.slot_index() + symbol + 1) * (2 * sequence_config.scrambling_id + 1) * pow2(17) +
             (2 * sequence_config.scrambling_id + (sequence_config.n_scid ? 1 : 0))) %
            pow2(31);
        prg->init(c_init);
        dmrs_sequence_generate(
            sequence, *prg, inverse_sqrt_two, 0, get_nof_re_per_prb(sequence_config.type), config.rb_mask);
      }

      unsigned sequence_index = 0;
      config.rb_mask.for_each(0, config.rb_mask.size(), [&](unsigned rb) {
        parameters.re_pattern.for_each(0, parameters.re_pattern.size(), [&](unsigned re) {
          references[nof_pilots]          = sequence[sequence_index++] * config.scaling;
          coordinates[2 * nof_pilots]     = static_cast<uint16_t>(symbol - first_symbol);
          coordinates[2 * nof_pilots + 1] = static_cast<uint16_t>(rb * NOF_SUBCARRIERS_PER_RB + re - first_subcarrier);
          ++nof_pilots;
        });
      });
      if (sequence_index != pilots_per_symbol) {
        return false;
      }
    }
    return nof_pilots == pilots_per_symbol * dmrs_symbols;
  }

  bool commit_outputs() noexcept
  {
    const size_t channel_count = static_cast<size_t>(nof_rx_ports) * nof_tx_layers * nof_symbols * nof_subcarriers;
    std::fill(path_rsrp.begin(), path_rsrp.end(), 0.0F);
    for (size_t index = 0; index != channel_count; ++index) {
      const cf_t value = output_storage[index];
      if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
        return false;
      }
      committed_storage[index] = to_cbf16(value);
      const size_t re_per_path = static_cast<size_t>(nof_symbols) * nof_subcarriers;
      path_rsrp[index / re_per_path] += std::norm(value);
    }
    const float re_scale = 1.0F / static_cast<float>(nof_symbols * nof_subcarriers);
    for (unsigned port = 0; port != nof_rx_ports; ++port) {
      for (unsigned layer = 0; layer != nof_tx_layers; ++layer) {
        const size_t path  = static_cast<size_t>(port) * nof_tx_layers + layer;
        const float  noise = noise_storage[path];
        if (!std::isfinite(noise) || noise < 0.0F) {
          return false;
        }
        path_rsrp[path] *= re_scale;
        committed_noise[path] = std::max(noise, limits.noise_variance_floor);
      }
    }
    return true;
  }

  size_t channel_index(unsigned port, unsigned layer, unsigned symbol, unsigned subcarrier) const noexcept
  {
    return (((static_cast<size_t>(port) * nof_tx_layers + layer) * nof_symbols + symbol) * nof_subcarriers +
            subcarrier);
  }

  float get_noise_variance(unsigned rx_port) const override
  {
    float value = 0.0F;
    for (unsigned layer = 0; layer != nof_tx_layers; ++layer) {
      value += committed_noise[static_cast<size_t>(rx_port) * nof_tx_layers + layer];
    }
    return value / static_cast<float>(nof_tx_layers);
  }

  float get_rsrp(unsigned rx_port, unsigned tx_layer) const override
  {
    return path_rsrp[static_cast<size_t>(rx_port) * nof_tx_layers + tx_layer];
  }

  static_vector<float, MAX_PORTS> get_rsrp_all_ports(unsigned tx_layer) const override
  {
    static_vector<float, MAX_PORTS> values(nof_rx_ports);
    for (unsigned port = 0; port != nof_rx_ports; ++port) {
      values[port] = get_rsrp(port, tx_layer);
    }
    return values;
  }

  float get_epre(unsigned rx_port) const override { return get_rsrp(rx_port, 0) + get_noise_variance(rx_port); }

  float get_snr(unsigned rx_port) const override
  {
    const float noise = get_noise_variance(rx_port);
    return noise > 0.0F ? get_rsrp(rx_port, 0) / noise : 0.0F;
  }

  float get_layer_average_snr(unsigned tx_layer) const override
  {
    float signal = 0.0F;
    float noise  = 0.0F;
    for (unsigned port = 0; port != nof_rx_ports; ++port) {
      signal += get_rsrp(port, tx_layer);
      noise += committed_noise[static_cast<size_t>(port) * nof_tx_layers + tx_layer];
    }
    return noise > 0.0F ? signal / noise : 0.0F;
  }

  phy_time_unit get_time_alignment(unsigned) const override { return {}; }

  std::optional<float> get_cfo_Hz(unsigned) const override { return std::nullopt; }

  void
  get_symbol_ch_estimate(span<cbf16_t> estimates, unsigned i_symbol, unsigned rx_port, unsigned tx_layer) const override
  {
    std::fill(estimates.begin(), estimates.end(), cbf16_t{});
    if (i_symbol < first_symbol || i_symbol >= first_symbol + nof_symbols) {
      return;
    }
    const unsigned relative_symbol = i_symbol - first_symbol;
    const unsigned count           = std::min<unsigned>(estimates.size(), nof_subcarriers);
    for (unsigned subcarrier = 0; subcarrier != count; ++subcarrier) {
      estimates[subcarrier] = committed_storage[channel_index(rx_port, tx_layer, relative_symbol, subcarrier)];
    }
  }

  void get_symbol_ch_estimate(span<cbf16_t>                              estimates,
                              unsigned                                   i_symbol,
                              unsigned                                   rx_port,
                              unsigned                                   tx_layer,
                              const bounded_bitset<MAX_NOF_SUBCARRIERS>& re_mask) const override
  {
    std::fill(estimates.begin(), estimates.end(), cbf16_t{});
    if (i_symbol < first_symbol || i_symbol >= first_symbol + nof_symbols) {
      return;
    }
    const unsigned relative_symbol = i_symbol - first_symbol;
    unsigned       output_index    = 0;
    re_mask.for_each(0, std::min<size_t>(re_mask.size(), nof_subcarriers), [&](unsigned subcarrier) {
      if (output_index < estimates.size()) {
        estimates[output_index++] = committed_storage[channel_index(rx_port, tx_layer, relative_symbol, subcarrier)];
      }
    });
  }

  void get_channel_state_information(channel_state_information& csi) const override
  {
    const auto rsrp = get_rsrp_all_ports(0);
    csi.set_rsrp_lin(rsrp);
    float epre = 0.0F;
    for (unsigned port = 0; port != nof_rx_ports; ++port) {
      epre += get_epre(port);
    }
    csi.set_epre(convert_power_to_dB(epre / static_cast<float>(nof_rx_ports)));
    csi.set_time_alignment({});
    csi.set_sinr_dB(channel_state_information::sinr_type::channel_estimator,
                    convert_power_to_dB(get_layer_average_snr(0)));
  }

  std::unique_ptr<dmrs_pusch_estimator>                fallback;
  std::shared_ptr<pusch_dapp_channel_estimator_target> target;
  uint32_t                                             lane;
  std::unique_ptr<pseudo_random_generator>             prg;
  std::unique_ptr<low_papr_sequence_generator>         low_papr;
  pusch_dapp_channel_estimator_config                  limits;
  std::vector<cf_t>                                    grid_storage;
  std::vector<cf_t>                                    output_storage;
  std::vector<cbf16_t>                                 committed_storage;
  std::vector<float>                                   noise_storage;
  std::vector<float>                                   committed_noise;
  std::vector<float>                                   path_rsrp;
  std::vector<cf_t>                                    references;
  std::vector<uint16_t>                                coordinates;
  std::vector<cf_t>                                    pilot_sequence;
  uint64_t                                             invocation_sequence{};
  unsigned                                             first_subcarrier{};
  unsigned                                             nof_subcarriers{};
  unsigned                                             first_symbol{};
  unsigned                                             nof_symbols{};
  unsigned                                             nof_rx_ports{};
  unsigned                                             nof_tx_layers{};
};

class channel_estimator_factory final : public dmrs_pusch_estimator_factory
{
public:
  channel_estimator_factory(std::shared_ptr<dmrs_pusch_estimator_factory>        fallback_,
                            std::shared_ptr<pusch_dapp_channel_estimator_target> target_,
                            std::shared_ptr<pseudo_random_generator_factory>     prg_factory_,
                            std::shared_ptr<low_papr_sequence_generator_factory> low_papr_factory_,
                            pusch_dapp_channel_estimator_config                  config_) :
    fallback(std::move(fallback_)),
    target(std::move(target_)),
    prg_factory(std::move(prg_factory_)),
    low_papr_factory(std::move(low_papr_factory_)),
    config(config_)
  {
  }

  std::unique_ptr<dmrs_pusch_estimator> create() override
  {
    const uint32_t lane = next_lane.fetch_add(1, std::memory_order_relaxed);
    if (lane >= config.nof_lanes) {
      return {};
    }
    auto conventional  = fallback->create();
    auto lane_prg      = prg_factory->create();
    auto lane_low_papr = low_papr_factory->create();
    if (!conventional || !lane_prg || !lane_low_papr) {
      return {};
    }
    try {
      return std::make_unique<channel_estimator_decorator>(
          std::move(conventional), target, lane, std::move(lane_prg), std::move(lane_low_papr), config);
    } catch (...) {
      return {};
    }
  }

private:
  std::shared_ptr<dmrs_pusch_estimator_factory>        fallback;
  std::shared_ptr<pusch_dapp_channel_estimator_target> target;
  std::shared_ptr<pseudo_random_generator_factory>     prg_factory;
  std::shared_ptr<low_papr_sequence_generator_factory> low_papr_factory;
  pusch_dapp_channel_estimator_config                  config;
  std::atomic<uint32_t>                                next_lane{};
};

class native_channel_estimator_target final : public pusch_dapp_channel_estimator_target
{
public:
  explicit native_channel_estimator_target(std::shared_ptr<dapp::native_hook_point> point_) : point(std::move(point_))
  {
  }

  bool     active() const noexcept override { return point->active(); }
  uint32_t lane_count() const noexcept override { return point->lane_count(); }
  uint32_t worker_id(uint32_t lane) const noexcept override { return point->worker_id(lane); }
  uint32_t backend_id() const noexcept override { return point->backend_id(); }
  bool set_completion_barrier(uint32_t lane, std::shared_ptr<dapp::native_completion_barrier> barrier) noexcept override
  {
    return point->set_completion_barrier(lane, std::move(barrier));
  }

  dapp::native_invocation_result invoke(uint32_t                                     lane,
                                        const ocudu_dapp_channel_estimator_input_v1& input,
                                        ocudu_dapp_channel_estimator_output_v1&      output) noexcept override
  {
    return point->invoke(lane, input, output);
  }

  void report_invalid_output(uint32_t lane, const ocudu_dapp_invocation_v1& invocation) noexcept override
  {
    point->report_failure(lane, invocation, OCUDU_DAPP_INVALID_OUTPUT_V1);
  }

  void report_completion_deadline(uint32_t                        lane,
                                  const ocudu_dapp_invocation_v1& invocation,
                                  uint64_t                        observed_latency_ns,
                                  uint64_t                        deadline_ns) noexcept override
  {
    point->report_completion_deadline(lane, invocation, observed_latency_ns, deadline_ns);
  }

  void report_fallback(uint32_t lane, dapp::class_a_fallback_reason reason) noexcept override
  {
    point->record_class_a_fallback(lane, reason);
  }

private:
  std::shared_ptr<dapp::native_hook_point> point;
};

/// The channel-estimator view of a grant routed to another hook or to the stock receiver.
class inactive_channel_estimator_target final : public pusch_dapp_channel_estimator_target
{
public:
  explicit inactive_channel_estimator_target(uint32_t nof_lanes_) : nof_lanes(nof_lanes_) {}

  bool                           active() const noexcept override { return false; }
  uint32_t                       lane_count() const noexcept override { return nof_lanes; }
  uint32_t                       worker_id(uint32_t lane) const noexcept override { return lane; }
  dapp::native_invocation_result invoke(uint32_t,
                                        const ocudu_dapp_channel_estimator_input_v1&,
                                        ocudu_dapp_channel_estimator_output_v1&) noexcept override
  {
    return {OCUDU_DAPP_BYPASS_V1, true};
  }

private:
  const uint32_t nof_lanes;
};

/// Route-table slots of the channel-estimator hook behind one target; see routed_demodulator_target.
class routed_channel_estimator_target final : public pusch_dapp_channel_estimator_target
{
public:
  routed_channel_estimator_target(std::vector<std::shared_ptr<dapp::native_hook_point>> slots_,
                                  std::shared_ptr<const dapp::pusch_route_table>        table_) :
    slots(std::move(slots_)), table(std::move(table_)), inactive(lanes_of(slots))
  {
    for (const auto& point : slots) {
      views.push_back(std::make_unique<native_channel_estimator_target>(point));
    }
  }

  dapp::pusch_route resolve_route(uint32_t cell_index, uint16_t rnti) const noexcept override
  {
    return table->resolve(cell_index, rnti);
  }

  pusch_dapp_channel_estimator_target& route(const dapp::pusch_route& selected) noexcept override
  {
    switch (selected.type) {
      case dapp::pusch_route::kind::primary:
        return *views.front();
      case dapp::pusch_route::kind::conventional:
        return inactive;
      case dapp::pusch_route::kind::instance:
        break;
    }
    return selected.hook == dapp::pusch_route_hook::channel_estimator && selected.slot < views.size()
               ? static_cast<pusch_dapp_channel_estimator_target&>(*views[selected.slot])
               : inactive;
  }

  bool     active() const noexcept override { return views.front()->active(); }
  uint32_t lane_count() const noexcept override { return inactive.lane_count(); }
  uint32_t worker_id(uint32_t lane) const noexcept override { return views.front()->worker_id(lane); }
  uint32_t backend_id() const noexcept override { return views.front()->backend_id(); }
  bool set_completion_barrier(uint32_t lane, std::shared_ptr<dapp::native_completion_barrier> barrier) noexcept override
  {
    bool ok = true;
    for (const auto& point : slots) {
      ok &= point->set_completion_barrier(lane, barrier);
    }
    return ok;
  }
  dapp::native_invocation_result invoke(uint32_t                                     lane,
                                        const ocudu_dapp_channel_estimator_input_v1& input,
                                        ocudu_dapp_channel_estimator_output_v1&      output) noexcept override
  {
    return views.front()->invoke(lane, input, output);
  }
  void report_invalid_output(uint32_t lane, const ocudu_dapp_invocation_v1& invocation) noexcept override
  {
    views.front()->report_invalid_output(lane, invocation);
  }
  void report_completion_deadline(uint32_t                        lane,
                                  const ocudu_dapp_invocation_v1& invocation,
                                  uint64_t                        observed_latency_ns,
                                  uint64_t                        deadline_ns) noexcept override
  {
    views.front()->report_completion_deadline(lane, invocation, observed_latency_ns, deadline_ns);
  }
  void report_fallback(uint32_t lane, dapp::class_a_fallback_reason reason) noexcept override
  {
    views.front()->report_fallback(lane, reason);
  }

private:
  static uint32_t lanes_of(const std::vector<std::shared_ptr<dapp::native_hook_point>>& points) noexcept
  {
    uint32_t lanes = UINT32_MAX;
    for (const auto& point : points) {
      lanes = std::min(lanes, point->lane_count());
    }
    return lanes;
  }

  std::vector<std::shared_ptr<dapp::native_hook_point>>         slots;
  std::shared_ptr<const dapp::pusch_route_table>                table;
  inactive_channel_estimator_target                             inactive;
  std::vector<std::unique_ptr<native_channel_estimator_target>> views;
};

bool valid_config(const pusch_dapp_channel_estimator_config& config) noexcept
{
  return config.nof_lanes != 0 && config.max_nof_prb != 0 && config.max_nof_prb <= MAX_NOF_PRBS &&
         config.max_nof_rx_ports != 0 && config.max_nof_rx_ports <= DMRS_MAX_NPORTS && config.max_nof_layers != 0 &&
         config.max_nof_layers <= pusch_constants::MAX_NOF_LAYERS && config.deadline.count() > 0 &&
         std::isfinite(config.noise_variance_floor) && config.noise_variance_floor > 0.0F;
}

} // namespace

std::shared_ptr<dmrs_pusch_estimator_factory> ocudu::create_pusch_dapp_channel_estimator_factory(
    std::shared_ptr<dmrs_pusch_estimator_factory>        fallback,
    std::shared_ptr<pusch_dapp_channel_estimator_target> target,
    std::shared_ptr<pseudo_random_generator_factory>     prg_factory,
    std::shared_ptr<low_papr_sequence_generator_factory> low_papr_factory,
    pusch_dapp_channel_estimator_config                  config) noexcept
{
  if (!fallback || !target || !prg_factory || !low_papr_factory || !valid_config(config) ||
      target->lane_count() < config.nof_lanes) {
    return {};
  }
  try {
    return std::make_shared<channel_estimator_factory>(
        std::move(fallback), std::move(target), std::move(prg_factory), std::move(low_papr_factory), config);
  } catch (...) {
    return {};
  }
}

std::shared_ptr<pusch_dapp_channel_estimator_target>
ocudu::create_native_pusch_dapp_channel_estimator_target(std::shared_ptr<dapp::native_hook_point> point) noexcept
{
  if (!point || point->interface_id() != OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1) {
    return {};
  }
  try {
    return std::make_shared<native_channel_estimator_target>(std::move(point));
  } catch (...) {
    return {};
  }
}

std::shared_ptr<pusch_dapp_channel_estimator_target>
ocudu::create_native_pusch_dapp_channel_estimator_target(std::vector<std::shared_ptr<dapp::native_hook_point>> slots,
                                                         std::shared_ptr<const dapp::pusch_route_table> table) noexcept
{
  if (table == nullptr || slots.empty() || slots.size() > table->slots_per_hook() ||
      std::any_of(slots.begin(), slots.end(), [](const auto& point) {
        return !point || point->interface_id() != OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1;
      })) {
    return {};
  }
  try {
    return std::make_shared<routed_channel_estimator_target>(std::move(slots), std::move(table));
  } catch (...) {
    return {};
  }
}
