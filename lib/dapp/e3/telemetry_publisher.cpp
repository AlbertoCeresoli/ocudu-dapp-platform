// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/telemetry_publisher.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <type_traits>
#include <vector>

using namespace ocudu;
using namespace ocudu::dapp::e3;

namespace {

uint64_t monotonic_ns() noexcept
{
  const auto value =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return value > 0 ? static_cast<uint64_t>(value) : 1;
}

/// Non-finite measurements are published as NaN and omitted by the codec.
float sanitized(float value) noexcept
{
  return std::isfinite(value) ? value : std::numeric_limits<float>::quiet_NaN();
}

telemetry_radio_slot to_radio_slot(slot_point slot) noexcept
{
  telemetry_radio_slot result{};
  if (slot.valid()) {
    result.sfn        = static_cast<uint16_t>(slot.sfn());
    result.slot       = static_cast<uint8_t>(slot.slot_index());
    result.numerology = static_cast<uint8_t>(slot.numerology());
  }
  return result;
}

constexpr std::size_t nof_streams        = 5;
constexpr std::size_t rate_table_entries = 1024;

std::size_t stream_index(phy_diagnostic_stream stream) noexcept
{
  return static_cast<std::size_t>(stream) < nof_streams ? static_cast<std::size_t>(stream) : nof_streams;
}

uint32_t stream_interface(phy_diagnostic_stream stream) noexcept
{
  switch (stream) {
    case phy_diagnostic_stream::pusch_symbols:
      return telemetry_stream_pusch_symbols;
    case phy_diagnostic_stream::pusch_crc:
      return telemetry_stream_pusch_crc;
    case phy_diagnostic_stream::srs_channel:
      return telemetry_stream_srs_channel;
    case phy_diagnostic_stream::pusch_dmrs_channel:
      return telemetry_stream_pusch_dmrs_channel;
    case phy_diagnostic_stream::pusch_grid:
      return telemetry_stream_pusch_grid;
  }
  return 0;
}

} // namespace

