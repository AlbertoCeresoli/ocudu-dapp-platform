// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "dapp_service.h"
#include "dapp_process_package_factory.h"
#include "ocudu/dapp/abi/v1/module.h"
#include "ocudu/dapp/process/v1/shared_ring.h"
#include "ocudu/dapp/runtime/abi_validation.h"
#include "ocudu/dapp/runtime/class_c_process_supervisor.h"
#include "ocudu/dapp/runtime/incident.h"
#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/dapp/runtime/native_hook_registry.h"
#include "ocudu/dapp/runtime/package_manifest.h"
#include "ocudu/dapp/runtime/prepared_output.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include "ocudu/phy/upper/dapp/pusch_dapp_channel_estimator.h"
#include "ocudu/phy/upper/dapp/pusch_dapp_demodulator.h"
#include "ocudu/phy/upper/dapp/srs_dapp_channel_estimator.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_data_router.h"
#include "ocudu/ran/du_cell_index.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/ran/srs/srs_information.h"
#include "ocudu/ran/srs/srs_resource_configuration.h"
#include "ocudu/scheduler/dapp/scheduler_dapp_context.h"
#include "ocudu/scheduler/dapp/scheduler_dapp_policy.h"
#include "ocudu/scheduler/result/sched_result.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <sys/resource.h>
#include <utility>

#ifdef OCUDU_DAPP_APP_SERVICE_E3
#include "ocudu/dapp/e3/e3_connection_registry.h"
#include "ocudu/dapp/e3/embedded_agent.h"
#include "ocudu/dapp/e3/management_service.h"
#include "ocudu/dapp/e3/portable_data_store.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_portable_e3_target.h"
#ifdef OCUDU_DAPP_APP_SERVICE_SCTP_E3
#include "ocudu/dapp/e3/e3ap_sctp_server.h"
#include "ocudu/dapp/e3/telemetry_publisher.h"
#include "ocudu/e2/e2sm/e2sm_dapp.h" // [srs-sharing]
#ifdef OCUDU_DAPP_E3DP_CARRIER
#include "ocudu/dapp/e3/e3dp_sctp_server.h"
#endif
#endif
#endif

#ifdef OCUDU_DAPP_APP_SERVICE_LOCAL_E3
#include "ocudu/dapp/e3/local_connector.h"
#endif

using namespace ocudu;
using namespace ocudu::app_services;

namespace {

uint64_t monotonic_ns(void*) noexcept
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool same_radio_time(const ocudu_dapp_radio_time_v1& left, const ocudu_dapp_radio_time_v1& right) noexcept
{
  return left.sfn == right.sfn && left.slot == right.slot && left.numerology == right.numerology &&
         left.reserved == right.reserved;
}

bool all_zero(const uint64_t* values, std::size_t count) noexcept
{
  return std::all_of(values, values + count, [](uint64_t value) { return value == 0; });
}

struct normalized_interference_map {
  ocudu_dapp_context_header_v1                         context{};
  std::array<uint8_t, OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1> confidence{};
  uint32_t                                             nof_prbs{};
};

static_assert(sizeof(normalized_interference_map) <= dapp::prepared_output_transaction::maximum_record_bytes);
static_assert(OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 >= MAX_NOF_PRBS);
static_assert(sizeof(ocudu_dapp_srs_isac_cir_preview_v1) <= dapp::prepared_output_transaction::maximum_record_bytes);

class bounded_host_incident_queue
{
public:
  bool try_push(const dapp::incident_record& record) noexcept
  {
    if (lock.test_and_set(std::memory_order_acquire)) {
      dropped.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    bool stored = false;
    if (count != records.size()) {
      records[count++] = record;
      stored           = true;
    } else {
      dropped.fetch_add(1, std::memory_order_relaxed);
    }
    lock.clear(std::memory_order_release);
    return stored;
  }

  std::size_t drain(dapp::incident_store& destination)
  {
    std::array<dapp::incident_record, 64> snapshot;
    uint32_t                              snapshot_count = 0;
    if (!lock.test_and_set(std::memory_order_acquire)) {
      snapshot_count = count;
      std::copy_n(records.begin(), count, snapshot.begin());
      count = 0;
      lock.clear(std::memory_order_release);
    }
    for (uint32_t index = 0; index != snapshot_count; ++index) {
      destination.ingest(snapshot[index]);
    }
    destination.account_source_drops(dropped.exchange(0, std::memory_order_acq_rel));
    return snapshot_count;
  }

private:
  std::atomic_flag                      lock = ATOMIC_FLAG_INIT;
  std::array<dapp::incident_record, 64> records;
  uint32_t                              count{};
  std::atomic<uint64_t>                 dropped{};
};

const char* quiet_rejection_detail(uint16_t reason, quiet_reservation_constraint constraint) noexcept
{
  switch (reason) {
    case OCUDU_DAPP_QUIET_REASON_BUDGET_V1:
      if (constraint == quiet_reservation_constraint::source_budget) {
        return "quiet-period intent rejected by the per-source rolling duty budget";
      }
      return "quiet-period intent rejected by host duty or retained-volume budget";
    case OCUDU_DAPP_QUIET_REASON_CONFLICT_V1:
      return "quiet-period intent rejected by an existing host reservation conflict";
    default:
      return "quiet-period intent rejected by host policy";
  }
}

struct prepared_output_broker final : dapp::native_hook_binding_observer, dapp::class_c_process_result_sink {
  prepared_output_broker(std::shared_ptr<scheduler_dapp_interference_cache> cache_,
                         std::shared_ptr<quiet_reservation_manager>         reservations_,
                         const std::vector<dapp_service_cell_config>&       cells_,
                         std::shared_ptr<dapp_srs_isac_report_sink>         srs_sink_) :
    cache(std::move(cache_)), reservations(std::move(reservations_)), srs_sink(std::move(srs_sink_))
  {
    for (const auto& cell : cells_) {
      cells[cell.cell_id] = true;
    }
  }

  ocudu_dapp_status_v1 activated(const dapp::management::identifier&                    instance_id,
                                 const std::vector<dapp::management::interface_record>& interfaces) noexcept override
  {
    if (!has_result_authority(interfaces)) {
      return OCUDU_DAPP_OK_V1;
    }
    const auto identity = identity_words(instance_id);
    if (!claim_authority(identity)) {
      return OCUDU_DAPP_CONFLICT_V1;
    }
    const auto status = invalidate_spectrum();
    if (status != OCUDU_DAPP_OK_V1) {
      release_authority(identity);
    }
    return status;
  }

  ocudu_dapp_status_v1 deactivated(const dapp::management::identifier&                    instance_id,
                                   const std::vector<dapp::management::interface_record>& interfaces) noexcept override
  {
    if (!has_result_authority(interfaces)) {
      return OCUDU_DAPP_OK_V1;
    }
    const auto identity = identity_words(instance_id);
    release_authority(identity);
    return invalidate_spectrum(identity);
  }

  ocudu_dapp_status_v1 publish(const dapp::class_c_process_result_source&   source,
                               const ocudu_dapp_process_spectrum_result_v1& result) override;
  ocudu_dapp_status_v1 publish(const dapp::class_c_process_result_source&       source,
                               const ocudu_dapp_process_quiet_intent_result_v1& result) override;
  ocudu_dapp_status_v1 publish(const dapp::class_c_process_result_source&   source,
                               const ocudu_dapp_process_srs_isac_result_v1& result) override;

  ocudu_dapp_status_v1 publication_changed(const dapp::class_c_process_result_source& source, bool active) override;

  void record_quiet_rejection(dapp::incident_context       context,
                              uint16_t                     reason,
                              quiet_reservation_constraint constraint) noexcept
  {
    context.phase   = dapp::incident_phase::report;
    context.hook_id = OCUDU_DAPP_INTERFACE_SPECTRUM_V1;
    host_incidents.try_push(dapp::make_incident(context,
                                                dapp::incident_cause::policy_rejection,
                                                dapp::incident_action::observed,
                                                OCUDU_DAPP_CONFLICT_V1,
                                                quiet_rejection_detail(reason, constraint),
                                                false));
  }

  std::size_t drain_host_incidents(dapp::incident_store& destination) { return host_incidents.drain(destination); }

  static bool has_result_authority(const std::vector<dapp::management::interface_record>& interfaces) noexcept
  {
    return std::any_of(interfaces.begin(), interfaces.end(), [](const auto& interface) {
      return interface.interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 &&
             (interface.capability_bits &
              (OCUDU_DAPP_SPECTRUM_CAP_INTERFERENCE_MAP_V1 | OCUDU_DAPP_SPECTRUM_CAP_QUIET_PERIOD_INTENT_V1)) != 0;
    });
  }

  static std::array<uint64_t, 2> identity_words(const dapp::management::identifier& identity) noexcept
  {
    std::array<uint64_t, 2> result{};
    std::memcpy(result.data(), identity.bytes.data(), identity.bytes.size());
    return result;
  }

  bool claim_authority(const std::array<uint64_t, 2>& identity) noexcept
  {
    if (identity[0] == 0 && identity[1] == 0) {
      return false;
    }
    std::lock_guard<std::mutex> lock(authority_mutex);
    if (spectrum_authority != std::array<uint64_t, 2>{} && spectrum_authority != identity) {
      return false;
    }
    spectrum_authority = identity;
    return true;
  }

  void release_authority(const std::array<uint64_t, 2>& identity) noexcept
  {
    std::lock_guard<std::mutex> lock(authority_mutex);
    if (spectrum_authority == identity) {
      spectrum_authority = {};
    }
  }

  bool owns_authority(const uint64_t identity[2]) noexcept
  {
    std::lock_guard<std::mutex> lock(authority_mutex);
    return spectrum_authority[0] == identity[0] && spectrum_authority[1] == identity[1] &&
           spectrum_authority != std::array<uint64_t, 2>{};
  }

  uint64_t next_sequence(uint32_t cell_id) noexcept
  {
    uint64_t sequence = cell_sequences[cell_id].fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence == 0) {
      sequence = cell_sequences[cell_id].fetch_add(1, std::memory_order_relaxed) + 1;
    }
    return sequence;
  }

  uint64_t next_srs_sequence(uint32_t cell_id) noexcept
  {
    uint64_t sequence = srs_cell_sequences[cell_id].fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence == 0) {
      sequence = srs_cell_sequences[cell_id].fetch_add(1, std::memory_order_relaxed) + 1;
    }
    return sequence;
  }

  ocudu_dapp_status_v1 invalidate_spectrum(std::array<uint64_t, 2> identity = {}) noexcept
  {
    for (uint32_t cell_id = 0; cell_id != cells.size(); ++cell_id) {
      if (cells[cell_id] && cache->invalidate(cell_id) != scheduler_dapp_context_result::accepted) {
        return OCUDU_DAPP_CONFLICT_V1;
      }
    }
    if (identity != std::array<uint64_t, 2>{} && reservations) {
      const auto result = reservations->invalidate_source(identity, 0);
      if (result == quiet_reservation_result::busy || result == quiet_reservation_result::invalid) {
        return OCUDU_DAPP_CONFLICT_V1;
      }
    }
    return OCUDU_DAPP_OK_V1;
  }

  std::shared_ptr<scheduler_dapp_interference_cache>  cache;
  std::shared_ptr<quiet_reservation_manager>          reservations;
  std::shared_ptr<dapp_srs_isac_report_sink>          srs_sink;
  std::array<bool, MAX_NOF_DU_CELLS>                  cells{};
  std::array<std::atomic<uint64_t>, MAX_NOF_DU_CELLS> cell_sequences{};
  std::array<std::atomic<uint64_t>, MAX_NOF_DU_CELLS> srs_cell_sequences{};
  std::atomic<uint64_t>                               accepted{};
  std::atomic<uint64_t>                               busy{};
  std::atomic<uint64_t>                               invalid{};
  bounded_host_incident_queue                         host_incidents;
  std::mutex                                          authority_mutex;
  std::array<uint64_t, 2>                             spectrum_authority{};
};

class ul_plan_broker final : public scheduler_dapp_ul_plan_sink
{
  struct cell_state {
    std::atomic_flag                                                                     publishing = ATOMIC_FLAG_INIT;
    uint16_t                                                                             nof_prbs{};
    uint64_t                                                                             generation{};
    std::array<ocudu_dapp_ul_resource_span_v1, OCUDU_DAPP_UL_RESOURCE_PLAN_MAX_SPANS_V1> spans{};
    std::vector<std::shared_ptr<upper_phy_dapp_publication_target>>                      native_targets;
  };

public:
  explicit ul_plan_broker(const std::vector<dapp_service_cell_config>& cells_)
  {
    for (const auto& cell : cells_) {
      cells[cell.cell_id].nof_prbs = cell.nof_ul_prbs;
    }
  }

