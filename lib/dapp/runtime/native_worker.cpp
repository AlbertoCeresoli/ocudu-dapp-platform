// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/native_worker.h"
#include "native_worker_internal.h"
#include "native_worker_validation.h"
#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/runtime/abi_validation.h"
#include "ocudu/dapp/runtime/guarded_call.h"
#include "ocudu/dapp/runtime/native_instance.h"
#include "ocudu/dapp/runtime/prepared_output.h"
#include "ocudu/ran/gnb_constants.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <optional>

using namespace ocudu::dapp;
using namespace ocudu::dapp::native_worker_detail;

namespace {

const ocudu_dapp_receiver_metrics_v1* read_receiver_metrics(const ocudu_dapp_typed_metadata_v1& wrapper) noexcept
{
  constexpr uint32_t wrapper_minimum =
      offsetof(ocudu_dapp_typed_metadata_v1, metadata) + sizeof(ocudu_dapp_blob_view_v1);
  constexpr uint32_t metrics_minimum =
      offsetof(ocudu_dapp_receiver_metrics_v1, post_equalization_noise) + sizeof(ocudu_dapp_tensor_view_v1);
  if (wrapper.struct_size < wrapper_minimum || wrapper.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      wrapper.metadata_schema_id != OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1 || wrapper.metadata_schema_version != 1 ||
      wrapper.metadata.data == nullptr || wrapper.metadata.size < metrics_minimum) {
    return nullptr;
  }
  const auto* metrics = static_cast<const ocudu_dapp_receiver_metrics_v1*>(wrapper.metadata.data);
  return metrics->struct_size >= metrics_minimum && metrics->abi_major == OCUDU_DAPP_ABI_MAJOR_V1 ? metrics : nullptr;
}

bool same_metadata_storage(const ocudu_dapp_typed_metadata_v1& expected,
                           const ocudu_dapp_typed_metadata_v1& actual) noexcept
{
  return expected.struct_size == actual.struct_size && expected.abi_major == actual.abi_major &&
         expected.abi_minor == actual.abi_minor && expected.metadata_schema_id == actual.metadata_schema_id &&
         expected.metadata_schema_version == actual.metadata_schema_version &&
         expected.metadata.data == actual.metadata.data && expected.metadata.size == actual.metadata.size &&
         expected.metadata.reserved == actual.metadata.reserved;
}

} // namespace

void native_worker_incident_source::record(const class_b_policy_measurement& measurement) noexcept
{
  class_b_invocations.fetch_add(1, std::memory_order_relaxed);
  class_b_conventional_time_ns.fetch_add(measurement.conventional_time_ns, std::memory_order_relaxed);
  class_b_feature_time_ns.fetch_add(measurement.feature_time_ns, std::memory_order_relaxed);
  class_b_dapp_time_ns.fetch_add(measurement.dapp_time_ns, std::memory_order_relaxed);
  class_b_validation_time_ns.fetch_add(measurement.validation_time_ns, std::memory_order_relaxed);
  class_b_commit_time_ns.fetch_add(measurement.commit_time_ns, std::memory_order_relaxed);
  class_b_total_time_ns.fetch_add(measurement.total_time_ns, std::memory_order_relaxed);
  class_b_deadline_misses.fetch_add(measurement.deadline_miss, std::memory_order_relaxed);
  switch (measurement.outcome) {
    case class_b_policy_outcome::accepted:
      class_b_accepted.fetch_add(1, std::memory_order_relaxed);
      break;
    case class_b_policy_outcome::evaluation_accepted:
      class_b_evaluation_accepted.fetch_add(1, std::memory_order_relaxed);
      break;
    case class_b_policy_outcome::fallback:
      class_b_fallbacks.fetch_add(1, std::memory_order_relaxed);
      break;
    case class_b_policy_outcome::validation_rejection:
      class_b_validation_rejections.fetch_add(1, std::memory_order_relaxed);
      break;
  }
}

void native_worker_incident_source::record(class_a_fallback_reason reason) noexcept
{
  const auto index = static_cast<std::size_t>(reason);
  if (index < class_a_fallbacks.size()) {
    class_a_fallbacks[index].fetch_add(1, std::memory_order_relaxed);
  }
}

