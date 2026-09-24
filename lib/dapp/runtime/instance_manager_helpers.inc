// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// Shared private helpers for instance_manager TUs (included inside anonymous namespace).

[[maybe_unused]] std::array<uint64_t, 2> words(const identifier& id) noexcept
{
  std::array<uint64_t, 2> result{};
  std::memcpy(result.data(), id.bytes.data(), id.bytes.size());
  return result;
}

[[maybe_unused]] bool supports_backend(const package_record& package, backend selected)
{
  return std::find(package.backends.begin(), package.backends.end(), selected) != package.backends.end();
}

[[maybe_unused]] bool host_supports_backend(uint64_t mask, backend selected) noexcept
{
  if (mask == 0) {
    return true;
  }
  constexpr uint64_t host = uint64_t{1} << OCUDU_DAPP_BACKEND_HOST_V1;
  constexpr uint64_t cuda = uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1;
  switch (selected) {
    case backend::cpu:
      return (mask & host) != 0;
    case backend::cuda:
      return (mask & cuda) != 0;
    case backend::other_accelerator:
      return (mask & ~(host | cuda)) != 0;
  }
  return false;
}

[[maybe_unused]] std::optional<uint32_t> abi_backend(backend selected) noexcept
{
  switch (selected) {
    case backend::cpu:
      return OCUDU_DAPP_BACKEND_HOST_V1;
    case backend::cuda:
      return OCUDU_DAPP_BACKEND_CUDA_V1;
    case backend::other_accelerator:
      return std::nullopt;
  }
  return std::nullopt;
}

[[maybe_unused]] bool same_interfaces(const std::vector<interface_record>& left,
                     const std::vector<interface_record>& right,
                     bool                                 compare_binding_policy = true) noexcept
{
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index != left.size(); ++index) {
    if (left[index].interface_id != right[index].interface_id ||
        left[index].interface_major != right[index].interface_major ||
        left[index].interface_minor != right[index].interface_minor ||
        left[index].dapp_class != right[index].dapp_class ||
        left[index].placement_mask != right[index].placement_mask || left[index].threading != right[index].threading ||
        left[index].capability_bits != right[index].capability_bits || left[index].pusch != right[index].pusch ||
        left[index].srs != right[index].srs ||
        (compare_binding_policy && left[index].requirement != right[index].requirement)) {
      return false;
    }
  }
  return true;
}

[[maybe_unused]] bool action_valid_for_state(lifecycle_action action, ocudu_dapp_lifecycle_state_v1 state) noexcept
{
  switch (action) {
    case lifecycle_action::prepare:
      return state == OCUDU_DAPP_STATE_LOADED_V1;
    case lifecycle_action::arm:
      // inactive→armed: re-activate without unload/destroy (safer for CUDA dApps).
      return state == OCUDU_DAPP_STATE_PREPARED_V1 || state == OCUDU_DAPP_STATE_INACTIVE_V1;
    case lifecycle_action::enter_shadow:
      return state == OCUDU_DAPP_STATE_ARMED_V1;
    case lifecycle_action::activate:
      return state == OCUDU_DAPP_STATE_SHADOW_V1;
    case lifecycle_action::deactivate:
      return state == OCUDU_DAPP_STATE_ACTIVE_V1 || state == OCUDU_DAPP_STATE_SHADOW_V1 ||
             state == OCUDU_DAPP_STATE_DEGRADED_V1 || state == OCUDU_DAPP_STATE_ERROR_V1;
    case lifecycle_action::retry_quiescence:
      return state == OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1;
    case lifecycle_action::unload:
      return state == OCUDU_DAPP_STATE_INACTIVE_V1 || state == OCUDU_DAPP_STATE_LOADED_V1;
    case lifecycle_action::breaker_probe:
      return state != OCUDU_DAPP_STATE_UNLOADED_V1 && state != OCUDU_DAPP_STATE_DISCOVERED_V1;
  }
  return false;
}

[[maybe_unused]] bool identifier_less(const identifier& left, const identifier& right) noexcept
{
  return left.bytes < right.bytes;
}