  void add_native(uint32_t cell_id, std::shared_ptr<upper_phy_dapp_publication_target> target)
  {
    cells[cell_id].native_targets.push_back(std::move(target));
  }

  void add_process(std::shared_ptr<dapp_process_package_factory> factory)
  {
    process_factories.push_back(std::move(factory));
  }

  void publish(uint32_t cell_id, slot_point slot, const ul_sched_result& result) noexcept override
  {
    if (cell_id >= cells.size() || cells[cell_id].nof_prbs == 0 || !slot.valid()) {
      return;
    }
    cell_state& cell = cells[cell_id];
    if (cell.publishing.test_and_set(std::memory_order_acquire)) {
      return;
    }
    struct release_flag {
      std::atomic_flag& flag;
      ~release_flag() { flag.clear(std::memory_order_release); }
    } release{cell.publishing};

    uint32_t count      = 0;
    uint32_t allocation = 1;
    bool     complete   = result.nof_ul_symbols <= 14 && result.prachs.empty();
    auto     append     = [&](uint8_t  symbol_start,
                      uint8_t  symbol_count,
                      uint16_t first_prb,
                      uint16_t nof_prbs,
                      uint8_t  category,
                      uint16_t dmrs = 0) {
      if (symbol_count == 0 || symbol_start + symbol_count > 14 || nof_prbs == 0 ||
          static_cast<uint32_t>(first_prb) + nof_prbs > cell.nof_prbs || count == cell.spans.size()) {
        complete = false;
        return;
      }
      auto& span            = cell.spans[count++];
      span                  = {};
      span.struct_size      = sizeof(span);
      span.symbol_start     = symbol_start;
      span.symbol_count     = symbol_count;
      span.first_prb        = first_prb;
      span.nof_prbs         = nof_prbs;
      span.use_category     = category;
      span.dmrs_symbol_mask = dmrs;
      span.allocation_id    = allocation++;
    };

    for (const auto& grant : result.puschs) {
      const auto& pusch = grant.pusch_cfg;
      if (pusch.bwp_cfg == nullptr || !pusch.rbs.is_type1()) {
        complete = false;
        continue;
      }
      const auto vrbs = pusch.rbs.type1();
      append(static_cast<uint8_t>(pusch.symbols.start()),
             static_cast<uint8_t>(pusch.symbols.length()),
             static_cast<uint16_t>(pusch.bwp_cfg->crbs.start() + vrbs.start()),
             static_cast<uint16_t>(vrbs.length()),
             OCUDU_DAPP_UL_USE_PUSCH_V1,
             static_cast<uint16_t>(pusch.dmrs.dmrs_symb_pos.to_uint64()));
    }
    for (const auto& grant : result.pucchs) {
      if (grant.bwp_cfg == nullptr || grant.res == nullptr) {
        complete = false;
        continue;
      }
      const auto prbs          = grant.grant_prbs();
      const auto symbols       = grant.res->syms;
      const auto bwp_start     = grant.bwp_cfg->crbs.start();
      const auto second_symbol = symbols.start() + symbols.length() / 2;
      if (grant.res->second_hop_prb && symbols.length() > 1) {
        append(static_cast<uint8_t>(symbols.start()),
               static_cast<uint8_t>(second_symbol - symbols.start()),
               static_cast<uint16_t>(bwp_start + prbs.start()),
               static_cast<uint16_t>(prbs.length()),
               OCUDU_DAPP_UL_USE_PUCCH_V1);
        append(static_cast<uint8_t>(second_symbol),
               static_cast<uint8_t>(symbols.stop() - second_symbol),
               static_cast<uint16_t>(bwp_start + *grant.res->second_hop_prb),
               static_cast<uint16_t>(prbs.length()),
               OCUDU_DAPP_UL_USE_PUCCH_V1);
      } else {
        append(static_cast<uint8_t>(symbols.start()),
               static_cast<uint8_t>(symbols.length()),
               static_cast<uint16_t>(bwp_start + prbs.start()),
               static_cast<uint16_t>(prbs.length()),
               OCUDU_DAPP_UL_USE_PUCCH_V1);
      }
    }
    for (const auto& grant : result.srss) {
      if (grant.bwp_cfg == nullptr || grant.group_or_seq_hopping != srs_group_or_sequence_hopping::neither ||
          grant.freq_hopping < grant.bw_index) {
        complete = false;
        continue;
      }
      srs_resource_configuration resource{};
      resource.nof_antenna_ports = static_cast<srs_resource_configuration::one_two_four_enum>(grant.nof_antenna_ports);
      resource.nof_symbols       = static_cast<srs_nof_symbols>(grant.symbols.length());
      resource.start_symbol      = grant.symbols.start();
      resource.configuration_index = grant.config_index;
      resource.sequence_id         = grant.sequence_id;
      resource.bandwidth_index     = grant.bw_index;
      resource.comb_size           = grant.tx_comb;
      resource.comb_offset         = grant.comb_offset;
      resource.cyclic_shift        = grant.cyclic_shift;
      resource.freq_position       = grant.freq_position;
      resource.freq_shift          = grant.freq_shift;
      resource.freq_hopping        = grant.freq_hopping;
      resource.hopping             = grant.group_or_seq_hopping;
      const auto info              = get_srs_information(resource, 0);
      const auto first_prb         = info.mapping_initial_subcarrier / NOF_SUBCARRIERS_PER_RB;
      const auto last_subcarrier   = info.mapping_initial_subcarrier +
                                   (info.sequence_length == 0 ? 0 : (info.sequence_length - 1) * info.comb_size);
      append(static_cast<uint8_t>(grant.symbols.start()),
             static_cast<uint8_t>(grant.symbols.length()),
             static_cast<uint16_t>(grant.bwp_cfg->crbs.start() + first_prb),
             static_cast<uint16_t>(last_subcarrier / NOF_SUBCARRIERS_PER_RB - first_prb + 1),
             OCUDU_DAPP_UL_USE_SRS_V1);
    }

    if (++cell.generation == 0) {
      ++cell.generation;
    }
    const ocudu_dapp_radio_time_v1 radio_time{static_cast<uint16_t>(slot.sfn()),
                                              static_cast<uint16_t>(slot.slot_index()),
                                              static_cast<uint16_t>(slot.numerology()),
                                              0};
    ocudu_dapp_ul_resource_plan_v1 plan{};
    plan.struct_size                     = sizeof(plan);
    plan.abi_major                       = OCUDU_DAPP_ABI_MAJOR_V1;
    plan.abi_minor                       = OCUDU_DAPP_ABI_MINOR_V1;
    plan.context.struct_size             = sizeof(plan.context);
    plan.context.schema_id               = OCUDU_DAPP_CONTEXT_UL_RESOURCE_PLAN_V1;
    plan.context.schema_version          = 1;
    plan.context.producer_instance_id[0] = UINT64_C(0x4f43554455000002);
    plan.context.producer_instance_id[1] = 1;
    plan.context.producer_generation     = cell.generation;
    plan.context.source_sequence         = cell.generation;
    plan.context.cell_id                 = cell_id;
    plan.context.confidence_q16          = 1U << 16U;
    plan.context.created                 = radio_time;
    plan.context.valid_from              = radio_time;
    plan.context.valid_until             = radio_time;
    plan.planned_time                    = radio_time;
    plan.plan_generation                 = cell.generation;
    plan.spans                           = cell.spans.data();
    plan.nof_spans                       = count;
    plan.flags                           = complete ? OCUDU_DAPP_UL_RESOURCE_PLAN_COMPLETE_V1 : 0;
    plan.cell_nof_prbs                   = cell.nof_prbs;
    for (const auto& target : cell.native_targets) {
      target->publish(plan);
    }
    for (const auto& factory : process_factories) {
      factory->publish(plan);
    }
  }

private:
  std::array<cell_state, MAX_NOF_DU_CELLS>                   cells;
  std::vector<std::shared_ptr<dapp_process_package_factory>> process_factories;
};

// [srs-sharing] Glue between the MAC-side UE events and the E3 telemetry publisher (not in upstream). The bridge
// exists before the publisher: the hook topology, and with it the scheduler, is built first, the E3 server later. The
// publisher pointer is therefore attached afterwards and read atomically. Events before attach() are dropped; none
// occur in practice, since the service is created during gNB start-up, before any UE can attach.
// Converting to the packed srs_config_metadata_v1 happens here so that the scheduler library stays free of E3 types.
/// Forwards UE-context SRS configurations and timing advance commands from the scheduler hooks to the E3 telemetry
/// publisher, once one is attached (the publisher is created after the hook topology).
class ue_event_bridge final : public scheduler_dapp_ue_event_sink
{
public:
#ifdef OCUDU_DAPP_APP_SERVICE_SCTP_E3
  void attach(std::shared_ptr<dapp::e3::telemetry_publisher> publisher_)
  {
    holder = std::move(publisher_);
    publisher.store(holder.get(), std::memory_order_release);
  }
#endif

  void publish_srs_config(const scheduler_dapp_srs_config_event& event) noexcept override
  {
#ifdef OCUDU_DAPP_APP_SERVICE_SCTP_E3
    auto* target = publisher.load(std::memory_order_acquire);
    if (target == nullptr) {
      return;
    }
    dapp::e3::srs_config_metadata_v1 record;
    record.rnti  = event.rnti;
    record.event = static_cast<uint8_t>(event.event);
    record.flags =
        static_cast<uint8_t>((event.srs_configured ? 0x01U : 0U) | (event.srs_configured && event.derived_valid ? 0x02U : 0U));
    record.ue_index                  = event.ue_index;
    record.numerology                = event.numerology;
    record.n_ta_offset               = event.n_ta_offset;
    record.bwp_start_rb              = event.bwp_start_rb;
    record.bwp_size_rb               = event.bwp_size_rb;
    if (event.srs_configured) {
      record.resource_id               = event.resource_id;
      record.nof_ports                 = event.nof_ports;
      record.comb_size                 = event.comb_size;
      record.comb_offset               = event.comb_offset;
      record.cyclic_shift              = event.cyclic_shift;
      record.start_position            = event.start_position;
      record.nof_symbols               = event.nof_symbols;
      record.repetition_factor         = event.repetition_factor;
      record.freq_domain_position      = event.freq_domain_position;
      record.c_srs                     = event.c_srs;
      record.b_srs                     = event.b_srs;
      record.b_hop                     = event.b_hop;
      record.group_or_sequence_hopping = event.group_or_sequence_hopping;
      record.freq_domain_shift         = event.freq_domain_shift;
      record.sequence_id               = event.sequence_id;
      record.period_slots              = event.period_slots;
      record.offset_slots              = event.offset_slots;
      record.start_symbol              = event.start_symbol;
    }
    if (event.srs_configured && event.derived_valid) {
      record.cyclic_shift_maximum       = event.cyclic_shift_maximum;
      record.sequence_length            = event.sequence_length;
      record.mapping_initial_subcarrier = event.mapping_initial_subcarrier;
      record.sequence_group             = event.sequence_group;
      record.sequence_number            = event.sequence_number;
    }
    target->publish_srs_config(static_cast<uint16_t>(event.cell_id), record);
#endif
  }

  // Called by the MAC once per cell and slot. Besides gating the TA scan, it is the per-slot tick that drives the
  // srs.config snapshot replay, so no extra timer or thread is needed for it.
  bool ta_commands_active() noexcept override
  {
#ifdef OCUDU_DAPP_APP_SERVICE_SCTP_E3
    auto* target = publisher.load(std::memory_order_acquire);
    if (target == nullptr) {
      return false;
    }
    target->poll_srs_config_snapshot();
    return target->ta_command_active();
#else
    return false;
#endif
  }

  void publish_ta_command(slot_point slot, const scheduler_dapp_ta_command_event& event) noexcept override
  {
#ifdef OCUDU_DAPP_APP_SERVICE_SCTP_E3
    auto* target = publisher.load(std::memory_order_acquire);
    if (target == nullptr) {
      return;
    }
    dapp::e3::ta_command_metadata_v1 record;
    record.rnti = event.rnti;
    if (slot.valid()) {
      record.radio_slot.sfn        = static_cast<uint16_t>(slot.sfn());
      record.radio_slot.slot       = static_cast<uint8_t>(slot.slot_index());
      record.radio_slot.numerology = static_cast<uint8_t>(slot.numerology());
    }
    record.source     = static_cast<uint8_t>(event.source);
    record.tag_id     = event.tag_id;
    record.ta_command = event.ta_command;
    target->publish_ta_command(static_cast<uint16_t>(event.cell_id), record);
#endif
  }

private:
#ifdef OCUDU_DAPP_APP_SERVICE_SCTP_E3
  std::shared_ptr<dapp::e3::telemetry_publisher> holder;
  std::atomic<dapp::e3::telemetry_publisher*>    publisher{nullptr};
#endif
};