struct telemetry_publisher::implementation {
  explicit implementation(telemetry_publisher_config config_) : config(std::move(config_)), route_epoch(monotonic_ns())
  {
    minimum_interval_ns[stream_index(phy_diagnostic_stream::pusch_symbols)] =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config.pusch_symbols_minimum_interval).count();
    minimum_interval_ns[stream_index(phy_diagnostic_stream::pusch_crc)] =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config.pusch_crc_minimum_interval).count();
    minimum_interval_ns[stream_index(phy_diagnostic_stream::srs_channel)] =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config.srs_channel_minimum_interval).count();
    minimum_interval_ns[stream_index(phy_diagnostic_stream::pusch_dmrs_channel)] =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config.pusch_dmrs_minimum_interval).count();
    minimum_interval_ns[stream_index(phy_diagnostic_stream::pusch_grid)] =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config.pusch_grid_minimum_interval).count();
    hard_floor_ns[stream_index(phy_diagnostic_stream::pusch_symbols)] =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config.pusch_symbols_hard_floor).count();
    hard_floor_ns[stream_index(phy_diagnostic_stream::pusch_crc)] =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config.pusch_crc_hard_floor).count();
    hard_floor_ns[stream_index(phy_diagnostic_stream::srs_channel)] =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config.srs_channel_hard_floor).count();
    hard_floor_ns[stream_index(phy_diagnostic_stream::pusch_dmrs_channel)] =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config.pusch_dmrs_hard_floor).count();
    hard_floor_ns[stream_index(phy_diagnostic_stream::pusch_grid)] =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config.pusch_grid_hard_floor).count();
  }

  /// Effective per-RNTI floor: the fastest interval an active subscriber asked
  /// for (unspecified requests count as the stream default), never below the
  /// hard floor. A recorder asking for zero therefore gets every event while a
  /// dashboard on the same stream is thinned back to its own request by the
  /// carrier.
  uint64_t effective_floor_ns(std::size_t index, uint32_t interface_id) const noexcept
  {
    const auto demand   = config.store->demand(interface_id);
    uint64_t   floor_ns = demand.unspecified != 0 ? minimum_interval_ns[index] : std::numeric_limits<uint64_t>::max();
    if (demand.fastest_requested_us != portable_interval_unspecified) {
      floor_ns = std::min<uint64_t>(floor_ns, demand.fastest_requested_us * 1000ULL);
    }
    if (floor_ns == std::numeric_limits<uint64_t>::max()) {
      floor_ns = minimum_interval_ns[index];
    }
    return std::max(floor_ns, hard_floor_ns[index]);
  }

  bool active(phy_diagnostic_stream stream) const noexcept
  {
    return config.store->has_active(stream_interface(stream));
  }

  /// Per-stream publication floor: the default/requested floor is per RNTI, the configured hard floor bounds the whole
  /// stream. Consuming a token is a CAS on the last emission timestamp so concurrent lanes never double-publish.
  bool due(phy_diagnostic_stream stream, uint16_t rnti, bool consume) noexcept
  {
    const std::size_t index = stream_index(stream);
    if (index == nof_streams) {
      return false;
    }
    const uint64_t floor_ns = effective_floor_ns(index, stream_interface(stream));
    if (floor_ns == 0) {
      return true;
    }
    const uint64_t now = monotonic_ns();
    // The hard floor is a real-time budget for the stream as a whole (a raw-grid capture stages the accelerated grid
    // regardless of which UE it belongs to), so it is enforced across all RNTIs before the per-RNTI floor is checked.
    const uint64_t hard_ns = hard_floor_ns[index];
    if (hard_ns != 0 && !claim(last_emission_ns[index][rate_table_entries], now, hard_ns, consume)) {
      return false;
    }
    return claim(last_emission_ns[index][rnti % rate_table_entries], now, floor_ns, consume);
  }

  /// Claims one publication token on a last-emission timestamp: false while the floor has not elapsed.
  bool claim(std::atomic<uint64_t>& last, uint64_t now, uint64_t floor_ns, bool consume) noexcept
  {
    uint64_t seen = last.load(std::memory_order_relaxed);
    while (true) {
      if (seen != 0 && now - seen < floor_ns) {
        if (consume) {
          rate_limited.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
      }
      if (!consume || last.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
        return true;
      }
    }
  }

  template <typename Metadata>
  static telemetry_metadata_header& header_of(Metadata& metadata) noexcept
  {
    if constexpr (std::is_same_v<Metadata, pusch_symbols_metadata_v2> ||
                  std::is_same_v<Metadata, srs_channel_metadata_v2>) {
      return metadata.base.header;
    } else {
      return metadata.header;
    }
  }

  template <typename Metadata>
  void
  publish(uint32_t interface_id, uint16_t cell_id, Metadata& metadata, const void* tensor, std::size_t bytes) noexcept
  {
    header_of(metadata).monotonic_ns = monotonic_ns();
    portable_publication_source source;
    source.interface_id         = interface_id;
    source.cell_id              = cell_id;
    source.publication_sequence = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    source.route_epoch          = route_epoch;
    source.metadata             = {reinterpret_cast<const uint8_t*>(&metadata), sizeof(metadata)};
    source.tensor               = {static_cast<const uint8_t*>(tensor), bytes};
    account(config.store->publish(source));
  }

  void account(portable_publish_result result) noexcept
  {
    switch (result) {
      case portable_publish_result::published:
        published.fetch_add(1, std::memory_order_relaxed);
        break;
      case portable_publish_result::no_subscription:
      case portable_publish_result::sampled:
        no_subscription.fetch_add(1, std::memory_order_relaxed);
        break;
      case portable_publish_result::full:
      case portable_publish_result::busy:
        dropped.fetch_add(1, std::memory_order_relaxed);
        break;
      case portable_publish_result::object_too_large:
        oversized.fetch_add(1, std::memory_order_relaxed);
        break;
      case portable_publish_result::invalid:
        invalid.fetch_add(1, std::memory_order_relaxed);
        break;
    }
  }

  telemetry_publisher_config                                                     config;
  uint64_t                                                                       route_epoch;
  std::array<uint64_t, nof_streams>                                              minimum_interval_ns{};
  std::array<uint64_t, nof_streams>                                              hard_floor_ns{};
  /// Per-stream last-emission timestamps: one slot per hashed RNTI plus one aggregate slot (index rate_table_entries)
  /// used for the stream-wide hard floor.
  std::array<std::array<std::atomic<uint64_t>, rate_table_entries + 1>, nof_streams> last_emission_ns{};
  std::atomic<uint64_t>                                                          sequence{0};
  std::atomic<uint64_t>                                                          published{0};
  std::atomic<uint64_t>                                                          rate_limited{0};
  std::atomic<uint64_t>                                                          no_subscription{0};
  std::atomic<uint64_t>                                                          dropped{0};
  std::atomic<uint64_t>                                                          oversized{0};
  std::atomic<uint64_t>                                                          invalid{0};

  /// Latest SRS configuration per (cell << 16 | RNTI), with its cell; guarded by srs_config_mutex.
  std::map<uint32_t, std::pair<uint16_t, srs_config_metadata_v1>> srs_configs;
  uint32_t                                                        srs_config_epoch{0};
  std::mutex                                                      srs_config_mutex;
  /// Subscriber count of srs.config seen by the last snapshot poll.
  std::atomic<uint32_t> srs_config_subscribers{0};
};

