// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "channel_emulator_hook.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/support/prach_buffer.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/ran/prach/prach_preamble_information.h"
#include "ocudu/ran/prach/prach_subcarrier_spacing.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/support/channel_emulator_plugin_api_v1.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>

using namespace ocudu;

namespace {

/// Parses "key=value,key=value" arguments.
double arg_double(const std::string& args, const char* key, double fallback)
{
  const std::string needle = std::string(key) + "=";
  const size_t      pos    = args.find(needle);
  if (pos == std::string::npos || (pos != 0 && args[pos - 1] != ',')) {
    return fallback;
  }
  const char*  start = args.c_str() + pos + needle.size();
  char*        end   = nullptr;
  const double v     = std::strtod(start, &end);
  return (end == start) ? fallback : v;
}

std::string arg_string(const std::string& args, const char* key, const std::string& fallback)
{
  const std::string needle = std::string(key) + "=";
  const size_t      pos    = args.find(needle);
  if (pos == std::string::npos || (pos != 0 && args[pos - 1] != ',')) {
    return fallback;
  }
  const size_t start = pos + needle.size();
  const size_t end   = args.find(',', start);
  return args.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool env_explicitly_disabled(const char* name)
{
  const char* value = std::getenv(name);
  return value != nullptr &&
         (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 || std::strcmp(value, "off") == 0 ||
          std::strcmp(value, "no") == 0 || std::strcmp(value, "disabled") == 0);
}

bool whole_slot_cuda_rx_batch_configured()
{
  if (env_explicitly_disabled("OCUDU_LOWPHY_RX_ACCELERATION") ||
      env_explicitly_disabled("OCUDU_LOWPHY_PUXCH_DEMODULATION_ACCELERATION") ||
      env_explicitly_disabled("OCUDU_LOWPHY_RX_SYMBOL_BATCH") ||
      env_explicitly_disabled("OCUDU_LOWPHY_PUXCH_SYMBOL_BATCH")) {
    return false;
  }
  const char* size = std::getenv("OCUDU_LOWPHY_RX_SYMBOL_BATCH_SIZE");
  if (size == nullptr || size[0] == '\0') {
    return true; // CUDA lower-PHY default is OCUDU_LOWPHY_PUXCH_RX_MAX_SYMBOLS (14).
  }
  char*               end = nullptr;
  const unsigned long n   = std::strtoul(size, &end, 10);
  return end != size && *end == '\0' && n >= 14;
}

} // namespace

struct channel_emulator_hook::impl {
  void*                                lib                      = nullptr;
  const ocudu_channel_plugin_api_v1_t* api                      = nullptr;
  ocudu_channel_plugin_t*              inst                     = nullptr;
  unsigned                             nof_subc                 = 0;
  unsigned                             nof_symbols              = 14;
  unsigned                             nof_ports                = 1;
  bool                                 dims_checked             = false;
  bool                                 warned_no_device_grid    = false;
  bool                                 warned_ul_requires_batch = false;

  // Per-direction bookkeeping. Each direction is driven by exactly one
  // thread in the SDR RU (the RU UL execution context and the DL request
  // path), so this state is deliberately LOCK-FREE: each struct is only
  // ever touched by its own thread. Do not share across directions.
  struct direction_state {
    // slot_point wraps every 1024 frames; keep a monotonic count so the
    // plugin's Doppler time base never jumps backwards.
    uint32_t last_raw_count = 0;
    uint64_t wrap_offset    = 0;
    // UL only: symbols already applied for the in-flight slot (per-sector
    // state would be needed for >1 sector; v1 targets one sector).
    slot_point last_slot;
    unsigned   next_symbol = 0;
    // UL owned snapshot dest for this slot (written by the fade kernel).
    void*    snap_dest        = nullptr;
    unsigned snap_from_symbol = 0;