void native_worker_incident_source::record_completion_deadline() noexcept
{
  class_a_completion_deadline_misses.fetch_add(1, std::memory_order_relaxed);
}

void native_worker_incident_source::observe_cell(uint32_t cell_id) noexcept
{
  if (cell_id < OCUDU_DAPP_HOST_INPUT_CELL_BITMAP_BITS_V1) {
    input_cell_bitmap.fetch_or(uint32_t{1} << cell_id, std::memory_order_relaxed);
  }
}

uint32_t native_worker_incident_source::observed_cells() const noexcept
{
  return input_cell_bitmap.load(std::memory_order_relaxed);
}

class_a_fallback_snapshot native_worker_incident_source::class_a_snapshot() const noexcept
{
  class_a_fallback_snapshot result;
  for (std::size_t index = 0; index != result.reasons.size(); ++index) {
    result.reasons[index] = class_a_fallbacks[index].load(std::memory_order_relaxed);
  }
  return result;
}

class_a_completion_snapshot native_worker_incident_source::class_a_completion() const noexcept
{
  return {class_a_completion_deadline_misses.load(std::memory_order_relaxed)};
}

class_b_policy_snapshot native_worker_incident_source::class_b_snapshot() const noexcept
{
  return {class_b_invocations.load(std::memory_order_relaxed),
          class_b_conventional_time_ns.load(std::memory_order_relaxed),
          class_b_feature_time_ns.load(std::memory_order_relaxed),
          class_b_dapp_time_ns.load(std::memory_order_relaxed),
          class_b_validation_time_ns.load(std::memory_order_relaxed),
          class_b_commit_time_ns.load(std::memory_order_relaxed),
          class_b_total_time_ns.load(std::memory_order_relaxed),
          class_b_accepted.load(std::memory_order_relaxed),
          class_b_evaluation_accepted.load(std::memory_order_relaxed),
          class_b_fallbacks.load(std::memory_order_relaxed),
          class_b_validation_rejections.load(std::memory_order_relaxed),
          class_b_deadline_misses.load(std::memory_order_relaxed)};
}

struct native_worker::implementation {
  std::shared_ptr<native_module_library>         library;
  std::shared_ptr<quiescence_tracker>            activity;
  std::shared_ptr<native_worker_incident_source> incident_source;
  ocudu_dapp_worker_api_v1                       worker_api{};
  const void*                                    interface_vtable{};
  void*                                          module_instance{};
  void*                                          worker_context{};
  activity_token                                 lifetime;
  uint32_t                                       bound_interface_id{};
  uint32_t                                       bound_worker_id{};
  uint64_t                                       interface_capabilities{};
  std::optional<ocudu_dapp_pusch_profile_v1>     pusch_profile;
  uint32_t                                       selected_backend{};
  void*                                          host_context{};
  uint64_t (*monotonic_ns)(void*){};
  std::unique_ptr<prepared_output_transaction> prepared_outputs;

  uint64_t now_ns() const noexcept
  {
    if (monotonic_ns == nullptr) {
      return steady_now_ns();
    }
    try {
      return monotonic_ns(host_context);
    } catch (...) {
      return steady_now_ns();
    }
  }

  bool class_a() const noexcept
  {
    return bound_interface_id == OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1 ||
           bound_interface_id == OCUDU_DAPP_INTERFACE_EQUALIZER_V1 ||
           bound_interface_id == OCUDU_DAPP_INTERFACE_RECEIVER_V1;
  }

  incident_context make_context(const ocudu_dapp_invocation_v1& invocation) const noexcept
  {
    incident_context context    = incident_source->context();
    context.phase               = incident_phase::invoke;
    context.invocation_sequence = invocation.invocation_id;
    context.cell_id =
        static_cast<uint16_t>(std::min<uint32_t>(invocation.cell_id, std::numeric_limits<uint16_t>::max()));
    context.worker_id = static_cast<uint16_t>(bound_worker_id);
    return context;
  }