constexpr uint32_t    maximum_management_items  = 1024;
constexpr uint32_t    maximum_management_blob   = 1024 * 1024;
constexpr std::size_t maximum_management_string = 4096;

[[maybe_unused]] std::optional<std::string> copy_string(const char* source)
{
  if (source == nullptr) {
    return std::nullopt;
  }
  const std::size_t size = strnlen(source, maximum_management_string + 1);
  if (size > maximum_management_string) {
    return std::nullopt;
  }
  return std::string(source, size);
}

[[maybe_unused]] std::optional<value> copy_value(const ocudu_dapp_value_v1& source)
{
  switch (source.kind) {
    case 0:
      return std::nullopt;
    case OCUDU_DAPP_VALUE_BOOL_V1:
      if (source.value.boolean > 1) {
        return std::nullopt;
      }
      return value{source.value.boolean != 0};
    case OCUDU_DAPP_VALUE_INT64_V1:
      return value{source.value.int64_value};
    case OCUDU_DAPP_VALUE_UINT64_V1:
      return value{source.value.uint64_value};
    case OCUDU_DAPP_VALUE_DOUBLE_V1:
      return value{source.value.double_value};
    case OCUDU_DAPP_VALUE_STRING_V1: {
      if (source.value.blob.size != 0 && source.value.blob.data == nullptr) {
        return std::nullopt;
      }
      if (source.value.blob.size > maximum_management_string) {
        return std::nullopt;
      }
      const auto* data = static_cast<const char*>(source.value.blob.data);
      return value{std::string(data != nullptr ? data : "", source.value.blob.size)};
    }
    case OCUDU_DAPP_VALUE_BYTES_V1: {
      if (source.value.blob.size > maximum_management_blob ||
          (source.value.blob.size != 0 && source.value.blob.data == nullptr)) {
        return std::nullopt;
      }
      const auto* begin = static_cast<const uint8_t*>(source.value.blob.data);
      return value{source.value.blob.size == 0 ? std::vector<uint8_t>{}
                                               : std::vector<uint8_t>(begin, begin + source.value.blob.size)};
    }
    default:
      return std::nullopt;
  }
}

[[maybe_unused]] ocudu_dapp_value_v1 borrow_value(const value& source)
{
  ocudu_dapp_value_v1 result{};
  std::visit(
      [&result](const auto& item) {
        using item_type = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<item_type, bool>) {
          result.kind          = OCUDU_DAPP_VALUE_BOOL_V1;
          result.value.boolean = item;
        } else if constexpr (std::is_same_v<item_type, int64_t>) {
          result.kind              = OCUDU_DAPP_VALUE_INT64_V1;
          result.value.int64_value = item;
        } else if constexpr (std::is_same_v<item_type, uint64_t>) {
          result.kind               = OCUDU_DAPP_VALUE_UINT64_V1;
          result.value.uint64_value = item;
        } else if constexpr (std::is_same_v<item_type, double>) {
          result.kind               = OCUDU_DAPP_VALUE_DOUBLE_V1;
          result.value.double_value = item;
        } else if constexpr (std::is_same_v<item_type, std::string>) {
          result.kind            = OCUDU_DAPP_VALUE_STRING_V1;
          result.value.blob.data = item.data();
          result.value.blob.size = item.size();
        } else {
          result.kind            = OCUDU_DAPP_VALUE_BYTES_V1;
          result.value.blob.data = item.data();
          result.value.blob.size = item.size();
        }
      },
      source);
  return result;
}

result_code map_status(ocudu_dapp_status_v1 status) noexcept
{
  switch (status) {
    case OCUDU_DAPP_OK_V1:
      return result_code::ok;
    case OCUDU_DAPP_CONFLICT_V1:
      return result_code::conflict;
    case OCUDU_DAPP_UNSUPPORTED_V1:
      return result_code::incompatible;
    case OCUDU_DAPP_INVALID_INPUT_V1:
    case OCUDU_DAPP_INVALID_OUTPUT_V1:
      return result_code::invalid_request;
    case OCUDU_DAPP_RESOURCE_V1:
      return result_code::resource_exhausted;
    default:
      return result_code::lifecycle_failed;
  }
}