std::pair<std::shared_ptr<telemetry_publisher>, std::string>
telemetry_publisher::create(telemetry_publisher_config config) noexcept
{
  if (!config.store) {
    return {{}, "telemetry publisher requires a portable data store"};
  }
  for (const auto interval : {config.pusch_symbols_minimum_interval,
                              config.pusch_crc_minimum_interval,
                              config.srs_channel_minimum_interval,
                              config.pusch_dmrs_minimum_interval,
                              config.pusch_grid_minimum_interval,
                              config.pusch_grid_hard_floor,
                              config.pusch_symbols_hard_floor,
                              config.pusch_crc_hard_floor,
                              config.srs_channel_hard_floor,
                              config.pusch_dmrs_hard_floor}) {
    if (interval.count() < 0 || interval > std::chrono::seconds(60)) {
      return {{}, "telemetry publication intervals must be in the range 0..60 s"};
    }
  }
  try {
    return {std::shared_ptr<telemetry_publisher>(new telemetry_publisher(std::make_unique<implementation>(config))),
            {}};
  } catch (const std::bad_alloc&) {
    return {{}, "unable to allocate telemetry publisher"};
  } catch (...) {
    return {{}, "unknown telemetry publisher construction failure"};
  }
}

telemetry_publisher::telemetry_publisher(std::unique_ptr<implementation> state_) : state(std::move(state_)) {}
telemetry_publisher::~telemetry_publisher() = default;

bool telemetry_publisher::should_collect(phy_diagnostic_stream stream, uint16_t rnti)
{
  return state->active(stream) && state->due(stream, rnti, true);
}

bool telemetry_publisher::is_collect_due(phy_diagnostic_stream stream, uint16_t rnti)
{
  return state->active(stream) && state->due(stream, rnti, false);
}

void telemetry_publisher::on_pusch_crc(const pusch_crc_diagnostic& event)
{
  pusch_crc_metadata_v1 metadata;
  metadata.rnti       = event.rnti;
  metadata.radio_slot = to_radio_slot(event.slot);
  metadata.crc_ok     = event.crc_ok ? 1 : 0;
  state->publish(telemetry_stream_pusch_crc, state->config.default_cell_id, metadata, &metadata.crc_ok, 1);
}

namespace {

pusch_grant_metadata_v1 pack_grant(const pusch_grant_diagnostic& grant) noexcept
{
  pusch_grant_metadata_v1 packed;
  packed.harq_id            = grant.harq_id;
  packed.rv                 = grant.rv;
  packed.new_data           = grant.new_data ? 1 : 0;
  packed.ldpc_base_graph    = grant.ldpc_base_graph;
  packed.n_id               = grant.n_id;
  packed.bwp_start_rb       = grant.bwp_start_rb;
  packed.bwp_size_rb        = grant.bwp_size_rb;
  packed.dmrs_symbol_mask   = grant.dmrs_symbol_mask;
  packed.dmrs_scrambling_id = grant.dmrs_scrambling_id;
  packed.dc_position        = grant.dc_position;
  packed.nof_harq_ack_bits  = grant.nof_harq_ack_bits;
  packed.nof_csi_part1_bits = grant.nof_csi_part1_bits;
  packed.n_rs_id            = grant.n_rs_id;
  packed.tbs_bytes          = grant.tbs_bytes;
  packed.tbs_lbrm_bytes     = grant.tbs_lbrm_bytes;
  packed.flags        = static_cast<uint8_t>((grant.mcs_known ? 0x01U : 0U) | (grant.transform_precoding ? 0x02U : 0U) |
                                      (grant.n_scid ? 0x04U : 0U) | (grant.dc_present ? 0x08U : 0U) |
                                      (grant.csi_part2_expected ? 0x10U : 0U));
  packed.mcs_index    = grant.mcs_index;
  packed.mcs_table    = grant.mcs_table;
  packed.start_symbol = grant.start_symbol;
  packed.nof_symbols  = grant.nof_symbols;
  packed.dmrs_type    = grant.dmrs_type;
  packed.nof_cdm_groups_without_data = grant.nof_cdm_groups_without_data;
  packed.nof_rx_ports                = grant.nof_rx_ports;
  packed.nof_layers                  = grant.nof_layers;
  packed.cyclic_prefix               = grant.cyclic_prefix;
  packed.alpha_scaling               = grant.alpha_scaling;
  packed.beta_offset_harq_ack        = grant.beta_offset_harq_ack;
  packed.beta_offset_csi_part1       = grant.beta_offset_csi_part1;
  packed.beta_offset_csi_part2       = grant.beta_offset_csi_part2;
  std::memcpy(packed.prb_mask, grant.prb_mask.data(), sizeof(packed.prb_mask));
  return packed;
}

} // namespace

