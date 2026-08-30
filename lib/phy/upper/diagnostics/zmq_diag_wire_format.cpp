// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "zmq_diag_wire_format.h"
#include "zmq_diag_generated.h"
#include <complex>

using namespace ocudu;

namespace {

static constexpr const char* ZMQ_DIAG_TOPIC_PUSCH_SYM = "PUSCH.SYM";
static constexpr const char* ZMQ_DIAG_TOPIC_PUSCH_CRC = "PUSCH.CRC";
static constexpr const char* ZMQ_DIAG_TOPIC_SRS_CHAN  = "SRS.CHAN";
static constexpr const char* ZMQ_DIAG_TOPIC_PUSCH_DMRS_CHAN = "PUSCH.DMRS";

static constexpr uint64_t ZMQ_DIAG_PUSCH_SYM_MIN_PERIOD_NS = 50'000'000ULL; // 20 Hz
static constexpr uint64_t ZMQ_DIAG_PUSCH_CRC_MIN_PERIOD_NS = 0ULL;
static constexpr uint64_t ZMQ_DIAG_SRS_CHAN_MIN_PERIOD_NS  = 0ULL;
static constexpr uint64_t ZMQ_DIAG_PUSCH_DMRS_CHAN_MIN_PERIOD_NS = 20'000'000ULL; // 50 Hz

flatbuffers::Offset<flatbuffers::Vector<float>> create_cf32_vector(flatbuffers::FlatBufferBuilder& builder,
                                                                   span<const std::complex<float>> values)
{
  if (values.empty()) {
    return {};
  }

  float* samples = nullptr;
  auto   vector  = builder.CreateUninitializedVector<float>(values.size() * 2, &samples);
  for (std::size_t i = 0, e = values.size(); i != e; ++i) {
    samples[2 * i]     = values[i].real();
    samples[2 * i + 1] = values[i].imag();
  }
  return vector;
}

} // namespace

const char* ocudu::zmq_diag_topic_name(zmq_diag_topic topic)
{
  switch (topic) {
    case zmq_diag_topic::pusch_sym:
      return ZMQ_DIAG_TOPIC_PUSCH_SYM;
    case zmq_diag_topic::pusch_crc:
      return ZMQ_DIAG_TOPIC_PUSCH_CRC;
    case zmq_diag_topic::srs_chan:
      return ZMQ_DIAG_TOPIC_SRS_CHAN;
    case zmq_diag_topic::pusch_dmrs_chan:
      return ZMQ_DIAG_TOPIC_PUSCH_DMRS_CHAN;
    default:
      return ZMQ_DIAG_TOPIC_PUSCH_SYM;
  }
}

uint64_t ocudu::zmq_diag_min_period_ns(zmq_diag_topic topic)
{
  switch (topic) {
    case zmq_diag_topic::pusch_sym:
      return ZMQ_DIAG_PUSCH_SYM_MIN_PERIOD_NS;
    case zmq_diag_topic::pusch_crc:
      return ZMQ_DIAG_PUSCH_CRC_MIN_PERIOD_NS;
    case zmq_diag_topic::srs_chan:
      return ZMQ_DIAG_SRS_CHAN_MIN_PERIOD_NS;
    case zmq_diag_topic::pusch_dmrs_chan:
      return ZMQ_DIAG_PUSCH_DMRS_CHAN_MIN_PERIOD_NS;
    default:
      return ZMQ_DIAG_PUSCH_SYM_MIN_PERIOD_NS;
  }
}

zmq_diag_topic ocudu::to_zmq_diag_topic(phy_diagnostic_stream stream)
{
  switch (stream) {
    case phy_diagnostic_stream::pusch_symbols:
      return zmq_diag_topic::pusch_sym;
    case phy_diagnostic_stream::pusch_crc:
      return zmq_diag_topic::pusch_crc;
    case phy_diagnostic_stream::srs_channel:
      return zmq_diag_topic::srs_chan;
    case phy_diagnostic_stream::pusch_dmrs_channel:
      return zmq_diag_topic::pusch_dmrs_chan;
    default:
      return zmq_diag_topic::pusch_sym;
  }
}