template <typename Native, typename Owned, typename Invoke, typename Convert>
query_result<std::vector<Owned>> collect_bounded(Invoke&& invoke, Convert&& convert, const char* invalid_detail)
{
  auto sized = invoke(static_cast<Native*>(nullptr), 0);
  if (!sized) {
    return {map_status(sized.operation.status), {}, sized.operation.detail};
  }
  if (sized.count > maximum_management_items) {
    return {result_code::resource_exhausted, {}, "module result exceeds management item limit"};
  }
  std::vector<Native> native(sized.count);
  auto                read = invoke(native.data(), native.size());
  if (!read) {
    return {map_status(read.operation.status), {}, read.operation.detail};
  }
  if (read.count != native.size()) {
    return {result_code::conflict, {}, "module result changed during snapshot"};
  }
  std::vector<Owned> result;
  result.reserve(native.size());
  for (const auto& item : native) {
    auto converted = convert(item);
    if (!converted) {
      return {result_code::invalid_request, {}, invalid_detail};
    }
    result.push_back(std::move(*converted));
  }
  return {result_code::ok, std::move(result), {}};
}

[[maybe_unused]] bool has_class_b_scheduler(const std::vector<interface_record>& interfaces) noexcept
{
  return std::any_of(interfaces.begin(), interfaces.end(), [](const auto& interface) {
    return interface.interface_id == OCUDU_DAPP_INTERFACE_SCHEDULER_V1 &&
           interface.dapp_class == OCUDU_DAPP_CLASS_B_DEADLINE_V1;
  });
}

[[maybe_unused]] bool has_class_b_scheduler(const instance_record& record) noexcept
{
  return has_class_b_scheduler(record.interfaces);
}

[[maybe_unused]] bool has_class_a_interface(const instance_record& record) noexcept
{
  return std::any_of(record.interfaces.begin(), record.interfaces.end(), [](const auto& interface) {
    return interface.dapp_class == OCUDU_DAPP_CLASS_A_INLINE_V1;
  });
}

[[maybe_unused]] bool has_class_a_interface(const package_record& package) noexcept
{
  return std::any_of(package.interfaces.begin(), package.interfaces.end(), [](const auto& interface) {
    return interface.dapp_class == OCUDU_DAPP_CLASS_A_INLINE_V1;
  });
}

/// Class-A resident hooks are path-specific: host adapters only invoke HOST
/// backend packages; the accelerated PUSCH and SRS paths only invoke CUDA packages.
/// When the host advertises CUDA in backend_mask, acceleration is enabled and
/// the CUDA Class-A path is primary — loading a CPU Class-A package would
/// activate lifecycle successfully but never receive invokes (silent dead-path).
/// Reject that combination so operators get a clear incompatible result.
[[maybe_unused]] const char* class_a_l1_path_incompatibility(const package_record& package,
                                            backend               selected,
                                            uint64_t              host_backend_mask) noexcept
{
  if (!has_class_a_interface(package) || host_backend_mask == 0) {
    // backend_mask==0 means unrestricted (unit-test / pre-configure defaults).
    return nullptr;
  }
  constexpr uint64_t host_bit = uint64_t{1} << OCUDU_DAPP_BACKEND_HOST_V1;
  constexpr uint64_t cuda_bit = uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1;
  const bool         cuda_l1  = (host_backend_mask & cuda_bit) != 0;
  const bool         host_l1  = (host_backend_mask & host_bit) != 0;
  // When CUDA is advertised, the accelerated Class-A path is primary and
  // only CUDA-backend packages receive invokes on that path.
  if (selected == backend::cpu && cuda_l1) {
    return "Class-A cpu backend is incompatible with the active accelerated L1 path "
           "(pusch_acceleration enables CUDA Class-A); use backend=cuda or disable acceleration";
  }
  if (selected == backend::cuda && !cuda_l1) {
    return "Class-A cuda backend is incompatible with the active host L1 path "
           "(pusch_acceleration disabled or CUDA unavailable); use backend=cpu or enable acceleration";
  }
  if (selected == backend::cpu && !host_l1) {
    return "Class-A cpu backend requires host L1 path support in host backend_mask";
  }
  return nullptr;
}

