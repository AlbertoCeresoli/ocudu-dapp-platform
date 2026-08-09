// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "../../../../lib/ofh/receiver/ofh_uplane_rx_symbol_data_flow_writer.h"
#include "helpers.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ofh/compression/compression_properties.h"
#include "ocudu/ofh/serdes/ofh_message_decoder_properties.h"
#include "ocudu/ofh/serdes/ofh_uplane_message_decoder.h"
#include "ocudu/ofh/serdes/ofh_uplane_message_decoder_properties.h"
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <numeric>
#include <vector>

using namespace ocudu;
using namespace ofh;
using namespace ofh::testing;

namespace {

class resource_grid_device_writer_spy : public resource_grid_writer_bool_spy
{
public:
  explicit resource_grid_device_writer_spy(unsigned nof_prbs) : resource_grid_writer_bool_spy(nof_prbs) {}

  bool supports_device_grid_mapping() const override { return true; }

  void* get_device_grid_bf16() override { return this; }

  bool prepare_device_grid_mapping(void*) override
  {
    ++prepare_count;
    return true;
  }

  bool on_device_grid_mapping_enqueued(void*) override
  {
    ++enqueue_count;
    return true;
  }

  unsigned prepare_count = 0;
  unsigned enqueue_count = 0;
};

class uplane_message_decoder_device_spy : public uplane_message_decoder
{
public:
  bool decode(uplane_message_decoder_results& results, span<const uint8_t> message) override
  {
    (void)results;
    (void)message;
    return false;
  }

  bool supports_resource_grid_decompression() const override { return true; }

  bool prefers_full_symbol_decompression(const ru_compression_params&) const override
  {
    return assemble_fragments;
  }

  iq_decompression_result decompress_to_resource_grid(resource_grid_writer&        grid,
                                                       unsigned                     port_,
                                                       unsigned                     symbol_,
                                                       unsigned                     start_prb_,
                                                       unsigned                     nof_prbs_,
                                                       const uplane_section_params& section_) override
  {
    (void)grid;
    called    = true;
    port      = port_;
    symbol    = symbol_;
    start_prb = start_prb_;
    nof_prbs  = nof_prbs_;
    section   = &section_;
    payload.assign(section_.compressed_iq_data.begin(), section_.compressed_iq_data.end());
    if (call_entered != nullptr) {
      call_entered->set_value();
    }
    if (call_release.valid()) {
      call_release.wait();
    }
    if (is_iq_decompression_success(result) &&
        !(grid.prepare_device_grid_mapping(nullptr) && grid.on_device_grid_mapping_enqueued(nullptr))) {
      return iq_decompression_result::destination_failure;
    }
    return result;
  }

  iq_decompression_result      result             = iq_decompression_result::success;
  bool                         assemble_fragments = false;
  bool                         called             = false;
  unsigned                     port               = 0;
  unsigned                     symbol             = 0;
  unsigned                     start_prb          = 0;
  unsigned                     nof_prbs           = 0;
  const uplane_section_params* section            = nullptr;
  std::vector<uint8_t>         payload;
  std::promise<void>*          call_entered = nullptr;
  std::shared_future<void>     call_release;
};

class blocking_shared_resource_grid_pool : private shared_resource_grid::pool_interface
{
public:
  blocking_shared_resource_grid_pool(resource_grid& grid_, std::shared_future<void> release_allowed_) :
    grid(grid_), release_allowed(std::move(release_allowed_))
  {
  }

  ~blocking_shared_resource_grid_pool() override
  {
    report_fatal_error_if_not(ref_count == 0, "A grid is still active in {} scopes.", ref_count.load());
  }

  shared_resource_grid get_grid()
  {
    unsigned expected  = 0;
    bool     available = ref_count.compare_exchange_strong(expected, 1);
    ocudu_assert(available, "The grid must not be reserved.");
    return {*this, ref_count};
  }

  std::future<void> get_release_entered_future() { return release_entered.get_future(); }

private:
  resource_grid& get() override
  {
    ocudu_assert(ref_count > 0, "The grid must be reserved.");
    return grid;
  }