std::size_t ocudu::serialize_zmq_diag_pusch_symbols(flatbuffers::FlatBufferBuilder& builder,
                                                    const pusch_symbols_diagnostic& event)
{
  builder.Clear();

  const auto symbols = create_cf32_vector(builder, event.equalized_symbols);
  const auto message = diagnostics::CreatePuschSymbols(builder,
                                                       ZMQ_DIAG_FLATBUFFER_VERSION,
                                                       event.rnti,
                                                       static_cast<uint16_t>(event.slot.sfn()),
                                                       static_cast<uint16_t>(event.slot.slot_index()),
                                                       event.nof_prb,
                                                       event.nof_data_symbols,
                                                       event.nof_layers,
                                                       event.mod_bits,
                                                       event.sinr_dB,
                                                       event.epre_dB,
                                                       event.rsrp_dB,
                                                       event.evm,
                                                       event.time_alignment_us,
                                                       event.cfo_Hz,
                                                       event.target_code_rate,
                                                       symbols);
  builder.Finish(message, ZMQ_DIAG_FLATBUFFER_FILE_IDENTIFIER);
  return builder.GetSize();
}

std::size_t ocudu::serialize_zmq_diag_pusch_crc(flatbuffers::FlatBufferBuilder& builder,
                                                const pusch_crc_diagnostic&     event)
{
  builder.Clear();

  const auto message = diagnostics::CreatePuschCrc(builder,
                                                   ZMQ_DIAG_FLATBUFFER_VERSION,
                                                   event.rnti,
                                                   static_cast<uint16_t>(event.slot.sfn()),
                                                   static_cast<uint16_t>(event.slot.slot_index()),
                                                   event.crc_ok);
  builder.Finish(message, ZMQ_DIAG_FLATBUFFER_FILE_IDENTIFIER);
  return builder.GetSize();
}

std::size_t ocudu::serialize_zmq_diag_srs_channel(flatbuffers::FlatBufferBuilder& builder,
                                                  const srs_channel_diagnostic&   event)
{
  if (event.h_coefficients.empty()) {
    builder.Clear();
    return 0;
  }

  builder.Clear();

  const auto h_coefficients = create_cf32_vector(builder, event.h_coefficients);
  const auto message        = diagnostics::CreateSrsChannel(builder,
                                                     ZMQ_DIAG_FLATBUFFER_VERSION,
                                                     event.comb_size,
                                                     event.rnti,
                                                     static_cast<uint16_t>(event.slot.sfn()),
                                                     static_cast<uint16_t>(event.slot.slot_index()),
                                                     event.nof_subcarriers,
                                                     event.nof_rx_ports,
                                                     event.nof_tx_ports,
                                                     event.rx_port_index,
                                                     event.tx_port_index,
                                                     event.rsrp_dB,
                                                     event.epre_dB,
                                                     event.noise_variance,
                                                     event.time_alignment_ns,
                                                     h_coefficients,
                                                     event.start_symbol,
                                                     event.nof_symbols,
                                                     event.mapping_initial_subcarrier,
                                                     event.comb_offset,
                                                     event.cyclic_shift,
                                                     event.sequence_id,
                                                     event.sequence_group,
                                                     event.sequence_number,
                                                     event.n_cs,
                                                     event.n_cs_max,
                                                     event.bandwidth_index,
                                                     event.freq_position,
                                                     event.freq_shift,
                                                     event.freq_hopping);
  builder.Finish(message, ZMQ_DIAG_FLATBUFFER_FILE_IDENTIFIER);
  return builder.GetSize();
}

std::size_t ocudu::serialize_zmq_diag_pusch_dmrs_channel(flatbuffers::FlatBufferBuilder&       builder,
                                                         const pusch_dmrs_channel_diagnostic& event)
{
  if (event.h_estimates.empty()) {
    builder.Clear();
    return 0;
  }

  builder.Clear();

  const auto h_estimates = create_cf32_vector(builder, event.h_estimates);
  const auto message     = diagnostics::CreatePuschDmrsChannel(builder,
                                                           ZMQ_DIAG_FLATBUFFER_VERSION,
                                                           event.rnti,
                                                           static_cast<uint16_t>(event.slot.sfn()),
                                                           static_cast<uint16_t>(event.slot.slot_index()),
                                                           event.start_prb,
                                                           event.nof_prb,
                                                           event.nof_subcarriers,
                                                           event.dmrs_symbol_index,
                                                           event.start_symbol,
                                                           event.nof_symbols,
                                                           event.nof_rx_ports,
                                                           event.nof_tx_layers,
                                                           event.rx_port_index,
                                                           event.tx_layer_index,
                                                           event.dmrs_type,
                                                           event.nof_cdm_groups_without_data,
                                                           event.transform_precoding,
                                                           event.scrambling_id,
                                                           event.n_scid,
                                                           event.sinr_dB,
                                                           event.epre_dB,
                                                           event.rsrp_dB,
                                                           event.noise_variance,
                                                           event.time_alignment_us,
                                                           event.cfo_Hz,
                                                           event.target_code_rate,
                                                           h_estimates);
  builder.Finish(message, ZMQ_DIAG_FLATBUFFER_FILE_IDENTIFIER);
  return builder.GetSize();
}
