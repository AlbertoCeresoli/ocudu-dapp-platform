// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/phy/upper/dapp/pusch_dapp_demodulator.h"
#include "../signal_processors/dmrs_helper.h"
#include "ocudu/dapp/runtime/abi_validation.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/upper/channel_modulation/channel_modulation_factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_codeword_buffer.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator_notifier.h"
#include "ocudu/phy/upper/dapp/pusch_dapp_channel_estimator.h"
#include "ocudu/phy/upper/dapp/pusch_resident_dapp.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/ran/pusch/pusch_constants.h"
#include "ocudu/support/math/math_utils.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <optional>
#include <vector>

using namespace ocudu;

namespace {

constexpr float    inverse_sqrt_two   = 0.70710678118654752440F;
constexpr unsigned max_dmrs_re_per_rb = NOF_SUBCARRIERS_PER_RB / 2;

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
  if (now < 0 || budget.count() < 0) {
    return 0;
  }
  const uint64_t value = static_cast<uint64_t>(now);
  const uint64_t delta = static_cast<uint64_t>(budget.count());
  return delta > std::numeric_limits<uint64_t>::max() - value ? std::numeric_limits<uint64_t>::max() : value + delta;
}

ocudu_dapp_modulation_v1 to_dapp_modulation(modulation_scheme modulation) noexcept
{
  switch (modulation) {
    case modulation_scheme::QPSK:
      return OCUDU_DAPP_MODULATION_QPSK_V1;
    case modulation_scheme::QAM16:
      return OCUDU_DAPP_MODULATION_QAM16_V1;
    case modulation_scheme::QAM64:
      return OCUDU_DAPP_MODULATION_QAM64_V1;
    case modulation_scheme::QAM256:
      return OCUDU_DAPP_MODULATION_QAM256_V1;
    default:
      return OCUDU_DAPP_MODULATION_UNSPECIFIED_V1;
  }
}

class demodulator_decorator final : public pusch_demodulator
{
public:
  demodulator_decorator(std::unique_ptr<pusch_demodulator>                   fallback_,
                        std::shared_ptr<pusch_dapp_demodulator_target>       target_,
                        std::shared_ptr<pusch_dapp_channel_estimator_target> channel_estimator,
                        uint32_t                                             lane_,
                        std::unique_ptr<pseudo_random_generator>             pilot_prg_,
                        std::unique_ptr<pseudo_random_generator>             resident_pilot_prg,
                        std::unique_ptr<pseudo_random_generator>             descrambler_,
                        std::unique_ptr<demodulation_mapper>                 demapper_,
                        const pusch_dapp_demodulator_config&                 config_) :
    fallback(std::move(fallback_)),
    target(std::move(target_)),
    lane(lane_),
    pilot_prg(std::move(pilot_prg_)),
    descrambler(std::move(descrambler_)),
    demapper(std::move(demapper_)),
    limits(config_),
    grid_storage(static_cast<size_t>(limits.max_nof_rx_ports) * MAX_NSYMB_PER_SLOT * limits.max_nof_prb *
                 NOF_SUBCARRIERS_PER_RB),
    channel_storage(static_cast<size_t>(limits.max_nof_rx_ports) * MAX_NSYMB_PER_SLOT * limits.max_nof_prb *
                    NOF_SUBCARRIERS_PER_RB),
    equalized_storage(static_cast<size_t>(MAX_NSYMB_PER_SLOT) * limits.max_nof_prb * NOF_SUBCARRIERS_PER_RB),
    noise_storage(equalized_storage.size()),
    data_mask(static_cast<size_t>(MAX_NSYMB_PER_SLOT) * limits.max_nof_prb * NOF_SUBCARRIERS_PER_RB),
    data_re_indices(data_mask.size()),
    references(static_cast<size_t>(MAX_NSYMB_PER_SLOT) * limits.max_nof_prb * max_dmrs_re_per_rb),
    coordinates(2 * references.size()),
    pilot_sequence(static_cast<size_t>(limits.max_nof_prb) * max_dmrs_re_per_rb),
    data_symbols(equalized_storage.size()),
    data_noise(noise_storage.size()),
    receiver_llrs(static_cast<size_t>(MAX_NSYMB_PER_SLOT) * limits.max_nof_prb * NOF_SUBCARRIERS_PER_RB *
                  pusch_constants::MAX_MODULATION_ORDER),
    committed_llrs(receiver_llrs.size())
  {
    if (fallback->supports_resident_mode() && resident_pilot_prg) {
      auto resident = create_pusch_resident_dapp(target,
                                                 std::move(channel_estimator),
                                                 lane,
                                                 std::move(resident_pilot_prg),
                                                 limits.max_nof_prb,
                                                 limits.max_nof_rx_ports,
                                                 limits.max_nof_layers,
                                                 limits.demodulator_deadline,
                                                 limits.noise_variance_floor,
                                                 limits.channel_estimator_deadline);
      if (resident) {
        static_cast<void>(fallback->install_resident_dapp(std::move(resident)));
      }
    }
  }