  void notify_release_scope() override
  {
    ocudu_assert(ref_count == 0, "The grid cannot be reserved.");
    release_entered.set_value();
    release_allowed.wait();
  }

  resource_grid&           grid;
  std::atomic<unsigned>    ref_count = {};
  std::promise<void>       release_entered;
  std::shared_future<void> release_allowed;
};

} // namespace

class ofh_uplane_rx_symbol_data_flow_writer_fixture : public ::testing::Test
{
protected:
  static constexpr ofdm_symbol_range                            symbol_range = {0, 14};
  static constexpr unsigned                                     nof_ports    = 1;
  static constexpr unsigned                                     nof_prb      = 51;
  static constexpr unsigned                                     sector       = 0;
  static constexpr unsigned                                     symbol_id    = 0;
  static constexpr std::array<unsigned, MAX_NOF_SUPPORTED_EAXC> eaxc         = {0, 1, 2, 3};
  static constexpr slot_point                                   slot         = {0, 0, 1};
  resource_grid_writer_bool_spy                                 rg_writer;
  resource_grid_reader_spy                                      rg_reader;
  resource_grid_spy                                             grid;
  shared_resource_grid_spy                                      shared_grid;
  std::shared_ptr<uplink_context_repository>                    repo = std::make_shared<uplink_context_repository>(1);
  uplane_message_decoder_results                                results;
  uplane_rx_symbol_data_flow_writer                             writer;

public:
  ofh_uplane_rx_symbol_data_flow_writer_fixture() :
    rg_writer(nof_prb),
    rg_reader(nof_ports, symbol_range.stop(), nof_prb),
    grid(rg_reader, rg_writer),
    shared_grid(grid),
    writer(eaxc, 0, ocudulog::fetch_basic_logger("TEST"), repo)
  {
    results.params.slot      = slot;
    results.params.symbol_id = symbol_id;
    results.sections.emplace_back();
  }
};

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, empty_context_does_not_write)
{
  writer.write_to_resource_grid(eaxc[0], results);

  ASSERT_TRUE(repo->get(results.params.slot, results.params.symbol_id).empty());
  ASSERT_FALSE(rg_writer.has_grid_been_written());
}