struct class_a_metric_definition {
  const char* name;
  const char* description;
};

constexpr std::array<class_a_metric_definition, nof_class_a_fallback_reasons> class_a_metrics{{
    {OCUDU_DAPP_METRIC_CLASS_A_ADMISSION_CLOSED_V1, "Invocation admission was closed during lifecycle transition"},
    {OCUDU_DAPP_METRIC_CLASS_A_BREAKER_OPEN_V1, "The per-lane circuit breaker was open"},
    {OCUDU_DAPP_METRIC_CLASS_A_OVERLAPPED_HOOK_V1, "A deeper mutually exclusive Class-A hook owned the boundary"},
    {OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_METADATA_V1, "Required PUSCH or DM-RS metadata was incomplete"},
    {OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_LAYERS_V1, "PUSCH layer count was outside the admitted profile"},
    {OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_RX_PORTS_V1, "Receive-port count was outside the admitted profile"},
    {OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_ALLOCATION_V1, "PUSCH time-frequency allocation was unsupported"},
    {OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_MODULATION_V1, "Modulation was outside the admitted profile"},
    {OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_DMRS_V1, "DM-RS type or pilot structure was unsupported"},
    {OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_TP_V1, "Transform precoding was outside the admitted profile"},
    {OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_DC_V1, "DC-overlapping allocation was outside the admitted profile"},
    {OCUDU_DAPP_METRIC_CLASS_A_MODULE_BYPASS_V1, "Module explicitly requested conventional bypass"},
    {OCUDU_DAPP_METRIC_CLASS_A_MODULE_UNSUPPORTED_V1, "Module returned unsupported after host admission"},
    {OCUDU_DAPP_METRIC_CLASS_A_INVALID_INPUT_V1, "Host or module rejected invocation input"},
    {OCUDU_DAPP_METRIC_CLASS_A_INVALID_OUTPUT_V1, "Host rejected module output"},
    {OCUDU_DAPP_METRIC_CLASS_A_DEADLINE_V1, "Module result missed the invocation deadline"},
    {OCUDU_DAPP_METRIC_CLASS_A_EXCEPTION_V1, "Module callback threw an exception"},
    {OCUDU_DAPP_METRIC_CLASS_A_DEVICE_V1, "Module reported an accelerator device failure"},
    {OCUDU_DAPP_METRIC_CLASS_A_RESOURCE_V1, "Module or adapter reported bounded resource exhaustion"},
    {OCUDU_DAPP_METRIC_CLASS_A_RETURNED_ERROR_V1, "Module returned another failure status"},
}};

