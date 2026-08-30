// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "diagnostic_plugin_sink.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/diagnostic_plugin.h"
#include <cstdlib>
#include <dlfcn.h>
#include <vector>

using namespace ocudu;

namespace {

uint8_t to_plugin_stream(phy_diagnostic_stream stream)
{
  switch (stream) {
    case phy_diagnostic_stream::pusch_crc:
      return static_cast<uint8_t>(OCUDU_DIAGNOSTIC_PLUGIN_PUSCH_CRC);
    case phy_diagnostic_stream::srs_channel:
      return static_cast<uint8_t>(OCUDU_DIAGNOSTIC_PLUGIN_SRS_CHANNEL);
    case phy_diagnostic_stream::pusch_dmrs_channel:
      return static_cast<uint8_t>(OCUDU_DIAGNOSTIC_PLUGIN_PUSCH_DMRS_CHANNEL);
    case phy_diagnostic_stream::pusch_symbols:
    default:
      return static_cast<uint8_t>(OCUDU_DIAGNOSTIC_PLUGIN_PUSCH_SYMBOLS);
  }
}

class diagnostic_plugin_sink final : public phy_diagnostic_sink
{
public:
  diagnostic_plugin_sink(void*                                      library,
                         ocudu_diagnostic_plugin_handle*            handle,
                         ocudu_diagnostic_plugin_close_fn           close_fn,
                         ocudu_diagnostic_plugin_should_collect_fn  should_collect_fn,
                         ocudu_diagnostic_plugin_is_collect_due_fn  is_collect_due_fn,
                         ocudu_diagnostic_plugin_publish_crc_fn     publish_crc_fn,
                         ocudu_diagnostic_plugin_publish_symbols_fn publish_symbols_fn,
                         ocudu_diagnostic_plugin_publish_srs_fn     publish_srs_fn) :
    library_(library),
    handle_(handle),
    close_fn_(close_fn),
    should_collect_fn_(should_collect_fn),
    is_collect_due_fn_(is_collect_due_fn),
    publish_crc_fn_(publish_crc_fn),
    publish_symbols_fn_(publish_symbols_fn),
    publish_srs_fn_(publish_srs_fn)
  {
  }

  ~diagnostic_plugin_sink() override
  {
    if (handle_ != nullptr && close_fn_ != nullptr) {
      close_fn_(handle_);
      handle_ = nullptr;
    }
    if (library_ != nullptr) {
      dlclose(library_);
      library_ = nullptr;
    }
  }

  bool should_collect(phy_diagnostic_stream stream, uint16_t rnti) override
  {
    if (should_collect_fn_ == nullptr || handle_ == nullptr) {
      return false;
    }
    return should_collect_fn_(handle_, to_plugin_stream(stream), rnti) != 0;
  }

  bool is_collect_due(phy_diagnostic_stream stream, uint16_t rnti) override
  {
    if (is_collect_due_fn_ == nullptr || handle_ == nullptr) {
      return false;
    }
    return is_collect_due_fn_(handle_, to_plugin_stream(stream), rnti) != 0;
  }

  void on_pusch_crc(const pusch_crc_diagnostic& event) override
  {
    if (publish_crc_fn_ == nullptr || handle_ == nullptr) {
      return;
    }
    publish_crc_fn_(handle_,
                    event.rnti,
                    static_cast<uint16_t>(event.slot.sfn()),
                    static_cast<uint16_t>(event.slot.slot_index()),
                    event.crc_ok ? 1 : 0);
  }

  void on_pusch_symbols(const pusch_symbols_diagnostic& event) override
  {
    if (publish_symbols_fn_ == nullptr || handle_ == nullptr) {
      return;
    }
    std::vector<float> interleaved;
    const float*       ptr  = nullptr;
    uint32_t           nflt = 0;
    if (!event.equalized_symbols.empty()) {
      interleaved.resize(event.equalized_symbols.size() * 2);
      for (std::size_t i = 0; i != event.equalized_symbols.size(); ++i) {
        interleaved[2 * i]     = event.equalized_symbols[i].real();
        interleaved[2 * i + 1] = event.equalized_symbols[i].imag();
      }
      ptr  = interleaved.data();
      nflt = static_cast<uint32_t>(interleaved.size());
    }
    publish_symbols_fn_(handle_,
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
                        ptr,
                        nflt);
  }

  void on_srs_channel(const srs_channel_diagnostic& event) override
  {
    if (publish_srs_fn_ == nullptr || handle_ == nullptr || event.h_coefficients.empty()) {
      return;
    }
    std::vector<float> interleaved(event.h_coefficients.size() * 2);
    for (std::size_t i = 0; i != event.h_coefficients.size(); ++i) {
      interleaved[2 * i]     = event.h_coefficients[i].real();
      interleaved[2 * i + 1] = event.h_coefficients[i].imag();
    }
    publish_srs_fn_(handle_,
                    event.rnti,
                    static_cast<uint16_t>(event.slot.sfn()),
                    static_cast<uint16_t>(event.slot.slot_index()),
                    event.nof_subcarriers,
                    event.comb_size,
                    event.nof_rx_ports,
                    event.nof_tx_ports,
                    event.rx_port_index,
                    event.tx_port_index,
                    event.rsrp_dB,
                    event.epre_dB,
                    event.noise_variance,
                    event.time_alignment_ns,
                    interleaved.data(),
                    static_cast<uint32_t>(interleaved.size()));
  }