  template <typename Callback, typename OutputValidator>
  native_invocation_result call(const ocudu_dapp_invocation_v1& invocation,
                                Callback&&                      callback,
                                OutputValidator&&               output_validator,
                                uint64_t                        input_plan_generation = 0,
                                uint32_t                        input_srs_resource_id = 0,
                                uint32_t                        input_srs_config_id   = 0) noexcept
  {
    auto invocation_token = activity->try_acquire(activity_kind::invocation);
    if (!invocation_token) {
      if (class_a()) {
        incident_source->record(class_a_fallback_reason::admission_closed);
      }
      return {OCUDU_DAPP_BYPASS_V1, true};
    }

    const incident_context context = make_context(invocation);
    prepared_output_scope  output_scope(
        prepared_outputs.get(),
        {context, invocation, input_plan_generation, input_srs_resource_id, input_srs_config_id});
    bool       callback_entered = false;
    const auto guarded          = guarded_call(
        [&]() {
          incident_source->observe_cell(invocation.cell_id);
          callback_entered  = true;
          const auto status = callback();
          if (status != OCUDU_DAPP_OK_V1) {
            return status;
          }
          if (!output_validator()) {
            return OCUDU_DAPP_INVALID_OUTPUT_V1;
          }
          if (invocation.deadline_monotonic_ns != 0 && now_ns() > invocation.deadline_monotonic_ns) {
            return OCUDU_DAPP_DEADLINE_V1;
          }
          return prepared_outputs != nullptr ? prepared_outputs->commit() : OCUDU_DAPP_OK_V1;
        },
        incident_source->breaker,
        incident_source->incidents,
        context);
    if (class_a() && guarded.status != OCUDU_DAPP_OK_V1) {
      incident_source->record(!callback_entered && guarded.status == OCUDU_DAPP_BYPASS_V1
                                  ? class_a_fallback_reason::breaker_open
                                  : status_reason(guarded.status));
    }
    return {guarded.status, guarded.invoke_fallback};
  }
};

native_worker::native_worker(std::unique_ptr<implementation> implementation_) : state(std::move(implementation_)) {}

native_worker::~native_worker()
{
  if (state == nullptr || state->worker_context == nullptr) {
    return;
  }
  incident_context context = state->incident_source->context();
  context.phase            = incident_phase::completion;
  guarded_call(
      [&]() {
        state->worker_api.destroy_worker_context(state->module_instance, state->worker_context);
        return OCUDU_DAPP_OK_V1;
      },
      state->incident_source->breaker,
      state->incident_source->incidents,
      context,
      true);
  state->worker_context = nullptr;
  state->lifetime.reset();
}

uint32_t native_worker::interface_id() const noexcept
{
  return state->bound_interface_id;
}

uint32_t native_worker::worker_id() const noexcept
{
  return state->bound_worker_id;
}

uint32_t native_worker::backend_id() const noexcept
{
  return state->selected_backend;
}