    uint64_t unwrap(slot_point slot)
    {
      const uint32_t raw         = slot.count();
      const uint32_t wrap_period = 1024U * slot.nof_slots_per_frame(); // NOF_SFNS frames
      if (raw + wrap_period / 2 < last_raw_count) {                    // wrapped (tolerate reordering)
        // last_raw_count must restart below raw or this branch re-fires on
        // every slot of the new epoch, adding wrap_period each call: the time
        // base then gains ~2e8 slots per epoch and float(t) in the generator
        // loses sub-slot resolution, turning Doppler into per-update jumps.
        wrap_offset += wrap_period;
        last_raw_count = raw;
      } else if (wrap_offset >= wrap_period && last_raw_count + wrap_period / 2 < raw) {
        // Stale pre-wrap slot arriving after the wrap was applied: map it into
        // the previous epoch without disturbing the tracker.
        return wrap_offset - wrap_period + raw;
      } else {
        last_raw_count = std::max(last_raw_count, raw);
      }
      return wrap_offset + raw;
    }
  };
  direction_state ul;
  direction_state dl;
  direction_state prach;
  bool            warned_no_prach_device = false;
  // Apply the whole UL slot in ONE kernel at the slot's first notification.
  // Correct when the lower PHY produces the slot as a single batched device
  // launch before notifying (the CUDA lower PHY default: it accumulates all
  // 14 symbols, demodulates once, then notifies in a burst) because the
  // grid's producer event covers the whole batch. NOT correct for the CPU
  // per-symbol demod path, where later symbols would be written after our
  // multiply -- hence a config flag rather than always-on.
  bool ul_slot_batch = false;
};

channel_emulator_hook& channel_emulator_hook::get()
{
  static channel_emulator_hook instance;
  return instance;
}

channel_emulator_hook::channel_emulator_hook()
{
  const char* args_env = std::getenv("OCUDU_CHANNEL_EMULATOR_ARGS");
  const char* lib_env  = std::getenv("OCUDU_CHANNEL_EMULATOR_LIB");
  // LIB alone is enough: start passthrough/disabled so the Class-A dApp can
  // enable and configure over E3 without a gNB restart. ARGS empty + no LIB
  // keeps the historical no-op (one atomic load per slot).
  if ((args_env == nullptr || args_env[0] == '\0') && (lib_env == nullptr || lib_env[0] == '\0')) {
    return;
  }
  const std::string args(args_env != nullptr ? args_env : "model=passthrough");
  const bool        start_enabled = args_env != nullptr && args_env[0] != '\0';
  const std::string lib_path =
      (lib_env != nullptr && lib_env[0] != '\0') ? std::string(lib_env) : std::string("libocudu_channel_plugin.so");

  auto& logger = ocudulog::fetch_basic_logger("PHY");

  pimpl      = new impl();
  pimpl->lib = ::dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (pimpl->lib == nullptr) {
    logger.warning("Channel emulator: dlopen('{}') failed: {}. Feature disabled.", lib_path, ::dlerror());
    return;
  }
  auto get_api = reinterpret_cast<const ocudu_channel_plugin_api_v1_t* (*)()>(
      ::dlsym(pimpl->lib, OCUDU_CHANNEL_PLUGIN_ENTRY_SYMBOL));
  if (get_api == nullptr || (pimpl->api = get_api()) == nullptr || pimpl->api->abi_version != 1) {
    logger.warning("Channel emulator: '{}' does not expose a v1 plugin ABI. Feature disabled.", lib_path);
    return;
  }

  const std::string model  = arg_string(args, "model", "passthrough");
  const unsigned    nof_rb = unsigned(arg_double(args, "nof_rb", 51));
  pimpl->nof_subc          = nof_rb * NOF_SUBCARRIERS_PER_RB;
  pimpl->nof_ports         = unsigned(arg_double(args, "ports", 1));

  const double scs_hz = arg_double(args, "scs_hz", 30e3);

  ocudu_channel_plugin_config_v1_t cfg = {};
  cfg.struct_size                      = sizeof(cfg);
  cfg.nof_subcarriers                  = pimpl->nof_subc;
  cfg.nof_symbols_per_slot             = pimpl->nof_symbols;
  cfg.nof_rx_ports                     = pimpl->nof_ports;
  cfg.nof_tx_ports                     = pimpl->nof_ports;
  cfg.carrier_frequency_hz             = arg_double(args, "carrier_hz", 3.5e9);
  cfg.subcarrier_spacing_hz            = scs_hz;
  cfg.slot_duration_s                  = 1e-3 / (scs_hz / 15e3);
  cfg.model                            = model.c_str();
  cfg.delay_spread_s                   = arg_double(args, "delay_spread_ns", 0.0) * 1e-9;
  cfg.ue_speed_kmh                     = arg_double(args, "speed_kmh", 3.0);
  cfg.seed                             = uint64_t(arg_double(args, "seed", 1));
  cfg.bulk_gain_db                     = arg_double(args, "gain_db", 0.0);
  cfg.dl_backoff_db                    = arg_double(args, "dl_backoff_db", 0.0);
  cfg.update_period_slots              = unsigned(arg_double(args, "update_slots", 20));
  cfg.drop_period_s                    = arg_double(args, "drop_period_s", 0.0);
  cfg.noise_floor_db                   = arg_double(args, "noise_floor_db", NAN);
  cfg.noise_ul                         = uint8_t(arg_double(args, "noise_ul", 1));
  cfg.noise_dl                         = uint8_t(arg_double(args, "noise_dl", 0));
  cfg.cuda_device                      = int(arg_double(args, "cuda_device", 0));
  pimpl->ul_slot_batch                 = arg_double(args, "ul_slot_batch", 0) != 0;

  const char* error_msg = nullptr;
  pimpl->inst           = pimpl->api->create(&cfg, &error_msg);
  if (pimpl->inst == nullptr) {
    logger.error("Channel emulator: plugin create failed: {}. Feature disabled.",
                 error_msg != nullptr ? error_msg : "unknown");
    return;
  }

  if (!start_enabled && pimpl->api->set_param != nullptr) {
    static_cast<void>(pimpl->api->set_param(pimpl->inst, "enabled", 0.0));
  }
  if (arg_double(args, "los", 0.0) != 0.0 && pimpl->api->set_param != nullptr) {
    if (pimpl->api->set_param(pimpl->inst, "los", 1.0) != 0) {
      logger.warning("Channel emulator: los=1 rejected (model has no 38.901 LOS table)");
    }
  }
  logger.info("Channel emulator {}: {} ({} RB, {} ports, apply UL{} DL noise_ul={} noise_dl={})",
              start_enabled ? "ACTIVE" : "LOADED (E3-disabled, passthrough until activate)",
              pimpl->api->describe(pimpl->inst),
              nof_rb,
              pimpl->nof_ports,
              cfg.noise_ul ? "+noise" : "",
              unsigned(cfg.noise_ul),
              unsigned(cfg.noise_dl));
  // Hook stays armed so apply() is called; the plugin's own enabled flag
  // (E3-tunable) is what makes apply a no-op.
  enabled.store(true, std::memory_order_release);
}

channel_emulator_hook::~channel_emulator_hook()
{
  // This function is registered as an executable atexit destructor and runs
  // before the dynamic loader finalizes libcudart. Stop the plugin worker and
  // release its CUDA objects here. Leaking it leaves worker_loop issuing CUDA
  // calls while libcudart tears down, which produced a reproducible shutdown
  // SIGSEGV in cudaStreamWaitEvent (null pthread rwlock).
  enabled.store(false, std::memory_order_release);
  if (pimpl == nullptr) {
    return;
  }
  if (pimpl->inst != nullptr && pimpl->api != nullptr && pimpl->api->destroy != nullptr) {
    pimpl->api->destroy(pimpl->inst);
    pimpl->inst = nullptr;
  }
  if (pimpl->lib != nullptr) {
    ::dlclose(pimpl->lib);
    pimpl->lib = nullptr;
  }
  delete pimpl;
  pimpl = nullptr;
}

void channel_emulator_hook::apply_uplink(const shared_resource_grid& grid, slot_point slot, unsigned last_symbol)
{
  if (!is_enabled()) {
    return;
  }

  // The safe GB10 path requires the CUDA lower PHY's whole-slot batch. Its
  // producer event covers all 14 symbols before the first notification, so we
  // can home the live managed grid exactly once, copy it into ordinary device
  // memory, and fade only that owned copy. Per-symbol/O-RAN notification would
  // require replacing an asynchronously consumed snapshot several times per
  // slot; fail open there until that lifetime protocol exists.
  if (!pimpl->ul_slot_batch || !whole_slot_cuda_rx_batch_configured()) {
    if (!pimpl->warned_ul_requires_batch) {
      pimpl->warned_ul_requires_batch = true;
      ocudulog::fetch_basic_logger("PHY").warning("Channel emulator: UL fade bypassed; safe owned-snapshot mode "
                                                  "requires ul_slot_batch=1 and a 14-symbol CUDA RX batch");
    }
    return;
  }

  // Lock-free: this path is single-threaded (RU UL execution context).
  impl::direction_state& st = pimpl->ul;
  if (slot != st.last_slot) {
    st.last_slot   = slot;
    st.next_symbol = 0;
  }
  if (st.next_symbol != 0) {
    return;
  }
  (void)last_symbol;
  st.next_symbol = pimpl->nof_symbols;
  apply(grid, slot, 0, pimpl->nof_symbols, OCUDU_CHANNEL_UPLINK);
}

void channel_emulator_hook::apply_downlink(const shared_resource_grid& grid, slot_point slot)
{
  if (!is_enabled()) {
    return;
  }
  apply(grid, slot, 0, pimpl->nof_symbols, OCUDU_CHANNEL_DOWNLINK);
}

void channel_emulator_hook::apply_prach(prach_buffer& buffer, const prach_buffer_context& context)
{
  if (!is_enabled()) {
    return;
  }
  if (!buffer.supports_device_prach_buffer_mapping()) {
    if (!pimpl->warned_no_prach_device) {
      pimpl->warned_no_prach_device = true;
      ocudulog::fetch_basic_logger("PHY").warning(
          "Channel emulator: PRACH buffer has no device mapping; PRACH stays unfaded.");
    }
    return;
  }

  // PRACH subcarrier spacing relative to the grid axis H is generated on.
  const double grid_scs_hz =
      to_numerology_value(context.pusch_scs) >= 0 ? 15e3 * (1U << to_numerology_value(context.pusch_scs)) : 30e3;
  double   ra_scs_hz = grid_scs_hz;
  unsigned seq_len   = buffer.get_sequence_length();
  if (is_long_preamble(context.format)) {
    ra_scs_hz = (context.format == prach_format_type::three) ? 5e3 : 1.25e3;
  }
  const double k_step = ra_scs_hz / grid_scs_hz;

  void* stream = pimpl->api->get_apply_stream(pimpl->inst, OCUDU_CHANNEL_UPLINK);
  if (!buffer.prepare_device_prach_buffer_mapping(stream)) {
    return;
  }

  uint32_t* base = static_cast<uint32_t*>(buffer.get_device_prach_buffer_cbf16());
  if (base == nullptr) {
    buffer.cancel_device_prach_buffer_mapping();
    return;
  }

  const uint64_t slot_count = pimpl->prach.unwrap(context.slot);
  const unsigned nof_ports  = std::min<unsigned>(buffer.get_max_nof_ports(), 4);
  bool           ok         = true;
  for (unsigned port = 0; port != nof_ports && ok; ++port) {
    for (unsigned td = 0; td != context.nof_td_occasions && ok; ++td) {
      for (unsigned fd = 0; fd != context.nof_fd_occasions && ok; ++fd) {
        // Occasion fd starts at the PRACH frequency-domain origin plus one
        // occasion bandwidth per previous occasion (approximate to the
        // nearest grid subcarrier; H varies over ~100 subcarriers, so a
        // couple of subcarriers of mapping slack is irrelevant).
        const double k_first =
            double(context.rb_offset) * NOF_SUBCARRIERS_PER_RB + double(fd) * double(seq_len) * k_step;

        ocudu_channel_grid_view_v1_t view = {};
        view.struct_size                  = sizeof(view);
        view.format                       = OCUDU_CHANNEL_FORMAT_CBF16;
        view.nof_ports                    = 1;
        view.nof_symbols                  = buffer.get_max_nof_symbols();
        view.nof_subcarriers              = seq_len;
        view.port_stride                  = 0;
        view.symbol_stride                = seq_len;
        view.first_symbol                 = context.start_symbol;
        view.data                         = base + buffer.get_device_prach_symbol_offset(port, td, fd, 0);

        ok = (pimpl->api->apply_prach(pimpl->inst, &view, slot_count, stream, k_first, k_step) == 0);
      }
    }
  }

  if (ok) {
    buffer.on_device_prach_buffer_mapping_enqueued(stream);
  } else {
    buffer.cancel_device_prach_buffer_mapping();
  }
}

void channel_emulator_hook::apply(const shared_resource_grid& grid,
                                  slot_point                  slot,
                                  unsigned                    first_symbol,
                                  unsigned                    end_symbol,
                                  int                         direction)
{
  resource_grid&              rg = grid.get();
  resource_grid_writer&       wr = rg.get_writer();
  const resource_grid_reader& rd = rg.get_reader();

  if (!wr.supports_device_grid_mapping()) {
    if (!pimpl->warned_no_device_grid) {
      pimpl->warned_no_device_grid = true;
      ocudulog::fetch_basic_logger("PHY").warning(
          "Channel emulator: resource grid has no device mapping (CUDA-visible grids required); "
          "emulation disabled.");
      enabled.store(false, std::memory_order_release);
    }
    return;
  }

  // One-time geometry sanity check against the real grid: a mismatch means
  // the configured nof_rb disagrees with the cell and the device view could
  // overrun the allocation.
  if (!pimpl->dims_checked) {
    const size_t actual = rd.get_nof_subc();
    if (actual != pimpl->nof_subc) {
      ocudulog::fetch_basic_logger("PHY").error(
          "Channel emulator: configured nof_rb gives {} subcarriers but the grid has {}; "
          "emulation disabled.",
          pimpl->nof_subc,
          actual);
      enabled.store(false, std::memory_order_release);
      return;
    }
    pimpl->dims_checked = true;
  }

  void* stream = pimpl->api->get_apply_stream(pimpl->inst, direction);

  // GB10-safe UL route: never launch a kernel that reads or writes the live
  // cudaMallocManaged VA. First migrate it home after its producer event,
  // CPU-copy into mapped pinned storage, H2D into an owned cudaMalloc, then
  // apply the channel in place to that owned allocation. The apply kernel also
  // writes the faded values into the mapped host twin. The resource-grid
  // reader advertises this completed snapshot to CUDA and host consumers.
  if (direction == OCUDU_CHANNEL_UPLINK) {
    // The whole-slot CUDA OFDM producer normally already published a safe
    // device-owned snapshot. Fade that copy directly. The host-stage fallback
    // remains for CPU lower-PHY or incomplete-slot producers.
    if (!rd.owned_device_snapshot_is_pusch_compatible() && !rd.stage_owned_snapshot_from_host()) {
      static std::atomic<bool> logged_ul_stage_skip{false};
      if (!logged_ul_stage_skip.exchange(true, std::memory_order_relaxed)) {
        ocudulog::fetch_basic_logger("PHY").warning(
            "Channel emulator: UL fade skipped because safe host-to-owned snapshot staging failed");
      }
      return;
    }
    void* owned = const_cast<void*>(rd.get_owned_device_snapshot_cbf16());
    if (owned == nullptr || !rd.prepare_owned_device_snapshot_reading(stream)) {
      return;
    }

    ocudu_channel_grid_view_v1_t view = {};
    view.struct_size                  = sizeof(view);
    view.format                       = OCUDU_CHANNEL_FORMAT_CBF16;
    view.nof_ports                    = pimpl->nof_ports;
    view.nof_symbols                  = end_symbol - first_symbol;
    view.nof_subcarriers              = pimpl->nof_subc;
    view.port_stride                  = size_t(pimpl->nof_symbols) * pimpl->nof_subc;
    view.symbol_stride                = pimpl->nof_subc;
    view.first_symbol                 = first_symbol;
    view.data                         = static_cast<uint32_t*>(owned) + size_t(first_symbol) * pimpl->nof_subc;
    view.snapshot_host                = rd.get_owned_host_mapped_device_ptr();
    if (view.snapshot_host != nullptr) {
      view.snapshot_host_port_stride   = view.port_stride;
      view.snapshot_host_symbol_stride = view.symbol_stride;
    }

    const uint64_t slot_count = pimpl->ul.unwrap(slot);
    if (pimpl->api->apply(pimpl->inst, &view, direction, slot_count, stream) != 0 ||
        !rd.complete_owned_device_snapshot(stream)) {
      rd.release_owned_device_snapshot_reading();
      return;
    }
    if (!rd.on_owned_device_snapshot_reading_enqueued(stream)) {
      // Fail closed: keep the snapshot held if its completion fence could not
      // be recorded. Reusing it while the plugin stream may still be active
      // would be unsafe; a CUDA context error requires cell recovery anyway.
      ocudulog::fetch_basic_logger("PHY").error(
          "Channel emulator: failed to fence UL owned-snapshot completion; snapshot reuse disabled");
    }
    static std::atomic<bool> logged_ul_owned{false};
    if (!logged_ul_owned.exchange(true, std::memory_order_relaxed)) {
      ocudulog::fetch_basic_logger("PHY").warning(
          "Channel emulator: UL fade armed on owned snapshot (live managed grid remains untouched)");
    }
    return;
  }

  // Fence: our stream waits for every producer of this grid...
  if (!rd.prepare_device_grid_reading(stream)) {
    return;
  }

  ocudu_channel_grid_view_v1_t view = {};
  view.struct_size                  = sizeof(view);
  view.format                       = OCUDU_CHANNEL_FORMAT_CBF16;
  view.nof_ports                    = pimpl->nof_ports;
  view.nof_symbols                  = end_symbol - first_symbol;
  view.nof_subcarriers              = pimpl->nof_subc;
  view.port_stride                  = size_t(pimpl->nof_symbols) * pimpl->nof_subc;
  view.symbol_stride                = pimpl->nof_subc;
  view.first_symbol                 = first_symbol;
  view.data = static_cast<uint32_t*>(wr.get_device_grid_bf16()) + size_t(first_symbol) * pimpl->nof_subc;

  impl::direction_state& st = (direction == OCUDU_CHANNEL_DOWNLINK) ? pimpl->dl : pimpl->ul;

  // UL: fade in-place AND store faded REs into an owned cudaMalloc from
  // registers. Class-C/Spectrum/ISAC read that snapshot. PUCCH reads a
  // pinned host copy. If the grid cannot fork (no snapshot / would home
  // live), skip UL fade — GPU-load of a later-homed live VA is Xid 31.
  if (direction == OCUDU_CHANNEL_UPLINK) {
    if (first_symbol == 0 || st.snap_dest == nullptr) {
      st.snap_dest        = rd.begin_owned_device_snapshot();
      st.snap_from_symbol = first_symbol;
    }
    if (st.snap_dest == nullptr) {
      static std::atomic<bool> logged_ul_skip{false};
      if (!logged_ul_skip.exchange(true, std::memory_order_relaxed)) {
        ocudulog::fetch_basic_logger("PHY").warning(
            "Channel emulator: UL fade skipped (no owned snapshot fork; live GPU-load after host-migrate is Xid 31)");
      }
      rd.release_device_grid_reading();
      return;
    }
    view.snapshot               = st.snap_dest;
    view.snapshot_port_stride   = view.port_stride;
    view.snapshot_symbol_stride = view.symbol_stride;
    view.snapshot_host          = rd.get_owned_host_mapped_device_ptr();
    if (view.snapshot_host != nullptr) {
      view.snapshot_host_port_stride   = view.port_stride;
      view.snapshot_host_symbol_stride = view.symbol_stride;
    }
    static std::atomic<bool> logged_ul_fade{false};
    if (!logged_ul_fade.exchange(true, std::memory_order_relaxed)) {
      ocudulog::fetch_basic_logger("PHY").warning(
          "Channel emulator: UL fade armed (owned device snapshot + pinned PUCCH copy)");
    }
  }

  if (pimpl->api->apply(pimpl->inst, &view, direction, st.unwrap(slot), stream) != 0) {
    rd.release_device_grid_reading();
    if (direction == OCUDU_CHANNEL_UPLINK) {
      st.snap_dest = nullptr;
    }
    return;
  }

  if (direction == OCUDU_CHANNEL_UPLINK && st.snap_dest != nullptr && end_symbol >= pimpl->nof_symbols) {
    (void)rd.complete_owned_device_snapshot(stream);
    st.snap_dest = nullptr;
  }

  rd.on_device_grid_reading_enqueued(stream);
}