#ifdef ASSERTS_ENABLED
TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, death_test_no_eaxc_found)
{
  unsigned invalid_eaxc = 4;

  repo->add({results.params.slot, sector}, shared_grid.get_grid(), symbol_range, ocudulog::fetch_basic_logger("TEST"));
  repo->process_pending_contexts();
  ASSERT_FALSE(repo->get(results.params.slot, results.params.symbol_id).empty());

  ASSERT_DEATH(writer.write_to_resource_grid(invalid_eaxc, results), "");
}
#endif

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, decoded_prbs_outside_grid_prbs_do_not_write)
{
  auto& section     = results.sections.back();
  section.nof_prbs  = 50;
  section.start_prb = nof_prb;
  section.iq_samples.resize(section.nof_prbs * NOF_SUBCARRIERS_PER_RB);

  repo->add({results.params.slot, sector}, shared_grid.get_grid(), symbol_range, ocudulog::fetch_basic_logger("TEST"));
  repo->process_pending_contexts();
  writer.write_to_resource_grid(eaxc[0], results);

  ASSERT_FALSE(repo->get(results.params.slot, results.params.symbol_id).empty());
  ASSERT_FALSE(rg_writer.has_grid_been_written());

  const uplink_context& context  = repo->get(slot, symbol_id);
  const auto&           sym_data = context.get_re_written_mask();
  ASSERT_TRUE(std::all_of(sym_data.begin(), sym_data.end(), [](const auto& port) { return port.none(); }));
}

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, decoded_prbs_match_grid_prbs_write)
{
  auto& section     = results.sections.back();
  section.nof_prbs  = 51;
  section.start_prb = 0;
  section.iq_samples.resize(section.nof_prbs * NOF_SUBCARRIERS_PER_RB);

  repo->add({results.params.slot, sector}, shared_grid.get_grid(), symbol_range, ocudulog::fetch_basic_logger("TEST"));
  repo->process_pending_contexts();
  writer.write_to_resource_grid(eaxc[0], results);

  ASSERT_FALSE(repo->get(results.params.slot, results.params.symbol_id).empty());
  ASSERT_TRUE(rg_writer.has_grid_been_written());
  ASSERT_EQ(section.nof_prbs, rg_writer.get_nof_prbs_written());

  {
    const uplink_context& context  = repo->get(slot, symbol_id);
    const auto&           sym_data = context.get_re_written_mask();
    ASSERT_TRUE(std::all_of(sym_data.begin(), sym_data.end(), [](const auto& port) { return port.all(); }));
  }

  repo->clear();
}

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, decoded_prbs_bigger_than_grid_prbs_write)
{
  auto& section     = results.sections.back();
  section.nof_prbs  = 273;
  section.start_prb = 0;
  section.iq_samples.resize(section.nof_prbs * NOF_SUBCARRIERS_PER_RB);

  repo->add({results.params.slot, sector}, shared_grid.get_grid(), symbol_range, ocudulog::fetch_basic_logger("TEST"));
  repo->process_pending_contexts();
  writer.write_to_resource_grid(eaxc[0], results);

  ASSERT_FALSE(repo->get(results.params.slot, results.params.symbol_id).empty());
  ASSERT_TRUE(rg_writer.has_grid_been_written());
  ASSERT_EQ(rg_writer.get_nof_subc() / NOF_SUBCARRIERS_PER_RB, rg_writer.get_nof_prbs_written());

  const uplink_context& context  = repo->get(slot, symbol_id);
  const auto&           sym_data = context.get_re_written_mask();
  ASSERT_TRUE(std::all_of(sym_data.begin(), sym_data.end(), [](const auto& port) { return port.all(); }));
}

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, segmented_prbs_inside_the_grid_write)
{
  auto& section     = results.sections.back();
  section.nof_prbs  = 10;
  section.start_prb = 0;
  section.iq_samples.resize(section.nof_prbs * NOF_SUBCARRIERS_PER_RB);

  repo->add({results.params.slot, sector}, shared_grid.get_grid(), symbol_range, ocudulog::fetch_basic_logger("TEST"));
  repo->process_pending_contexts();
  writer.write_to_resource_grid(eaxc[0], results);

  ASSERT_FALSE(repo->get(results.params.slot, results.params.symbol_id).empty());
  ASSERT_TRUE(rg_writer.has_grid_been_written());
  ASSERT_EQ(section.nof_prbs, rg_writer.get_nof_prbs_written());

  const uplink_context& context  = repo->get(slot, symbol_id);
  const auto&           sym_data = context.get_re_written_mask();
  ASSERT_TRUE(std::all_of(sym_data.begin(), sym_data.end(), [&section](const auto& port) {
    return port.all(0, (section.nof_prbs - 1) * NOF_SUBCARRIERS_PER_RB);
  }));
}

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, segmented_prbs_write_the_prbs_overlapped_with_grid)
{
  auto& section     = results.sections.back();
  section.nof_prbs  = 60;
  section.start_prb = 40;
  section.iq_samples.resize(section.nof_prbs * NOF_SUBCARRIERS_PER_RB);

  repo->add({results.params.slot, sector}, shared_grid.get_grid(), symbol_range, ocudulog::fetch_basic_logger("TEST"));
  repo->process_pending_contexts();
  writer.write_to_resource_grid(eaxc[0], results);

  ASSERT_FALSE(repo->get(results.params.slot, results.params.symbol_id).empty());
  ASSERT_TRUE(rg_writer.has_grid_been_written());
  ASSERT_EQ(11, rg_writer.get_nof_prbs_written());

  const uplink_context& context  = repo->get(slot, symbol_id);
  const auto&           sym_data = context.get_re_written_mask();
  ASSERT_TRUE(std::all_of(sym_data.begin(), sym_data.end(), [](const auto& port) {
    return port.all(40 * NOF_SUBCARRIERS_PER_RB, 50 * NOF_SUBCARRIERS_PER_RB);
  }));
}

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, segmented_prbs_fill_the_grid)
{
  auto& section     = results.sections.back();
  section.nof_prbs  = 50;
  section.start_prb = 0;
  section.iq_samples.resize(section.nof_prbs * NOF_SUBCARRIERS_PER_RB);

  repo->add({results.params.slot, sector}, shared_grid.get_grid(), symbol_range, ocudulog::fetch_basic_logger("TEST"));
  repo->process_pending_contexts();
  writer.write_to_resource_grid(eaxc[0], results);
  ASSERT_EQ(section.nof_prbs, rg_writer.get_nof_prbs_written());
  {
    const uplink_context& context  = repo->get(slot, symbol_id);
    const auto&           sym_data = context.get_re_written_mask();
    ASSERT_TRUE(std::all_of(sym_data.begin(), sym_data.end(), [&section](const auto& port) {
      return port.all(0, (section.nof_prbs - 1) * NOF_SUBCARRIERS_PER_RB);
    }));
  }

  unsigned nof_prbs = section.nof_prbs;

  section.nof_prbs  = 1;
  section.start_prb = 50;
  section.iq_samples.resize(section.nof_prbs * NOF_SUBCARRIERS_PER_RB);
  nof_prbs += section.nof_prbs;

  writer.write_to_resource_grid(eaxc[0], results);

  ASSERT_FALSE(repo->get(results.params.slot, results.params.symbol_id).empty());
  ASSERT_TRUE(rg_writer.has_grid_been_written());
  ASSERT_EQ(nof_prbs, rg_writer.get_nof_prbs_written());
  const uplink_context& context  = repo->get(slot, symbol_id);
  const auto&           sym_data = context.get_re_written_mask();
  ASSERT_TRUE(std::all_of(sym_data.begin(), sym_data.end(), [](const auto& port) { return port.all(); }));
}

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, compressed_prbs_write_through_device_grid_path)
{
  resource_grid_device_writer_spy   device_writer(nof_prb);
  resource_grid_spy                 device_grid(rg_reader, device_writer);
  shared_resource_grid_spy          device_shared_grid(device_grid);
  std::array<uint8_t, 128>          compressed_payload = {};
  uplane_message_decoder_device_spy decoder;

  auto& section              = results.sections.back();
  section.nof_prbs           = nof_prb;
  section.start_prb          = 0;
  section.compressed_iq_data = compressed_payload;

  repo->add(
      {results.params.slot, sector}, device_shared_grid.get_grid(), symbol_range, ocudulog::fetch_basic_logger("TEST"));
  repo->process_pending_contexts();

  auto direct_result   = writer.write_compressed_to_resource_grid(eaxc[0], results, decoder);
  bool all_res_written = false;
  {
    const uplink_context& context  = repo->get(slot, symbol_id);
    const auto&           sym_data = context.get_re_written_mask();
    all_res_written = std::all_of(sym_data.begin(), sym_data.end(), [](const auto& port) { return port.all(); });
  }
  repo->clear();

  ASSERT_EQ(uplane_rx_symbol_data_flow_writer::direct_write_result::success, direct_result);
  ASSERT_TRUE(decoder.called);
  ASSERT_EQ(0, decoder.port);
  ASSERT_EQ(symbol_id, decoder.symbol);
  ASSERT_EQ(section.start_prb, decoder.start_prb);
  ASSERT_EQ(section.nof_prbs, decoder.nof_prbs);
  ASSERT_EQ(&section, decoder.section);
  ASSERT_EQ(1, device_writer.prepare_count);
  ASSERT_EQ(1, device_writer.enqueue_count);
  ASSERT_FALSE(device_writer.has_grid_been_written());
  ASSERT_TRUE(all_res_written);
}

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, device_grid_callback_does_not_hold_repository_mutex)
{
  resource_grid_device_writer_spy   device_writer(nof_prb);
  resource_grid_spy                 device_grid(rg_reader, device_writer);
  shared_resource_grid_spy          device_shared_grid(device_grid);
  std::array<uint8_t, 128>          compressed_payload = {};
  uplane_message_decoder_device_spy decoder;
  std::promise<void>                call_entered;
  std::promise<void>                allow_call_to_finish;
  std::future<void>                 call_entered_future = call_entered.get_future();

  decoder.call_entered       = &call_entered;
  decoder.call_release       = allow_call_to_finish.get_future().share();
  auto& section              = results.sections.back();
  section.nof_prbs           = nof_prb;
  section.start_prb          = 0;
  section.compressed_iq_data = compressed_payload;

  repo->add(
      {results.params.slot, sector}, device_shared_grid.get_grid(), symbol_range, ocudulog::fetch_basic_logger("TEST"));
  repo->process_pending_contexts();

  auto write_future = std::async(std::launch::async, [this, &decoder]() {
    return writer.write_compressed_to_resource_grid(eaxc[0], results, decoder);
  });
  if (call_entered_future.wait_for(std::chrono::seconds(1)) != std::future_status::ready) {
    allow_call_to_finish.set_value();
    FAIL() << "Timed out waiting for the device-grid callback";
  }

  auto get_future = std::async(std::launch::async, [this]() { return repo->get(slot, symbol_id).empty(); });
  EXPECT_EQ(std::future_status::ready, get_future.wait_for(std::chrono::milliseconds(100)));

  allow_call_to_finish.set_value();
  EXPECT_FALSE(get_future.get());
  EXPECT_EQ(uplane_rx_symbol_data_flow_writer::direct_write_result::success, write_future.get());
  repo->clear();
}

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, grid_release_does_not_hold_repository_mutex)
{
  std::promise<void>                 allow_release_to_finish;
  std::shared_future<void>           release_allowed = allow_release_to_finish.get_future().share();
  blocking_shared_resource_grid_pool grid_pool(grid, std::move(release_allowed));
  std::future<void>                  release_entered = grid_pool.get_release_entered_future();

  repo->add({results.params.slot, sector}, grid_pool.get_grid(), symbol_range, ocudulog::fetch_basic_logger("TEST"));
  repo->process_pending_contexts();

  auto clear_future = std::async(std::launch::async, [this]() { repo->clear(); });
  if (release_entered.wait_for(std::chrono::seconds(1)) != std::future_status::ready) {
    allow_release_to_finish.set_value();
    FAIL() << "Timed out waiting for the resource-grid release";
  }

  auto get_future = std::async(std::launch::async, [this]() { return repo->get(slot, symbol_id).empty(); });
  EXPECT_EQ(std::future_status::ready, get_future.wait_for(std::chrono::milliseconds(100)));

  allow_release_to_finish.set_value();
  EXPECT_TRUE(get_future.get());
  clear_future.get();
}

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, compressed_prbs_request_fallback_when_device_grid_unavailable)
{
  std::array<uint8_t, 128>          compressed_payload = {};
  uplane_message_decoder_device_spy decoder;

  auto& section              = results.sections.back();
  section.nof_prbs           = nof_prb;
  section.start_prb          = 0;
  section.compressed_iq_data = compressed_payload;

  repo->add({results.params.slot, sector}, shared_grid.get_grid(), symbol_range, ocudulog::fetch_basic_logger("TEST"));
  repo->process_pending_contexts();

  ASSERT_EQ(uplane_rx_symbol_data_flow_writer::direct_write_result::fallback_unsupported_destination,
            writer.write_compressed_to_resource_grid(eaxc[0], results, decoder));
  ASSERT_FALSE(decoder.called);
  ASSERT_FALSE(rg_writer.has_grid_been_written());
}

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, compressed_prbs_report_success_after_acceleration_retry)
{
  resource_grid_device_writer_spy   device_writer(nof_prb);
  resource_grid_spy                 device_grid(rg_reader, device_writer);
  shared_resource_grid_spy          device_shared_grid(device_grid);
  std::array<uint8_t, 128>          compressed_payload = {};
  uplane_message_decoder_device_spy decoder;
  decoder.result = iq_decompression_result::success_after_retry;

  auto& section              = results.sections.back();
  section.nof_prbs           = nof_prb;
  section.start_prb          = 0;
  section.compressed_iq_data = compressed_payload;

  repo->add(
      {results.params.slot, sector}, device_shared_grid.get_grid(), symbol_range, ocudulog::fetch_basic_logger("TEST"));
  repo->process_pending_contexts();

  auto direct_result = writer.write_compressed_to_resource_grid(eaxc[0], results, decoder);
  repo->clear();

  ASSERT_EQ(uplane_rx_symbol_data_flow_writer::direct_write_result::success_after_retry, direct_result);
  ASSERT_TRUE(decoder.called);
  ASSERT_EQ(1, device_writer.prepare_count);
  ASSERT_EQ(1, device_writer.enqueue_count);
}

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, compressed_fragments_are_assembled_before_device_grid_write)
{
  resource_grid_device_writer_spy   device_writer(nof_prb);
  resource_grid_spy                 device_grid(rg_reader, device_writer);
  shared_resource_grid_spy          device_shared_grid(device_grid);
  const ru_compression_params       compression = {compression_type::BFP, 9};
  unsigned                          prb_size    = get_compressed_prb_size(compression).value();
  std::vector<uint8_t>              compressed_payload(nof_prb * prb_size);
  uplane_message_decoder_device_spy decoder;
  decoder.assemble_fragments = true;
  decoder.result             = iq_decompression_result::success_after_retry;
  std::iota(compressed_payload.begin(), compressed_payload.end(), uint8_t{0});

  auto& section              = results.sections.back();
  section.nof_prbs           = 25;
  section.start_prb          = 0;
  section.ud_comp_hdr        = compression;
  section.compressed_iq_data = span<const uint8_t>(compressed_payload).first(section.nof_prbs * prb_size);

  repo->add(
      {results.params.slot, sector}, device_shared_grid.get_grid(), symbol_range, ocudulog::fetch_basic_logger("TEST"));
  repo->process_pending_contexts();

  ASSERT_EQ(uplane_rx_symbol_data_flow_writer::direct_write_result::fragment_buffered,
            writer.write_compressed_to_resource_grid(eaxc[0], results, decoder));
  ASSERT_FALSE(decoder.called);
  ASSERT_EQ(0, device_writer.prepare_count);
  ASSERT_TRUE(repo->get(slot, symbol_id).get_re_written_mask().front().none());

  section.start_prb          = 25;
  section.nof_prbs           = nof_prb - section.start_prb;
  section.compressed_iq_data =
      span<const uint8_t>(compressed_payload).subspan(section.start_prb * prb_size, section.nof_prbs * prb_size);

  ASSERT_EQ(uplane_rx_symbol_data_flow_writer::direct_write_result::success_after_retry,
            writer.write_compressed_to_resource_grid(eaxc[0], results, decoder));
  ASSERT_TRUE(decoder.called);
  ASSERT_EQ(0, decoder.start_prb);
  ASSERT_EQ(nof_prb, decoder.nof_prbs);
  ASSERT_EQ(compressed_payload, decoder.payload);
  ASSERT_EQ(1, device_writer.prepare_count);
  ASSERT_EQ(1, device_writer.enqueue_count);
  ASSERT_TRUE(repo->get(slot, symbol_id).get_re_written_mask().front().all());
  repo->clear();
}

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, wideband_wire_fragments_are_assembled_into_one_device_grid_write)
{
  static constexpr unsigned     wideband_nof_prb = 273;
  static constexpr unsigned     fragment_nof_prb = 52;
  resource_grid_reader_spy      wideband_reader(nof_ports, symbol_range.stop(), wideband_nof_prb);
  resource_grid_device_writer_spy wideband_writer(wideband_nof_prb);
  resource_grid_spy               wideband_grid(wideband_reader, wideband_writer);
  shared_resource_grid_spy        wideband_shared_grid(wideband_grid);
  const ru_compression_params     compression = {compression_type::BFP, 9};
  const unsigned                  prb_size = get_compressed_prb_size(compression).value();
  std::vector<uint8_t>            compressed_payload(wideband_nof_prb * prb_size);
  uplane_message_decoder_device_spy decoder;
  decoder.assemble_fragments = true;
  std::iota(compressed_payload.begin(), compressed_payload.end(), uint8_t{0});

  repo->add({results.params.slot, sector},
            wideband_shared_grid.get_grid(),
            symbol_range,
            ocudulog::fetch_basic_logger("TEST"));
  repo->process_pending_contexts();

  auto& section       = results.sections.back();
  section.ud_comp_hdr = compression;
  for (unsigned start_prb = 0; start_prb != wideband_nof_prb;) {
    section.start_prb          = start_prb;
    section.nof_prbs           = std::min(fragment_nof_prb, wideband_nof_prb - start_prb);
    section.compressed_iq_data = span<const uint8_t>(compressed_payload)
                                     .subspan(static_cast<size_t>(start_prb) * prb_size,
                                              static_cast<size_t>(section.nof_prbs) * prb_size);

    auto expected = (start_prb + section.nof_prbs == wideband_nof_prb)
                        ? uplane_rx_symbol_data_flow_writer::direct_write_result::success
                        : uplane_rx_symbol_data_flow_writer::direct_write_result::fragment_buffered;
    ASSERT_EQ(expected, writer.write_compressed_to_resource_grid(eaxc[0], results, decoder));
    start_prb += section.nof_prbs;
  }

  ASSERT_TRUE(decoder.called);
  EXPECT_EQ(0, decoder.start_prb);
  EXPECT_EQ(wideband_nof_prb, decoder.nof_prbs);
  EXPECT_EQ(compressed_payload, decoder.payload);
  EXPECT_EQ(1, wideband_writer.prepare_count);
  EXPECT_EQ(1, wideband_writer.enqueue_count);
  EXPECT_TRUE(repo->get(slot, symbol_id).get_re_written_mask().front().all());
  repo->clear();
}

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, compressed_prbs_return_failure_when_context_is_expired)
{
  std::array<uint8_t, 128>          compressed_payload = {};
  uplane_message_decoder_device_spy decoder;

  auto& section              = results.sections.back();
  section.nof_prbs           = nof_prb;
  section.start_prb          = 0;
  section.compressed_iq_data = compressed_payload;

  ASSERT_EQ(uplane_rx_symbol_data_flow_writer::direct_write_result::failure_missing_context,
            writer.write_compressed_to_resource_grid(eaxc[0], results, decoder));
  ASSERT_FALSE(decoder.called);
}

TEST_F(ofh_uplane_rx_symbol_data_flow_writer_fixture, compressed_prbs_return_failure_after_device_write_error)
{
  resource_grid_device_writer_spy   device_writer(nof_prb);
  resource_grid_spy                 device_grid(rg_reader, device_writer);
  shared_resource_grid_spy          device_shared_grid(device_grid);
  std::array<uint8_t, 128>          compressed_payload = {};
  uplane_message_decoder_device_spy decoder;
  decoder.result = iq_decompression_result::acceleration_failure;

  auto& section              = results.sections.back();
  section.nof_prbs           = nof_prb;
  section.start_prb          = 0;
  section.compressed_iq_data = compressed_payload;

  repo->add(
      {results.params.slot, sector}, device_shared_grid.get_grid(), symbol_range, ocudulog::fetch_basic_logger("TEST"));
  repo->process_pending_contexts();

  auto direct_result = writer.write_compressed_to_resource_grid(eaxc[0], results, decoder);
  repo->clear();

  ASSERT_EQ(uplane_rx_symbol_data_flow_writer::direct_write_result::failure_acceleration, direct_result);
  ASSERT_TRUE(decoder.called);
  ASSERT_FALSE(device_writer.has_grid_been_written());
}
