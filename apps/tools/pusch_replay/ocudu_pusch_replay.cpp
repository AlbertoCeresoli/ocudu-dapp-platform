// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Offline PUSCH replay: takes one recorded burst (equalized data symbols plus
// the grant parameters published on the E3 pusch.symbols stream) and runs the
// receiver chain the gNB would run after equalization — soft demapping,
// descrambling, UL-SCH de-multiplexing, rate de-matching, LDPC decoding and
// transport-block CRC — using the same ocudu components. Prints the CRC
// verdict next to the one the gNB reported, so recordings can be validated and
// alternative front-ends (channel estimation, equalization) can be scored by
// re-encoding their output into the same JSON + cf32 pair.
//
//   ocudu-pusch-replay burst.json [--iterations N] [--verbose]
//
// The JSON is written by ocudu-dapp-sdk/examples/e3_telemetry_python/export_pusch_burst.py.

#include "CLI/CLI11.hpp"
#include "ocudu/adt/bit_buffer.h"
#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc.h"
#include "ocudu/phy/upper/channel_modulation/channel_modulation_factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_codeword_buffer.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_decoder.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_decoder_buffer.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_decoder_notifier.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_decoder_result.h"
#include "ocudu/phy/upper/channel_processors/pusch/ulsch_demultiplex.h"
#include "ocudu/phy/upper/rx_buffer_pool.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/trx_buffer_identifier.h"
#include "ocudu/phy/upper/unique_rx_buffer.h"
#include "ocudu/ran/pusch/ulsch_info.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/ran/sch/sch_segmentation.h"
#include "ocudu/support/units.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace ocudu;
using json = nlohmann::json;

namespace {

/// Accepts and discards soft bits (UCI fields we do not decode here).
class discard_decoder_buffer final : public pusch_decoder_buffer
{
public:
  span<log_likelihood_ratio> get_next_block_view(unsigned block_size) override
  {
    scratch.resize(block_size);
    return scratch;
  }
  void on_new_softbits(span<const log_likelihood_ratio> /*softbits*/) override {}
  void on_end_softbits() override {}

private:
  std::vector<log_likelihood_ratio> scratch;
};

class result_notifier final : public pusch_decoder_notifier
{
public:
  void on_sch_data(const pusch_decoder_result& result_) override
  {
    result   = result_;
    notified = true;
  }
  pusch_decoder_result result;
  bool                 notified = false;
};

struct burst {
  uint16_t                         rnti{};
  slot_point                       slot;
  unsigned                         modulation_bits{};
  float                            sinr_db{};
  unsigned                         nof_prb{};
  unsigned                         nof_layers{};
  bool                             crc_ok_reported{};
  bool                             crc_known{};
  bool                             symbols_truncated{};
  json                             grant;
  std::vector<std::complex<float>> symbols;
};

burst load(const std::string& path)
{
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open " + path);
  }
  json  root = json::parse(input);
  burst b;
  b.rnti           = root.at("rnti").get<uint16_t>();
  const auto& slot = root.at("radio_slot");
  b.slot           = slot_point(
      slot.at("numerology").get<unsigned>(), slot.at("sfn").get<unsigned>(), slot.at("slot").get<unsigned>());
  b.modulation_bits   = root.at("modulation_bits").get<unsigned>();
  // The exporter writes JSON null for values the gNB did not report; nlohmann's value() only defaults on absence.
  const auto number_or = [](const nlohmann::json& object, const char* key, float fallback) {
    return object.contains(key) && object.at(key).is_number() ? object.at(key).get<float>() : fallback;
  };
  b.sinr_db           = number_or(root, "sinr_db", 20.0F);
  b.nof_prb           = root.at("nof_prb").get<unsigned>();
  b.nof_layers        = root.at("nof_layers").get<unsigned>();
  b.crc_known         = root.contains("crc_ok") && !root.at("crc_ok").is_null();
  b.crc_ok_reported   = b.crc_known && root.at("crc_ok").get<bool>();
  b.symbols_truncated = root.contains("symbols_truncated") && root.at("symbols_truncated").is_boolean() &&
                        root.at("symbols_truncated").get<bool>();
  b.grant             = root.at("grant");

