// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/management_service.h"
#include "ocudu/dapp/e3/e3_connection_registry.h"
#include "ocudu/dapp/e3/embedded_agent.h"
#include "ocudu/dapp/e3/portable_data_store.h"
#include "ocudu/ran/du_cell_index.h"
#include "ocudu/scheduler/dapp/quiet_reservation.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <new>
#include <type_traits>

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;
using namespace ocudu::dapp::management;

namespace {

const request_header& header(const management_request& request)
{
  return std::visit([](const auto& operation) -> const request_header& { return operation.header; }, request);
}

management_response failure(const identifier& transaction, result_code code, std::string detail)
{
  return {transaction, code, std::nullopt, std::nullopt, {}, std::move(detail)};
}

management_response failure(const request_header& request, result_code code, std::string detail)
{
  return failure(request.transaction_id, code, std::move(detail));
}

template <typename T>
management_response query_response(const request_header& request, query_result<T> result, management_payload payload)
{
  if (!result) {
    return failure(request, result.code, std::move(result.detail));
  }
  return {request.transaction_id, result_code::ok, std::nullopt, std::nullopt, std::move(payload), {}};
}

management_response submitted(const request_header& request, job_submit_result result)
{
  if (result) {
    return {request.transaction_id, result_code::accepted, result.job_id, std::nullopt, {}, std::move(result.detail)};
  }
  result_code code = result_code::invalid_request;
  switch (result.code) {
    case job_submit_code::transaction_mismatch:
      code = result_code::transaction_mismatch;
      break;
    case job_submit_code::queue_full:
      code = result_code::resource_exhausted;
      break;
    case job_submit_code::stopping:
      code = result_code::lifecycle_failed;
      break;
    case job_submit_code::accepted:
    case job_submit_code::replayed:
    case job_submit_code::invalid_request:
      break;
  }
  return failure(request, code, std::move(result.detail));
}

result_code portable_code(portable_subscription_code code) noexcept
{
  switch (code) {
    case portable_subscription_code::ok:
      return result_code::ok;
    case portable_subscription_code::not_found:
      return result_code::not_found;
    case portable_subscription_code::duplicate:
      return result_code::conflict;
    case portable_subscription_code::resource_exhausted:
    case portable_subscription_code::busy:
      return result_code::resource_exhausted;
    case portable_subscription_code::invalid_request:
      return result_code::invalid_request;
  }
  return result_code::invalid_request;
}

std::optional<management::lifecycle_action> runtime_action(ocudu::dapp::e3::lifecycle_action action)
{
  switch (action) {
    case ocudu::dapp::e3::lifecycle_action::prepare:
      return management::lifecycle_action::prepare;
    case ocudu::dapp::e3::lifecycle_action::arm:
      return management::lifecycle_action::arm;
    case ocudu::dapp::e3::lifecycle_action::enter_shadow:
      return management::lifecycle_action::enter_shadow;
    case ocudu::dapp::e3::lifecycle_action::activate:
      return management::lifecycle_action::activate;
    case ocudu::dapp::e3::lifecycle_action::deactivate:
      return management::lifecycle_action::deactivate;
    case ocudu::dapp::e3::lifecycle_action::retry_quiescence:
      return management::lifecycle_action::retry_quiescence;
    case ocudu::dapp::e3::lifecycle_action::unload:
      return management::lifecycle_action::unload;
    case ocudu::dapp::e3::lifecycle_action::breaker_probe:
      return management::lifecycle_action::breaker_probe;
    case ocudu::dapp::e3::lifecycle_action::load:
      return std::nullopt;
  }
  return std::nullopt;
}

} // namespace