native_worker_create_result native_worker::create(native_instance& owner, uint32_t interface_id, uint32_t worker_id)
{
  if (worker_id > std::numeric_limits<uint16_t>::max()) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, "worker identifier exceeds incident ABI range", {}};
  }
  const auto* descriptor =
      std::find_if(owner.module.interfaces,
                   owner.module.interfaces + owner.module.nof_interfaces,
                   [interface_id](const auto& candidate) {
                     return candidate.interface_id == interface_id && candidate.interface_major == 1;
                   });
  if (descriptor == owner.module.interfaces + owner.module.nof_interfaces) {
    return {OCUDU_DAPP_UNSUPPORTED_V1, "module does not implement the requested interface", {}};
  }
  const bool valid_class =
      ((interface_id == OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1 ||
        interface_id == OCUDU_DAPP_INTERFACE_EQUALIZER_V1 || interface_id == OCUDU_DAPP_INTERFACE_RECEIVER_V1) &&
       descriptor->dapp_class == OCUDU_DAPP_CLASS_A_INLINE_V1) ||
      ((interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 || interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) &&
       descriptor->dapp_class == OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1) ||
      (interface_id == OCUDU_DAPP_INTERFACE_SCHEDULER_V1 && descriptor->dapp_class == OCUDU_DAPP_CLASS_B_DEADLINE_V1);
  const bool class_a_interface = interface_id == OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1 ||
                                 interface_id == OCUDU_DAPP_INTERFACE_EQUALIZER_V1 ||
                                 interface_id == OCUDU_DAPP_INTERFACE_RECEIVER_V1;
  const bool resident_accelerator =
      class_a_interface && (descriptor->capability_bits & OCUDU_DAPP_PUSCH_CAP_RESIDENT_ACCELERATOR_V1) != 0;
  // Class-A residency determines the tensor-memory contract. Class-B always
  // exchanges bounded host records, but its in-process implementation may use
  // any admitted backend internally and must still return synchronously.
  const bool compatible_backend =
      !class_a_interface || resident_accelerator == (owner.backend_id() == OCUDU_DAPP_BACKEND_CUDA_V1);
  if (!valid_class || (descriptor->placement_mask & (1U << (OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 - 1))) == 0 ||
      descriptor->threading == OCUDU_DAPP_THREADING_SERIALIZED_V1 || !compatible_backend) {
    return {OCUDU_DAPP_UNSUPPORTED_V1, "requested interface is not an admitted direct in-process hook", {}};
  }
  const auto*        worker_api         = get_worker_api(*descriptor);
  constexpr uint32_t worker_api_minimum = offsetof(ocudu_dapp_worker_api_v1, reserved);
  if (worker_api == nullptr || worker_api->struct_size < worker_api_minimum ||
      worker_api->abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || worker_api->create_worker_context == nullptr ||
      worker_api->destroy_worker_context == nullptr || !valid_interface_callback(*descriptor) ||
      (interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 &&
       (descriptor->capability_bits & OCUDU_DAPP_SPECTRUM_CAP_FUTURE_UL_PLAN_V1) != 0 &&
       static_cast<const ocudu_dapp_spectrum_interface_v1*>(descriptor->interface_vtable)->publish_plan == nullptr)) {
    return {OCUDU_DAPP_INVALID_OUTPUT_V1, "module returned an invalid direct-hook worker vtable", {}};
  }
  {
    std::lock_guard<std::mutex> lock(owner.worker_sources_mutex);
    if (owner.worker_sources.size() >= max_worker_sources) {
      return {OCUDU_DAPP_RESOURCE_V1, "module worker source limit reached", {}};
    }
  }

  auto lifetime = owner.activity->try_acquire(activity_kind::callback);
  if (!lifetime) {
    return {OCUDU_DAPP_BYPASS_V1, "instance is not accepting worker bindings", {}};
  }
  std::shared_ptr<native_worker_incident_source> source;
  std::shared_ptr<native_worker>                 worker;
  try {
    source                     = std::make_shared<native_worker_incident_source>();
    source->identity           = owner.incident_identity;
    source->identity.hook_id   = interface_id;
    source->identity.worker_id = static_cast<uint16_t>(worker_id);
    source->app_generation.store(owner.incident_identity.app_generation, std::memory_order_relaxed);
    auto implementation_                    = std::make_unique<implementation>();
    implementation_->library                = owner.library;
    implementation_->activity               = owner.activity;
    implementation_->incident_source        = source;
    implementation_->worker_api             = *worker_api;
    implementation_->interface_vtable       = descriptor->interface_vtable;
    implementation_->module_instance        = owner.module_instance;
    implementation_->bound_interface_id     = interface_id;
    implementation_->bound_worker_id        = worker_id;
    implementation_->interface_capabilities = descriptor->capability_bits;
    if (descriptor->use_case_profile != nullptr) {
      implementation_->pusch_profile = *static_cast<const ocudu_dapp_pusch_profile_v1*>(descriptor->use_case_profile);
    }
    implementation_->selected_backend = owner.backend_id();
    implementation_->host_context     = owner.host_api.host_context;
    implementation_->monotonic_ns     = owner.host_api.monotonic_ns;
    if (owner.host_api.publish_prepared != nullptr &&
        (interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 || interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1)) {
      implementation_->prepared_outputs = std::make_unique<prepared_output_transaction>();
    }
    worker = std::shared_ptr<native_worker>(new native_worker(std::move(implementation_)));
    std::lock_guard<std::mutex> lock(owner.worker_sources_mutex);
    owner.worker_sources.push_back(source);
  } catch (const std::bad_alloc&) {
    return {OCUDU_DAPP_RESOURCE_V1, "unable to allocate module worker ownership", {}};
  }

  void* worker_context = nullptr;
  auto  created        = guarded_call(
      [&]() {
        const auto status = worker_api->create_worker_context(owner.module_instance, worker_id, &worker_context);
        return status == OCUDU_DAPP_OK_V1 && worker_context == nullptr ? OCUDU_DAPP_INVALID_OUTPUT_V1 : status;
      },
      source->breaker,
      source->incidents,
      source->context());
  if (created.status != OCUDU_DAPP_OK_V1) {
    incident_record incident;
    while (source->incidents.try_pop(incident)) {
      owner.incidents.try_push(incident);
    }
    std::lock_guard<std::mutex> lock(owner.worker_sources_mutex);
    auto                        found = std::find(owner.worker_sources.begin(), owner.worker_sources.end(), source);
    if (found != owner.worker_sources.end()) {
      owner.worker_sources.erase(found);
    }
    return {created.status, "module worker creation failed", {}};
  }
  worker->state->worker_context = worker_context;
  worker->state->lifetime       = std::move(lifetime);
  return {OCUDU_DAPP_OK_V1, {}, std::move(worker)};
}