bool valid_srs_preview(const ocudu_dapp_srs_isac_cir_preview_v1& output,
                       uint64_t                                  expected_source_sequence,
                       uint32_t                                  expected_resource_id,
                       uint32_t                                  expected_config_id,
                       uint32_t                                  expected_cell_id,
                       const ocudu_dapp_radio_time_v1&           expected_created) noexcept
{
  constexpr uint32_t object_minimum  = offsetof(ocudu_dapp_srs_isac_cir_preview_v1, reserved);
  constexpr uint32_t context_minimum = offsetof(ocudu_dapp_context_header_v1, reserved);
  const auto&        context         = output.context;
  if (output.struct_size < object_minimum || output.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      context.struct_size < context_minimum || context.schema_id != OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1 ||
      context.schema_version != 1 || context.flags != 0 || context.producer_instance_id[0] != 0 ||
      context.producer_instance_id[1] != 0 || context.producer_generation != 0 ||
      context.source_sequence != expected_source_sequence || context.cell_id != expected_cell_id ||
      context.confidence_q16 > (1U << 16U) || !same_radio_time(context.created, expected_created) ||
      !same_radio_time(context.valid_from, expected_created) ||
      !same_radio_time(context.valid_until, expected_created) || context.provenance_hash != 0 ||
      !all_zero(context.reserved, std::size(context.reserved)) ||
      output.source_srs_sequence != expected_source_sequence || output.srs_resource_id != expected_resource_id ||
      output.srs_config_id != expected_config_id || output.tap_count == 0 ||
      output.tap_count > OCUDU_DAPP_SRS_ISAC_MAX_CIR_TAPS_V1 || output.peak_delay_bin >= output.tap_count ||
      !std::isfinite(output.mean_channel_power) || output.mean_channel_power < 0.0F ||
      !std::isfinite(output.noise_corrected_power) || output.noise_corrected_power < 0.0F ||
      !std::isfinite(output.peak_cir_power) || output.peak_cir_power < 0.0F || !std::isfinite(output.noise_variance) ||
      output.noise_variance < 0.0F || !all_zero(output.reserved, std::size(output.reserved))) {
    return false;
  }
  for (uint32_t tap = 0; tap != output.tap_count; ++tap) {
    if (!std::isfinite(output.tap_power[tap]) || output.tap_power[tap] < 0.0F ||
        output.tap_power[tap] > output.peak_cir_power) {
      return false;
    }
  }
  return output.tap_power[output.peak_delay_bin] == output.peak_cir_power &&
         std::all_of(output.tap_power + output.tap_count,
                     output.tap_power + OCUDU_DAPP_SRS_ISAC_MAX_CIR_TAPS_V1,
                     [](float value) { return value == 0.0F; });
}

ocudu_dapp_srs_isac_cir_preview_v1 normalize_srs_preview(prepared_output_broker&                   broker,
                                                         const ocudu_dapp_srs_isac_cir_preview_v1& output,
                                                         const uint64_t                            instance_id[2],
                                                         uint64_t app_generation) noexcept
{
  auto normalized                            = output;
  normalized.struct_size                     = sizeof(normalized);
  normalized.context.struct_size             = sizeof(normalized.context);
  normalized.context.producer_instance_id[0] = instance_id[0];
  normalized.context.producer_instance_id[1] = instance_id[1];
  normalized.context.producer_generation     = app_generation;
  normalized.context.source_sequence         = broker.next_srs_sequence(output.context.cell_id);
  return normalized;
}

ocudu_dapp_status_v1 publish_srs_report(dapp_srs_isac_report_sink&                sink,
                                        const ocudu_dapp_srs_isac_cir_preview_v1& report) noexcept
{
  try {
    return sink.publish(report);
  } catch (...) {
    return OCUDU_DAPP_EXCEPTION_V1;
  }
}