[[maybe_unused]] std::vector<metric_descriptor> host_metric_schema(bool class_a, bool class_b)
{
  constexpr auto                 uint_kind = OCUDU_DAPP_VALUE_UINT64_V1;
  constexpr auto                 gauge     = OCUDU_DAPP_METRIC_GAUGE_V1;
  constexpr auto                 counter   = OCUDU_DAPP_METRIC_COUNTER_V1;
  std::vector<metric_descriptor> result{
      {OCUDU_DAPP_METRIC_HOST_LIFECYCLE_STATE_V1, "Host lifecycle state", "state", uint_kind, gauge},
      {OCUDU_DAPP_METRIC_HOST_BREAKER_STATE_V1, "Host circuit-breaker state", "state", uint_kind, gauge},
      {OCUDU_DAPP_METRIC_HOST_BREAKER_FAILURES_V1, "Consecutive host-observed failures", "failures", uint_kind, gauge},
      {OCUDU_DAPP_METRIC_HOST_ACTIVITY_INVOCATIONS_V1, "Outstanding invocations", "operations", uint_kind, gauge},
      {OCUDU_DAPP_METRIC_HOST_ACTIVITY_LEASES_V1, "Outstanding buffer leases", "leases", uint_kind, gauge},
      {OCUDU_DAPP_METRIC_HOST_ACTIVITY_CALLBACKS_V1, "Outstanding callbacks", "callbacks", uint_kind, gauge},
      {OCUDU_DAPP_METRIC_HOST_ACTIVITY_ACCELERATOR_V1, "Outstanding accelerator work", "operations", uint_kind, gauge},
      {OCUDU_DAPP_METRIC_HOST_INPUT_CELL_BITMAP_V1,
       "DU cells whose valid input reached this instance; bit N represents cell N",
       "bitmap",
       uint_kind,
       gauge},
      {OCUDU_DAPP_METRIC_HOST_NATIVE_WORKERS_V1,
       "Native worker contexts bound to this instance; zero for process placement",
       "workers",
       uint_kind,
       gauge},
      {OCUDU_DAPP_METRIC_HOST_NATIVE_WORKERS_ACTIVE_V1,
       "Native worker contexts that observed at least one valid DU input",
       "workers",
       uint_kind,
       gauge},
      {OCUDU_DAPP_METRIC_HOST_INCIDENTS_DROPPED_V1,
       "Incidents dropped by the bounded RT ring",
       "incidents",
       uint_kind,
       counter}};
  if (class_a) {
    for (const auto& metric : class_a_metrics) {
      result.push_back({metric.name, metric.description, "fallbacks", uint_kind, counter});
    }
    result.push_back({OCUDU_DAPP_METRIC_CLASS_A_COMPLETION_DEADLINE_MISSES_V1,
                      "Resident accelerator output completed after its declared deadline",
                      "misses",
                      uint_kind,
                      counter});
  }
  if (class_b) {
    result.insert(result.end(),
                  {{OCUDU_DAPP_METRIC_CLASS_B_INVOCATIONS_V1,
                    "Class-B policy invocations measured by the host",
                    "invocations",
                    uint_kind,
                    counter},
                   {OCUDU_DAPP_METRIC_CLASS_B_CONVENTIONAL_TIME_NS_V1,
                    "Cumulative conventional policy execution time before Class-B evaluation",
                    "ns",
                    uint_kind,
                    counter},
                   {OCUDU_DAPP_METRIC_CLASS_B_FEATURE_TIME_NS_V1,
                    "Cumulative Class-B feature, context, and output preparation time",
                    "ns",
                    uint_kind,
                    counter},
                   {OCUDU_DAPP_METRIC_CLASS_B_DAPP_TIME_NS_V1,
                    "Cumulative guarded Class-B dApp execution time",
                    "ns",
                    uint_kind,
                    counter},
                   {OCUDU_DAPP_METRIC_CLASS_B_VALIDATION_TIME_NS_V1,
                    "Cumulative host Class-B output validation time",
                    "ns",
                    uint_kind,
                    counter},
                   {OCUDU_DAPP_METRIC_CLASS_B_COMMIT_TIME_NS_V1,
                    "Cumulative Class-B output commit or late-result rollback time",
                    "ns",
                    uint_kind,
                    counter},
                   {OCUDU_DAPP_METRIC_CLASS_B_TOTAL_TIME_NS_V1,
                    "Cumulative feature-to-commit Class-B policy-boundary time",
                    "ns",
                    uint_kind,
                    counter},
                   {OCUDU_DAPP_METRIC_CLASS_B_ACCEPTED_V1,
                    "Validated Class-B outputs applied by host authority",
                    "outputs",
                    uint_kind,
                    counter},
                   {OCUDU_DAPP_METRIC_CLASS_B_EVALUATION_ACCEPTED_V1,
                    "Validated Class-B outputs discarded in evaluation-only mode",
                    "outputs",
                    uint_kind,
                    counter},
                   {OCUDU_DAPP_METRIC_CLASS_B_FALLBACKS_V1,
                    "Class-B invocations retaining complete conventional fallback",
                    "fallbacks",
                    uint_kind,
                    counter},
                   {OCUDU_DAPP_METRIC_CLASS_B_VALIDATION_REJECTIONS_V1,
                    "Class-B outputs rejected by host validation",
                    "outputs",
                    uint_kind,
                    counter},
                   {OCUDU_DAPP_METRIC_CLASS_B_DEADLINE_MISSES_V1,
                    "Class-B invocations rejected after their host deadline",
                    "misses",
                    uint_kind,
                    counter}});
  }
  return result;
}