  void demodulate(pusch_codeword_buffer&              codeword_buffer,
                  pusch_demodulator_notifier&         notifier,
                  const resource_grid_reader&         grid,
                  const dmrs_pusch_estimator_results& est_results,
                  const configuration&                config) override
  {
    last_delegated                         = true;
    captured_symbols_valid                 = false;
    const pusch_dapp_demodulator_mode mode = target->active_mode();
    if (mode == pusch_dapp_demodulator_mode::conflict) {
      target->report_fallback(lane, mode, dapp::class_a_fallback_reason::overlapped_hook);
      fallback->demodulate(codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    if (mode != pusch_dapp_demodulator_mode::equalizer && mode != pusch_dapp_demodulator_mode::receiver) {
      fallback->demodulate(codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    if (target->backend_id(mode) != OCUDU_DAPP_BACKEND_HOST_V1) {
      fallback->demodulate(codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    if (fallback->is_resident_mode_enabled()) {
      target->report_fallback(lane, mode, dapp::class_a_fallback_reason::overlapped_hook);
      fallback->demodulate(codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    if (const auto reason = preflight(grid, config)) {
      target->report_fallback(lane, mode, *reason);
      fallback->demodulate(codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    if (const auto reason = prepare(mode, grid, config)) {
      target->report_fallback(lane, mode, *reason);
      fallback->demodulate(codeword_buffer, notifier, grid, est_results, config);
      return;
    }

    if (!invoke(mode, config)) {
      fallback->demodulate(codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    last_delegated = false;
    emit_codeword(codeword_buffer, notifier, config, mode);
  }

  bool                    supports_resident_mode() const override { return fallback->supports_resident_mode(); }
  void                    enable_resident_mode() override { fallback->enable_resident_mode(); }
  void                    disable_resident_mode() override { fallback->disable_resident_mode(); }
  bool                    is_resident_mode_enabled() const override { return fallback->is_resident_mode_enabled(); }
  resident_softbit_buffer get_resident_softbits() const override { return fallback->get_resident_softbits(); }
  bool                used_host_codeword_fallback() const override { return fallback->used_host_codeword_fallback(); }
  bool                    used_device_grid() const override { return last_delegated && fallback->used_device_grid(); }
  bool used_owned_device_grid() const override { return last_delegated && fallback->used_owned_device_grid(); }
  resident_uci_buffer get_resident_uci() const override { return fallback->get_resident_uci(); }
  void                finalize_resident_uci() override { fallback->finalize_resident_uci(); }
  float               get_last_grid_staging_us() const override { return fallback->get_last_grid_staging_us(); }
  float               get_last_demod_sync_us() const override { return fallback->get_last_demod_sync_us(); }
  float               report_deferred_sinr(pusch_demodulator_notifier& notifier) override
  {
    return fallback->report_deferred_sinr(notifier);
  }
  void report_deferred_dapp_completion() override { fallback->report_deferred_dapp_completion(); }
  void enable_resident_dapp_diagnostic_timing(bool enabled) override
  {
    fallback->enable_resident_dapp_diagnostic_timing(enabled);
  }
  resident_dapp_diagnostic_timing get_resident_dapp_diagnostic_timing(unsigned completion_slot) const override
  {
    return fallback->get_resident_dapp_diagnostic_timing(completion_slot);
  }

  // Constellation diagnostics: must forward to the GPU/layer_select fallback. Without these
  // overrides the base no-ops stay in the vtable and GCC elides the virtual calls entirely
  // (empty-base optimization), so PUSCH.SYM never gets IQ.
  void enable_symbol_capture() override
  {
    capture_symbols = true;
    fallback->enable_symbol_capture();
  }
  void disable_symbol_capture() override
  {
    capture_symbols        = false;
    captured_symbols_valid = false;
    fallback->disable_symbol_capture();
  }
  span<const std::complex<float>> get_captured_symbols() const override
  {
    if (last_delegated) {
      return fallback->get_captured_symbols();
    }
    return captured_symbols_valid ? span<const cf_t>(data_symbols).first(data_re_count) : span<const cf_t>{};
  }
  void wait_captured_symbols() override
  {
    if (last_delegated) {
      fallback->wait_captured_symbols();
    }
  }

private:
  std::optional<dapp::class_a_fallback_reason> preflight(const resource_grid_reader& grid,
                                                         const configuration&        config) const noexcept
  {
    if (config.nof_tx_layers != 1 || limits.max_nof_layers < 1) {
      return dapp::class_a_fallback_reason::unsupported_layers;
    }
    if (config.enable_transform_precoding) {
      return dapp::class_a_fallback_reason::unsupported_transform_precoding;
    }
    if (config.rx_ports.empty() || config.rx_ports.size() > limits.max_nof_rx_ports ||
        std::any_of(config.rx_ports.begin(), config.rx_ports.end(), [&](uint8_t port) {
          return port >= grid.get_nof_ports();
        })) {
      return dapp::class_a_fallback_reason::unsupported_rx_ports;
    }
    const int first_rb = config.rb_mask.find_lowest();
    const int last_rb  = config.rb_mask.find_highest();
    if (config.nof_symbols == 0 || config.start_symbol_index + config.nof_symbols > MAX_NSYMB_PER_SLOT ||
        config.start_symbol_index + config.nof_symbols > grid.get_nof_symbols() || first_rb < 0 || last_rb < first_rb ||
        static_cast<unsigned>(last_rb + 1) > limits.max_nof_prb ||
        static_cast<unsigned>(last_rb + 1) * NOF_SUBCARRIERS_PER_RB > grid.get_nof_subc()) {
      return dapp::class_a_fallback_reason::unsupported_allocation;
    }
    if (to_dapp_modulation(config.modulation) == OCUDU_DAPP_MODULATION_UNSPECIFIED_V1) {
      return dapp::class_a_fallback_reason::unsupported_modulation;
    }
    if (config.dc_position.has_value() &&
        *config.dc_position >= static_cast<unsigned>(first_rb) * NOF_SUBCARRIERS_PER_RB &&
        *config.dc_position < static_cast<unsigned>(last_rb + 1) * NOF_SUBCARRIERS_PER_RB) {
      return dapp::class_a_fallback_reason::unsupported_dc_overlap;
    }
    if (!std::isfinite(config.dmrs_scaling) || !(config.dmrs_scaling > 0.0F)) {
      return dapp::class_a_fallback_reason::unsupported_dmrs;
    }
    return std::nullopt;
  }

  std::optional<dapp::class_a_fallback_reason>
  prepare(pusch_dapp_demodulator_mode mode, const resource_grid_reader& grid, const configuration& config) noexcept
  {
    try {
      return prepare_impl(mode, grid, config) ? std::nullopt
                                              : std::optional{dapp::class_a_fallback_reason::unsupported_dmrs};
    } catch (...) {
      return dapp::class_a_fallback_reason::exception;
    }
  }

  bool prepare_impl(pusch_dapp_demodulator_mode mode, const resource_grid_reader& grid, const configuration& config)
  {
    const int first_rb_i = config.rb_mask.find_lowest();
    const int last_rb_i  = config.rb_mask.find_highest();
    first_symbol         = config.start_symbol_index;
    nof_symbols          = config.nof_symbols;
    first_subcarrier     = static_cast<unsigned>(first_rb_i) * NOF_SUBCARRIERS_PER_RB;
    nof_subcarriers      = static_cast<unsigned>(last_rb_i - first_rb_i + 1) * NOF_SUBCARRIERS_PER_RB;
    nof_rx_ports         = config.rx_ports.size();
    modulation_order     = get_bits_per_symbol(config.modulation);
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
    if (!build_data_mask(config) || !generate_pilots(config, nof_pilots)) {
      return false;
    }

    uint64_t generation = ++invocation_sequence;
    if (generation == 0) {
      generation = ++invocation_sequence;
    }
    const uint32_t worker_id       = target->worker_id(lane, mode);
    const uint64_t allocation_base = (static_cast<uint64_t>(worker_id) + 1U) << 8U;

    pusch_metadata                                = {};
    pusch_metadata.struct_size                    = sizeof(pusch_metadata);
    pusch_metadata.abi_major                      = OCUDU_DAPP_ABI_MAJOR_V1;
    pusch_metadata.abi_minor                      = OCUDU_DAPP_ABI_MINOR_V1;
    pusch_metadata.nof_rx_ports                   = nof_rx_ports;
    pusch_metadata.nof_layers                     = 1;
    pusch_metadata.modulation                     = to_dapp_modulation(config.modulation);
    pusch_metadata.start_symbol                   = first_symbol;
    pusch_metadata.nof_symbols                    = nof_symbols;
    pusch_metadata.start_subcarrier               = first_subcarrier;
    pusch_metadata.nof_subcarriers                = nof_subcarriers;
    pusch_metadata.noise_variance_floor           = limits.noise_variance_floor;
    pusch_metadata.flags                          = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;
    pusch_metadata.reserved[OCUDU_DAPP_PUSCH_RESERVED_SUBCARRIER_SPACING_HZ_V1] =
        static_cast<uint64_t>(scs_to_khz(config.slot.scs())) * 1000U;
    pusch_metadata.reserved[OCUDU_DAPP_PUSCH_RESERVED_GRID_NOF_SUBCARRIERS_V1] = grid.get_nof_subc();
    pusch_metadata.data_re_mask                   = make_host_tensor(data_mask.data(),
                                                   OCUDU_DAPP_ELEMENT_U8_V1,
                                                   OCUDU_DAPP_ACCESS_READ_V1,
                                                   allocation_base + 2,
                                                   generation,
                                                                     {nof_symbols, nof_subcarriers});
    pusch_metadata.data_re_mask.layout_id         = OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_MASK_U8_V1;
    pusch_metadata.data_re_mask.layout_version    = 1;
    pusch_metadata.data_re_indices                = make_host_tensor(data_re_indices.data(),
                                                      OCUDU_DAPP_ELEMENT_U32_V1,
                                                      OCUDU_DAPP_ACCESS_READ_V1,
                                                      allocation_base + 5,
                                                      generation,
                                                                     {data_re_count});
    pusch_metadata.data_re_indices.layout_id      = OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1;
    pusch_metadata.data_re_indices.layout_version = 1;
    pusch_metadata.nof_data_re                    = static_cast<uint32_t>(data_re_count);

    dmrs_metadata             = {};
    dmrs_metadata.struct_size = sizeof(dmrs_metadata);
    dmrs_metadata.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
    dmrs_metadata.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
    dmrs_metadata.nof_pilots  = nof_pilots;
    dmrs_metadata.flags =
        OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
    dmrs_metadata.dmrs_type         = static_cast<uint32_t>(config.dmrs_type);
    dmrs_metadata.reference_symbols = make_host_tensor(references.data(),
                                                       OCUDU_DAPP_ELEMENT_CF32_V1,
                                                       OCUDU_DAPP_ACCESS_READ_V1,
                                                       allocation_base + 3,
                                                       generation,
                                                       {1, nof_pilots});
    dmrs_metadata.coordinates       = make_host_tensor(coordinates.data(),
                                                 OCUDU_DAPP_ELEMENT_U16_V1,
                                                 OCUDU_DAPP_ACCESS_READ_V1,
                                                 allocation_base + 4,
                                                 generation,
                                                       {nof_pilots, 2});

    invocation                       = {};
    invocation.struct_size           = sizeof(invocation);
    invocation.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
    invocation.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
    invocation.invocation_id         = generation;
    invocation.cell_id               = config.cell_id;
    invocation.worker_id             = worker_id;
    invocation.radio_time.sfn        = config.slot.sfn();
    invocation.radio_time.slot       = config.slot.slot_index();
    invocation.radio_time.numerology = config.slot.numerology();
    invocation.deadline_monotonic_ns = deadline_from_now(limits.demodulator_deadline);
    invocation.input_generation      = generation;
    invocation.output_generation     = generation;
    rx_grid_view                     = make_host_tensor(grid_storage.data(),
                                    OCUDU_DAPP_ELEMENT_CF32_V1,
                                    OCUDU_DAPP_ACCESS_READ_V1,
                                    allocation_base + 1,
                                    generation,
                                                        {nof_rx_ports, nof_symbols, nof_subcarriers});
    pusch_view                       = make_metadata(OCUDU_DAPP_METADATA_PUSCH_V1, pusch_metadata);
    dmrs_view                        = make_metadata(OCUDU_DAPP_METADATA_DMRS_V1, dmrs_metadata);
    output_allocation_base           = allocation_base + 6;
    output_generation                = generation;
    return true;
  }

  bool build_data_mask(const configuration& config) noexcept
  {
    const re_prb_mask dmrs_mask = get_dmrs_prb_mask(config.dmrs_type, config.nof_cdm_groups_without_data);
    std::fill_n(data_mask.data(), static_cast<size_t>(nof_symbols) * nof_subcarriers, uint8_t{0});
    data_re_count = 0;
    symbol_data_re_counts.fill(0);
    for (unsigned relative_symbol = 0; relative_symbol != nof_symbols; ++relative_symbol) {
      const unsigned symbol = first_symbol + relative_symbol;
      config.rb_mask.for_each(0, config.rb_mask.size(), [&](unsigned rb) {
        for (unsigned re = 0; re != NOF_SUBCARRIERS_PER_RB; ++re) {
          if (config.dmrs_symb_pos.test(symbol) && dmrs_mask.test(re)) {
            continue;
          }
          const unsigned relative_subcarrier = rb * NOF_SUBCARRIERS_PER_RB + re - first_subcarrier;
          const size_t   flat_index      = static_cast<size_t>(relative_symbol) * nof_subcarriers + relative_subcarrier;
          data_mask[flat_index]          = 1;
          data_re_indices[data_re_count] = static_cast<uint32_t>(flat_index);
          ++data_re_count;
          ++symbol_data_re_counts[relative_symbol];
        }
      });
    }
    return data_re_count != 0 && data_re_count <= equalized_storage.size() &&
           data_re_count <= receiver_llrs.size() / modulation_order;
  }

  bool generate_pilots(const configuration& config, unsigned& nof_pilots)
  {
    const dmrs_pxsch_parameters parameters        = get_pxsch_dmrs_params(config.dmrs_type, 0);
    const unsigned              pilots_per_symbol = config.rb_mask.count() * parameters.re_pattern.count();
    unsigned                    nof_dmrs_symbols  = 0;
    for (unsigned symbol = first_symbol; symbol != first_symbol + nof_symbols; ++symbol) {
      nof_dmrs_symbols += config.dmrs_symb_pos.test(symbol);
    }
    if (pilots_per_symbol == 0 || nof_dmrs_symbols == 0 ||
        static_cast<size_t>(pilots_per_symbol) * nof_dmrs_symbols > references.size()) {
      return false;
    }

    nof_pilots = 0;
    for (unsigned symbol = first_symbol; symbol != first_symbol + nof_symbols; ++symbol) {
      if (!config.dmrs_symb_pos.test(symbol)) {
        continue;
      }
      span<cf_t>     sequence(pilot_sequence.data(), pilots_per_symbol);
      const unsigned c_init = ((get_nsymb_per_slot(config.c_prefix) * config.slot.slot_index() + symbol + 1) *
                                   (2 * config.dmrs_scrambling_id + 1) * pow2(17) +
                               2 * config.dmrs_scrambling_id + (config.n_scid ? 1 : 0)) %
                              pow2(31);
      pilot_prg->init(c_init);
      dmrs_sequence_generate(
          sequence, *pilot_prg, inverse_sqrt_two, 0, get_nof_re_per_prb(config.dmrs_type), config.rb_mask);

      unsigned sequence_index = 0;
      config.rb_mask.for_each(0, config.rb_mask.size(), [&](unsigned rb) {
        parameters.re_pattern.for_each(0, parameters.re_pattern.size(), [&](unsigned re) {
          references[nof_pilots]          = sequence[sequence_index++] * config.dmrs_scaling;
          coordinates[2 * nof_pilots]     = static_cast<uint16_t>(symbol - first_symbol);
          coordinates[2 * nof_pilots + 1] = static_cast<uint16_t>(rb * NOF_SUBCARRIERS_PER_RB + re - first_subcarrier);
          ++nof_pilots;
        });
      });
      if (sequence_index != pilots_per_symbol) {
        return false;
      }
    }
    return nof_pilots == pilots_per_symbol * nof_dmrs_symbols;
  }

  bool invoke(pusch_dapp_demodulator_mode mode, const configuration& config) noexcept
  {
    try {
      return invoke_impl(mode, config);
    } catch (...) {
      return false;
    }
  }

  bool invoke_impl(pusch_dapp_demodulator_mode mode, const configuration& config)
  {
    if (mode == pusch_dapp_demodulator_mode::equalizer) {
      ocudu_dapp_equalizer_input_v1 input{};
      input.invocation = invocation;
      input.rx_grid    = rx_grid_view;
      input.pusch      = pusch_view;
      input.dmrs       = dmrs_view;

      ocudu_dapp_equalizer_output_v1 output{};
      output.channel_estimates       = make_host_tensor(channel_storage.data(),
                                                  OCUDU_DAPP_ELEMENT_CF32_V1,
                                                  OCUDU_DAPP_ACCESS_WRITE_V1,
                                                  output_allocation_base,
                                                  output_generation,
                                                        {nof_rx_ports, 1, nof_symbols, nof_subcarriers});
      output.equalized_symbols       = make_host_tensor(equalized_storage.data(),
                                                  OCUDU_DAPP_ELEMENT_CF32_V1,
                                                  OCUDU_DAPP_ACCESS_WRITE_V1,
                                                  output_allocation_base + 1,
                                                  output_generation,
                                                        {1, nof_symbols, nof_subcarriers});
      output.post_equalization_noise = make_host_tensor(noise_storage.data(),
                                                        OCUDU_DAPP_ELEMENT_F32_V1,
                                                        OCUDU_DAPP_ACCESS_WRITE_V1,
                                                        output_allocation_base + 2,
                                                        output_generation,
                                                        {1, nof_symbols, nof_subcarriers});
      const auto expected_channel    = output.channel_estimates;
      const auto expected_symbols    = output.equalized_symbols;
      const auto expected_noise      = output.post_equalization_noise;
      const auto result              = target->invoke(lane, input, output);
      if (result.status != OCUDU_DAPP_OK_V1 || result.invoke_fallback) {
        return false;
      }
      if (!dapp::same_tensor_storage(expected_channel, output.channel_estimates) ||
          !dapp::same_tensor_storage(expected_symbols, output.equalized_symbols) ||
          !dapp::same_tensor_storage(expected_noise, output.post_equalization_noise) || !commit_equalizer(config)) {
        target->report_invalid_output(lane, mode, invocation);
        return false;
      }
      return true;
    }

    ocudu_dapp_receiver_input_v1 input{};
    input.invocation = invocation;
    input.rx_grid    = rx_grid_view;
    input.pusch      = pusch_view;
    input.dmrs       = dmrs_view;
    ocudu_dapp_receiver_output_v1 output{};
    output.llrs = make_host_tensor(receiver_llrs.data(),
                                   OCUDU_DAPP_ELEMENT_I8_V1,
                                   OCUDU_DAPP_ACCESS_WRITE_V1,
                                   output_allocation_base,
                                   output_generation,
                                   {static_cast<uint64_t>(data_re_count) * modulation_order});
    std::fill_n(data_noise.data(), data_re_count, std::numeric_limits<float>::quiet_NaN());
    ocudu_dapp_receiver_metrics_v1 metrics{};
    metrics.struct_size                            = sizeof(metrics);
    metrics.abi_major                              = OCUDU_DAPP_ABI_MAJOR_V1;
    metrics.abi_minor                              = OCUDU_DAPP_ABI_MINOR_V1;
    metrics.post_equalization_noise                = make_host_tensor(data_noise.data(),
                                                       OCUDU_DAPP_ELEMENT_F32_V1,
                                                       OCUDU_DAPP_ACCESS_WRITE_V1,
                                                       output_allocation_base + 1,
                                                       output_generation,
                                                                      {data_re_count, 1});
    metrics.post_equalization_noise.layout_id      = OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1;
    metrics.post_equalization_noise.layout_version = 1;
    if (capture_symbols) {
      metrics.equalized_symbols                = make_host_tensor(data_symbols.data(),
                                                   OCUDU_DAPP_ELEMENT_CF32_V1,
                                                   OCUDU_DAPP_ACCESS_WRITE_V1,
                                                   output_allocation_base + 2,
                                                   output_generation,
                                                                  {data_re_count, 1});
      metrics.equalized_symbols.layout_id      = OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1;
      metrics.equalized_symbols.layout_version = 1;
    }
    output.metrics                                 = make_metadata(OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1, metrics);
    const auto expected_llrs                       = output.llrs;
    const auto expected_noise                      = metrics.post_equalization_noise;
    const auto expected_symbols                    = metrics.equalized_symbols;
    const auto result                              = target->invoke(lane, input, output);
    if (result.status != OCUDU_DAPP_OK_V1 || result.invoke_fallback) {
      return false;
    }
    const bool metrics_valid = (output.reserved0 & OCUDU_DAPP_RECEIVER_METRICS_VALID_V1) != 0;
    const bool symbols_valid = (output.reserved0 & OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_VALID_V1) != 0;
    if (!dapp::same_tensor_storage(expected_llrs, output.llrs) ||
        (metrics_valid && !dapp::same_tensor_storage(expected_noise, metrics.post_equalization_noise)) ||
        (symbols_valid &&
         (!capture_symbols || !dapp::same_tensor_storage(expected_symbols, metrics.equalized_symbols))) ||
        !commit_receiver(metrics_valid, symbols_valid)) {
      target->report_invalid_output(lane, mode, invocation);
      return false;
    }
    return true;
  }

  bool commit_equalizer(const configuration& config) noexcept
  {
    const size_t channel_count = static_cast<size_t>(nof_rx_ports) * nof_symbols * nof_subcarriers;
    for (size_t index = 0; index != channel_count; ++index) {
      if (!std::isfinite(channel_storage[index].real()) || !std::isfinite(channel_storage[index].imag())) {
        return false;
      }
    }

    size_t data_index = 0;
    for (unsigned symbol = 0; symbol != nof_symbols; ++symbol) {
      for (unsigned subcarrier = 0; subcarrier != nof_subcarriers; ++subcarrier) {
        const size_t grid_index = static_cast<size_t>(symbol) * nof_subcarriers + subcarrier;
        if (data_mask[grid_index] == 0) {
          continue;
        }
        const cf_t  value = equalized_storage[grid_index];
        const float noise = noise_storage[grid_index];
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag()) || !std::isfinite(noise) || noise < 0.0F) {
          return false;
        }
        data_symbols[data_index] = value;
        data_noise[data_index]   = std::max(noise, limits.noise_variance_floor);
        ++data_index;
      }
    }
    if (data_index != data_re_count) {
      return false;
    }
    demapper->demodulate_soft(span<log_likelihood_ratio>(committed_llrs).first(data_re_count * modulation_order),
                              span<const cf_t>(data_symbols).first(data_re_count),
                              span<const float>(data_noise).first(data_re_count),
                              config.modulation);
    captured_symbols_valid = capture_symbols;
    return true;
  }

  bool commit_receiver(bool metrics_valid, bool symbols_valid) noexcept
  {
    receiver_noise_valid   = false;
    const size_t llr_count = data_re_count * modulation_order;
    for (size_t index = 0; index != llr_count; ++index) {
      if (receiver_llrs[index] == std::numeric_limits<int8_t>::min()) {
        return false;
      }
      committed_llrs[index] = log_likelihood_ratio(receiver_llrs[index]);
    }
    if (metrics_valid) {
      for (size_t index = 0; index != data_re_count; ++index) {
        if (!std::isfinite(data_noise[index]) || data_noise[index] < 0.0F) {
          return false;
        }
        data_noise[index] = std::max(data_noise[index], limits.noise_variance_floor);
      }
      receiver_noise_valid = true;
    }
    if (symbols_valid) {
      // Diagnostics do not confer SINR authority or change the receiver's LLRs.
      for (const cf_t value : span<const cf_t>(data_symbols).first(data_re_count)) {
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
          return false;
        }
      }
      captured_symbols_valid = true;
    }
    return true;
  }

  void emit_codeword(pusch_codeword_buffer&      codeword_buffer,
                     pusch_demodulator_notifier& notifier,
                     const configuration&        config,
                     pusch_dapp_demodulator_mode mode) noexcept
  {
    unsigned c_init = to_value(config.rnti) * pow2(15) + config.n_id;
    if (config.n_rapid.has_value()) {
      c_init = to_value(config.rnti) * pow2(16) + *config.n_rapid * pow2(10) + config.n_id;
    }
    descrambler->init(c_init);

    // Reference Class-A implementations publish physical post-equalization
    // disturbance in the same normalization used below, so the safe default is
    // no calibration. A positive default from the older self-fit-residual
    // estimator inflated scheduler SINR and made closed-loop PUSCH power control
    // command the UE down on successive grants. Keep an explicit override for a
    // legacy/vendor module whose noise convention requires one.
    static const float sinr_calibration_db = []() {
      if (const char* env = std::getenv("OCUDU_DAPP_UL_SINR_CAL_DB")) {
        char*       end   = nullptr;
        const float value = std::strtof(env, &end);
        if (end != env && std::isfinite(value) && value >= -20.0F && value <= 20.0F) {
          return value;
        }
      }
      return 0.0F;
    }();

    // SINR from the ratio of measured powers, not from the noise alone.
    //
    // The previous report was -10log10(mean(noise)), i.e. SINR under an assumed
    // unit signal power. The equalizer contract only requires symbols and noise
    // to be mutually consistent for soft demapping -- it says nothing about the
    // absolute symbol scale -- so any equalizer whose output is not exactly
    // unit-power biases the report by its scale while producing perfectly good
    // LLRs. Measured OTA against the host receiver on the same link, that bias
    // ran 6-7 dB pessimistic, and uplink link adaptation believed it: the MCS
    // ceiling sat at roughly a third of what the same channel carried under the
    // host receiver, with dApp BLER idling far below target.
    //
    // mean(|s|^2) measures signal-plus-noise, so the noise is subtracted before
    // the ratio; without that the estimate reads (S+N)/N, +3 dB at true 0 dB.
    const auto sinr_from_measured = [](float signal_plus_noise, float noise, float count) {
      if (!(count > 0.0F) || !(noise > 0.0F)) {
        return std::numeric_limits<float>::infinity();
      }
      const float signal = (signal_plus_noise - noise) / noise;
      return (signal > 0.0F ? convert_power_to_dB(signal) : -convert_power_to_dB(noise / count)) + sinr_calibration_db;
    };
    const auto sinr_from_noise = [](float noise, float count) {
      if (!(count > 0.0F) || !(noise > 0.0F)) {
        return std::numeric_limits<float>::infinity();
      }
      return -convert_power_to_dB(noise / count) + sinr_calibration_db;
    };

    size_t llr_offset        = 0;
    size_t data_offset       = 0;
    float  total_noise       = 0.0F;
    float  total_signal_pn   = 0.0F;
    size_t total_noise_count = 0;
    for (unsigned symbol = 0; symbol != nof_symbols; ++symbol) {
      const size_t symbol_re_count  = symbol_data_re_counts[symbol];
      const size_t symbol_llr_count = symbol_re_count * modulation_order;
      if (symbol_llr_count == 0) {
        continue;
      }

      pusch_demodulator_notifier::demodulation_stats stats;
      // REs the module marked as erasures (huge noise: DC subcarrier, singular channel) produce zero LLRs and say
      // nothing about the link; keep them out of the SINR that feeds link adaptation and power control.
      constexpr float erasure_noise_threshold = 1.0e5F;
      if (mode == pusch_dapp_demodulator_mode::equalizer) {
        const auto symbol_noise = span<const float>(data_noise).subspan(data_offset, symbol_re_count);
        const auto symbol_syms  = span<const cf_t>(data_symbols).subspan(data_offset, symbol_re_count);
        float      sum          = 0.0F;
        float      sum_pn       = 0.0F;
        size_t     used         = 0;
        for (size_t i = 0; i != symbol_noise.size(); ++i) {
          const float var = symbol_noise[i];
          if (!(var < erasure_noise_threshold)) {
            continue;
          }
          const cf_t& value = symbol_syms[i];
          sum += var;
          sum_pn += value.real() * value.real() + value.imag() * value.imag();
          ++used;
        }
        stats.sinr_dB = sinr_from_measured(sum_pn, sum, static_cast<float>(used));
        total_noise += sum;
        total_signal_pn += sum_pn;
        total_noise_count += used;
      } else if (receiver_noise_valid) {
        const auto symbol_noise = span<const float>(data_noise).subspan(data_offset, symbol_re_count);
        float      sum          = 0.0F;
        size_t     used         = 0;
        for (const float var : symbol_noise) {
          if (var < erasure_noise_threshold) {
            sum += var;
            ++used;
          }
        }
        stats.sinr_dB = sinr_from_noise(sum, static_cast<float>(used));
        total_noise += sum;
        total_noise_count += used;
      }
      notifier.on_provisional_stats(first_symbol + symbol, stats);

      size_t remaining = symbol_llr_count;
      while (remaining != 0) {
        span<log_likelihood_ratio> destination = codeword_buffer.get_next_block_view(remaining);
        if (destination.empty() || destination.size() > remaining || destination.size() % modulation_order != 0 ||
            destination.size() > pusch_constants::MAX_NOF_BITS_PER_OFDM_SYMBOL) {
          notifier.on_end_stats({});
          codeword_buffer.on_end_codeword();
          return;
        }
        static_bit_buffer<pusch_constants::MAX_NOF_BITS_PER_OFDM_SYMBOL> sequence(destination.size());
        descrambler->generate(sequence);
        for (size_t index = 0; index != destination.size(); ++index) {
          const log_likelihood_ratio value = committed_llrs[llr_offset + index];
          destination[index]               = sequence.extract(index, 1) == 0 ? value : -value;
        }
        codeword_buffer.on_new_block(destination, sequence);
        llr_offset += destination.size();
        remaining -= destination.size();
      }
      data_offset += symbol_re_count;
    }

    pusch_demodulator_notifier::demodulation_stats final_stats;
    if (mode == pusch_dapp_demodulator_mode::equalizer && total_noise_count != 0) {
      final_stats.sinr_dB = sinr_from_measured(total_signal_pn, total_noise, static_cast<float>(total_noise_count));
    } else if (mode == pusch_dapp_demodulator_mode::receiver && receiver_noise_valid && total_noise_count != 0) {
      final_stats.sinr_dB = sinr_from_noise(total_noise, static_cast<float>(total_noise_count));
    }
    notifier.on_end_stats(final_stats);
    codeword_buffer.on_end_codeword();
  }

  std::unique_ptr<pusch_demodulator>             fallback;
  bool                                           last_delegated         = false;
  bool                                           capture_symbols        = false;
  bool                                           captured_symbols_valid = false;
  std::shared_ptr<pusch_dapp_demodulator_target> target;
  uint32_t                                       lane;
  std::unique_ptr<pseudo_random_generator>       pilot_prg;
  std::unique_ptr<pseudo_random_generator>       descrambler;
  std::unique_ptr<demodulation_mapper>           demapper;
  pusch_dapp_demodulator_config                  limits;
  std::vector<cf_t>                              grid_storage;
  std::vector<cf_t>                              channel_storage;
  std::vector<cf_t>                              equalized_storage;
  std::vector<float>                             noise_storage;
  std::vector<uint8_t>                           data_mask;
  std::vector<uint32_t>                          data_re_indices;
  std::vector<cf_t>                              references;
  std::vector<uint16_t>                          coordinates;
  std::vector<cf_t>                              pilot_sequence;
  std::vector<cf_t>                              data_symbols;
  std::vector<float>                             data_noise;
  std::vector<int8_t>                            receiver_llrs;
  std::vector<log_likelihood_ratio>              committed_llrs;
  std::array<size_t, MAX_NSYMB_PER_SLOT>         symbol_data_re_counts{};
  ocudu_dapp_invocation_v1                       invocation{};
  ocudu_dapp_pusch_metadata_v1                   pusch_metadata{};
  ocudu_dapp_dmrs_metadata_v1                    dmrs_metadata{};
  ocudu_dapp_tensor_view_v1                      rx_grid_view{};
  ocudu_dapp_typed_metadata_v1                   pusch_view{};
  ocudu_dapp_typed_metadata_v1                   dmrs_view{};
  uint64_t                                       invocation_sequence{};
  uint64_t                                       output_allocation_base{};
  uint64_t                                       output_generation{};
  size_t                                         data_re_count{};
  unsigned                                       first_symbol{};
  unsigned                                       nof_symbols{};
  unsigned                                       first_subcarrier{};
  unsigned                                       nof_subcarriers{};
  unsigned                                       nof_rx_ports{};
  unsigned                                       modulation_order{};
  bool                                           receiver_noise_valid{};
};

class demodulator_factory final : public pusch_demodulator_factory
{
public:
  demodulator_factory(std::shared_ptr<pusch_demodulator_factory>           fallback_,
                      std::shared_ptr<pusch_dapp_demodulator_target>       target_,
                      std::shared_ptr<pusch_dapp_channel_estimator_target> channel_estimator_,
                      std::shared_ptr<pseudo_random_generator_factory>     prg_factory_,
                      std::shared_ptr<demodulation_mapper_factory>         demapper_factory_,
                      pusch_dapp_demodulator_config                        config_) :
    fallback(std::move(fallback_)),
    target(std::move(target_)),
    channel_estimator(std::move(channel_estimator_)),
    prg_factory(std::move(prg_factory_)),
    demapper_factory(std::move(demapper_factory_)),
    config(config_)
  {
  }

  std::unique_ptr<pusch_demodulator> create() override
  {
    const uint32_t lane = next_lane.fetch_add(1, std::memory_order_relaxed);
    if (lane >= config.nof_lanes) {
      return {};
    }
    auto conventional = fallback->create();
    auto pilot_prg    = prg_factory->create();
    auto resident_prg = prg_factory->create();
    auto descrambler  = prg_factory->create();
    auto demapper     = demapper_factory->create();
    if (!conventional || !pilot_prg || !resident_prg || !descrambler || !demapper) {
      return {};
    }
    try {
      return std::make_unique<demodulator_decorator>(std::move(conventional),
                                                     target,
                                                     channel_estimator,
                                                     lane,
                                                     std::move(pilot_prg),
                                                     std::move(resident_prg),
                                                     std::move(descrambler),
                                                     std::move(demapper),
                                                     config);
    } catch (...) {
      return {};
    }
  }

private:
  std::shared_ptr<pusch_demodulator_factory>           fallback;
  std::shared_ptr<pusch_dapp_demodulator_target>       target;
  std::shared_ptr<pusch_dapp_channel_estimator_target> channel_estimator;
  std::shared_ptr<pseudo_random_generator_factory>     prg_factory;
  std::shared_ptr<demodulation_mapper_factory>         demapper_factory;
  pusch_dapp_demodulator_config                        config;
  std::atomic<uint32_t>                                next_lane{};
};

class native_demodulator_target final : public pusch_dapp_demodulator_target
{
public:
  native_demodulator_target(std::shared_ptr<dapp::native_hook_point> equalizer_,
                            std::shared_ptr<dapp::native_hook_point> receiver_) :
    equalizer(std::move(equalizer_)), receiver(std::move(receiver_))
  {
  }

  pusch_dapp_demodulator_mode active_mode() const noexcept override
  {
    const bool equalizer_active = equalizer && equalizer->active();
    const bool receiver_active  = receiver && receiver->active();
    if (equalizer_active && receiver_active) {
      return pusch_dapp_demodulator_mode::conflict;
    }
    if (receiver_active) {
      return pusch_dapp_demodulator_mode::receiver;
    }
    return equalizer_active ? pusch_dapp_demodulator_mode::equalizer : pusch_dapp_demodulator_mode::inactive;
  }

  uint32_t lane_count() const noexcept override
  {
    if (equalizer && receiver) {
      return std::min(equalizer->lane_count(), receiver->lane_count());
    }
    return equalizer ? equalizer->lane_count() : receiver->lane_count();
  }

  uint32_t worker_id(uint32_t lane, pusch_dapp_demodulator_mode mode) const noexcept override
  {
    return mode == pusch_dapp_demodulator_mode::equalizer ? equalizer->worker_id(lane) : receiver->worker_id(lane);
  }

  uint32_t backend_id(pusch_dapp_demodulator_mode mode) const noexcept override
  {
    const auto& point = mode == pusch_dapp_demodulator_mode::equalizer ? equalizer : receiver;
    return point ? point->backend_id() : OCUDU_DAPP_BACKEND_HOST_V1;
  }

  bool set_completion_barrier(uint32_t                                         lane,
                              pusch_dapp_demodulator_mode                      mode,
                              std::shared_ptr<dapp::native_completion_barrier> barrier) noexcept override
  {
    auto& point = mode == pusch_dapp_demodulator_mode::equalizer ? equalizer : receiver;
    return point && point->set_completion_barrier(lane, std::move(barrier));
  }

  dapp::native_invocation_result invoke(uint32_t                             lane,
                                        const ocudu_dapp_equalizer_input_v1& input,
                                        ocudu_dapp_equalizer_output_v1&      output) noexcept override
  {
    return equalizer ? equalizer->invoke(lane, input, output)
                     : dapp::native_invocation_result{OCUDU_DAPP_BYPASS_V1, true};
  }

  dapp::native_invocation_result invoke(uint32_t                            lane,
                                        const ocudu_dapp_receiver_input_v1& input,
                                        ocudu_dapp_receiver_output_v1&      output) noexcept override
  {
    return receiver ? receiver->invoke(lane, input, output)
                    : dapp::native_invocation_result{OCUDU_DAPP_BYPASS_V1, true};
  }

  void report_invalid_output(uint32_t                        lane,
                             pusch_dapp_demodulator_mode     mode,
                             const ocudu_dapp_invocation_v1& invocation_) noexcept override
  {
    auto& point = mode == pusch_dapp_demodulator_mode::equalizer ? equalizer : receiver;
    if (point) {
      point->report_failure(lane, invocation_, OCUDU_DAPP_INVALID_OUTPUT_V1);
    }
  }

  void report_completion_deadline(uint32_t                        lane,
                                  pusch_dapp_demodulator_mode     mode,
                                  const ocudu_dapp_invocation_v1& invocation,
                                  uint64_t                        observed_latency_ns,
                                  uint64_t                        deadline_ns) noexcept override
  {
    auto& point = mode == pusch_dapp_demodulator_mode::equalizer ? equalizer : receiver;
    if (point) {
      point->report_completion_deadline(lane, invocation, observed_latency_ns, deadline_ns);
    }
  }

  void report_fallback(uint32_t                      lane,
                       pusch_dapp_demodulator_mode   mode,
                       dapp::class_a_fallback_reason reason) noexcept override
  {
    if (mode == pusch_dapp_demodulator_mode::conflict) {
      if (equalizer) {
        equalizer->record_class_a_fallback(lane, reason);
      }
      if (receiver) {
        receiver->record_class_a_fallback(lane, reason);
      }
      return;
    }
    auto& point = mode == pusch_dapp_demodulator_mode::equalizer ? equalizer : receiver;
    if (point) {
      point->record_class_a_fallback(lane, reason);
    }
  }

private:
  std::shared_ptr<dapp::native_hook_point> equalizer;
  std::shared_ptr<dapp::native_hook_point> receiver;
};

bool valid_config(const pusch_dapp_demodulator_config& config) noexcept
{
  return config.nof_lanes != 0 && config.max_nof_prb != 0 && config.max_nof_prb <= MAX_NOF_PRBS &&
         config.max_nof_rx_ports != 0 && config.max_nof_rx_ports <= DMRS_MAX_NPORTS && config.max_nof_layers != 0 &&
         config.max_nof_layers <= pusch_constants::MAX_NOF_LAYERS && config.demodulator_deadline.count() > 0 &&
         config.channel_estimator_deadline.count() > 0 && std::isfinite(config.noise_variance_floor) &&
         config.noise_variance_floor > 0.0F;
}

} // namespace

std::shared_ptr<pusch_demodulator_factory> ocudu::create_pusch_dapp_demodulator_factory(
    std::shared_ptr<pusch_demodulator_factory>           fallback,
    std::shared_ptr<pusch_dapp_demodulator_target>       target,
    std::shared_ptr<pseudo_random_generator_factory>     prg_factory,
    std::shared_ptr<demodulation_mapper_factory>         demapper_factory,
    pusch_dapp_demodulator_config                        config,
    std::shared_ptr<pusch_dapp_channel_estimator_target> channel_estimator) noexcept
{
  if (!fallback || !target || !prg_factory || !demapper_factory || !valid_config(config) ||
      target->lane_count() < config.nof_lanes ||
      (channel_estimator && channel_estimator->lane_count() < config.nof_lanes)) {
    return {};
  }
  try {
    return std::make_shared<demodulator_factory>(std::move(fallback),
                                                 std::move(target),
                                                 std::move(channel_estimator),
                                                 std::move(prg_factory),
                                                 std::move(demapper_factory),
                                                 config);
  } catch (...) {
    return {};
  }
}

std::shared_ptr<pusch_dapp_demodulator_target>
ocudu::create_native_pusch_dapp_demodulator_target(std::shared_ptr<dapp::native_hook_point> equalizer,
                                                   std::shared_ptr<dapp::native_hook_point> receiver) noexcept
{
  if ((!equalizer && !receiver) || (equalizer && equalizer->interface_id() != OCUDU_DAPP_INTERFACE_EQUALIZER_V1) ||
      (receiver && receiver->interface_id() != OCUDU_DAPP_INTERFACE_RECEIVER_V1)) {
    return {};
  }
  try {
    return std::make_shared<native_demodulator_target>(std::move(equalizer), std::move(receiver));
  } catch (...) {
    return {};
  }
}