  void on_pusch_dmrs_channel(const pusch_dmrs_channel_diagnostic& /*event*/) override {}

private:
  void*                                      library_            = nullptr;
  ocudu_diagnostic_plugin_handle*            handle_             = nullptr;
  ocudu_diagnostic_plugin_close_fn           close_fn_           = nullptr;
  ocudu_diagnostic_plugin_should_collect_fn  should_collect_fn_  = nullptr;
  ocudu_diagnostic_plugin_is_collect_due_fn  is_collect_due_fn_  = nullptr;
  ocudu_diagnostic_plugin_publish_crc_fn     publish_crc_fn_     = nullptr;
  ocudu_diagnostic_plugin_publish_symbols_fn publish_symbols_fn_ = nullptr;
  ocudu_diagnostic_plugin_publish_srs_fn     publish_srs_fn_     = nullptr;
};

void* try_dlopen(const std::string& path)
{
  if (path.empty()) {
    return nullptr;
  }
  void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    ocudulog::fetch_basic_logger("PHY").warning("Diagnostic plugin dlopen({}) failed: {}", path, dlerror());
  }
  return library;
}

} // namespace

std::shared_ptr<phy_diagnostic_sink> ocudu::create_diagnostic_plugin_sink(const std::string& endpoint)
{
  if (endpoint.empty()) {
    return nullptr;
  }

  std::vector<std::string> candidates;
  if (const char* env = std::getenv("OCUDU_DIAGNOSTIC_PLUGIN_LIB"); env != nullptr && env[0] != '\0') {
    candidates.emplace_back(env);
  }
  candidates.emplace_back("libocudu_diagnostic_plugin.so");

  void*       library = nullptr;
  std::string opened_path;
  for (const std::string& candidate : candidates) {
    library = try_dlopen(candidate);
    if (library != nullptr) {
      opened_path = candidate;
      break;
    }
  }
  if (library == nullptr) {
    return nullptr;
  }

  auto* open_fn  = reinterpret_cast<ocudu_diagnostic_plugin_open_fn>(dlsym(library, "ocudu_diagnostic_plugin_open"));
  auto* close_fn = reinterpret_cast<ocudu_diagnostic_plugin_close_fn>(dlsym(library, "ocudu_diagnostic_plugin_close"));
  auto* should_collect_fn = reinterpret_cast<ocudu_diagnostic_plugin_should_collect_fn>(
      dlsym(library, "ocudu_diagnostic_plugin_should_collect"));
  auto* is_collect_due_fn = reinterpret_cast<ocudu_diagnostic_plugin_is_collect_due_fn>(
      dlsym(library, "ocudu_diagnostic_plugin_is_collect_due"));
  auto* publish_crc_fn =
      reinterpret_cast<ocudu_diagnostic_plugin_publish_crc_fn>(dlsym(library, "ocudu_diagnostic_plugin_publish_crc"));
  auto* publish_symbols_fn = reinterpret_cast<ocudu_diagnostic_plugin_publish_symbols_fn>(
      dlsym(library, "ocudu_diagnostic_plugin_publish_symbols"));
  auto* publish_srs_fn =
      reinterpret_cast<ocudu_diagnostic_plugin_publish_srs_fn>(dlsym(library, "ocudu_diagnostic_plugin_publish_srs"));

  if (open_fn == nullptr || close_fn == nullptr || should_collect_fn == nullptr || publish_crc_fn == nullptr ||
      publish_symbols_fn == nullptr) {
    ocudulog::fetch_basic_logger("PHY").warning("Diagnostic plugin {} is missing required symbols", opened_path);
    dlclose(library);
    return nullptr;
  }

  ocudu_diagnostic_plugin_handle* handle = open_fn(endpoint.c_str());
  if (handle == nullptr) {
    ocudulog::fetch_basic_logger("PHY").warning(
        "Diagnostic plugin {} failed to open endpoint {}", opened_path, endpoint);
    dlclose(library);
    return nullptr;
  }

  ocudulog::fetch_basic_logger("PHY").info("Diagnostic plugin loaded: {} endpoint={}", opened_path, endpoint);

  return std::make_shared<diagnostic_plugin_sink>(library,
                                                  handle,
                                                  close_fn,
                                                  should_collect_fn,
                                                  is_collect_due_fn,
                                                  publish_crc_fn,
                                                  publish_symbols_fn,
                                                  publish_srs_fn);
}