ocudu_dapp_status_v1 commit_interference_map(void*       destination,
                                             const void* normalized_object,
                                             uint32_t    object_size,
                                             const dapp::prepared_output_source&) noexcept
{
  auto* broker = static_cast<prepared_output_broker*>(destination);
  if (broker == nullptr || broker->cache == nullptr || normalized_object == nullptr ||
      object_size != sizeof(normalized_interference_map)) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  const auto&                        map = *static_cast<const normalized_interference_map*>(normalized_object);
  scheduler_dapp_interference_update update{};
  update.context              = map.context;
  update.prb_confidence       = {map.confidence.data(), map.nof_prbs};
  update.publish_monotonic_ns = monotonic_ns(nullptr);
  switch (broker->cache->publish(update)) {
    case scheduler_dapp_context_result::accepted:
      broker->accepted.fetch_add(1, std::memory_order_relaxed);
      return OCUDU_DAPP_OK_V1;
    case scheduler_dapp_context_result::busy:
      broker->busy.fetch_add(1, std::memory_order_relaxed);
      return OCUDU_DAPP_BYPASS_V1;
    case scheduler_dapp_context_result::invalid:
      broker->invalid.fetch_add(1, std::memory_order_relaxed);
      return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  return OCUDU_DAPP_INTERNAL_ERROR_V1;
}

ocudu_dapp_status_v1 prepared_output_broker::publication_changed(const dapp::class_c_process_result_source& source,
                                                                 bool                                       active)
{
  if (source.interface_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1) {
    return OCUDU_DAPP_OK_V1;
  }
  const std::array<uint64_t, 2> identity{source.instance_id[0], source.instance_id[1]};
  if (active && !claim_authority(identity)) {
    return OCUDU_DAPP_CONFLICT_V1;
  }
  if (!active) {
    release_authority(identity);
  }
  const auto status = invalidate_spectrum(active ? std::array<uint64_t, 2>{} : identity);
  if (active && status != OCUDU_DAPP_OK_V1) {
    release_authority(identity);
  }
  return status;
}

ocudu_dapp_status_v1 prepared_output_broker::publish(const dapp::class_c_process_result_source&   source,
                                                     const ocudu_dapp_process_spectrum_result_v1& result)
{
  if (cache == nullptr || source.interface_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 || source.route_generation == 0 ||
      source.route_id == 0 || source.app_generation == 0 ||
      (source.instance_id[0] == 0 && source.instance_id[1] == 0) || !owns_authority(source.instance_id) ||
      result.struct_size != sizeof(result) || result.schema_id != OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1 ||
      result.route_id != source.route_id || result.source_grid_sequence == 0 || result.cell_id >= cells.size() ||
      !cells[result.cell_id] || result.confidence_q16 > (1U << 16U) || result.first_prb != 0 || result.nof_prbs == 0 ||
      result.nof_prbs > MAX_NOF_PRBS || result.nof_prbs > OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 ||
      std::any_of(std::begin(result.reserved0), std::end(result.reserved0), [](uint8_t value) { return value != 0; }) ||
      !all_zero(result.reserved, std::size(result.reserved))) {
    invalid.fetch_add(1, std::memory_order_relaxed);
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }

  normalized_interference_map normalized{};
  normalized.context.struct_size             = sizeof(normalized.context);
  normalized.context.schema_id               = OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1;
  normalized.context.schema_version          = 1;
  normalized.context.producer_instance_id[0] = source.instance_id[0];
  normalized.context.producer_instance_id[1] = source.instance_id[1];
  normalized.context.producer_generation     = source.app_generation;
  normalized.context.source_sequence         = next_sequence(result.cell_id);
  normalized.context.cell_id                 = result.cell_id;
  normalized.context.confidence_q16          = result.confidence_q16;
  normalized.context.created                 = result.created;
  normalized.context.valid_from              = result.valid_from;
  normalized.context.valid_until             = result.valid_until;
  normalized.nof_prbs                        = result.nof_prbs;
  std::copy_n(result.prb_confidence, result.nof_prbs, normalized.confidence.begin());
  return commit_interference_map(this, &normalized, sizeof(normalized), {});
}

ocudu_dapp_status_v1 prepared_output_broker::publish(const dapp::class_c_process_result_source&       source,
                                                     const ocudu_dapp_process_quiet_intent_result_v1& result)
{
  if (reservations == nullptr || source.interface_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 ||
      source.route_generation == 0 || source.route_id == 0 || source.app_generation == 0 ||
      (source.instance_id[0] == 0 && source.instance_id[1] == 0) || !owns_authority(source.instance_id) ||
      result.struct_size != sizeof(result) || result.schema_id != OCUDU_DAPP_SPECTRUM_OUTPUT_QUIET_PERIOD_INTENT_V1 ||
      result.route_id != source.route_id || result.source_grid_sequence == 0 || result.cell_id >= cells.size() ||
      !cells[result.cell_id] || result.reserved0 != 0 || !all_zero(result.reserved, std::size(result.reserved))) {
    invalid.fetch_add(1, std::memory_order_relaxed);
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  quiet_reservation_request request{};
  request.intent               = result.intent;
  request.source_instance_id   = {source.instance_id[0], source.instance_id[1]};
  request.source_generation    = source.app_generation;
  request.source_sequence      = result.source_grid_sequence;
  request.cell_id              = result.cell_id;
  request.observed_slot        = slot_point{result.created.numerology, result.created.sfn, result.created.slot};
  request.publish_monotonic_ns = monotonic_ns(nullptr);
  const auto decision          = reservations->submit(request);
  switch (decision.result) {
    case quiet_reservation_result::accepted:
    case quiet_reservation_result::clamped:
      accepted.fetch_add(1, std::memory_order_relaxed);
      return OCUDU_DAPP_OK_V1;
    case quiet_reservation_result::busy:
      busy.fetch_add(1, std::memory_order_relaxed);
      return OCUDU_DAPP_BYPASS_V1;
    case quiet_reservation_result::rejected: {
      dapp::incident_context context{};
      context.instance_id[0]      = source.instance_id[0];
      context.instance_id[1]      = source.instance_id[1];
      context.app_generation      = source.app_generation;
      context.invocation_sequence = result.source_grid_sequence;
      context.cell_id             = result.cell_id;
      record_quiet_rejection(context, decision.reservation.reason, decision.constraint);
      return OCUDU_DAPP_BYPASS_V1;
    }
    case quiet_reservation_result::invalid:
      invalid.fetch_add(1, std::memory_order_relaxed);
      return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  return OCUDU_DAPP_INTERNAL_ERROR_V1;
}

ocudu_dapp_status_v1 prepared_output_broker::publish(const dapp::class_c_process_result_source&   source,
                                                     const ocudu_dapp_process_srs_isac_result_v1& result)
{
  const auto& output = result.preview;
  if (source.interface_id != OCUDU_DAPP_INTERFACE_SRS_ISAC_V1 || source.route_generation == 0 || source.route_id == 0 ||
      source.app_generation == 0 || (source.instance_id[0] == 0 && source.instance_id[1] == 0) ||
      result.route_id != source.route_id || result.struct_size != sizeof(result) ||
      result.schema_id != OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1 || output.context.cell_id >= cells.size() ||
      !cells[output.context.cell_id] || !all_zero(result.reserved, std::size(result.reserved)) ||
      !valid_srs_preview(output,
                         output.source_srs_sequence,
                         output.srs_resource_id,
                         output.srs_config_id,
                         output.context.cell_id,
                         output.context.created)) {
    invalid.fetch_add(1, std::memory_order_relaxed);
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  if (srs_sink == nullptr) {
    return OCUDU_DAPP_BYPASS_V1;
  }
  const auto normalized = normalize_srs_preview(*this, output, source.instance_id, source.app_generation);
  return publish_srs_report(*srs_sink, normalized);
}

ocudu_dapp_status_v1 commit_srs_preview(void*       destination,
                                        const void* normalized_object,
                                        uint32_t    object_size,
                                        const dapp::prepared_output_source&) noexcept
{
  auto* broker = static_cast<prepared_output_broker*>(destination);
  if (broker == nullptr || broker->srs_sink == nullptr || normalized_object == nullptr ||
      object_size != sizeof(ocudu_dapp_srs_isac_cir_preview_v1)) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  return publish_srs_report(*broker->srs_sink,
                            *static_cast<const ocudu_dapp_srs_isac_cir_preview_v1*>(normalized_object));
}

ocudu_dapp_status_v1 commit_quiet_intent(void*                               destination,
                                         const void*                         normalized_object,
                                         uint32_t                            object_size,
                                         const dapp::prepared_output_source& source) noexcept
{
  auto* broker = static_cast<prepared_output_broker*>(destination);
  if (broker == nullptr || broker->reservations == nullptr || normalized_object == nullptr ||
      object_size != sizeof(ocudu_dapp_quiet_period_intent_v1)) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  const auto&               intent = *static_cast<const ocudu_dapp_quiet_period_intent_v1*>(normalized_object);
  quiet_reservation_request request{};
  request.intent             = intent;
  request.source_instance_id = {source.identity.instance_id[0], source.identity.instance_id[1]};
  request.source_generation  = source.identity.app_generation;
  request.source_sequence    = source.invocation.input_generation;
  request.cell_id            = source.invocation.cell_id;
  request.observed_slot      = slot_point{
      source.invocation.radio_time.numerology, source.invocation.radio_time.sfn, source.invocation.radio_time.slot};
  request.publish_monotonic_ns = monotonic_ns(nullptr);
  const auto decision          = broker->reservations->submit(request);
  switch (decision.result) {
    case quiet_reservation_result::accepted:
    case quiet_reservation_result::clamped:
      return OCUDU_DAPP_OK_V1;
    case quiet_reservation_result::busy:
      return OCUDU_DAPP_BYPASS_V1;
    case quiet_reservation_result::rejected: {
      auto context                = source.identity;
      context.invocation_sequence = source.invocation.input_generation;
      context.cell_id             = source.invocation.cell_id;
      broker->record_quiet_rejection(context, decision.reservation.reason, decision.constraint);
      return OCUDU_DAPP_BYPASS_V1;
    }
    case quiet_reservation_result::invalid:
      return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  return OCUDU_DAPP_INTERNAL_ERROR_V1;
}

ocudu_dapp_status_v1 publish_prepared_quiet(prepared_output_broker&             broker,
                                            const dapp::prepared_output_source& source,
                                            uint32_t                            output_port_id,
                                            uint32_t                            schema_id,
                                            const void*                         typed_object,
                                            uint32_t                            object_size) noexcept
{
  constexpr uint32_t object_minimum = offsetof(ocudu_dapp_quiet_period_intent_v1, reserved);
  if (output_port_id != OCUDU_DAPP_SPECTRUM_OUTPUT_QUIET_PERIOD_INTENT_V1 ||
      schema_id != OCUDU_DAPP_SPECTRUM_OUTPUT_QUIET_PERIOD_INTENT_V1 || typed_object == nullptr ||
      object_size < object_minimum || broker.reservations == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto& intent = *static_cast<const ocudu_dapp_quiet_period_intent_v1*>(typed_object);
  if (intent.struct_size < object_minimum || intent.struct_size > object_size ||
      intent.abi_major != OCUDU_DAPP_ABI_MAJOR_V1) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  const uint64_t key = (static_cast<uint64_t>(output_port_id) << 32U) | schema_id;
  return dapp::stage_prepared_output(key, &broker, commit_quiet_intent, &intent, sizeof(intent));
}

ocudu_dapp_status_v1 publish_prepared_srs(prepared_output_broker&             broker,
                                          const dapp::prepared_output_source& source,
                                          uint32_t                            output_port_id,
                                          uint32_t                            schema_id,
                                          const void*                         typed_object,
                                          uint32_t                            object_size) noexcept
{
  constexpr uint32_t object_minimum = offsetof(ocudu_dapp_srs_isac_cir_preview_v1, reserved);
  if (output_port_id != OCUDU_DAPP_SRS_ISAC_OUTPUT_CIR_PREVIEW_V1 ||
      schema_id != OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1 || typed_object == nullptr || object_size < object_minimum) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  if (broker.srs_sink == nullptr) {
    return OCUDU_DAPP_BYPASS_V1;
  }
  const auto& output = *static_cast<const ocudu_dapp_srs_isac_cir_preview_v1*>(typed_object);
  if (output.struct_size > object_size || !valid_srs_preview(output,
                                                             source.invocation.input_generation,
                                                             source.input_srs_resource_id,
                                                             source.input_srs_config_id,
                                                             source.invocation.cell_id,
                                                             source.invocation.radio_time)) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  const auto normalized =
      normalize_srs_preview(broker, output, source.identity.instance_id, source.identity.app_generation);
  const uint64_t key = (static_cast<uint64_t>(output_port_id) << 32U) | schema_id;
  return dapp::stage_prepared_output(key, &broker, commit_srs_preview, &normalized, sizeof(normalized));
}

ocudu_dapp_status_v1 publish_prepared(void*       host_context,
                                      uint32_t    output_port_id,
                                      uint32_t    schema_id,
                                      const void* typed_object,
                                      uint32_t    object_size) noexcept
{
  auto*              broker          = static_cast<prepared_output_broker*>(host_context);
  const auto*        source          = dapp::current_prepared_output_source();
  constexpr uint32_t object_minimum  = offsetof(ocudu_dapp_spectrum_interference_map_v1, reserved);
  constexpr uint32_t context_minimum = offsetof(ocudu_dapp_context_header_v1, reserved);
  if (broker == nullptr || source == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  if (source->identity.hook_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) {
    return publish_prepared_srs(*broker, *source, output_port_id, schema_id, typed_object, object_size);
  }
  if (source->identity.hook_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 ||
      !broker->owns_authority(source->identity.instance_id)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  if (output_port_id == OCUDU_DAPP_SPECTRUM_OUTPUT_QUIET_PERIOD_INTENT_V1) {
    return publish_prepared_quiet(*broker, *source, output_port_id, schema_id, typed_object, object_size);
  }
  if (output_port_id != OCUDU_DAPP_SPECTRUM_OUTPUT_INTERFERENCE_MAP_V1 ||
      schema_id != OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1 || typed_object == nullptr || object_size < object_minimum) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto& output      = *static_cast<const ocudu_dapp_spectrum_interference_map_v1*>(typed_object);
  const auto& context     = output.context;
  const auto& tensor      = output.prb_confidence;
  const bool  host_domain = tensor.domain == OCUDU_DAPP_MEM_HOST_V1 || tensor.domain == OCUDU_DAPP_MEM_HOST_PINNED_V1 ||
                           tensor.domain == OCUDU_DAPP_MEM_MANAGED_COHERENT_V1;
  if (output.struct_size < object_minimum || output.struct_size > object_size ||
      output.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || context.struct_size < context_minimum ||
      context.schema_id != schema_id || context.schema_version != 1 || context.flags != 0 ||
      context.producer_instance_id[0] != 0 || context.producer_instance_id[1] != 0 ||
      context.producer_generation != 0 || context.source_sequence == 0 ||
      !all_zero(context.reserved, std::size(context.reserved)) ||
      context.source_sequence != source->invocation.input_generation || context.cell_id != source->invocation.cell_id ||
      context.cell_id >= MAX_NOF_DU_CELLS || context.confidence_q16 > (1U << 16U) ||
      !same_radio_time(context.created, source->invocation.radio_time) || output.source_grid_sequence == 0 ||
      output.source_grid_sequence != context.source_sequence ||
      output.source_plan_generation != source->input_plan_generation || output.first_prb != 0 || output.nof_prbs == 0 ||
      output.nof_prbs > OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 || !all_zero(output.reserved, std::size(output.reserved)) ||
      !host_domain || dapp::validate_tensor_view(tensor) != dapp::validation_error::none ||
      tensor.element_type != OCUDU_DAPP_ELEMENT_U8_V1 || tensor.rank != 1 || tensor.dimensions[0] != output.nof_prbs ||
      tensor.byte_strides[0] != sizeof(uint8_t) || tensor.layout_id != OCUDU_DAPP_LAYOUT_PRB_INTERFERENCE_U8_V1 ||
      tensor.layout_version != 1 || !all_zero(tensor.reserved, std::size(tensor.reserved)) ||
      (tensor.access & (OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1)) !=
          (OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1) ||
      tensor.native_handle == 0 || tensor.native_handle > std::numeric_limits<uintptr_t>::max() ||
      tensor.byte_offset > std::numeric_limits<uintptr_t>::max() - tensor.native_handle) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }

  normalized_interference_map normalized{};
  normalized.context                         = context;
  normalized.context.producer_instance_id[0] = source->identity.instance_id[0];
  normalized.context.producer_instance_id[1] = source->identity.instance_id[1];
  normalized.context.producer_generation     = source->identity.app_generation;
  normalized.context.source_sequence         = broker->next_sequence(context.cell_id);
  normalized.nof_prbs                        = output.nof_prbs;
  const auto address                         = static_cast<uintptr_t>(tensor.native_handle + tensor.byte_offset);
  std::memcpy(normalized.confidence.data(), reinterpret_cast<const void*>(address), output.nof_prbs);
  const uint64_t key = (static_cast<uint64_t>(output_port_id) << 32U) | schema_id;
  return dapp::stage_prepared_output(key, broker, commit_interference_map, &normalized, sizeof(normalized));
}

bool valid_config(const dapp_service_config& config) noexcept
{
  const auto& process     = config.class_c_process;
  const auto  ring_stride = (sizeof(ocudu_dapp_shared_ring_slot_v1) + uint64_t{process.payload_capacity_bytes} +
                            OCUDU_DAPP_SHARED_RING_ALIGNMENT_V1 - 1U) &
                           ~uint64_t{OCUDU_DAPP_SHARED_RING_ALIGNMENT_V1 - 1U};
  const auto ring_bytes = uint64_t{OCUDU_DAPP_SHARED_RING_HEADER_BYTES_V1} + process.ring_slots * ring_stride;
  const bool signature_policy_valid =
      config.package_signatures.trust_store_path.empty()
          ? config.package_signatures.mode == dapp::package_signature_mode::allow_unsigned &&
                config.package_signatures.minimum_policy_epoch == 0
          : std::filesystem::path(config.package_signatures.trust_store_path).is_absolute() &&
                std::filesystem::path(config.package_signatures.trust_store_path).lexically_normal() ==
                    std::filesystem::path(config.package_signatures.trust_store_path) &&
                config.package_signatures.minimum_policy_epoch != 0;
  if (config.catalog_root.empty() || config.native_preflight_helper.empty() || !signature_policy_valid ||
      config.native_preflight_timeout.count() <= 0 || config.native_preflight_timeout > std::chrono::minutes(1) ||
      config.cells.empty() || config.nof_pusch_lanes == 0 || config.nof_srs_lanes == 0 ||
      config.nof_scheduler_lanes == 0 || config.maximum_transactions == 0 || config.maximum_queued_jobs == 0 ||
      config.maximum_transactions > 65536 || config.maximum_queued_jobs > 4096 ||
      config.maximum_retained_jobs < config.maximum_queued_jobs || config.maximum_retained_jobs > 65536 ||
      config.package_manifests.size() > 256 || process.ring_slots < 2 || process.ring_slots > 64 ||
      process.payload_capacity_bytes < 64 * 1024 || process.payload_capacity_bytes > 64 * 1024 * 1024 ||
      ring_bytes > OCUDU_DAPP_SHARED_RING_MAX_BYTES_V1 || process.startup_timeout.count() <= 0 ||
      process.startup_timeout > std::chrono::minutes(1) || process.stop_timeout.count() <= 0 ||
      process.stop_timeout > std::chrono::minutes(1) || process.restart_backoff.count() <= 0 ||
      process.restart_backoff > std::chrono::minutes(1) || process.heartbeat_period.count() <= 0 ||
      process.heartbeat_period > std::chrono::minutes(1) || process.heartbeat_timeout.count() <= 0 ||
      process.heartbeat_timeout > std::chrono::minutes(1) || process.management_timeout.count() <= 0 ||
      process.management_timeout > std::chrono::minutes(1) ||
      (process.maximum_address_space_bytes != 0 && process.maximum_address_space_bytes < 64 * 1024 * 1024) ||
      process.maximum_core_file_bytes > std::numeric_limits<rlim_t>::max() || process.maximum_open_files < 16 ||
      process.maximum_open_files > 1048576 || process.maximum_restart_attempts > 100) {
    return false;
  }
  if (config.portable_e3_data) {
    const auto& portable = *config.portable_e3_data;
    if (!config.local_e3 || portable.maximum_subscriptions == 0 || portable.maximum_subscriptions > 32 ||
        portable.maximum_retained_bytes < 1024 * 1024 || portable.maximum_retained_bytes > 1024ULL * 1024 * 1024 ||
        portable.maximum_object_bytes == 0 || portable.maximum_object_bytes > 64 * 1024 * 1024 ||
        portable.maximum_metadata_bytes < 1024 || portable.maximum_metadata_bytes > 64 * 1024 ||
        portable.minimum_chunk_bytes < 1024 || portable.minimum_chunk_bytes > portable.maximum_chunk_bytes ||
        portable.maximum_chunk_bytes > 64 * 1024 || portable.minimum_lease_us == 0 ||
        portable.minimum_lease_us > portable.maximum_lease_us || portable.maximum_lease_us > 60'000'000) {
      return false;
    }
  }
  if (config.scheduler_deadline.count() <= 0 || config.scheduler_deadline > std::chrono::seconds(1)) {
    return false;
  }
  const auto& quiet = config.quiet_reservation;
  if (quiet.maximum_retained_prb_slots == 0 || quiet.maximum_retained_prb_slots > 1048576 ||
      quiet.maximum_prbs_per_slot == 0 || quiet.maximum_prbs_per_slot > MAX_NOF_PRBS ||
      quiet.duty_cycle_window_slots == 0 || quiet.duty_cycle_window_slots > 4096 ||
      quiet.maximum_prb_slots_per_window == 0 || quiet.maximum_prb_slots_per_window > 1048576 ||
      quiet.maximum_prb_slots_per_window >
          static_cast<uint32_t>(quiet.maximum_prbs_per_slot) * quiet.duty_cycle_window_slots ||
      quiet.maximum_retained_prb_slots < quiet.maximum_prb_slots_per_window ||
      quiet.maximum_source_prb_slots_per_window == 0 ||
      quiet.maximum_source_prb_slots_per_window > quiet.maximum_prb_slots_per_window) {
    return false;
  }
  for (std::size_t index = 0; index != config.package_manifests.size(); ++index) {
    const std::filesystem::path manifest(config.package_manifests[index]);
    if (manifest.empty() || manifest.is_absolute() || manifest.lexically_normal() != manifest || manifest == "." ||
        (manifest.begin() != manifest.end() && *manifest.begin() == "..") ||
        std::find(config.package_manifests.begin(),
                  config.package_manifests.begin() + index,
                  config.package_manifests[index]) != config.package_manifests.begin() + index) {
      return false;
    }
  }
  if (config.initial_instances.size() > 64) {
    return false;
  }
  for (std::size_t index = 0; index != config.initial_instances.size(); ++index) {
    const auto&                 instance = config.initial_instances[index];
    const std::filesystem::path manifest(instance.package_manifest);
    if (manifest.empty() || manifest.is_absolute() || manifest.lexically_normal() != manifest || manifest == "." ||
        (manifest.begin() != manifest.end() && *manifest.begin() == "..") ||
        std::find(config.package_manifests.begin(), config.package_manifests.end(), instance.package_manifest) ==
            config.package_manifests.end() ||
        std::any_of(config.initial_instances.begin(),
                    config.initial_instances.begin() + index,
                    [&](const auto& prior) { return prior.package_manifest == instance.package_manifest; }) ||
        (instance.placement != OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 &&
         instance.placement != OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1) ||
        (instance.backend != dapp::management::backend::cpu && instance.backend != dapp::management::backend::cuda &&
         instance.backend != dapp::management::backend::other_accelerator) ||
        (instance.target != dapp_initial_instance_target::shadow &&
         instance.target != dapp_initial_instance_target::active) ||
        instance.lifecycle_timeout.count() <= 0 || instance.lifecycle_timeout > std::chrono::minutes(1)) {
      return false;
    }
  }
  uint64_t spectrum_lanes = 0;
  for (std::size_t index = 0; index != config.cells.size(); ++index) {
    const auto& cell = config.cells[index];
    if (cell.cell_id >= MAX_NOF_DU_CELLS || cell.nof_uplink_producer_shards == 0 || cell.nof_ul_prbs <= 1 ||
        cell.nof_ul_prbs > MAX_NOF_PRBS ||
        std::any_of(config.cells.begin(), config.cells.begin() + index, [&](const auto& other) {
          return other.cell_id == cell.cell_id;
        })) {
      return false;
    }
    spectrum_lanes += cell.nof_uplink_producer_shards;
  }
  const uint64_t portable_routes = config.portable_e3_data ? spectrum_lanes + config.nof_srs_lanes : 0;
  return config.backend_mask != 0 && config.max_scheduler_ul_prbs_avoided < MAX_NOF_PRBS && spectrum_lanes != 0 &&
         config.pusch_route_slots >= 1 && config.pusch_route_slots <= dapp::pusch_route_table::max_slots_per_hook &&
         spectrum_lanes <= std::numeric_limits<uint32_t>::max() &&
         spectrum_lanes + config.nof_srs_lanes + portable_routes <= upper_phy_dapp_max_route_bindings;
}

ocudu_dapp_host_info_v1 make_host_info(const dapp_service_config& config) noexcept
{
  ocudu_dapp_host_info_v1 result{};
  result.struct_size       = sizeof(result);
  result.abi_major         = OCUDU_DAPP_ABI_MAJOR_V1;
  result.abi_minor         = OCUDU_DAPP_ABI_MINOR_V1;
  result.host_capabilities = config.host_capabilities;
  result.backend_mask      = config.backend_mask;
  result.host_build_id[0]  = config.host_build_id[0];
  result.host_build_id[1]  = config.host_build_id[1];
  return result;
}

ocudu_dapp_host_api_v1 make_host_api(prepared_output_broker& broker) noexcept
{
  ocudu_dapp_host_api_v1 result{};
  result.struct_size      = sizeof(result);
  result.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
  result.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
  result.host_context     = &broker;
  result.monotonic_ns     = monotonic_ns;
  result.publish_prepared = publish_prepared;
  return result;
}

} // namespace

#ifdef OCUDU_DAPP_APP_SERVICE_SCTP_E3
namespace {

// [srs-sharing] E3 side of E2SM-DAPP (not in upstream). The E2 agent's E2SM-DAPP module turns E3 dApp reports into E2
// indications and E2 controls into E3 xApp control actions; this class is all it sees of E3. The E2 agent belongs to
// the DU and may be torn down after this service stops, so the server pointer is cleared in stop() (detach()) and
// every use takes the mutex: after detach(), reports stop and controls fail instead of touching a stopped server.
class e3_e2sm_dapp_bridge final : public e2sm_dapp_bridge
{
public:
  e3_e2sm_dapp_bridge(dapp::e3::e3ap_sctp_server& server_, std::shared_ptr<dapp::e3::e3_connection_registry> connections_) :
    server(&server_), connections(std::move(connections_))
  {
  }

  /// Installs the E3 report handler; separate from the constructor because the handler holds a weak reference to
  /// this object, which needs it to be owned by a shared_ptr first.
  static std::shared_ptr<e3_e2sm_dapp_bridge> create(dapp::e3::e3ap_sctp_server&                       server,
                                                     std::shared_ptr<dapp::e3::e3_connection_registry> connections)
  {
    auto bridge = std::make_shared<e3_e2sm_dapp_bridge>(server, std::move(connections));
    server.set_dapp_report_handler(
        [weak = std::weak_ptr<e3_e2sm_dapp_bridge>(bridge)](const dapp::e3::e3ap_dapp_report_event& event) {
          if (auto self = weak.lock()) {
            self->on_report(event);
          }
        });
    return bridge;
  }

  void set_report_listener(std::function<void(e2sm_dapp_report)> listener_) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    listener = std::move(listener_);
  }

  bool send_control(uint32_t dapp_id, uint32_t e3_ran_function_id, std::vector<uint8_t> data) override
  {
    // E3 identifiers are 16 bits; E2SM-DAPP carries them in wider integers.
    if (dapp_id > std::numeric_limits<uint16_t>::max() || e3_ran_function_id > std::numeric_limits<uint16_t>::max()) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (server == nullptr || !is_connected(dapp_id)) {
      return false;
    }
    return server->send_xapp_control_action(
        static_cast<uint16_t>(dapp_id), static_cast<uint16_t>(e3_ran_function_id), std::move(data));
  }

  std::vector<e2sm_dapp_subscription_item> subscriptions() const override
  {
    // One item per dApp that finished E3 setup, with its E3 RAN functions sorted and deduplicated (a dApp may hold
    // several subscriptions on one RAN function, e.g. for different telemetry streams).
    std::map<uint32_t, std::vector<uint32_t>> by_dapp;
    if (connections != nullptr) {
      for (const auto& record : connections->snapshot()) {
        if (!record.setup_complete || !record.dapp_id) {
          continue;
        }
        auto& ids = by_dapp[*record.dapp_id];
        for (const auto& subscription : record.subscriptions) {
          ids.push_back(subscription.ran_function_id);
        }
      }
    }
    std::vector<e2sm_dapp_subscription_item> items;
    for (auto& [dapp_id, ids] : by_dapp) {
      std::sort(ids.begin(), ids.end());
      ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
      items.push_back({dapp_id, std::move(ids)});
    }
    return items;
  }

  void detach()
  {
    std::lock_guard<std::mutex> lock(mutex);
    server = nullptr;
    listener = nullptr;
  }

private:
  void on_report(const dapp::e3::e3ap_dapp_report_event& event)
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (server == nullptr || !listener) {
      return;
    }
    listener({event.dapp_identifier, event.ran_function_identifier, event.report_data});
  }

  /// The E3 server queues controls for unknown dApps and only counts them as dropped later; checking the registry
  /// first lets the E2 control response report the failure.
  bool is_connected(uint32_t dapp_id) const
  {
    if (connections == nullptr) {
      return true;
    }
    for (const auto& record : connections->snapshot()) {
      if (record.setup_complete && record.dapp_id && *record.dapp_id == dapp_id) {
        return true;
      }
    }
    return false;
  }

  mutable std::mutex                                mutex;
  dapp::e3::e3ap_sctp_server*                       server;
  std::shared_ptr<dapp::e3::e3_connection_registry> connections;
  std::function<void(e2sm_dapp_report)>             listener;
};

} // namespace
#endif

namespace {

struct started_initial_instance {
  dapp::management::identifier instance_id;
  std::chrono::milliseconds    timeout;
};

} // namespace

struct dapp_service::implementation {
  std::shared_ptr<dapp::native_hook_registry>                registry;
  std::shared_ptr<dapp::pusch_route_table>                   pusch_routes;
  std::shared_ptr<upper_phy_dapp_hook_provider>              upper_phy_provider;
  std::shared_ptr<scheduler_dapp_hook_provider>              scheduler_provider;
  std::shared_ptr<scheduler_dapp_interference_cache>         interference;
  std::shared_ptr<quiet_reservation_manager>                 reservations;
  std::shared_ptr<ul_plan_broker>                            ul_plans;
  std::shared_ptr<ue_event_bridge>                           ue_events; // [srs-sharing]
  std::shared_ptr<prepared_output_broker>                    output_broker;
  std::unique_ptr<dapp::instance_manager>                    manager;
  std::vector<std::shared_ptr<dapp_process_package_factory>> process_factories;
  std::vector<started_initial_instance>                      initial_instances;
  uint64_t                                                   bootstrap_transaction_sequence{1};
#ifdef OCUDU_DAPP_APP_SERVICE_E3
  std::unique_ptr<dapp::e3::embedded_agent>         agent;
  std::unique_ptr<dapp::e3::management_service>     management;
  std::shared_ptr<dapp::e3::portable_data_store>    portable_data;
  std::shared_ptr<dapp::e3::e3_connection_registry> connections;
#endif
#ifdef OCUDU_DAPP_APP_SERVICE_LOCAL_E3
  std::unique_ptr<dapp::e3::local_management_server> local_server;
#endif
#ifdef OCUDU_DAPP_APP_SERVICE_SCTP_E3
  std::shared_ptr<dapp::e3::telemetry_publisher> telemetry;
  std::shared_ptr<dapp_metrics_json_sink>        telemetry_json;
  std::unique_ptr<dapp::e3::e3ap_sctp_server>    e3ap_server;
  std::shared_ptr<e3_e2sm_dapp_bridge>           e2_dapp; // [srs-sharing]
#ifdef OCUDU_DAPP_E3DP_CARRIER
  std::unique_ptr<dapp::e3::e3dp_sctp_server> e3dp_server;
#endif
#endif
  std::atomic<bool> stopped{};
};

namespace {

std::shared_ptr<dapp::native_hook_point> add_point(const std::shared_ptr<dapp::native_hook_registry>& registry,
                                                   uint32_t                                           interface_id,
                                                   uint32_t                                           nof_lanes,
                                                   std::string&                                       detail)
{
  auto created = dapp::native_hook_point::create(interface_id, 0, nof_lanes);
  if (!created) {
    detail = std::move(created.detail);
    return {};
  }
  auto point      = std::shared_ptr<dapp::native_hook_point>(std::move(created.point));
  auto registered = registry->register_point(point);
  if (!registered) {
    detail = std::move(registered.detail);
    return {};
  }
  return point;
}

bool build_hook_topology(const std::shared_ptr<dapp::native_hook_registry>&          registry,
                         const std::shared_ptr<dapp::pusch_route_table>&             pusch_routes,
                         std::shared_ptr<upper_phy_dapp_hook_provider>&              upper_phy_provider,
                         std::shared_ptr<scheduler_dapp_hook_provider>&              scheduler_provider,
                         const std::shared_ptr<scheduler_dapp_interference_cache>&   interference,
                         const std::shared_ptr<quiet_reservation_manager>&           reservations,
                         const std::shared_ptr<ul_plan_broker>&                      ul_plans,
                         const std::shared_ptr<ue_event_bridge>&                     ue_events,
                         const std::shared_ptr<prepared_output_broker>&              output_broker,
                         std::vector<dapp::management::package_record>&              packages,
                         std::vector<std::shared_ptr<dapp_process_package_factory>>& process_factories,
#ifdef OCUDU_DAPP_APP_SERVICE_E3
                         const std::shared_ptr<dapp::e3::portable_data_store>& portable_data,
#endif
                         const dapp_service_config& config,
                         std::string&               detail)
{
  const uint32_t spectrum_lanes = static_cast<uint32_t>(
      std::accumulate(config.cells.begin(), config.cells.end(), uint64_t{}, [](uint64_t total, const auto& cell) {
        return total + cell.nof_uplink_producer_shards;
      }));
  auto spectrum = add_point(registry, OCUDU_DAPP_INTERFACE_SPECTRUM_V1, spectrum_lanes, detail);
  auto srs      = add_point(registry, OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, config.nof_srs_lanes, detail);
  auto sched    = add_point(registry, OCUDU_DAPP_INTERFACE_SCHEDULER_V1, config.nof_scheduler_lanes, detail);
  auto srs_ce   = add_point(registry, OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1, config.nof_srs_lanes, detail);
  if (!spectrum || !srs || !sched || !srs_ce) {
    return false;
  }
  // One point per route-table slot of each Class-A PUSCH hook; slot 0 is the single-owner point.
  std::vector<std::shared_ptr<dapp::native_hook_point>> ce_slots;
  std::vector<std::shared_ptr<dapp::native_hook_point>> eq_slots;
  std::vector<std::shared_ptr<dapp::native_hook_point>> rx_slots;
  for (unsigned slot = 0; slot != pusch_routes->slots_per_hook(); ++slot) {
    ce_slots.push_back(add_point(registry, OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, config.nof_pusch_lanes, detail));
    eq_slots.push_back(add_point(registry, OCUDU_DAPP_INTERFACE_EQUALIZER_V1, config.nof_pusch_lanes, detail));
    rx_slots.push_back(add_point(registry, OCUDU_DAPP_INTERFACE_RECEIVER_V1, config.nof_pusch_lanes, detail));
    if (!ce_slots.back() || !eq_slots.back() || !rx_slots.back()) {
      return false;
    }
  }

  // Per-hook backend policy follows the *data path* each hook is wired into:
  //   Class-A CE/EQ/RX and SRS CE — host adapters use HOST tensors; accelerated
  //     PUSCH and SRS use CUDA device tensors. When CUDA is in backend_mask,
  //     acceleration is the primary Class-A path → only CUDA packages may bind
  //     those hooks.
  //   Class-B scheduler — always host-record intents (CPU backend packages).
  //   Class-C spectrum/SRS — host publish path always; CUDA packages also allowed
  //     when the host advertises CUDA (device/process paths).
  constexpr uint32_t host_bit              = uint32_t{1} << OCUDU_DAPP_BACKEND_HOST_V1;
  constexpr uint32_t cuda_bit              = uint32_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1;
  const bool         cuda_l1               = (config.backend_mask & (uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1)) != 0;
  const uint32_t     class_a_hook_backends = cuda_l1 ? cuda_bit : host_bit;
  const uint32_t     class_b_hook_backends = host_bit;
  const uint32_t     class_c_hook_backends = host_bit | (cuda_l1 ? cuda_bit : 0u);
  for (unsigned slot = 0; slot != pusch_routes->slots_per_hook(); ++slot) {
    ce_slots[slot]->set_allowed_backend_mask(class_a_hook_backends);
    eq_slots[slot]->set_allowed_backend_mask(class_a_hook_backends);
    rx_slots[slot]->set_allowed_backend_mask(class_a_hook_backends);
  }
  srs_ce->set_allowed_backend_mask(class_a_hook_backends);
  sched->set_allowed_backend_mask(class_b_hook_backends);
  spectrum->set_allowed_backend_mask(class_c_hook_backends);
  srs->set_allowed_backend_mask(class_c_hook_backends);

  std::vector<upper_phy_dapp_route_binding> routes;
#ifdef OCUDU_DAPP_APP_SERVICE_E3
  const bool has_portable_data = portable_data != nullptr;
#else
  constexpr bool has_portable_data = false;
#endif
  routes.reserve((has_portable_data ? 2U : 1U) * (spectrum_lanes + config.nof_srs_lanes));
  uint32_t spectrum_lane = 0;
  for (const auto& cell : config.cells) {
    for (uint32_t shard = 0; shard != cell.nof_uplink_producer_shards; ++shard) {
      const uint32_t lane   = spectrum_lane++;
      auto           target = create_native_upper_phy_dapp_target(spectrum, lane);
      if (!target) {
        detail = "unable to create native spectrum route target";
        return false;
      }
      ul_plans->add_native(cell.cell_id, target);
      routes.push_back({{cell.cell_id, shard, 0}, std::move(target)});
#ifdef OCUDU_DAPP_APP_SERVICE_E3
      if (portable_data) {
        dapp::ipc::class_c_encoder_config encoder;
        encoder.producer_instance      = {UINT64_C(0x4f43554455453301), lane + 1U};
        encoder.ran_node               = {UINT64_C(0x4f4355445552414e), 1};
        encoder.lease_duration_ns      = config.portable_e3_data->maximum_lease_us * 1000U;
        encoder.metadata_capacity      = config.portable_e3_data->maximum_metadata_bytes;
        encoder.host_object_profile    = dapp::ipc::class_c_host_object_profile::embedded_payload;
        encoder.reference_frequency_hz = config.cells.empty() ? 0 : config.cells.front().rf_center_frequency_hz;
        auto portable =
            create_upper_phy_dapp_portable_e3_target({OCUDU_DAPP_INTERFACE_SPECTRUM_V1,
                                                      UINT64_C(0x4533500000000000) | (static_cast<uint64_t>(lane) + 1U),
                                                      std::move(encoder),
                                                      portable_data});
        if (!portable) {
          detail = "unable to create portable E3 spectrum route target: " + portable.detail;
          return false;
        }
        routes.push_back({{cell.cell_id, shard, 0}, std::move(portable.target)});
      }
#endif
    }
  }
  for (uint32_t lane = 0; lane != config.nof_srs_lanes; ++lane) {
    auto target = create_native_upper_phy_dapp_target(srs, lane);
    if (!target) {
      detail = "unable to create native SRS route target";
      return false;
    }
    routes.push_back({{0, 0, lane}, std::move(target), true});
#ifdef OCUDU_DAPP_APP_SERVICE_E3
    if (portable_data) {
      dapp::ipc::class_c_encoder_config encoder;
      encoder.producer_instance      = {UINT64_C(0x4f43554455453302), lane + 1U};
      encoder.ran_node               = {UINT64_C(0x4f4355445552414e), 1};
      encoder.lease_duration_ns      = config.portable_e3_data->maximum_lease_us * 1000U;
      encoder.metadata_capacity      = config.portable_e3_data->maximum_metadata_bytes;
      encoder.host_object_profile    = dapp::ipc::class_c_host_object_profile::embedded_payload;
      encoder.reference_frequency_hz = config.cells.empty() ? 0 : config.cells.front().rf_center_frequency_hz;
      auto portable =
          create_upper_phy_dapp_portable_e3_target({OCUDU_DAPP_INTERFACE_SRS_ISAC_V1,
                                                    UINT64_C(0x4533490000000000) | (static_cast<uint64_t>(lane) + 1U),
                                                    std::move(encoder),
                                                    portable_data});
      if (!portable) {
        detail = "unable to create portable E3 SRS route target: " + portable.detail;
        return false;
      }
      routes.push_back({{0, 0, lane}, std::move(portable.target), true});
    }
#endif
  }
  for (auto& package : packages) {
    if (package.placement_mask != OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1) {
      continue;
    }
    auto incompatibility = process_package_incompatibility(package, config);
    if (!incompatibility.empty()) {
      package.compatible      = false;
      package.incompatibility = std::move(incompatibility);
      continue;
    }
    auto created = create_dapp_process_package_factory(package, config, output_broker);
    if (!created) {
      detail = std::move(created.detail);
      return false;
    }
    if (created.routes.size() > upper_phy_dapp_max_route_bindings - routes.size()) {
      detail = "Class-C process route topology exceeds the host bound";
      return false;
    }
    routes.insert(
        routes.end(), std::make_move_iterator(created.routes.begin()), std::make_move_iterator(created.routes.end()));
    ul_plans->add_process(created.factory);
    process_factories.push_back(std::move(created.factory));
  }
  auto router = create_upper_phy_dapp_data_router(std::move(routes));
  if (!router) {
    detail = std::move(router.detail);
    return false;
  }

  auto ce_target = create_native_pusch_dapp_channel_estimator_target(std::move(ce_slots), pusch_routes);
  auto demod_target =
      create_native_pusch_dapp_demodulator_target(std::move(eq_slots), std::move(rx_slots), pusch_routes);
  auto srs_ce_target = create_native_srs_dapp_channel_estimator_target(std::move(srs_ce));
  if (!ce_target || !demod_target || !srs_ce_target) {
    detail = "unable to create the Class-A PUSCH and SRS targets";
    return false;
  }
  upper_phy_provider                         = create_upper_phy_dapp_hook_provider(std::move(router.router),
                                                           std::move(ce_target),
                                                           std::move(demod_target),
                                                           config.nof_srs_lanes,
                                                           std::move(srs_ce_target));
  auto                      scheduler_target = create_native_scheduler_dapp_target(sched);
  scheduler_dapp_guardrails sched_guardrails{};
  sched_guardrails.max_ul_prbs_avoided     = config.max_scheduler_ul_prbs_avoided;
  sched_guardrails.allow_mcs               = config.scheduler_allow_mcs;
  sched_guardrails.allow_prb_limits        = config.scheduler_allow_prb_limits;
  sched_guardrails.allow_prb_preference    = config.scheduler_allow_prb_preference;
  sched_guardrails.allow_prb_assignment    = config.scheduler_allow_prb_assignment;
  sched_guardrails.allow_tpc               = config.scheduler_allow_tpc;
  sched_guardrails.allow_layers            = config.scheduler_allow_layers;
  sched_guardrails.max_mcs_delta_from_host = config.scheduler_max_mcs_delta_from_host;
  scheduler_provider =
      create_scheduler_dapp_hook_provider(std::move(scheduler_target),
                                          interference,
                                          sched_guardrails,
                                          config.scheduler_deadline,
                                          reservations,
                                          ul_plans,
                                          config.scheduler_evaluation_only ? scheduler_dapp_policy_mode::evaluation_only
                                                                           : scheduler_dapp_policy_mode::authoritative,
                                          ue_events);
  if (!upper_phy_provider || !interference || !reservations || !scheduler_provider) {
    detail = "unable to construct dApp hook providers";
    return false;
  }
  return true;
}

dapp::management::identifier bootstrap_transaction(uint64_t sequence) noexcept
{
  dapp::management::identifier     result;
  constexpr std::array<uint8_t, 8> domain{'O', 'C', 'U', 'D', 'U', 'B', 'O', 'T'};
  std::copy(domain.begin(), domain.end(), result.bytes.begin());
  for (unsigned index = 0; index != sizeof(sequence); ++index) {
    result.bytes[8 + index] = static_cast<uint8_t>(sequence >> (8U * (sizeof(sequence) - index - 1U)));
  }
  return result;
}

const char* lifecycle_name(dapp::management::lifecycle_action action) noexcept
{
  using dapp::management::lifecycle_action;
  switch (action) {
    case lifecycle_action::prepare:
      return "prepare";
    case lifecycle_action::arm:
      return "arm";
    case lifecycle_action::enter_shadow:
      return "shadow";
    case lifecycle_action::activate:
      return "activate";
    case lifecycle_action::deactivate:
      return "deactivate";
    case lifecycle_action::retry_quiescence:
      return "retry-quiescence";
    case lifecycle_action::unload:
      return "unload";
    case lifecycle_action::breaker_probe:
      return "breaker-probe";
  }
  return "unknown";
}

dapp::management::operation_result control_initial_instance(dapp::instance_manager&             manager,
                                                            const dapp::management::identifier& instance_id,
                                                            uint64_t&                           transaction_sequence,
                                                            dapp::management::lifecycle_action  action,
                                                            uint64_t                            generation,
                                                            std::chrono::milliseconds           timeout)
{
  dapp::management::lifecycle_request request;
  request.transaction_id      = bootstrap_transaction(transaction_sequence++);
  request.instance_id         = instance_id;
  request.action              = action;
  request.expected_generation = generation;
  request.timeout_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(timeout).count());
  request.payload_fingerprint = request.transaction_id.bytes.back();
  return manager.control(request);
}

bool rollback_initial_instances(dapp::instance_manager&                      manager,
                                const std::vector<started_initial_instance>& started,
                                uint64_t&                                    transaction_sequence) noexcept
{
  bool complete = true;
  for (auto current = started.rbegin(); current != started.rend(); ++current) {
    try {
      auto record = manager.instance(current->instance_id);
      if (!record) {
        complete = false;
        continue;
      }
      if (record->state == OCUDU_DAPP_STATE_ACTIVE_V1 || record->state == OCUDU_DAPP_STATE_SHADOW_V1 ||
          record->state == OCUDU_DAPP_STATE_DEGRADED_V1 || record->state == OCUDU_DAPP_STATE_ERROR_V1) {
        auto drained = control_initial_instance(manager,
                                                current->instance_id,
                                                transaction_sequence,
                                                dapp::management::lifecycle_action::deactivate,
                                                record->generation,
                                                current->timeout);
        complete &= static_cast<bool>(drained);
        record = manager.instance(current->instance_id);
      }
      if (record && record->state == OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1) {
        auto retried = control_initial_instance(manager,
                                                current->instance_id,
                                                transaction_sequence,
                                                dapp::management::lifecycle_action::retry_quiescence,
                                                record->generation,
                                                current->timeout);
        complete &= static_cast<bool>(retried);
        record = manager.instance(current->instance_id);
      }
      if (record && (record->state == OCUDU_DAPP_STATE_INACTIVE_V1 || record->state == OCUDU_DAPP_STATE_LOADED_V1)) {
        auto unloaded = control_initial_instance(manager,
                                                 current->instance_id,
                                                 transaction_sequence,
                                                 dapp::management::lifecycle_action::unload,
                                                 record->generation,
                                                 current->timeout);
        complete &= static_cast<bool>(unloaded);
        record = manager.instance(current->instance_id);
      }
      complete &= record && record->state == OCUDU_DAPP_STATE_UNLOADED_V1;
    } catch (...) {
      complete = false;
    }
  }
  return complete;
}

std::string start_initial_instances(dapp::instance_manager&                              manager,
                                    const dapp_service_config&                           config,
                                    const std::vector<dapp::management::package_record>& packages,
                                    std::vector<started_initial_instance>&               started,
                                    uint64_t&                                            transaction_sequence)
{
  started.clear();
  started.reserve(config.initial_instances.size());
  const auto fail = [&](const dapp_initial_instance_config&       selected,
                        const char*                               phase,
                        const dapp::management::operation_result& operation) {
    const bool  rolled_back = rollback_initial_instances(manager, started, transaction_sequence);
    std::string detail = "initial dApp instance '" + selected.package_manifest + "' failed during " + phase + ": " +
                         dapp::management::to_string(operation.code);
    if (!operation.detail.empty()) {
      detail += " (" + operation.detail + ")";
    }
    if (!rolled_back) {
      detail += "; startup rollback retained at least one safely disabled resident instance";
    }
    return detail;
  };

  for (const auto& selected : config.initial_instances) {
    const auto manifest =
        std::find(config.package_manifests.begin(), config.package_manifests.end(), selected.package_manifest);
    const auto package_index = static_cast<std::size_t>(std::distance(config.package_manifests.begin(), manifest));
    if (manifest == config.package_manifests.end() || package_index >= packages.size()) {
      dapp::management::operation_result invalid;
      invalid.code   = dapp::management::result_code::invalid_request;
      invalid.detail = "selected manifest is absent from the registered package set";
      return fail(selected, "selection", invalid);
    }
    dapp::management::load_request load;
    load.transaction_id      = bootstrap_transaction(transaction_sequence++);
    load.package_id          = packages[package_index].package_id;
    load.placement           = selected.placement;
    load.selected_backend    = selected.backend;
    load.payload_fingerprint = load.transaction_id.bytes.back();
    auto loaded              = manager.load(load);
    if (!loaded || !loaded.instance_id) {
      return fail(selected, "load", loaded);
    }
    started.push_back({*loaded.instance_id, selected.lifecycle_timeout});

    constexpr std::array<dapp::management::lifecycle_action, 4> actions{
        dapp::management::lifecycle_action::prepare,
        dapp::management::lifecycle_action::arm,
        dapp::management::lifecycle_action::enter_shadow,
        dapp::management::lifecycle_action::activate};
    const std::size_t nof_actions = selected.target == dapp_initial_instance_target::active ? actions.size() : 3;
    auto              result      = loaded;
    for (std::size_t index = 0; index != nof_actions; ++index) {
      result = control_initial_instance(manager,
                                        *loaded.instance_id,
                                        transaction_sequence,
                                        actions[index],
                                        result.instance_generation,
                                        selected.lifecycle_timeout);
      if (!result) {
        return fail(selected, lifecycle_name(actions[index]), result);
      }
    }
    const auto expected = selected.target == dapp_initial_instance_target::active ? OCUDU_DAPP_STATE_ACTIVE_V1
                                                                                  : OCUDU_DAPP_STATE_SHADOW_V1;
    if (result.state != expected) {
      result.code   = dapp::management::result_code::lifecycle_failed;
      result.detail = "lifecycle returned an unexpected target state";
      return fail(selected, "target-state validation", result);
    }
  }
  return {};
}

} // namespace

dapp_service_create_result dapp_service::create(dapp_service_config config) noexcept
{
  if (!valid_config(config)) {
    return {{}, "invalid dApp service topology or resource bounds"};
  }
#ifndef OCUDU_DAPP_APP_SERVICE_LOCAL_E3
  if (config.local_e3) {
    return {{}, "local E3 endpoint requested but the APER carrier is not built"};
  }
#endif
#ifndef OCUDU_DAPP_APP_SERVICE_E3
  if (config.portable_e3_data) {
    return {{}, "portable E3 data profile requested but E3 is not built"};
  }
#endif
#ifndef OCUDU_DAPP_APP_SERVICE_SCTP_E3
  if (config.e3ap_sctp || config.e3dp_sctp) {
    return {{}, "SCTP E3 endpoint requested but the E3 SCTP carriers are not built"};
  }
#endif
#ifndef OCUDU_DAPP_E3DP_CARRIER
  if (config.e3dp_sctp) {
    return {{}, "E3 data-profile endpoint requested but FlatBuffers support is not built"};
  }
#endif
  try {
    auto signature_verifier = dapp::package_signature_verifier::create(config.package_signatures);
    if (!signature_verifier) {
      return {{}, "unable to initialize package signature policy: " + signature_verifier.detail};
    }
    // Retain the verifier past startup so the instance manager can re-verify a
    // package refreshed from disk with the same policy (closes the refresh
    // signature-bypass). Immutable after creation, so sharing read-only is safe.
    std::shared_ptr<const dapp::package_signature_verifier> shared_signature_verifier(
        std::move(signature_verifier.verifier));
    auto service          = std::make_unique<implementation>();
    service->registry     = std::make_shared<dapp::native_hook_registry>();
    service->pusch_routes = std::make_shared<dapp::pusch_route_table>(static_cast<uint8_t>(config.pusch_route_slots));
    if (auto attached = service->registry->set_route_table(service->pusch_routes); !attached) {
      return {{}, "unable to attach the PUSCH route table: " + attached.detail};
    }
    service->interference = create_scheduler_dapp_interference_cache();
    std::array<uint16_t, MAX_NOF_DU_CELLS> widths{};
    uint32_t                               nof_widths = 0;
    for (const auto& cell : config.cells) {
      widths[cell.cell_id] = cell.nof_ul_prbs;
      nof_widths           = std::max(nof_widths, cell.cell_id + 1);
    }
    quiet_reservation_policy quiet_policy;
    quiet_policy.maximum_slot_prb_budget            = config.quiet_reservation.maximum_retained_prb_slots;
    quiet_policy.maximum_quiet_prbs_per_slot        = config.quiet_reservation.maximum_prbs_per_slot;
    quiet_policy.duty_cycle_window_slots            = config.quiet_reservation.duty_cycle_window_slots;
    quiet_policy.maximum_quiet_prb_slots_per_window = config.quiet_reservation.maximum_prb_slots_per_window;
    quiet_policy.maximum_source_quiet_prb_slots_per_window =
        config.quiet_reservation.maximum_source_prb_slots_per_window;
    service->reservations  = create_quiet_reservation_manager({widths.data(), nof_widths}, quiet_policy);
    service->ul_plans      = std::make_shared<ul_plan_broker>(config.cells);
    service->ue_events     = std::make_shared<ue_event_bridge>();
    service->output_broker = std::make_shared<prepared_output_broker>(
        service->interference, service->reservations, config.cells, config.srs_isac_report_sink);
    if (!service->interference || !service->reservations || !service->ul_plans || !service->output_broker) {
      return {{}, "unable to construct dApp result broker"};
    }
#ifdef OCUDU_DAPP_APP_SERVICE_E3
    if (config.portable_e3_data) {
      const auto&                          source = *config.portable_e3_data;
      dapp::e3::portable_data_store_config portable;
      portable.maximum_subscriptions  = source.maximum_subscriptions;
      portable.maximum_retained_bytes = source.maximum_retained_bytes;
      portable.maximum_object_bytes   = source.maximum_object_bytes;
      portable.maximum_metadata_bytes = source.maximum_metadata_bytes;
      portable.minimum_chunk_bytes    = source.minimum_chunk_bytes;
      portable.maximum_chunk_bytes    = source.maximum_chunk_bytes;
      portable.minimum_lease_us       = source.minimum_lease_us;
      portable.maximum_lease_us       = source.maximum_lease_us;
      auto created                    = dapp::e3::portable_data_store::create(portable);
      if (!created.first) {
        return {{}, std::move(created.second)};
      }
      service->portable_data = std::move(created.first);
    }
#endif
    std::string                                   detail;
    std::vector<dapp::management::package_record> packages;
    packages.reserve(config.package_manifests.size());
    for (const auto& manifest_path : config.package_manifests) {
      auto manifest = dapp::read_package_manifest(config.catalog_root, manifest_path, shared_signature_verifier.get());
      if (!manifest) {
        return {{}, "unable to read package manifest '" + manifest_path + "': " + manifest.detail};
      }
      if (std::any_of(packages.begin(), packages.end(), [&](const auto& package) {
            return package.package_id == manifest.package->package_id;
          })) {
        return {{}, "package manifest '" + manifest_path + "' repeats a package identifier"};
      }
      packages.push_back(std::move(*manifest.package));
    }
    if (!build_hook_topology(service->registry,
                             service->pusch_routes,
                             service->upper_phy_provider,
                             service->scheduler_provider,
                             service->interference,
                             service->reservations,
                             service->ul_plans,
                             service->ue_events,
                             service->output_broker,
                             packages,
                             service->process_factories,
#ifdef OCUDU_DAPP_APP_SERVICE_E3
                             service->portable_data,
#endif
                             config,
                             detail)) {
      return {{}, std::move(detail)};
    }
    auto observer = service->registry->set_binding_observer(service->output_broker);
    if (!observer) {
      return {{}, "unable to bind prepared-output lifecycle: " + observer.detail};
    }
    service->manager = std::make_unique<dapp::instance_manager>(config.catalog_root,
                                                                make_host_info(config),
                                                                make_host_api(*service->output_broker),
                                                                config.maximum_transactions,
                                                                service->registry,
                                                                config.native_preflight_helper,
                                                                config.native_preflight_timeout,
                                                                shared_signature_verifier);
    if (!service->manager->set_host_incident_drain([broker = service->output_broker](dapp::incident_store& store) {
          return broker->drain_host_incidents(store);
        })) {
      return {{}, "unable to bind host policy incident source"};
    }
    for (const auto& package : packages) {
      auto registered = service->manager->register_package(package);
      if (!registered) {
        return {{}, "unable to register package '" + package.global_name + "': " + registered.detail};
      }
    }
    for (const auto& factory : service->process_factories) {
      auto registered = service->manager->register_process_factory(
          factory->package_id(),
          [factory](std::unique_ptr<dapp::approved_artifact> executable, dapp::management::backend selected_backend) {
            return factory->create_runtime(std::move(executable), selected_backend);
          });
      if (!registered) {
        return {{}, "unable to register Class-C process factory: " + registered.detail};
      }
    }
    auto startup_detail = start_initial_instances(
        *service->manager, config, packages, service->initial_instances, service->bootstrap_transaction_sequence);
    if (!startup_detail.empty()) {
      return {{}, std::move(startup_detail)};
    }
#ifdef OCUDU_DAPP_APP_SERVICE_E3
    service->agent = std::make_unique<dapp::e3::embedded_agent>(
        *service->manager, config.maximum_queued_jobs, config.maximum_retained_jobs);
    service->connections = std::make_shared<dapp::e3::e3_connection_registry>();
    service->management  = std::make_unique<dapp::e3::management_service>(*service->agent,
                                                                         service->reservations,
                                                                         service->portable_data,
                                                                         service->connections,
                                                                         config.ran_control,
                                                                         service->pusch_routes);
#endif
#ifdef OCUDU_DAPP_APP_SERVICE_LOCAL_E3
    if (config.local_e3) {
      auto endpoint = dapp::e3::local_management_server::create(
          *service->management,
          {std::move(config.local_e3->socket_path), config.local_e3->allowed_uid, config.local_e3->request_timeout});
      if (!endpoint) {
        return {{}, std::move(endpoint.detail)};
      }
      service->local_server = std::move(endpoint.server);
    }
#endif
#ifdef OCUDU_DAPP_APP_SERVICE_SCTP_E3
    if (service->portable_data) {
      dapp::e3::telemetry_publisher_config telemetry;
      telemetry.store                          = service->portable_data;
      telemetry.pusch_symbols_minimum_interval = config.telemetry.pusch_symbols_minimum_interval;
      telemetry.pusch_crc_minimum_interval     = config.telemetry.pusch_crc_minimum_interval;
      telemetry.srs_channel_minimum_interval   = config.telemetry.srs_channel_minimum_interval;
      telemetry.pusch_dmrs_minimum_interval    = config.telemetry.pusch_dmrs_minimum_interval;
      telemetry.pusch_symbols_hard_floor       = config.telemetry.pusch_symbols_hard_floor;
      telemetry.pusch_crc_hard_floor           = config.telemetry.pusch_crc_hard_floor;
      telemetry.srs_channel_hard_floor         = config.telemetry.srs_channel_hard_floor;
      telemetry.pusch_dmrs_hard_floor          = config.telemetry.pusch_dmrs_hard_floor;
      telemetry.pusch_grid_minimum_interval    = config.telemetry.pusch_grid_minimum_interval;
      telemetry.pusch_grid_hard_floor          = config.telemetry.pusch_grid_hard_floor;
      auto created                             = dapp::e3::telemetry_publisher::create(telemetry);
      if (!created.first) {
        return {{}, std::move(created.second)};
      }
      service->telemetry = std::move(created.first);
      // [srs-sharing] From here on srs.config / ta.command events reach E3 subscribers.
      service->ue_events->attach(service->telemetry);
      struct telemetry_json_adapter final : dapp_metrics_json_sink {
        explicit telemetry_json_adapter(std::shared_ptr<dapp::e3::telemetry_publisher> publisher_) :
          publisher(std::move(publisher_))
        {
        }
        bool publish_metrics_json(std::string_view json) noexcept override
        {
          return publisher->publish_metrics_json(json);
        }
        std::shared_ptr<dapp::e3::telemetry_publisher> publisher;
      };
      service->telemetry_json = std::make_shared<telemetry_json_adapter>(service->telemetry);
    }
    // Store queue depth must hold one maximum-size object in 64 KiB chunks.
    const uint32_t queue_depth = std::max<uint32_t>(
        64U,
        (config.portable_e3_data ? config.portable_e3_data->maximum_object_bytes : 4U * 1024U * 1024U) / (64U * 1024U) +
            1U);
    if (config.e3ap_sctp) {
      dapp::e3::e3ap_sctp_server_config e3ap;
      e3ap.transport.bind_address         = config.e3ap_sctp->bind_address;
      e3ap.transport.port                 = config.e3ap_sctp->port;
      e3ap.transport.allowed_peers        = config.e3ap_sctp->allowed_peers;
      e3ap.transport.maximum_associations = config.e3ap_sctp->maximum_associations;
      e3ap.transport.idle_timeout         = config.e3ap_sctp->idle_timeout;
      e3ap.setup_timeout                  = config.e3ap_sctp->setup_timeout;
      e3ap.maximum_subscriptions_per_dapp = config.e3ap_sctp->maximum_subscriptions_per_dapp;
      e3ap.ran_identifier                 = config.e3ap_sctp->ran_identifier;
      e3ap.queue_depth_chunks             = std::min<uint32_t>(queue_depth, 1024U);
      e3ap.maximum_payload_bytes =
          config.portable_e3_data ? config.portable_e3_data->maximum_object_bytes : 4U * 1024U * 1024U;
      for (const auto& cell : config.cells) {
        e3ap.cell_ids.push_back(static_cast<uint16_t>(cell.cell_id));
      }
      // Advertise the configured floors rather than the schema defaults.
      e3ap.streams = dapp::e3::default_telemetry_streams();
      for (auto& stream : e3ap.streams) {
        const auto set = [&stream](std::chrono::microseconds default_floor, std::chrono::microseconds hard_floor) {
          stream.minimum_interval_us =
              default_floor.count() > 0 ? std::optional<uint64_t>(default_floor.count()) : std::nullopt;
          stream.hard_floor_interval_us =
              hard_floor.count() > 0 ? std::optional<uint64_t>(hard_floor.count()) : std::nullopt;
        };
        if (stream.telemetry_id == dapp::e3::telemetry_id_pusch_symbols) {
          set(config.telemetry.pusch_symbols_minimum_interval, config.telemetry.pusch_symbols_hard_floor);
        } else if (stream.telemetry_id == dapp::e3::telemetry_id_pusch_crc) {
          set(config.telemetry.pusch_crc_minimum_interval, config.telemetry.pusch_crc_hard_floor);
        } else if (stream.telemetry_id == dapp::e3::telemetry_id_srs_channel) {
          set(config.telemetry.srs_channel_minimum_interval, config.telemetry.srs_channel_hard_floor);
        } else if (stream.telemetry_id == dapp::e3::telemetry_id_pusch_dmrs_channel) {
          set(config.telemetry.pusch_dmrs_minimum_interval, config.telemetry.pusch_dmrs_hard_floor);
        } else if (stream.telemetry_id == dapp::e3::telemetry_id_pusch_grid) {
          set(config.telemetry.pusch_grid_minimum_interval, config.telemetry.pusch_grid_hard_floor);
        }
      }
      auto endpoint = dapp::e3::e3ap_sctp_server::create(
          service->management.get(), service->portable_data, e3ap, service->connections);
      if (!endpoint) {
        return {{}, "unable to start the E3AP SCTP endpoint: " + endpoint.detail};
      }
      service->e3ap_server = std::move(endpoint.server);
      service->e2_dapp     = e3_e2sm_dapp_bridge::create(*service->e3ap_server, service->connections); // [srs-sharing]
    }
#ifdef OCUDU_DAPP_E3DP_CARRIER
    if (config.e3dp_sctp) {
      dapp::e3::e3dp_sctp_server_config e3dp;
      e3dp.transport.bind_address         = config.e3dp_sctp->bind_address;
      e3dp.transport.port                 = config.e3dp_sctp->port;
      e3dp.transport.allowed_peers        = config.e3dp_sctp->allowed_peers;
      e3dp.transport.maximum_associations = config.e3dp_sctp->maximum_associations;
      e3dp.transport.idle_timeout                 = config.e3dp_sctp->idle_timeout;
      e3dp.maximum_subscriptions_per_association  = config.e3dp_sctp->maximum_subscriptions_per_association;
      e3dp.queue_depth_chunks                     = std::min<uint32_t>(queue_depth, 1024U);
      for (const auto& cell : config.cells) {
        e3dp.cells.push_back(
            {static_cast<uint16_t>(cell.cell_id), cell.rf_center_frequency_hz, cell.cell_frequency_offset_hz});
      }
      auto endpoint = dapp::e3::e3dp_sctp_server::create(*service->management, service->portable_data, e3dp);
      if (!endpoint) {
        return {{}, "unable to start the E3 data-profile SCTP endpoint: " + endpoint.detail};
      }
      service->e3dp_server = std::move(endpoint.server);
    }
#endif
#endif
    return {std::unique_ptr<dapp_service>(new dapp_service(std::move(service))), {}};
  } catch (const std::exception& error) {
    return {{}, error.what()};
  } catch (...) {
    return {{}, "unknown dApp service construction failure"};
  }
}

dapp_service::dapp_service(std::unique_ptr<implementation> state_) : state(std::move(state_)) {}

dapp_service::~dapp_service()
{
  stop();
}

std::shared_ptr<upper_phy_dapp_hook_provider> dapp_service::upper_phy_hooks() const noexcept
{
  return state->upper_phy_provider;
}

std::shared_ptr<scheduler_dapp_hook_provider> dapp_service::scheduler_hooks() const noexcept
{
  return state->scheduler_provider;
}

std::shared_ptr<scheduler_dapp_interference_cache> dapp_service::interference_cache() const noexcept
{
  return state->interference;
}

std::shared_ptr<quiet_reservation_manager> dapp_service::quiet_reservations() const noexcept
{
  return state->reservations;
}

std::shared_ptr<dapp::native_hook_registry> dapp_service::native_hooks() const noexcept
{
  return state->registry;
}

dapp::instance_manager& dapp_service::instances() noexcept
{
  return *state->manager;
}

bool dapp_service::has_embedded_e3() const noexcept
{
#ifdef OCUDU_DAPP_APP_SERVICE_E3
  return state->agent != nullptr;
#else
  return false;
#endif
}

bool dapp_service::has_local_e3() const noexcept
{
#ifdef OCUDU_DAPP_APP_SERVICE_LOCAL_E3
  return state->local_server != nullptr;
#else
  return false;
#endif
}

bool dapp_service::has_e3ap_sctp() const noexcept
{
#ifdef OCUDU_DAPP_APP_SERVICE_SCTP_E3
  return state->e3ap_server != nullptr;
#else
  return false;
#endif
}

bool dapp_service::has_e3dp_sctp() const noexcept
{
#if defined(OCUDU_DAPP_APP_SERVICE_SCTP_E3) && defined(OCUDU_DAPP_E3DP_CARRIER)
  return state->e3dp_server != nullptr;
#else
  return false;
#endif
}

std::shared_ptr<phy_diagnostic_sink> dapp_service::phy_diagnostics() const noexcept
{
#ifdef OCUDU_DAPP_APP_SERVICE_SCTP_E3
  return state->telemetry;
#else
  return nullptr;
#endif
}

bool dapp_service::publish_metrics_json(std::string_view json) noexcept
{
#ifdef OCUDU_DAPP_APP_SERVICE_SCTP_E3
  return state->telemetry != nullptr && state->telemetry->publish_metrics_json(json);
#else
  static_cast<void>(json);
  return false;
#endif
}

std::shared_ptr<e2sm_dapp_bridge> dapp_service::e2_dapp_bridge() const noexcept
{
#ifdef OCUDU_DAPP_APP_SERVICE_SCTP_E3
  return state->e2_dapp;
#else
  return nullptr;
#endif
}

std::shared_ptr<dapp_metrics_json_sink> dapp_service::metrics_json_sink() const noexcept
{
#ifdef OCUDU_DAPP_APP_SERVICE_SCTP_E3
  return state->telemetry_json;
#else
  return nullptr;
#endif
}

void dapp_service::stop() noexcept
{
  if (state == nullptr || state->stopped.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
#ifdef OCUDU_DAPP_APP_SERVICE_SCTP_E3
#ifdef OCUDU_DAPP_E3DP_CARRIER
  if (state->e3dp_server) {
    state->e3dp_server->stop();
  }
#endif
  if (state->e2_dapp) {
    state->e2_dapp->detach(); // [srs-sharing]
  }
  if (state->e3ap_server) {
    state->e3ap_server->stop();
  }
#endif
#ifdef OCUDU_DAPP_APP_SERVICE_LOCAL_E3
  if (state->local_server) {
    state->local_server->stop();
  }
#endif
#ifdef OCUDU_DAPP_APP_SERVICE_E3
  if (state->agent) {
    state->agent->stop();
  }
#endif
  if (state->manager && !state->initial_instances.empty()) {
    static_cast<void>(
        rollback_initial_instances(*state->manager, state->initial_instances, state->bootstrap_transaction_sequence));
  }
}