management_response management_service::handle(const management_request& message) noexcept
{
  identifier transaction;
  try {
    if (message.valueless_by_exception()) {
      return failure(transaction, result_code::invalid_request, "management request has no operation");
    }
    const request_header& common = header(message);
    transaction                  = common.transaction_id;
    if (transaction.is_zero()) {
      return failure(common, result_code::invalid_request, "transaction ID is required");
    }
    return std::visit(
        [this](const auto& operation) -> management_response {
          using operation_type = std::decay_t<decltype(operation)>;
          const auto& request  = operation.header;

          if constexpr (std::is_same_v<operation_type, inventory_query>) {
            if (request.instance_id || request.expected_instance_generation || request.expected_inventory_generation ||
                request.dry_run) {
              return failure(request, result_code::invalid_request, "inventory query has unsupported header fields");
            }
            auto snapshot = agent.inventory();
            if (!operation.include_packages) {
              snapshot.packages.clear();
            }
            if (!operation.include_instances) {
              snapshot.instances.clear();
            }
            inventory_result payload;
            payload.inventory = std::move(snapshot);
            if (connections) {
              payload.connections = connections->snapshot();
            }
            return {request.transaction_id, result_code::ok, {}, {}, std::move(payload), {}};
          } else if constexpr (std::is_same_v<operation_type, lifecycle_control>) {
            if (operation.action == lifecycle_action::load) {
              if (!operation.package_id || !operation.placement || !operation.selected_backend || request.instance_id ||
                  request.expected_instance_generation || operation.timeout_us != 1'000'000) {
                return failure(request, result_code::invalid_request, "load requires package, placement, and backend");
              }
              load_request load;
              load.transaction_id                = request.transaction_id;
              load.package_id                    = *operation.package_id;
              load.expected_inventory_generation = request.expected_inventory_generation;
              load.placement                     = *operation.placement;
              load.selected_backend              = *operation.selected_backend;
              load.dry_run                       = request.dry_run;
              return submitted(request, agent.submit(std::move(load)));
            }
            auto action = runtime_action(operation.action);
            if (!request.instance_id || !action || operation.package_id || operation.placement ||
                operation.selected_backend || request.expected_inventory_generation) {
              return failure(request, result_code::invalid_request, "invalid instance lifecycle request");
            }
            lifecycle_request control;
            control.transaction_id      = request.transaction_id;
            control.instance_id         = *request.instance_id;
            control.action              = *action;
            control.expected_generation = request.expected_instance_generation;
            control.dry_run             = request.dry_run;
            control.timeout_us          = operation.timeout_us;
            return submitted(request, agent.submit(std::move(control)));
          } else if constexpr (std::is_same_v<operation_type, config_query>) {
            if (!request.instance_id || request.expected_instance_generation || request.expected_inventory_generation ||
                request.dry_run) {
              return failure(request, result_code::invalid_request, "config query requires an instance");
            }
            if (operation.kind == query_kind::schema) {
              auto          result = agent.config_schema(*request.instance_id);
              config_result payload;
              payload.descriptors = result.value;
              return query_response(request, std::move(result), std::move(payload));
            }
            auto          result = agent.config(*request.instance_id);
            config_result payload;
            payload.generation = result.value.generation;
            payload.entries    = result.value.entries;
            return query_response(request, std::move(result), std::move(payload));
          } else if constexpr (std::is_same_v<operation_type, config_set>) {
            if (!request.instance_id || request.expected_inventory_generation) {
              return failure(request, result_code::invalid_request, "config update requires an instance");
            }
            config_set_request update;
            update.transaction_id               = request.transaction_id;
            update.instance_id                  = *request.instance_id;
            update.expected_instance_generation = request.expected_instance_generation;
            update.expected_config_generation   = operation.expected_config_generation;
            update.boundary                     = operation.boundary;
            update.dry_run                      = request.dry_run;
            update.entries                      = operation.entries;
            return submitted(request, agent.submit(std::move(update)));
          } else if constexpr (std::is_same_v<operation_type, model_control>) {
            if (!request.instance_id || request.expected_inventory_generation || operation.model_id.empty()) {
              return failure(request, result_code::invalid_request, "model control requires instance and model ID");
            }
            model_request model;
            model.transaction_id               = request.transaction_id;
            model.instance_id                  = *request.instance_id;
            model.expected_instance_generation = request.expected_instance_generation;
            model.expected_model_generation    = operation.expected_model_generation;
            model.action                       = operation.action;
            model.model_id                     = operation.model_id;
            model.model_version                = operation.model_version;
            model.artifact                     = operation.artifact;
            model.dry_run                      = request.dry_run;
            return submitted(request, agent.submit(std::move(model)));
          } else if constexpr (std::is_same_v<operation_type, metric_query>) {
            if (!request.instance_id || request.expected_instance_generation || request.expected_inventory_generation ||
                request.dry_run) {
              return failure(request, result_code::invalid_request, "metric query requires an instance");
            }
            if (operation.kind == query_kind::schema) {
              auto          result = agent.metric_schema(*request.instance_id);
              metric_result payload;
              payload.descriptors = result.value;
              return query_response(request, std::move(result), std::move(payload));
            }
            auto          result = agent.metrics(*request.instance_id);
            metric_result payload;
            payload.samples = result.value;
            return query_response(request, std::move(result), std::move(payload));
          } else if constexpr (std::is_same_v<operation_type, incident_query>) {
            if (request.expected_instance_generation || request.expected_inventory_generation || request.dry_run ||
                operation.maximum_records == 0 || operation.maximum_records > 1024) {
              return failure(request, result_code::invalid_request, "incident result bound is invalid");
            }
            if (operation.action == incident_query_action::clear_through) {
              if (!operation.after_incident_id || request.instance_id || operation.cause) {
                return failure(
                    request, result_code::invalid_request, "clear requires only an acknowledged incident ID");
              }
              incident_result payload;
              payload.cleared_count = agent.clear_incidents_through(*operation.after_incident_id);
              return {request.transaction_id, result_code::ok, {}, {}, std::move(payload), {}};
            }
            incident_filter filter;
            if (request.instance_id) {
              std::array<uint64_t, 2> words{};
              std::memcpy(words.data(), request.instance_id->bytes.data(), request.instance_id->bytes.size());
              filter.instance_id = words;
            }
            filter.cause             = operation.cause;
            filter.after_incident_id = operation.after_incident_id;
            filter.maximum_records   = operation.maximum_records;
            incident_result payload;
            payload.incidents = agent.incidents(filter);
            return {request.transaction_id, result_code::ok, {}, {}, std::move(payload), {}};
          } else if constexpr (std::is_same_v<operation_type, health_query>) {
            if (request.instance_id || request.expected_instance_generation || request.expected_inventory_generation ||
                request.dry_run) {
              return failure(request, result_code::invalid_request, "health query has unsupported header fields");
            }
            return {request.transaction_id, result_code::ok, {}, {}, agent.health(), {}};
          } else if constexpr (std::is_same_v<operation_type, job_query>) {
            if (request.instance_id || request.expected_instance_generation || request.expected_inventory_generation ||
                request.dry_run) {
              return failure(request, result_code::invalid_request, "job query has unsupported header fields");
            }
            job_result payload;
            if (operation.action == job_query_action::list) {
              if (operation.job_id) {
                return failure(request, result_code::invalid_request, "job list does not accept a job ID");
              }
              payload.jobs = agent.list_jobs();
            } else {
              if (!operation.job_id) {
                return failure(request, result_code::invalid_request, "job operation requires a job ID");
              }
              auto record = agent.job(*operation.job_id);
              if (!record) {
                return failure(request, result_code::not_found, "job does not exist");
              }
              if (operation.action == job_query_action::cancel && !agent.cancel(*operation.job_id)) {
                return failure(request, result_code::conflict, "job is no longer cancellable");
              }
              if (operation.action == job_query_action::cancel) {
                record = agent.job(*operation.job_id);
              }
              payload.jobs.push_back(*record);
            }
            return {request.transaction_id, result_code::ok, {}, {}, std::move(payload), {}};
          } else if constexpr (std::is_same_v<operation_type, quiet_reservation_query>) {
            if (request.expected_instance_generation || request.expected_inventory_generation || request.dry_run ||
                operation.maximum_records == 0 || operation.maximum_records > 1024 ||
                (operation.cell_id && *operation.cell_id >= MAX_NOF_DU_CELLS)) {
              return failure(request, result_code::invalid_request, "quiet-reservation query is invalid");
            }
            if (!reservations) {
              return failure(request, result_code::incompatible, "quiet-reservation authority is unavailable");
            }

            quiet_reservation_result payload;
            payload.reservations.reserve(operation.maximum_records);
            const auto     now        = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                       std::chrono::steady_clock::now().time_since_epoch())
                                                       .count());
            const uint32_t first_cell = operation.cell_id.value_or(0);
            const uint32_t end_cell   = operation.cell_id ? first_cell + 1 : MAX_NOF_DU_CELLS;
            std::array<ocudu_dapp_quiet_period_reservation_v1, quiet_reservation_max_records> records{};
            for (uint32_t cell_id = first_cell; cell_id != end_cell; ++cell_id) {
              const auto snapshot = reservations->query(cell_id, now, records);
              if (!snapshot.valid_cell) {
                continue;
              }
              if (snapshot.busy) {
                return failure(request, result_code::resource_exhausted, "quiet-reservation snapshot is busy");
              }
              for (uint32_t index = 0; index != snapshot.count; ++index) {
                management::identifier source;
                std::memcpy(source.bytes.data(), records[index].request_source_instance_id, source.bytes.size());
                if (request.instance_id && source != *request.instance_id) {
                  continue;
                }
                if (payload.reservations.size() == operation.maximum_records) {
                  payload.truncated = true;
                  break;
                }
                payload.reservations.push_back({cell_id, records[index]});
              }
              if (payload.truncated) {
                break;
              }
            }
            return {request.transaction_id, result_code::ok, {}, {}, std::move(payload), {}};
          } else if constexpr (std::is_same_v<operation_type, data_subscription_control>) {
            if (request.instance_id || request.expected_instance_generation || request.expected_inventory_generation ||
                request.dry_run || !portable_data) {
              return failure(request,
                             portable_data ? result_code::invalid_request : result_code::incompatible,
                             portable_data ? "data subscription has unsupported header fields"
                                           : "portable E3 data profile is unavailable");
            }
            portable_subscription_result result;
            if (operation.action == data_subscription_action::list) {
              if (operation.subscription_id || operation.interface_id != 0 || operation.cell_id ||
                  operation.sample_every_n != 1 || operation.queue_depth_chunks != 64 ||
                  operation.maximum_chunk_bytes != 64 * 1024 || operation.lease_us != 1'000'000 ||
                  operation.drop_policy != portable_drop_policy::drop_newest) {
                return failure(request, result_code::invalid_request, "data subscription list has extra fields");
              }
              result = portable_data->subscriptions();
            } else if (operation.action == data_subscription_action::remove) {
              if (!operation.subscription_id || operation.interface_id != 0 || operation.cell_id ||
                  operation.sample_every_n != 1 || operation.queue_depth_chunks != 64 ||
                  operation.maximum_chunk_bytes != 64 * 1024 || operation.lease_us != 1'000'000 ||
                  operation.drop_policy != portable_drop_policy::drop_newest) {
                return failure(request, result_code::invalid_request, "data subscription removal is invalid");
              }
              result = portable_data->unsubscribe(*operation.subscription_id);
            } else {
              if (!operation.subscription_id || operation.interface_id == 0) {
                return failure(request,
                               result_code::invalid_request,
                               "data subscription creation requires subscription and interface IDs");
              }
              portable_subscription_request create;
              create.subscription_id     = *operation.subscription_id;
              create.interface_id        = operation.interface_id;
              create.cell_id             = operation.cell_id;
              create.sample_every_n      = operation.sample_every_n;
              create.queue_depth_chunks  = operation.queue_depth_chunks;
              create.maximum_chunk_bytes = operation.maximum_chunk_bytes;
              create.lease_us            = operation.lease_us;
              create.drop_policy         = operation.drop_policy;
              result                     = portable_data->subscribe(create);
            }
            if (!result) {
              return failure(request, portable_code(result.code), std::move(result.detail));
            }
            return {request.transaction_id,
                    result_code::ok,
                    {},
                    {},
                    data_subscription_result{std::move(result.subscriptions)},
                    {}};
          } else if constexpr (std::is_same_v<operation_type, data_poll_query>) {
            if (request.instance_id || request.expected_instance_generation || request.expected_inventory_generation ||
                request.dry_run || operation.subscription_id.is_zero() || operation.maximum_chunks == 0 ||
                operation.maximum_chunks > 8 || !portable_data) {
              return failure(request,
                             portable_data ? result_code::invalid_request : result_code::incompatible,
                             portable_data ? "portable E3 data poll is invalid"
                                           : "portable E3 data profile is unavailable");
            }
            auto result = portable_data->poll(operation.subscription_id, operation.maximum_chunks);
            if (!result) {
              return failure(request, portable_code(result.code), std::move(result.detail));
            }
            return {request.transaction_id,
                    result_code::ok,
                    {},
                    {},
                    data_publication_result{std::move(result.chunks), std::move(result.subscription)},
                    {}};
          } else if constexpr (std::is_same_v<operation_type, cell_info_query>) {
            if (request.instance_id || request.expected_instance_generation || request.expected_inventory_generation) {
              return failure(request, result_code::invalid_request, "cell info query takes no instance or generation");
            }
            cell_info_result payload;
            if (ran_actions) {
              payload.cells        = ran_actions->cells();
              payload.capabilities = ran_actions->capabilities();
            }
            return {request.transaction_id, result_code::ok, {}, {}, std::move(payload), {}};
          } else if constexpr (std::is_same_v<operation_type, ran_control>) {
            if (request.instance_id || request.expected_instance_generation || request.expected_inventory_generation ||
                operation.serving_pci > 1007) {
              return failure(request, result_code::invalid_request, "invalid RAN control request");
            }
            const bool ue_action = operation.action == ran_control_action::release_ue_to_idle ||
                                   operation.action == ran_control_action::handover_ue;
            const bool gain_action = operation.action == ran_control_action::set_tx_gain ||
                                     operation.action == ran_control_action::set_rx_gain;
            if (!ue_action && !gain_action) {
              return failure(request, result_code::invalid_request, "unknown RAN control action");
            }
            if (ue_action && operation.rnti == 0) {
              return failure(request, result_code::invalid_request, "RAN control on a UE requires its RNTI");
            }
            if (operation.action == ran_control_action::handover_ue &&
                (!operation.target_pci || *operation.target_pci > 1007 ||
                 *operation.target_pci == operation.serving_pci)) {
              return failure(request, result_code::invalid_request, "handover requires a different valid target PCI");
            }
            if (gain_action && (!operation.gain_db || !std::isfinite(*operation.gain_db) ||
                                *operation.gain_db < -30.0 || *operation.gain_db > 90.0)) {
              return failure(
                  request, result_code::invalid_request, "gain control requires a finite gain in -30..90 dB");
            }
            if (!ran_actions) {
              return failure(request, result_code::incompatible, "RAN control authority is unavailable");
            }
            const auto  capabilities = ran_actions->capabilities();
            const char* verb         = "UE release";
            bool        supported    = capabilities.ue_release;
            switch (operation.action) {
              case ran_control_action::handover_ue:
                verb      = "UE handover";
                supported = capabilities.handover;
                break;
              case ran_control_action::set_tx_gain:
                verb      = "TX gain change";
                supported = capabilities.tx_gain;
                break;
              case ran_control_action::set_rx_gain:
                verb      = "RX gain change";
                supported = capabilities.rx_gain;
                break;
              default:
                break;
            }
            if (!supported) {
              return failure(request, result_code::incompatible, std::string(verb) + " is not available on this host");
            }
            management::operation_result result;
            result.transaction_id = request.transaction_id;
            if (request.dry_run) {
              result.code   = result_code::dry_run_ok;
              result.detail = std::string(verb) + " request is valid and the host authority is available";
              return {request.transaction_id, result.code, {}, std::move(result), {}, {}};
            }
            bool accepted = false;
            switch (operation.action) {
              case ran_control_action::release_ue_to_idle:
                accepted = ran_actions->request_ue_release(operation.serving_pci, operation.rnti);
                break;
              case ran_control_action::handover_ue:
                accepted = ran_actions->request_handover(operation.serving_pci, operation.rnti, *operation.target_pci);
                break;
              case ran_control_action::set_tx_gain:
                accepted = ran_actions->set_tx_gain(operation.port_id.value_or(0), *operation.gain_db);
                break;
              case ran_control_action::set_rx_gain:
                accepted = ran_actions->set_rx_gain(operation.port_id.value_or(0), *operation.gain_db);
                break;
            }
            if (!accepted) {
              return failure(request, result_code::resource_exhausted, std::string(verb) + " was rejected by the host");
            }
            result.code   = result_code::accepted;
            result.detail = gain_action ? std::string(verb) + " applied on the radio unit"
                                        : std::string(verb) + " accepted for dispatch on the CU-CP control executor";
            return {request.transaction_id, result.code, {}, std::move(result), {}, {}};
          }
        },
        message);
  } catch (const std::bad_alloc&) {
    return failure(transaction, result_code::resource_exhausted, "management request allocation failed");
  } catch (const std::exception& exception) {
    return failure(transaction, result_code::lifecycle_failed, exception.what());
  } catch (...) {
    return failure(transaction, result_code::lifecycle_failed, "unknown management service failure");
  }
}