[[maybe_unused]] std::vector<metric_sample> host_metric_samples(const instance_health_snapshot& health, bool class_a, bool class_b)
{
  const auto timestamp = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
  std::vector<metric_sample> result{
      {OCUDU_DAPP_METRIC_HOST_LIFECYCLE_STATE_V1, uint64_t{health.state}, timestamp},
      {OCUDU_DAPP_METRIC_HOST_BREAKER_STATE_V1, static_cast<uint64_t>(health.breaker), timestamp},
      {OCUDU_DAPP_METRIC_HOST_BREAKER_FAILURES_V1, uint64_t{health.consecutive_failures}, timestamp},
      {OCUDU_DAPP_METRIC_HOST_ACTIVITY_INVOCATIONS_V1, uint64_t{health.activity.invocations}, timestamp},
      {OCUDU_DAPP_METRIC_HOST_ACTIVITY_LEASES_V1, uint64_t{health.activity.leases}, timestamp},
      {OCUDU_DAPP_METRIC_HOST_ACTIVITY_CALLBACKS_V1, uint64_t{health.activity.callbacks}, timestamp},
      {OCUDU_DAPP_METRIC_HOST_ACTIVITY_ACCELERATOR_V1, uint64_t{health.activity.accelerator_work}, timestamp},
      {OCUDU_DAPP_METRIC_HOST_INPUT_CELL_BITMAP_V1, uint64_t{health.input_cell_bitmap}, timestamp},
      {OCUDU_DAPP_METRIC_HOST_NATIVE_WORKERS_V1, uint64_t{health.native_workers}, timestamp},
      {OCUDU_DAPP_METRIC_HOST_NATIVE_WORKERS_ACTIVE_V1, uint64_t{health.active_native_workers}, timestamp},
      {OCUDU_DAPP_METRIC_HOST_INCIDENTS_DROPPED_V1, health.dropped_incidents, timestamp}};
  if (class_a) {
    for (std::size_t index = 0; index != class_a_metrics.size(); ++index) {
      result.push_back({class_a_metrics[index].name, health.class_a_fallbacks.reasons[index], timestamp});
    }
    result.push_back({OCUDU_DAPP_METRIC_CLASS_A_COMPLETION_DEADLINE_MISSES_V1,
                      health.class_a_completion.deadline_misses,
                      timestamp});
  }
  if (class_b) {
    const auto& policy = health.class_b_policy;
    result.insert(result.end(),
                  {{OCUDU_DAPP_METRIC_CLASS_B_INVOCATIONS_V1, policy.invocations, timestamp},
                   {OCUDU_DAPP_METRIC_CLASS_B_CONVENTIONAL_TIME_NS_V1, policy.conventional_time_ns, timestamp},
                   {OCUDU_DAPP_METRIC_CLASS_B_FEATURE_TIME_NS_V1, policy.feature_time_ns, timestamp},
                   {OCUDU_DAPP_METRIC_CLASS_B_DAPP_TIME_NS_V1, policy.dapp_time_ns, timestamp},
                   {OCUDU_DAPP_METRIC_CLASS_B_VALIDATION_TIME_NS_V1, policy.validation_time_ns, timestamp},
                   {OCUDU_DAPP_METRIC_CLASS_B_COMMIT_TIME_NS_V1, policy.commit_time_ns, timestamp},
                   {OCUDU_DAPP_METRIC_CLASS_B_TOTAL_TIME_NS_V1, policy.total_time_ns, timestamp},
                   {OCUDU_DAPP_METRIC_CLASS_B_ACCEPTED_V1, policy.accepted, timestamp},
                   {OCUDU_DAPP_METRIC_CLASS_B_EVALUATION_ACCEPTED_V1, policy.evaluation_accepted, timestamp},
                   {OCUDU_DAPP_METRIC_CLASS_B_FALLBACKS_V1, policy.fallbacks, timestamp},
                   {OCUDU_DAPP_METRIC_CLASS_B_VALIDATION_REJECTIONS_V1, policy.validation_rejections, timestamp},
                   {OCUDU_DAPP_METRIC_CLASS_B_DEADLINE_MISSES_V1, policy.deadline_misses, timestamp}});
  }
  return result;
}

