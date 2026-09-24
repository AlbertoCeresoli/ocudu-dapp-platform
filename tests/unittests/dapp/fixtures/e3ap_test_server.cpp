// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Loopback E3 agent for cross-language tests: hosts the management service, the
// portable data store, the telemetry publisher, the E3AP SCTP endpoint, and
// (when built) the E3 data-profile endpoint. Synthetic PUSCH symbol, CRC, SRS
// and gNB JSON telemetry is published every few milliseconds until stdin is
// closed. The bound ports are printed as "e3ap_port N" / "e3dp_port N".

#include "ocudu/dapp/e3/e3_connection_registry.h"
#include "ocudu/dapp/e3/e3ap_sctp_server.h"
#include "ocudu/dapp/e3/embedded_agent.h"
#include "ocudu/dapp/e3/management_service.h"
#include "ocudu/dapp/e3/telemetry_publisher.h"
#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/dapp/runtime/pusch_route_table.h"
#ifdef OCUDU_DAPP_E3DP_CARRIER
#include "ocudu/dapp/e3/e3dp_sctp_server.h"
#endif
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace ocudu;
using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;

namespace {

struct options {
  std::string catalog{"/tmp"};
  uint16_t    e3ap_port{0};
  uint16_t    e3dp_port{0};
  unsigned    period_ms{10};
  bool        e3dp{true};
};

options parse(int argc, char** argv)
{
  options result;
  for (int index = 1; index + 1 < argc; index += 2) {
    const std::string option = argv[index];
    const std::string value  = argv[index + 1];
    if (option == "--catalog") {
      result.catalog = value;
    } else if (option == "--e3ap-port") {
      result.e3ap_port = static_cast<uint16_t>(std::stoul(value));
    } else if (option == "--e3dp-port") {
      result.e3dp_port = static_cast<uint16_t>(std::stoul(value));
    } else if (option == "--period-ms") {
      result.period_ms = static_cast<unsigned>(std::stoul(value));
    } else if (option == "--e3dp") {
      result.e3dp = value != "0" && value != "off";
    } else {
      std::fprintf(stderr, "unknown option %s\n", option.c_str());
      std::exit(2);
    }
  }
  return result;
}

} // namespace