  std::string samples_path = root.at("samples_file").get<std::string>();
  if (samples_path.find('/') == std::string::npos) {
    const auto slash = path.rfind('/');
    if (slash != std::string::npos) {
      samples_path = path.substr(0, slash + 1) + samples_path;
    }
  }
  std::ifstream samples(samples_path, std::ios::binary);
  if (!samples) {
    throw std::runtime_error("cannot open " + samples_path);
  }
  std::vector<char> raw((std::istreambuf_iterator<char>(samples)), std::istreambuf_iterator<char>());
  if (raw.size() % sizeof(std::complex<float>) != 0) {
    throw std::runtime_error("sample file is not whole complex64 values");
  }
  b.symbols.resize(raw.size() / sizeof(std::complex<float>));
  std::memcpy(b.symbols.data(), raw.data(), raw.size());
  return b;
}

int run(const std::string& path, unsigned iterations, bool verbose)
{
  const burst b = load(path);
  const json& g = b.grant;

  const auto modulation = static_cast<modulation_scheme>(b.modulation_bits);
  const auto tbs        = units::bytes(g.at("tbs_bytes").get<unsigned>());
  const auto base_graph =
      g.at("ldpc_base_graph").get<unsigned>() == 2 ? ldpc_base_graph_type::BG2 : ldpc_base_graph_type::BG1;
  const unsigned    nof_codeblocks = compute_nof_codeblocks(tbs.to_bits(), base_graph);
  const units::bits Nref = ldpc::compute_N_ref(units::bytes(g.at("tbs_lbrm_bytes").get<unsigned>()), nof_codeblocks);

  // UL-SCH information: how many soft bits belong to data vs. UCI.
  ulsch_configuration ulsch;
  ulsch.tbs                        = tbs.to_bits();
  ulsch.mcs_descr.modulation       = modulation;
  ulsch.mcs_descr.target_code_rate = 0.0F; // not needed for bit accounting
  ulsch.nof_harq_ack_bits          = units::bits(g.at("nof_harq_ack_bits").get<unsigned>());
  ulsch.nof_csi_part1_bits         = units::bits(g.at("nof_csi_part1_bits").get<unsigned>());
  ulsch.nof_csi_part2_bits         = units::bits(0);
  ulsch.alpha_scaling              = g.at("alpha_scaling").get<float>();
  ulsch.beta_offset_harq_ack       = g.at("beta_offset_harq_ack").get<float>();
  ulsch.beta_offset_csi_part1      = g.at("beta_offset_csi_part1").get<float>();
  ulsch.beta_offset_csi_part2      = g.at("beta_offset_csi_part2").get<float>();
  ulsch.nof_rb                     = b.nof_prb;
  ulsch.start_symbol_index         = g.at("start_symbol").get<unsigned>();
  ulsch.nof_symbols                = g.at("nof_symbols").get<unsigned>();
  ulsch.dmrs_type          = g.at("dmrs_type").get<unsigned>() == 2 ? dmrs_config_type::type2 : dmrs_config_type::type1;
  const unsigned dmrs_mask = g.at("dmrs_symbol_mask").get<unsigned>();
  ulsch.dmrs_symbol_mask   = bounded_bitset<MAX_NSYMB_PER_SLOT>(MAX_NSYMB_PER_SLOT);
  for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
    if ((dmrs_mask >> l) & 1U) {
      ulsch.dmrs_symbol_mask.set(l);
    }
  }
  ulsch.nof_cdm_groups_without_data = g.at("nof_cdm_groups_without_data").get<unsigned>();
  ulsch.nof_layers                  = b.nof_layers;
  ulsch.contains_dc                 = false;
  if (g.contains("dc_position") && !g.at("dc_position").is_null()) {
    // The DC subcarrier overlaps the allocation when its PRB is allocated.
    const unsigned    dc_prb = g.at("dc_position").get<unsigned>() / 12;
    const std::string mask   = g.at("prb_mask").get<std::string>();
    const unsigned    byte   = std::stoul(mask.substr(2 * (dc_prb / 8), 2), nullptr, 16);
    ulsch.contains_dc        = ((byte >> (dc_prb % 8)) & 1U) != 0;
  }
  const ulsch_information info = get_ulsch_information(ulsch);

  // Components: demapper, descrambler, demultiplexer, decoder and its buffers.
  auto demapper_factory = create_demodulation_mapper_factory();
  auto prg_factory      = create_pseudo_random_generator_sw_factory();
  auto crc_factory      = create_crc_calculator_factory_sw("auto");
  auto ldpc_factory  = create_ldpc_decoder_factory_sw("auto", {.force_decoding = false, .early_stop_syndrome = true});
  auto dematcher     = create_ldpc_rate_dematcher_factory_sw("auto");
  auto segmenter     = create_ldpc_segmenter_rx_factory_sw();
  auto demux_factory = create_ulsch_demultiplex_factory_sw();
  if (!demapper_factory || !prg_factory || !crc_factory || !ldpc_factory || !dematcher || !segmenter ||
      !demux_factory) {
    std::fprintf(stderr, "failed to create PHY factories\n");
    return 2;
  }
  pusch_decoder_factory_sw_configuration decoder_config;
  decoder_config.crc_factory       = crc_factory;
  decoder_config.decoder_factory   = ldpc_factory;
  decoder_config.dematcher_factory = dematcher;
  decoder_config.segmenter_factory = segmenter;
  decoder_config.nof_prb           = MAX_NOF_PRBS;
  decoder_config.nof_layers        = 4;
  auto decoder_factory             = create_pusch_decoder_factory_sw(decoder_config);
  if (!decoder_factory) {
    std::fprintf(stderr, "failed to create the PUSCH decoder factory\n");
    return 2;
  }
  auto demapper = demapper_factory->create();
  auto prg      = prg_factory->create();
  auto demux    = demux_factory->create();
  auto decoder  = decoder_factory->create();

  rx_buffer_pool_config pool_config;
  pool_config.max_codeblock_size   = ldpc::MAX_CODEBLOCK_SIZE;
  pool_config.nof_buffers          = 1;
  pool_config.nof_codeblocks       = nof_codeblocks;
  pool_config.expire_timeout_slots = 100;
  pool_config.external_soft_bits   = false;
  auto             pool            = create_rx_buffer_pool(pool_config);
  unique_rx_buffer rm_buffer       = pool->get_pool().reserve(
      b.slot,
      trx_buffer_identifier(to_rnti(b.rnti), static_cast<uint8_t>(g.at("harq_id").get<unsigned>())),
      nof_codeblocks,
      true);
  if (!rm_buffer) {
    std::fprintf(stderr, "failed to reserve a soft buffer\n");
    return 2;
  }

  std::vector<uint8_t>         transport_block(tbs.value());
  result_notifier              notifier;
  pusch_decoder::configuration dec;
  dec.base_graph                       = base_graph;
  dec.rv                               = g.at("rv").get<unsigned>();
  dec.mod                              = modulation;
  dec.Nref                             = Nref.value();
  dec.nof_layers                       = b.nof_layers;
  dec.nof_ldpc_iterations              = iterations;
  dec.use_early_stop                   = true;
  dec.new_data                         = true; // a single burst: no soft combining across retransmissions
  pusch_decoder_buffer& decoder_buffer = decoder->new_data(transport_block, std::move(rm_buffer), notifier, dec);
  decoder->set_nof_softbits(info.nof_ul_sch_bits);

  ulsch_demultiplex::configuration demux_config;
  demux_config.modulation                  = modulation;
  demux_config.nof_layers                  = b.nof_layers;
  demux_config.nof_prb                     = b.nof_prb;
  demux_config.start_symbol_index          = ulsch.start_symbol_index;
  demux_config.nof_symbols                 = ulsch.nof_symbols;
  demux_config.nof_harq_ack_rvd            = info.nof_harq_ack_rvd.value();
  demux_config.dmrs                        = ulsch.dmrs_type;
  demux_config.dmrs_symbol_mask            = ulsch.dmrs_symbol_mask;
  demux_config.nof_cdm_groups_without_data = ulsch.nof_cdm_groups_without_data;
  demux_config.nof_harq_ack_bits           = ulsch.nof_harq_ack_bits.value();
  demux_config.nof_enc_harq_ack_bits       = info.nof_harq_ack_bits.value();
  demux_config.nof_csi_part1_bits          = ulsch.nof_csi_part1_bits.value();
  demux_config.nof_enc_csi_part1_bits      = info.nof_csi_part1_bits.value();
  discard_decoder_buffer harq_ack_sink;
  discard_decoder_buffer csi_part1_sink;
  pusch_codeword_buffer& codeword = demux->demultiplex(decoder_buffer, harq_ack_sink, csi_part1_sink, demux_config);

  // Scrambling: c_init = RNTI * 2^15 + n_ID (TS 38.211 6.3.1.1).
  prg->init((static_cast<unsigned>(b.rnti) << 15U) + g.at("n_id").get<unsigned>());

  // Soft demapping with a flat noise variance from the reported post-equalization SINR.
  const float    noise_var     = std::pow(10.0F, -b.sinr_db / 10.0F);
  const unsigned nof_re_symbol = b.nof_prb * 12;
  const unsigned bits_per_re   = b.modulation_bits * b.nof_layers;
  const unsigned data_symbols  = b.symbols.size() / (nof_re_symbol * b.nof_layers);
  if (b.symbols.size() != static_cast<std::size_t>(data_symbols) * nof_re_symbol * b.nof_layers) {
    std::fprintf(stderr, "sample count %zu is not a whole number of OFDM symbols\n", b.symbols.size());
    return 2;
  }
  std::vector<float>                                               noise(nof_re_symbol * b.nof_layers, noise_var);
  static_bit_buffer<pusch_constants::MAX_NOF_BITS_PER_OFDM_SYMBOL> scrambling;
  for (unsigned s = 0; s != data_symbols; ++s) {
    span<const std::complex<float>> eq_re(b.symbols.data() + static_cast<std::size_t>(s) * nof_re_symbol * b.nof_layers,
                                          nof_re_symbol * b.nof_layers);
    unsigned                        processed = 0;
    while (processed != nof_re_symbol) {
      span<log_likelihood_ratio> block     = codeword.get_next_block_view((nof_re_symbol - processed) * bits_per_re);
      const unsigned             block_res = block.size() / b.modulation_bits;
      const unsigned             offset    = processed * b.nof_layers;
      demapper->demodulate_soft(
          block, eq_re.subspan(offset, block_res), span<const float>(noise).first(block_res), modulation);
      scrambling.resize(block.size());
      prg->generate(scrambling);
      for (unsigned i = 0; i != block.size(); ++i) {
        if (scrambling.extract(i, 1) != 0) {
          block[i] = -block[i];
        }
      }
      codeword.on_new_block(block, scrambling);
      processed += block_res / b.nof_layers;
    }
  }
  codeword.on_end_codeword();

  if (!notifier.notified) {
    std::fprintf(stderr, "decoder did not report a result\n");
    return 2;
  }
  const bool ok = notifier.result.tb_crc_ok;
  std::printf(
      "rnti=0x%04x sfn=%u slot=%u tbs=%u B mcs=%s bg=%u rv=%u prb=%u Qm=%u layers=%u ulsch_bits=%u "
      "codeblocks=%u\n",
      b.rnti,
      b.slot.sfn(),
      b.slot.slot_index(),
      tbs.value(),
      g.contains("mcs_index") && !g.at("mcs_index").is_null()
          ? (std::to_string(g.at("mcs_index").get<unsigned>()) + "/table" +
             std::to_string(g.contains("mcs_table") && g.at("mcs_table").is_number() ? g.at("mcs_table").get<unsigned>()
                                                                                    : 0U))
                .c_str()
          : "unknown",
      g.at("ldpc_base_graph").get<unsigned>(),
      dec.rv,
      b.nof_prb,
      b.modulation_bits,
      b.nof_layers,
      info.nof_ul_sch_bits.value(),
      nof_codeblocks);
  std::printf("replay CRC: %s | gNB CRC: %s%s%s\n",
              ok ? "OK" : "FAIL",
              b.crc_known ? (b.crc_ok_reported ? "OK" : "FAIL") : "unknown",
              b.symbols_truncated ? " | capture incomplete" : "",
              (b.crc_known && ok != b.crc_ok_reported) ? " | MISMATCH" : "");
  if (verbose) {
    std::printf("ldpc iterations: mean %.1f over %u codeblocks\n",
                notifier.result.ldpc_decoder_stats.get_mean(),
                notifier.result.nof_codeblocks_total);
    if (ok) {
      std::printf("transport block (first 16 bytes):");
      for (unsigned i = 0; i != std::min<std::size_t>(16, transport_block.size()); ++i) {
        std::printf(" %02x", transport_block[i]);
      }
      std::printf("\n");
    }
  }
  return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
  CLI::App application{"Replays a PUSCH burst captured over E3 through the gNB's own receiver "
                       "chain and compares the transport-block CRC with the recorded verdict."};
  std::string path;
  unsigned    iterations = 10;
  bool        verbose    = false;
  application.add_option("burst", path, "Burst JSON exported by export_pusch_burst.py")
      ->required()
      ->check(CLI::ExistingFile);
  application.add_option("--iterations", iterations, "LDPC decoder iteration limit")
      ->capture_default_str()
      ->check(CLI::Range(1U, 10000U));
  application.add_flag("--verbose", verbose, "Print LDPC statistics and the first transport-block bytes");
  CLI11_PARSE(application, argc, argv);
  try {
    return run(path, iterations, verbose);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 2;
  }
}