native_invocation_result native_worker::invoke(const ocudu_dapp_channel_estimator_input_v1& input,
                                               ocudu_dapp_channel_estimator_output_v1&      output) noexcept
{
  if (state->bound_interface_id != OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  if (!valid_invocation(input.invocation, state->bound_worker_id) || !valid_input_tensor(input.rx_grid)) {
    state->incident_source->record(class_a_fallback_reason::invalid_input);
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  if (state->pusch_profile) {
    const auto reason =
        classify_unsupported_pusch(state->bound_interface_id, *state->pusch_profile, input.pusch, input.dmrs);
    if (reason) {
      state->incident_source->record(*reason);
      return {OCUDU_DAPP_UNSUPPORTED_V1, true};
    }
  }
  const auto  channel_storage = output.channel_estimates;
  const auto  noise_storage   = output.noise_variances;
  const auto* api             = static_cast<const ocudu_dapp_channel_estimator_interface_v1*>(state->interface_vtable);
  return state->call(
      input.invocation,
      [&]() { return api->invoke(state->worker_context, &input, &output); },
      [&]() {
        return valid_output_tensor(output.channel_estimates) && valid_output_tensor(output.noise_variances) &&
               same_tensor_storage(channel_storage, output.channel_estimates) &&
               same_tensor_storage(noise_storage, output.noise_variances);
      });
}

native_invocation_result native_worker::invoke(const ocudu_dapp_equalizer_input_v1& input,
                                               ocudu_dapp_equalizer_output_v1&      output) noexcept
{
  if (state->bound_interface_id != OCUDU_DAPP_INTERFACE_EQUALIZER_V1) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  if (!valid_invocation(input.invocation, state->bound_worker_id) || !valid_input_tensor(input.rx_grid)) {
    state->incident_source->record(class_a_fallback_reason::invalid_input);
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  if (state->pusch_profile) {
    const auto reason =
        classify_unsupported_pusch(state->bound_interface_id, *state->pusch_profile, input.pusch, input.dmrs);
    if (reason) {
      state->incident_source->record(*reason);
      return {OCUDU_DAPP_UNSUPPORTED_V1, true};
    }
  }
  const auto  channel_storage = output.channel_estimates;
  const auto  symbol_storage  = output.equalized_symbols;
  const auto  noise_storage   = output.post_equalization_noise;
  const auto* api             = static_cast<const ocudu_dapp_equalizer_interface_v1*>(state->interface_vtable);
  return state->call(
      input.invocation,
      [&]() { return api->invoke(state->worker_context, &input, &output); },
      [&]() {
        return valid_output_tensor(output.channel_estimates) && valid_output_tensor(output.equalized_symbols) &&
               valid_output_tensor(output.post_equalization_noise) &&
               same_tensor_storage(channel_storage, output.channel_estimates) &&
               same_tensor_storage(symbol_storage, output.equalized_symbols) &&
               same_tensor_storage(noise_storage, output.post_equalization_noise);
      });
}

native_invocation_result native_worker::invoke(const ocudu_dapp_receiver_input_v1& input,
                                               ocudu_dapp_receiver_output_v1&      output) noexcept
{
  if (state->bound_interface_id != OCUDU_DAPP_INTERFACE_RECEIVER_V1) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  if (!valid_invocation(input.invocation, state->bound_worker_id) || !valid_input_tensor(input.rx_grid)) {
    state->incident_source->record(class_a_fallback_reason::invalid_input);
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  if (state->pusch_profile) {
    const auto reason =
        classify_unsupported_pusch(state->bound_interface_id, *state->pusch_profile, input.pusch, input.dmrs);
    if (reason) {
      state->incident_source->record(*reason);
      return {OCUDU_DAPP_UNSUPPORTED_V1, true};
    }
  }
  const bool  metrics_requested = !empty_typed_metadata(output.metrics);
  const auto* requested_metrics = metrics_requested ? read_receiver_metrics(output.metrics) : nullptr;
  if (metrics_requested &&
      (requested_metrics == nullptr || !valid_output_tensor(requested_metrics->post_equalization_noise))) {
    state->incident_source->record(class_a_fallback_reason::invalid_input);
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  const auto* requested_symbols = metrics_requested ? receiver_symbol_storage(output.metrics) : nullptr;
  if (requested_symbols != nullptr && !valid_output_tensor(*requested_symbols)) {
    state->incident_source->record(class_a_fallback_reason::invalid_input);
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  const auto symbol_storage  = requested_symbols != nullptr ? *requested_symbols : ocudu_dapp_tensor_view_v1{};
  const auto llr_storage     = output.llrs;
  const auto metrics_storage = output.metrics;
  const auto noise_storage =
      requested_metrics != nullptr ? requested_metrics->post_equalization_noise : ocudu_dapp_tensor_view_v1{};
  const auto* api = static_cast<const ocudu_dapp_receiver_interface_v1*>(state->interface_vtable);
  return state->call(
      input.invocation,
      [&]() { return api->invoke(state->worker_context, &input, &output); },
      [&]() {
        const auto* returned_metrics = metrics_requested ? read_receiver_metrics(output.metrics) : nullptr;
        const bool  valid_metrics =
            !metrics_requested
                 ? empty_typed_metadata(output.metrics)
                 : returned_metrics != nullptr && same_metadata_storage(metrics_storage, output.metrics) &&
                      valid_output_tensor(returned_metrics->post_equalization_noise) &&
                      same_tensor_storage(noise_storage, returned_metrics->post_equalization_noise);
        const auto* returned_symbols =
            valid_metrics && metrics_requested ? receiver_symbol_storage(output.metrics) : nullptr;
        const bool valid_symbols = requested_symbols == nullptr
                                       ? returned_symbols == nullptr &&
                                             (output.reserved0 & OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_VALID_V1) == 0
                                       : returned_symbols != nullptr && valid_output_tensor(*returned_symbols) &&
                                             same_tensor_storage(symbol_storage, *returned_symbols);
        return valid_symbols && valid_output_tensor(output.llrs) && same_tensor_storage(llr_storage, output.llrs) &&
               empty_typed_metadata(output.uci) && valid_metrics && output.confidence_q16 <= (1U << 16U) &&
               (output.reserved0 & ~OCUDU_DAPP_RECEIVER_KNOWN_OUTPUT_FLAGS_V1) == 0 &&
               (metrics_requested || output.reserved0 == 0);
      });
}

native_invocation_result native_worker::invoke(const ocudu_dapp_scheduler_input_v1& input,
                                               ocudu_dapp_scheduler_output_v1&      output) noexcept
{
  constexpr uint32_t candidate_minimum = OCUDU_DAPP_SCHEDULER_CANDIDATE_BASE_SIZE_V1;
  constexpr uint32_t output_minimum    = offsetof(ocudu_dapp_scheduler_output_v1, reserved);
  if (state->bound_interface_id != OCUDU_DAPP_INTERFACE_SCHEDULER_V1 ||
      !valid_invocation(input.invocation, state->bound_worker_id) ||
      (input.direction != OCUDU_DAPP_SCHEDULER_DL_V1 && input.direction != OCUDU_DAPP_SCHEDULER_UL_V1) ||
      input.nof_candidates == 0 || input.nof_candidates > OCUDU_DAPP_SCHEDULER_MAX_CANDIDATES_V1 ||
      input.candidates == nullptr || input.candidate_generation == 0 || output.struct_size < output_minimum ||
      output.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || output.intents == nullptr ||
      output.intent_capacity < input.nof_candidates ||
      output.intent_capacity > OCUDU_DAPP_SCHEDULER_MAX_CANDIDATES_V1) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  // Host may grant any known Class-B authority bit (MCS / PRB / TPC / layers / avoid).
  // Avoid-mask still requires UL + interference; PRB assignment requires free-VRB snapshot.
  if ((input.allowed_output_flags & ~OCUDU_DAPP_SCHEDULER_ALLOW_KNOWN_MASK_V1) != 0 ||
      ((input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_AVOID_PRB_MASK_V1) != 0 &&
       (input.direction != OCUDU_DAPP_SCHEDULER_UL_V1 || input.interference_context_present == 0)) ||
      ((input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_PRB_ASSIGNMENT_V1) != 0 &&
       input.free_vrb_context_present == 0)) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  for (uint32_t index = 0; index != input.nof_candidates; ++index) {
    const auto& candidate = input.candidates[index];
    if (candidate.struct_size < candidate_minimum || candidate.candidate_id != index) {
      return {OCUDU_DAPP_INVALID_INPUT_V1, true};
    }
  }
  if (input.interference_context_present > 1 ||
      (input.interference_context_present != 0 && !valid_scheduler_interference(input))) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  if (input.free_vrb_context_present > 1 || (input.free_vrb_context_present != 0 && !valid_scheduler_free_vrb(input))) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }

  const auto* expected_intents  = output.intents;
  const auto  expected_capacity = output.intent_capacity;
  const auto  expected_mask     = output.avoid_prb_mask;
  const auto* api               = static_cast<const ocudu_dapp_scheduler_interface_v1*>(state->interface_vtable);
  return state->call(
      input.invocation,
      [&]() { return api->invoke(state->worker_context, &input, &output); },
      [&]() {
        const bool mask_requested = (output.flags & OCUDU_DAPP_SCHEDULER_OUTPUT_AVOID_PRB_MASK_V1) != 0;
        return output.struct_size >= output_minimum && output.abi_major == OCUDU_DAPP_ABI_MAJOR_V1 &&
               output.intents == expected_intents && output.intent_capacity == expected_capacity &&
               output.nof_intents <= output.intent_capacity &&
               (output.flags & ~OCUDU_DAPP_SCHEDULER_OUTPUT_AVOID_PRB_MASK_V1) == 0 &&
               same_tensor_storage(expected_mask, output.avoid_prb_mask) &&
               (!mask_requested || ((input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_AVOID_PRB_MASK_V1) != 0 &&
                                    valid_output_tensor(output.avoid_prb_mask)));
      });
}

void native_worker::report_failure(const ocudu_dapp_invocation_v1& invocation, ocudu_dapp_status_v1 status) noexcept
{
  if (status == OCUDU_DAPP_OK_V1 || status == OCUDU_DAPP_BYPASS_V1) {
    return;
  }
  guarded_call([status]() { return status; },
               state->incident_source->breaker,
               state->incident_source->incidents,
               state->make_context(invocation));
  if (state->class_a()) {
    state->incident_source->record(status_reason(status));
  }
}

void native_worker::report_completion_deadline(const ocudu_dapp_invocation_v1& invocation,
                                               uint64_t                        observed_latency_ns,
                                               uint64_t                        deadline_ns) noexcept
{
  state->incident_source->breaker.observe(OCUDU_DAPP_DEADLINE_V1);
  auto context               = state->make_context(invocation);
  context.phase              = incident_phase::completion;
  auto record                = make_incident(context,
                              incident_cause::deadline_miss,
                              incident_action::observed,
                              OCUDU_DAPP_DEADLINE_V1,
                              nullptr,
                              state->incident_source->breaker.state() == breaker_state::open);
  record.observed_latency_ns = observed_latency_ns;
  record.deadline_ns         = deadline_ns;
  state->incident_source->incidents.try_push(record);
  if (state->class_a()) {
    state->incident_source->record_completion_deadline();
  }
}

void native_worker::record_class_a_fallback(class_a_fallback_reason reason) noexcept
{
  if (state->class_a()) {
    state->incident_source->record(reason);
  }
}

void native_worker::record_class_b_policy(const class_b_policy_measurement& measurement) noexcept
{
  if (state->bound_interface_id == OCUDU_DAPP_INTERFACE_SCHEDULER_V1) {
    state->incident_source->record(measurement);
  }
}

native_invocation_result native_worker::publish(const ocudu_dapp_spectrum_input_v1& input) noexcept
{
  uint64_t plan_generation = 0;
  if (state->bound_interface_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 ||
      !valid_invocation(input.invocation, state->bound_worker_id) || !valid_input_tensor(input.uplink_resource_grid) ||
      !spectrum_plan_generation(input, plan_generation)) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  const auto* api = static_cast<const ocudu_dapp_spectrum_interface_v1*>(state->interface_vtable);
  return state->call(
      input.invocation,
      [&]() { return api->publish(state->worker_context, &input); },
      []() { return true; },
      plan_generation);
}

native_invocation_result native_worker::publish(const ocudu_dapp_ul_resource_plan_v1& plan) noexcept
{
  if (state->bound_interface_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 ||
      (state->interface_capabilities & OCUDU_DAPP_SPECTRUM_CAP_FUTURE_UL_PLAN_V1) == 0) {
    return {OCUDU_DAPP_BYPASS_V1, true};
  }
  if (!valid_ul_resource_plan(plan)) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  const auto* api = static_cast<const ocudu_dapp_spectrum_interface_v1*>(state->interface_vtable);
  if (api->publish_plan == nullptr) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  auto invocation_token = state->activity->try_acquire(activity_kind::invocation);
  if (!invocation_token) {
    return {OCUDU_DAPP_BYPASS_V1, true};
  }
  ocudu_dapp_invocation_v1 invocation{};
  invocation.struct_size      = sizeof(invocation);
  invocation.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
  invocation.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
  invocation.invocation_id    = plan.plan_generation;
  invocation.cell_id          = plan.context.cell_id;
  invocation.worker_id        = state->bound_worker_id;
  invocation.radio_time       = plan.planned_time;
  invocation.input_generation = plan.plan_generation;
  const auto guarded          = guarded_call([&]() { return api->publish_plan(state->worker_context, &plan); },
                                    state->incident_source->breaker,
                                    state->incident_source->incidents,
                                    state->make_context(invocation));
  return {guarded.status, guarded.invoke_fallback};
}

native_invocation_result native_worker::publish(const ocudu_dapp_srs_isac_input_v1& input) noexcept
{
  uint32_t resource_id = 0;
  uint32_t config_id   = 0;
  if (state->bound_interface_id != OCUDU_DAPP_INTERFACE_SRS_ISAC_V1 ||
      !valid_invocation(input.invocation, state->bound_worker_id) || !valid_input_tensor(input.channel_estimates) ||
      !srs_source_metadata(input, resource_id, config_id)) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  const auto* api = static_cast<const ocudu_dapp_srs_isac_interface_v1*>(state->interface_vtable);
  return state->call(
      input.invocation,
      [&]() { return api->publish(state->worker_context, &input); },
      []() { return true; },
      0,
      resource_id,
      config_id);
}