int main(int argc, char** argv)
{
  const options opts = parse(argc, argv);

  ocudu_dapp_host_info_v1 host{};
  host.struct_size = sizeof(host);
  host.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  ocudu_dapp_host_api_v1 api{};
  api.struct_size = sizeof(api);
  api.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  instance_manager           manager(opts.catalog, host, api, 32);
  embedded_agent             agent(manager, 8, 32);
  portable_data_store_config store_config;
  store_config.maximum_subscriptions  = 16;
  store_config.maximum_retained_bytes = 64 * 1024 * 1024;
  store_config.maximum_object_bytes   = 4 * 1024 * 1024;
  store_config.maximum_metadata_bytes = 64 * 1024;
  store_config.minimum_chunk_bytes    = 1024;
  store_config.maximum_chunk_bytes    = 64 * 1024;
  store_config.minimum_lease_us       = 100'000;
  store_config.maximum_lease_us       = 60'000'000;
  auto store                          = portable_data_store::create(store_config);
  if (!store.first) {
    std::fprintf(stderr, "store: %s\n", store.second.c_str());
    return 1;
  }
  auto               connections = std::make_shared<e3_connection_registry>();
  // Per-UE PUSCH routing with no bound instances: queries, validation and stock/primary routes only.
  auto               pusch_routes = std::make_shared<pusch_route_table>(2);
  management_service service(agent, {}, store.first, connections, {}, pusch_routes);

  telemetry_publisher_config publisher_config;
  publisher_config.store                          = store.first;
  publisher_config.pusch_symbols_minimum_interval = std::chrono::microseconds(0);
  publisher_config.pusch_crc_minimum_interval     = std::chrono::microseconds(0);
  publisher_config.srs_channel_minimum_interval   = std::chrono::microseconds(0);
  auto publisher                                  = telemetry_publisher::create(publisher_config);
  if (!publisher.first) {
    std::fprintf(stderr, "publisher: %s\n", publisher.second.c_str());
    return 1;
  }

  e3ap_sctp_server_config e3ap_config;
  e3ap_config.transport.port         = opts.e3ap_port;
  e3ap_config.transport.timer_period = std::chrono::milliseconds(2);
  e3ap_config.ran_identifier         = "ocudu-e3-test-agent";
  e3ap_config.cell_ids               = {0};
  auto e3ap                          = e3ap_sctp_server::create(&service, store.first, e3ap_config, connections);
  if (!e3ap) {
    std::fprintf(stderr, "e3ap: %s\n", e3ap.detail.c_str());
    return 1;
  }
  std::printf("e3ap_port %u\n", e3ap.server->port());
#ifdef OCUDU_DAPP_E3DP_CARRIER
  std::unique_ptr<e3dp_sctp_server> e3dp;
  if (opts.e3dp) {
    e3dp_sctp_server_config e3dp_config;
    e3dp_config.transport.port = opts.e3dp_port;
    e3dp_config.cells          = {{0, 3'500'000'000ULL, 0}};
    auto created               = e3dp_sctp_server::create(service, store.first, e3dp_config);
    if (!created) {
      std::fprintf(stderr, "e3dp: %s\n", created.detail.c_str());
      return 1;
    }
    e3dp = std::move(created.server);
    std::printf("e3dp_port %u\n", e3dp->port());
  }
#endif
  std::printf("ready\n");
  std::fflush(stdout);

  std::atomic<bool> stop{false};
  std::thread       producer([&] {
    std::vector<std::complex<float>> symbols(52 * 12 * 12);
    std::vector<std::complex<float>> coefficient(1);
    uint32_t                         counter = 0;
    while (!stop.load(std::memory_order_acquire)) {
      ++counter;
      const uint16_t   rnti = 0x4601 + (counter % 2);
      const slot_point slot{1, counter % 1024, counter % 20};
      for (std::size_t index = 0; index != symbols.size(); ++index) {
        const float phase = static_cast<float>((index + counter) % 4) * 1.5707963F;
        symbols[index]    = {std::cos(phase), std::sin(phase)};
      }
      if (publisher.first->should_collect(phy_diagnostic_stream::pusch_symbols, rnti)) {
        pusch_symbols_diagnostic event;
        event.rnti              = rnti;
        event.slot              = slot;
        event.nof_prb           = 52;
        event.nof_data_symbols  = 12;
        event.nof_layers        = 1;
        event.mod_bits          = 2;
        event.sinr_dB           = 20.0F + static_cast<float>(counter % 5);
        event.epre_dB           = -60.0F;
        event.rsrp_dB           = -62.0F;
        event.evm               = 0.1F;
        event.time_alignment_us = 0.5F;
        event.cfo_Hz            = 12.0F;
        event.target_code_rate  = 0.5F;
        event.equalized_symbols = symbols;
        publisher.first->on_pusch_symbols(event);
      }
      if (publisher.first->should_collect(phy_diagnostic_stream::pusch_crc, rnti)) {
        publisher.first->on_pusch_crc({rnti, slot, counter % 7 != 0});
      }
      if (publisher.first->should_collect(phy_diagnostic_stream::srs_channel, rnti)) {
        coefficient[0] = {0.7F, -0.2F};
        srs_channel_diagnostic srs;
        srs.rnti              = rnti;
        srs.slot              = slot;
        srs.nof_subcarriers   = 1;
        srs.comb_size         = 2;
        srs.nof_rx_ports      = 1;
        srs.nof_tx_ports      = 1;
        srs.rsrp_dB           = -70.0F;
        srs.epre_dB           = -68.0F;
        srs.noise_variance    = 1e-3F;
        srs.time_alignment_ns = 150.0F;
        srs.h_coefficients    = coefficient;
        publisher.first->on_srs_channel(srs);
      }
      if (counter % 50 == 0 && publisher.first->metrics_json_active()) {
        const std::string json =
            "{\"timestamp\":" + std::to_string(counter) +
            ",\"ue_list\":[{\"ue_container\":{\"rnti\":17921,\"pusch_snr_db\":21.5,\"ul_mcs\":16}}]}";
        publisher.first->publish_metrics_json(json);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(opts.period_ms));
    }
  });

  // Run until stdin closes so the parent test controls the lifetime. "xapp <dapp id> <ran function>
  // <hex>" queues an E3-XAppControlAction to that dApp (the interop tests drive the xApp -> dApp
  // direction this way) and answers "xapp sent" or "xapp refused" on stdout.
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "quit") {
      break;
    }
    if (line.rfind("xapp ", 0) == 0) {
      unsigned    dapp_id = 0, ran_function = 0;
      std::string hex;
      std::istringstream(line.substr(5)) >> dapp_id >> ran_function >> hex;
      std::vector<uint8_t> data;
      for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
        data.push_back(static_cast<uint8_t>(std::stoul(hex.substr(index, 2), nullptr, 16)));
      }
      const bool sent = e3ap.server->send_xapp_control_action(static_cast<uint16_t>(dapp_id),
                                                              static_cast<uint16_t>(ran_function), std::move(data));
      std::cout << (sent ? "xapp sent" : "xapp refused") << std::endl;
    }
  }
  stop.store(true, std::memory_order_release);
  producer.join();
  const auto metrics = e3ap.server->metrics();
  std::fprintf(stderr,
               "e3ap: setups=%llu subscriptions=%llu indications=%llu dropped=%llu filtered=%llu\n",
               static_cast<unsigned long long>(metrics.setups_completed),
               static_cast<unsigned long long>(metrics.subscriptions_created),
               static_cast<unsigned long long>(metrics.indications_sent),
               static_cast<unsigned long long>(metrics.indications_dropped),
               static_cast<unsigned long long>(metrics.indications_filtered));
  return 0;
}
