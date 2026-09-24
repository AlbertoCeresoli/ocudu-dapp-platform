// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

/// \file
/// \brief Writes golden vectors for OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1 implementations.
///
///     srs_dapp_channel_estimator_vectors <output.bin>
///
/// Each case is an SRS occasion synthesised on a resource grid and estimated through the Class-A adapter, whose target
/// records the module input and declines, so the adapter falls back to the host estimator
/// (srs_estimator_generic_impl): the vector pairs the exact input a module receives with the host's answer.
///
/// File layout, little endian: "OSRSCEV1", u32 version (1), u32 case count, then per case: u32 metadata size, the
/// ocudu_dapp_srs_channel_estimator_metadata_v1 bytes, rx_symbols (CF32), reference_symbols (CF32), the host channel
/// matrix (CF32 [rx, tx]), the host measurements (F32 [OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1], NaN when absent) and the
/// host time alignment in double precision (value, resolution, min, max).

#include "srs_dapp_test_occasions.h"
#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include "ocudu/phy/support/time_alignment_estimator/time_alignment_estimator_factories.h"
#include "ocudu/phy/upper/dapp/srs_dapp_channel_estimator.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_factory.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include <cstdio>
#include <fstream>
#include <limits>
#include <vector>

using namespace ocudu;
using namespace ocudu::srs_dapp_test;

namespace {

/// Copies the module input of every invocation and declines it.
class recording_target final : public srs_dapp_channel_estimator_target
{
public:
  bool     active() const noexcept override { return true; }
  uint32_t lane_count() const noexcept override { return 1; }
  uint32_t worker_id(uint32_t) const noexcept override { return 0; }
  uint32_t backend_id() const noexcept override { return OCUDU_DAPP_BACKEND_HOST_V1; }

  dapp::native_invocation_result invoke(uint32_t,
                                        const ocudu_dapp_srs_channel_estimator_input_v1& input,
                                        ocudu_dapp_srs_channel_estimator_output_v1&) noexcept override
  {
    metadata = *static_cast<const ocudu_dapp_srs_channel_estimator_metadata_v1*>(input.srs.metadata.data);
    copy(input.rx_symbols, rx_symbols);
    copy(input.reference_symbols, references);
    return {OCUDU_DAPP_BYPASS_V1, true};
  }

  void report_completion_deadline(uint32_t, const ocudu_dapp_invocation_v1&, uint64_t, uint64_t) noexcept override {}
  void report_invalid_output(uint32_t, const ocudu_dapp_invocation_v1&) noexcept override {}
  void report_fallback(uint32_t, dapp::class_a_fallback_reason) noexcept override { declined_by_adapter = true; }

  ocudu_dapp_srs_channel_estimator_metadata_v1 metadata{};
  std::vector<cf_t>                            rx_symbols;
  std::vector<cf_t>                            references;
  bool                                         declined_by_adapter = false;

private:
  static void copy(const ocudu_dapp_tensor_view_v1& tensor, std::vector<cf_t>& destination)
  {
    const auto* data = reinterpret_cast<const cf_t*>(static_cast<uintptr_t>(tensor.native_handle));
    destination.assign(data, data + tensor.byte_length / sizeof(cf_t));
  }
};

template <typename T>
void write(std::ofstream& file, const T& value)
{
  file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T>
void write(std::ofstream& file, const std::vector<T>& values)
{
  file.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T)));
}

} // namespace

int main(int argc, char** argv)
{
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <output.bin>\n", argv[0]);
    return 2;
  }
  auto dft = create_dft_processor_factory_fftw_slow();
  if (!dft) {
    dft = create_dft_processor_factory_generic();
  }
  auto host_factory = create_srs_estimator_generic_factory(create_low_papr_sequence_generator_sw_factory(),
                                                           create_time_alignment_estimator_dft_factory(dft),
                                                           MAX_NOF_PRBS,
                                                           "disabled");
  auto target       = std::make_shared<recording_target>();
  auto factory      = create_srs_dapp_channel_estimator_factory(host_factory,
                                                           target,
                                                           create_low_papr_sequence_generator_sw_factory(),
                                                                {.nof_lanes = 1, .max_nof_prb = MAX_NOF_PRBS});
  auto estimator    = factory ? factory->create() : nullptr;
  if (!estimator) {
    std::fprintf(stderr, "unable to build the SRS estimator\n");
    return 1;
  }

  const auto    list = golden_occasions();
  std::ofstream file(argv[1], std::ios::binary | std::ios::trunc);
  file.write("OSRSCEV1", 8);
  write(file, uint32_t{1});
  write(file, static_cast<uint32_t>(list.size()));
  for (unsigned index = 0; index != list.size(); ++index) {
    const auto& profile = list[index];
    const auto  config  = make_configuration(profile);
    auto        grid    = make_grid(profile);
    if (!write_occasion(*grid, profile, config)) {
      std::fprintf(stderr, "case %u: the SRS does not fit the grid\n", index);
      return 1;
    }
    target->rx_symbols.clear();
    const srs_estimator_result result = estimator->estimate(grid->get_reader(), config);
    if (target->declined_by_adapter || target->rx_symbols.empty()) {
      std::fprintf(stderr, "case %u: the adapter did not invoke the module\n", index);
      return 1;
    }

    const auto&       srs = target->metadata;
    std::vector<cf_t> channel;
    for (unsigned rx = 0; rx != srs.nof_rx_ports; ++rx) {
      for (unsigned tx = 0; tx != srs.nof_tx_ports; ++tx) {
        channel.push_back(result.channel_matrix.get_coefficient(rx, tx));
      }
    }
    constexpr float    absent = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> measurements(OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1, absent);
    measurements[OCUDU_DAPP_SRS_MEASUREMENT_NOISE_VARIANCE_V1] = result.noise_variance.value_or(absent);
    measurements[OCUDU_DAPP_SRS_MEASUREMENT_EPRE_DB_V1]        = result.epre_dB.value_or(absent);
    measurements[OCUDU_DAPP_SRS_MEASUREMENT_RSRP_DB_V1]        = result.rsrp_dB.value_or(absent);
    measurements[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_S_V1] =
        static_cast<float>(result.time_alignment.time_alignment);
    measurements[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_RESOLUTION_S_V1] =
        static_cast<float>(result.time_alignment.resolution);
    measurements[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_MIN_S_V1] = static_cast<float>(result.time_alignment.min);
    measurements[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_MAX_S_V1] = static_cast<float>(result.time_alignment.max);

    write(file, static_cast<uint32_t>(sizeof(srs)));
    write(file, srs);
    write(file, target->rx_symbols);
    write(file, target->references);
    write(file, channel);
    write(file, measurements);
    for (double value : {result.time_alignment.time_alignment,
                         result.time_alignment.resolution,
                         result.time_alignment.min,
                         result.time_alignment.max}) {
      write(file, value);
    }
    std::printf("case %2u: rx %u tx %u symbols %u comb %u length %4u sets %u ta %+.3e s noise %.3e rsrp %.2f dB\n",
                index,
                srs.nof_rx_ports,
                srs.nof_tx_ports,
                srs.nof_symbols,
                srs.comb_size,
                srs.sequence_length,
                srs.nof_pilot_sets,
                result.time_alignment.time_alignment,
                result.noise_variance.value_or(absent),
                result.rsrp_dB.value_or(absent));
  }
  return file.good() ? 0 : 1;
}