void telemetry_publisher::on_pusch_grid(const pusch_grid_diagnostic& event)
{
  if (event.resource_elements.empty() || event.nof_rx_ports == 0 || event.nof_symbols == 0) {
    state->invalid.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  pusch_grid_metadata_v1 metadata;
  metadata.rnti         = event.rnti;
  metadata.radio_slot   = to_radio_slot(event.slot);
  metadata.nof_prb      = event.nof_prb;
  metadata.nof_rx_ports = event.nof_rx_ports;
  metadata.start_symbol = event.start_symbol;
  metadata.nof_symbols  = event.nof_symbols;
  metadata.grant        = pack_grant(event.grant);
  state->publish(telemetry_stream_pusch_grid,
                 state->config.default_cell_id,
                 metadata,
                 event.resource_elements.data(),
                 event.resource_elements.size() * sizeof(std::complex<float>));
}

void telemetry_publisher::on_pusch_symbols(const pusch_symbols_diagnostic& event)
{
  if (event.equalized_symbols.empty()) {
    state->invalid.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  pusch_symbols_metadata_v2 metadata;
  metadata.base.header.version    = 2;
  metadata.base.rnti              = event.rnti;
  metadata.base.radio_slot        = to_radio_slot(event.slot);
  metadata.base.nof_prb           = event.nof_prb;
  metadata.base.nof_data_symbols  = static_cast<uint8_t>(event.nof_data_symbols);
  metadata.base.nof_layers        = event.nof_layers;
  metadata.base.modulation_bits   = event.mod_bits;
  metadata.base.sinr_db           = sanitized(event.sinr_dB);
  metadata.base.epre_db           = sanitized(event.epre_dB);
  metadata.base.rsrp_db           = sanitized(event.rsrp_dB);
  metadata.base.evm               = sanitized(event.evm);
  metadata.base.time_alignment_us = sanitized(event.time_alignment_us);
  metadata.base.cfo_hz            = sanitized(event.cfo_Hz);
  metadata.base.target_code_rate  = sanitized(event.target_code_rate);
  metadata.symbol_flags =
      static_cast<uint8_t>((event.crc_known ? 0x01U : 0U) | (event.crc_ok ? 0x02U : 0U) |
                           (event.symbols_truncated ? 0x04U : 0U) | (event.grant_valid ? 0x08U : 0U));
  if (event.grant_valid) {
    metadata.grant = pack_grant(event.grant);
  }
  state->publish(telemetry_stream_pusch_symbols,
                 state->config.default_cell_id,
                 metadata,
                 event.equalized_symbols.data(),
                 event.equalized_symbols.size() * sizeof(std::complex<float>));
}

void telemetry_publisher::on_srs_channel(const srs_channel_diagnostic& event)
{
  if (event.h_coefficients.empty()) {
    state->invalid.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  srs_channel_metadata_v2  extended;
  srs_channel_metadata_v1& metadata   = extended.base;
  metadata.header.version             = 2;
  metadata.rnti                       = event.rnti;
  metadata.radio_slot                 = to_radio_slot(event.slot);
  metadata.nof_subcarriers            = event.nof_subcarriers;
  metadata.comb_size                  = event.comb_size;
  metadata.nof_rx_ports               = event.nof_rx_ports;
  metadata.nof_tx_ports               = event.nof_tx_ports;
  metadata.rx_port_index              = event.rx_port_index;
  metadata.tx_port_index              = event.tx_port_index;
  metadata.start_symbol               = event.start_symbol;
  metadata.nof_symbols                = event.nof_symbols;
  metadata.comb_offset                = event.comb_offset;
  metadata.cyclic_shift               = event.cyclic_shift;
  metadata.bandwidth_index            = event.bandwidth_index;
  metadata.frequency_position         = event.freq_position;
  metadata.frequency_hopping          = event.freq_hopping;
  metadata.mapping_initial_subcarrier = event.mapping_initial_subcarrier;
  metadata.sequence_id                = event.sequence_id;
  metadata.sequence_group             = event.sequence_group;
  metadata.sequence_number            = event.sequence_number;
  metadata.cyclic_shift_index         = event.n_cs;
  metadata.cyclic_shift_maximum       = event.n_cs_max;
  metadata.frequency_shift            = event.freq_shift;
  metadata.rsrp_db                    = sanitized(event.rsrp_dB);
  metadata.epre_db                    = sanitized(event.epre_dB);
  metadata.noise_variance             = sanitized(event.noise_variance);
  metadata.time_alignment_ns          = sanitized(event.time_alignment_ns);
  // The pilots travel only as a complete, consistent set: symbols x length pilots and a length-long sequence.
  const std::size_t length = event.reference_symbols.size();
  const bool        pilots = length != 0 && length <= std::numeric_limits<uint16_t>::max() &&
                      event.received_pilots.size() == length * event.nof_symbols;
  if (!pilots) {
    state->publish(telemetry_stream_srs_channel,
                   state->config.default_cell_id,
                   extended,
                   event.h_coefficients.data(),
                   event.h_coefficients.size() * sizeof(std::complex<float>));
    return;
  }
  extended.pilot_length  = static_cast<uint16_t>(length);
  extended.pilot_symbols = event.nof_symbols;
  // One contiguous tensor per publication; the scratch is per SRS worker thread and sized by the largest occasion.
  thread_local std::vector<std::complex<float>> tensor;
  tensor.clear();
  tensor.insert(tensor.end(), event.h_coefficients.begin(), event.h_coefficients.end());
  tensor.insert(tensor.end(), event.received_pilots.begin(), event.received_pilots.end());
  tensor.insert(tensor.end(), event.reference_symbols.begin(), event.reference_symbols.end());
  state->publish(telemetry_stream_srs_channel,
                 state->config.default_cell_id,
                 extended,
                 tensor.data(),
                 tensor.size() * sizeof(std::complex<float>));
}

void telemetry_publisher::on_pusch_dmrs_channel(const pusch_dmrs_channel_diagnostic& event)
{
  if (event.h_estimates.empty()) {
    state->invalid.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  pusch_dmrs_channel_metadata_v1 metadata;
  metadata.rnti                        = event.rnti;
  metadata.radio_slot                  = to_radio_slot(event.slot);
  metadata.start_prb                   = event.start_prb;
  metadata.nof_prb                     = event.nof_prb;
  metadata.nof_subcarriers             = event.nof_subcarriers;
  metadata.dmrs_symbol_index           = event.dmrs_symbol_index;
  metadata.start_symbol                = event.start_symbol;
  metadata.nof_symbols                 = event.nof_symbols;
  metadata.nof_rx_ports                = event.nof_rx_ports;
  metadata.nof_tx_layers               = event.nof_tx_layers;
  metadata.rx_port_index               = event.rx_port_index;
  metadata.tx_layer_index              = event.tx_layer_index;
  metadata.dmrs_type                   = event.dmrs_type;
  metadata.nof_cdm_groups_without_data = event.nof_cdm_groups_without_data;
  metadata.transform_precoding         = event.transform_precoding ? 1 : 0;
  metadata.n_scid                      = event.n_scid ? 1 : 0;
  metadata.scrambling_id               = event.scrambling_id;
  metadata.sinr_db                     = sanitized(event.sinr_dB);
  metadata.epre_db                     = sanitized(event.epre_dB);
  metadata.rsrp_db                     = sanitized(event.rsrp_dB);
  metadata.noise_variance              = sanitized(event.noise_variance);
  metadata.time_alignment_us           = sanitized(event.time_alignment_us);
  metadata.cfo_hz                      = sanitized(event.cfo_Hz);
  metadata.target_code_rate            = sanitized(event.target_code_rate);
  state->publish(telemetry_stream_pusch_dmrs_channel,
                 state->config.default_cell_id,
                 metadata,
                 event.h_estimates.data(),
                 event.h_estimates.size() * sizeof(std::complex<float>));
}

bool telemetry_publisher::publish_metrics_json(std::string_view json) noexcept
{
  if (json.empty()) {
    state->invalid.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!metrics_json_active()) {
    state->no_subscription.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  gnb_metrics_json_metadata_v1 metadata;
  const uint64_t               before = state->published.load(std::memory_order_relaxed);
  state->publish(telemetry_stream_gnb_metrics_json, state->config.default_cell_id, metadata, json.data(), json.size());
  return state->published.load(std::memory_order_relaxed) != before;
}

bool telemetry_publisher::metrics_json_active() const noexcept
{
  return state->config.store->has_active(telemetry_stream_gnb_metrics_json);
}

namespace {

/// Configuration part of an SRS configuration record: everything after the event, epoch and UE index.
bool same_srs_config(const srs_config_metadata_v1& lhs, const srs_config_metadata_v1& rhs) noexcept
{
  constexpr std::size_t begin = offsetof(srs_config_metadata_v1, numerology);
  return lhs.flags == rhs.flags && std::memcmp(reinterpret_cast<const uint8_t*>(&lhs) + begin,
                                               reinterpret_cast<const uint8_t*>(&rhs) + begin,
                                               sizeof(srs_config_metadata_v1) - begin) == 0;
}

} // namespace

void telemetry_publisher::publish_srs_config(uint16_t cell_id, srs_config_metadata_v1 record) noexcept
{
  try {
    std::lock_guard<std::mutex> lock(state->srs_config_mutex);
    const uint32_t              key   = (static_cast<uint32_t>(cell_id) << 16U) | record.rnti;
    auto                        entry = state->srs_configs.find(key);
    const auto                  event = static_cast<srs_config_event>(record.event);
    const bool                  changed =
        event == srs_config_event::created || event == srs_config_event::removed || entry == state->srs_configs.end() ||
        !same_srs_config(entry->second.second, record);
    if (changed) {
      ++state->srs_config_epoch;
    }
    record.config_epoch = state->srs_config_epoch;
    if (event == srs_config_event::removed) {
      if (entry != state->srs_configs.end()) {
        state->srs_configs.erase(entry);
      }
    } else {
      state->srs_configs[key] = {cell_id, record};
    }
    if (state->config.store->has_active(telemetry_stream_srs_config)) {
      state->publish(telemetry_stream_srs_config, cell_id, record, &record.event, 1);
    }
  } catch (...) {
    state->invalid.fetch_add(1, std::memory_order_relaxed);
  }
}

void telemetry_publisher::poll_srs_config_snapshot() noexcept
{
  const uint32_t subscribers = state->config.store->demand(telemetry_stream_srs_config).active;
  const uint32_t seen        = state->srs_config_subscribers.load(std::memory_order_relaxed);
  if (subscribers <= seen) {
    if (subscribers < seen) {
      state->srs_config_subscribers.store(subscribers, std::memory_order_relaxed);
    }
    return;
  }
  std::unique_lock<std::mutex> lock(state->srs_config_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return;
  }
  state->srs_config_subscribers.store(subscribers, std::memory_order_relaxed);
  for (const auto& [key, entry] : state->srs_configs) {
    srs_config_metadata_v1 record = entry.second;
    record.event                  = static_cast<uint8_t>(srs_config_event::snapshot);
    state->publish(telemetry_stream_srs_config, entry.first, record, &record.event, 1);
  }
}

void telemetry_publisher::publish_ta_command(uint16_t cell_id, const ta_command_metadata_v1& record) noexcept
{
  ta_command_metadata_v1 copy = record;
  state->publish(telemetry_stream_ta_command, cell_id, copy, &copy.source, 1);
}

bool telemetry_publisher::ta_command_active() const noexcept
{
  return state->config.store->has_active(telemetry_stream_ta_command);
}

telemetry_publisher_metrics telemetry_publisher::metrics() const noexcept
{
  return {state->published.load(std::memory_order_relaxed),
          state->rate_limited.load(std::memory_order_relaxed),
          state->no_subscription.load(std::memory_order_relaxed),
          state->dropped.load(std::memory_order_relaxed),
          state->oversized.load(std::memory_order_relaxed),
          state->invalid.load(std::memory_order_relaxed)};
}
