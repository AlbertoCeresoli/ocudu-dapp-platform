// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/process/v1/control.h"
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <string_view>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

bool install_test_seccomp_filter()
{
  sock_filter instructions[]{{static_cast<uint16_t>(BPF_RET | BPF_K), 0, 0, SECCOMP_RET_ALLOW}};
  sock_fprog  program{1, instructions};
  return ::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) == 0;
}

struct descriptor_store {
  std::array<int, OCUDU_DAPP_MAX_PRODUCER_SHARDS_V1 * OCUDU_DAPP_ROUTE_DESCRIPTOR_COUNT_V1> values{};
  std::size_t                                                                               count{};

  descriptor_store() { values.fill(-1); }
  ~descriptor_store()
  {
    for (std::size_t index = 0; index != count; ++index) {
      ::close(values[index]);
    }
  }

  bool append(const int* descriptors, std::size_t descriptor_count)
  {
    if (descriptor_count > values.size() - count) {
      return false;
    }
    for (std::size_t index = 0; index != descriptor_count; ++index) {
      values[count++] = descriptors[index];
    }
    return true;
  }
};

struct received_offer {
  ocudu_dapp_control_header_v1 header{};
  ocudu_dapp_route_offer_v1    offer{};
};

bool receive_offer(int control_fd, pid_t expected_pid, descriptor_store& descriptors, received_offer& result)
{
  std::array<uint8_t, sizeof(result.header) + sizeof(result.offer)> data{};
  alignas(cmsghdr)
      std::array<uint8_t, CMSG_SPACE(OCUDU_DAPP_CONTROL_MAX_DESCRIPTORS_V1 * sizeof(int)) + CMSG_SPACE(sizeof(ucred))>
          control{};
  iovec   vector{data.data(), data.size()};
  msghdr  message{};
  message.msg_iov        = &vector;
  message.msg_iovlen     = 1;
  message.msg_control    = control.data();
  message.msg_controllen = control.size();
  const ssize_t received = ::recvmsg(control_fd, &message, MSG_CMSG_CLOEXEC);
  if (received != static_cast<ssize_t>(data.size()) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
    return false;
  }

  ucred             credentials{};
  bool              have_credentials   = false;
  const std::size_t descriptors_before = descriptors.count;
  for (auto* item = CMSG_FIRSTHDR(&message); item != nullptr; item = CMSG_NXTHDR(&message, item)) {
    if (item->cmsg_level != SOL_SOCKET) {
      continue;
    }
    if (item->cmsg_type == SCM_CREDENTIALS && item->cmsg_len == CMSG_LEN(sizeof(credentials))) {
      std::memcpy(&credentials, CMSG_DATA(item), sizeof(credentials));
      have_credentials = true;
    } else if (item->cmsg_type == SCM_RIGHTS && item->cmsg_len >= CMSG_LEN(sizeof(int))) {
      const std::size_t count = (item->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      if (!descriptors.append(reinterpret_cast<const int*>(CMSG_DATA(item)), count)) {
        return false;
      }
    }
  }
  std::memcpy(&result.header, data.data(), sizeof(result.header));
  std::memcpy(&result.offer, data.data() + sizeof(result.header), sizeof(result.offer));
  if (!have_credentials || credentials.pid != expected_pid || credentials.uid != ::geteuid() ||
      descriptors.count - descriptors_before != OCUDU_DAPP_ROUTE_DESCRIPTOR_COUNT_V1 ||
      result.header.magic != OCUDU_DAPP_CONTROL_MAGIC_V1 || result.header.version != OCUDU_DAPP_CONTROL_VERSION_V1 ||
      result.header.type != OCUDU_DAPP_CONTROL_ROUTE_OFFER_V1 || result.header.payload_size != sizeof(result.offer) ||
      result.header.descriptor_count != OCUDU_DAPP_ROUTE_DESCRIPTOR_COUNT_V1 || result.header.reserved != 0 ||
      result.header.sequence == 0 || result.offer.struct_size != sizeof(result.offer) ||
      result.offer.interface_id == 0 || result.offer.metadata_schema_id == 0 ||
      result.offer.metadata_schema_version == 0 || result.offer.route_id == 0 ||
      (result.offer.flags != 0 && (result.offer.interface_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 ||
                                   result.offer.flags != OCUDU_DAPP_ROUTE_FLAG_UL_RESOURCE_PLAN_V1)) ||
      result.offer.producer_shard_count == 0 || result.offer.producer_shard_count > OCUDU_DAPP_MAX_PRODUCER_SHARDS_V1 ||
      result.offer.producer_shard_id >= result.offer.producer_shard_count) {
    return false;
  }
  return result.offer.transport_kind == OCUDU_DAPP_ROUTE_TRANSPORT_CPU_SHARED_RING_V1 && result.offer.backend_id == 0 &&
         result.offer.export_slot_count == 0 && result.offer.reserved0 == 0 && result.offer.export_slot_capacity == 0 &&
         result.offer.reserved == 0;
}

bool send_ready(int                   control_fd,
                const received_offer& reference,
                bool                  heartbeat_capability,
                bool                  parent_death_capability)
{
  ocudu_dapp_route_ready_v1 ready{};
  ready.struct_size          = sizeof(ready);
  ready.interface_id         = reference.offer.interface_id;
  ready.route_generation     = reference.header.sequence;
  ready.producer_shard_count = reference.offer.producer_shard_count;
  ready.flags                = OCUDU_DAPP_PROCESS_REQUIRED_CAPABILITIES_V1;
  if (!heartbeat_capability) {
    ready.flags &= ~OCUDU_DAPP_PROCESS_CAP_HEARTBEAT_V1;
  }
  if (!parent_death_capability) {
    ready.flags &= ~OCUDU_DAPP_PROCESS_CAP_PARENT_DEATH_SIGNAL_V1;
  }
  ocudu_dapp_control_header_v1 header{};
  header.magic        = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version      = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type         = OCUDU_DAPP_CONTROL_ROUTE_READY_V1;
  header.payload_size = sizeof(ready);
  header.sequence     = reference.header.sequence;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&ready, sizeof(ready)}}};
  msghdr               message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  return ::sendmsg(control_fd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(header) + sizeof(ready));
}

bool receive_bundle(int control_fd, pid_t expected_pid, ocudu_dapp_route_bundle_v1& bundle)
{
  std::array<uint8_t, sizeof(ocudu_dapp_control_header_v1) + sizeof(bundle)> data{};
  alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(ucred))>            control{};
  iovec                                                                      vector{data.data(), data.size()};
  msghdr                                                                     message{};
  message.msg_iov        = &vector;
  message.msg_iovlen     = 1;
  message.msg_control    = control.data();
  message.msg_controllen = control.size();
  const ssize_t received = ::recvmsg(control_fd, &message, MSG_CMSG_CLOEXEC);
  ucred         credentials{};
  bool          have_credentials = false;
  for (auto* item = CMSG_FIRSTHDR(&message); item != nullptr; item = CMSG_NXTHDR(&message, item)) {
    if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_CREDENTIALS &&
        item->cmsg_len == CMSG_LEN(sizeof(credentials))) {
      std::memcpy(&credentials, CMSG_DATA(item), sizeof(credentials));
      have_credentials = true;
    }
  }
  ocudu_dapp_control_header_v1 header{};
  if (received == static_cast<ssize_t>(data.size())) {
    std::memcpy(&header, data.data(), sizeof(header));
    std::memcpy(&bundle, data.data() + sizeof(header), sizeof(bundle));
  }
  return received == static_cast<ssize_t>(data.size()) && (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) == 0 &&
         have_credentials && credentials.pid == expected_pid && credentials.uid == ::geteuid() &&
         header.magic == OCUDU_DAPP_CONTROL_MAGIC_V1 && header.version == OCUDU_DAPP_CONTROL_VERSION_V1 &&
         header.type == OCUDU_DAPP_CONTROL_ROUTE_BUNDLE_V1 && header.payload_size == sizeof(bundle) &&
         header.descriptor_count == 0 && header.reserved == 0 && header.sequence == bundle.route_generation &&
         bundle.struct_size == sizeof(bundle) && bundle.interface_count == 2 && bundle.route_count != 0 &&
         bundle.route_generation != 0 && bundle.bundle_id != 0 && bundle.flags == 0 && bundle.reserved0 == 0 &&
         bundle.interfaces[0].interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1 &&
         bundle.interfaces[0].route_count == bundle.route_count &&
         bundle.interfaces[0].requirement == OCUDU_DAPP_PROCESS_BINDING_REQUIRED_V1 &&
         bundle.interfaces[0].state == OCUDU_DAPP_PROCESS_BINDING_BOUND_V1 &&
         bundle.interfaces[1].interface_id == 0x7f010002U && bundle.interfaces[1].route_count == 0 &&
         bundle.interfaces[1].requirement == OCUDU_DAPP_PROCESS_BINDING_OPTIONAL_V1 &&
         bundle.interfaces[1].state == OCUDU_DAPP_PROCESS_BINDING_UNAVAILABLE_V1;
}

bool send_bundle_ready(int                               control_fd,
                       const ocudu_dapp_route_bundle_v1& bundle,
                       bool                              heartbeat_capability,
                       bool                              parent_death_capability)
{
  ocudu_dapp_route_bundle_ready_v1 ready{};
  ready.struct_size      = sizeof(ready);
  ready.interface_count  = bundle.interface_count;
  ready.route_count      = bundle.route_count;
  ready.route_generation = bundle.route_generation;
  ready.bundle_id        = bundle.bundle_id;
  ready.flags            = OCUDU_DAPP_PROCESS_REQUIRED_CAPABILITIES_V1 | OCUDU_DAPP_PROCESS_CAP_MULTI_INTERFACE_V1;
  if (!heartbeat_capability) {
    ready.flags &= ~OCUDU_DAPP_PROCESS_CAP_HEARTBEAT_V1;
  }
  if (!parent_death_capability) {
    ready.flags &= ~OCUDU_DAPP_PROCESS_CAP_PARENT_DEATH_SIGNAL_V1;
  }
  ocudu_dapp_control_header_v1 header{};
  header.magic        = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version      = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type         = OCUDU_DAPP_CONTROL_ROUTE_BUNDLE_READY_V1;
  header.payload_size = sizeof(ready);
  header.sequence     = bundle.route_generation;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&ready, sizeof(ready)}}};
  msghdr               message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  return ::sendmsg(control_fd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(header) + sizeof(ready));
}

bool send_spectrum_result(int control_fd, const received_offer& reference, uint64_t route_id)
{
  ocudu_dapp_process_spectrum_result_v1 result{};
  result.struct_size          = sizeof(result);
  result.schema_id            = OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1;
  result.route_id             = route_id;
  result.source_grid_sequence = 77;
  result.cell_id              = 3;
  result.confidence_q16       = 1U << 16U;
  result.created              = {4, 2, 0, 0};
  result.valid_from           = {4, 3, 0, 0};
  result.valid_until          = {4, 6, 0, 0};
  result.nof_prbs             = 2;
  result.prb_confidence[0]    = UINT8_MAX;
  ocudu_dapp_control_header_v1 header{};
  header.magic        = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version      = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type         = OCUDU_DAPP_CONTROL_SPECTRUM_RESULT_V1;
  header.payload_size = sizeof(result);
  header.sequence     = reference.header.sequence;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&result, sizeof(result)}}};
  msghdr               message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  return ::sendmsg(control_fd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(header) + sizeof(result));
}

bool send_srs_result(int control_fd, const received_offer& reference, uint64_t route_id)
{
  ocudu_dapp_process_srs_isac_result_v1 result{};
  result.struct_size              = sizeof(result);
  result.schema_id                = OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1;
  result.route_id                 = route_id;
  auto& preview                   = result.preview;
  preview.struct_size             = sizeof(preview);
  preview.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  preview.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
  preview.context.struct_size     = sizeof(preview.context);
  preview.context.schema_id       = OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1;
  preview.context.schema_version  = 1;
  preview.context.source_sequence = 88;
  preview.context.cell_id         = 3;
  preview.context.confidence_q16  = 1U << 16U;
  preview.context.created         = {4, 2, 0, 0};
  preview.context.valid_from      = preview.context.created;
  preview.context.valid_until     = preview.context.created;
  preview.source_srs_sequence     = 88;
  preview.srs_resource_id         = 17;
  preview.srs_config_id           = 19;
  preview.tap_count               = 2;
  preview.peak_delay_bin          = 1;
  preview.mean_channel_power      = 2.0F;
  preview.noise_corrected_power   = 1.75F;
  preview.peak_cir_power          = 1.0F;
  preview.noise_variance          = 0.25F;
  preview.tap_power[0]            = 0.25F;
  preview.tap_power[1]            = 1.0F;
  ocudu_dapp_control_header_v1 header{};
  header.magic        = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version      = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type         = OCUDU_DAPP_CONTROL_SRS_ISAC_RESULT_V1;
  header.payload_size = sizeof(result);
  header.sequence     = reference.header.sequence;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&result, sizeof(result)}}};
  msghdr               message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  return ::sendmsg(control_fd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(header) + sizeof(result));
}

bool send_quiet_result(int control_fd, const received_offer& reference, uint64_t route_id)
{
  ocudu_dapp_process_quiet_intent_result_v1 result{};
  result.struct_size          = sizeof(result);
  result.schema_id            = OCUDU_DAPP_SPECTRUM_OUTPUT_QUIET_PERIOD_INTENT_V1;
  result.route_id             = route_id;
  result.source_grid_sequence = 77;
  result.cell_id              = 3;
  result.created              = {4, 2, 0, 0};
  auto& intent                = result.intent;
  intent.struct_size          = sizeof(intent);
  intent.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
  intent.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
  intent.request_id[0]        = 77;
  intent.earliest_start       = {4, 6, 0, 0};
  intent.latest_start         = intent.earliest_start;
  intent.duration_slots       = 1;
  intent.repeat_count         = 1;
  intent.first_prb            = 5;
  intent.nof_prbs             = 2;
  intent.requested_level      = OCUDU_DAPP_QUIET_NO_NEW_PUSCH_V1;
  intent.budget_class         = 1;
  intent.confidence_q16       = 1U << 15U;
  intent.expiry_monotonic_ns  = UINT64_MAX;
  ocudu_dapp_control_header_v1 header{};
  header.magic        = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version      = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type         = OCUDU_DAPP_CONTROL_QUIET_INTENT_RESULT_V1;
  header.payload_size = sizeof(result);
  header.sequence     = reference.header.sequence;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&result, sizeof(result)}}};
  msghdr               message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  return ::sendmsg(control_fd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(header) + sizeof(result));
}

bool send_consumed_spectrum_result(int control_fd, const received_offer& reference, uint64_t route_id)
{
  ocudu_dapp_process_spectrum_result_v1 result{};
  result.struct_size            = sizeof(result);
  result.schema_id              = OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1;
  result.route_id               = route_id;
  result.source_grid_sequence   = 1;
  result.source_plan_generation = 1;
  result.cell_id                = 1;
  result.confidence_q16         = 1U << 16U;
  result.created                = {0, 2, 0, 0};
  result.valid_from             = {0, 3, 0, 0};
  result.valid_until            = {0, 6, 0, 0};
  result.nof_prbs               = 2;
  result.prb_confidence[0]      = UINT8_MAX;
  ocudu_dapp_control_header_v1 header{};
  header.magic        = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version      = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type         = OCUDU_DAPP_CONTROL_SPECTRUM_RESULT_V1;
  header.payload_size = sizeof(result);
  header.sequence     = reference.header.sequence;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&result, sizeof(result)}}};
  msghdr               message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  return ::sendmsg(control_fd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(header) + sizeof(result));
}

struct management_state {
  bool     enabled{true};
  uint64_t config_generation{1};
  uint64_t model_generation{1};
};

bool service_control(int control_fd, const received_offer& reference, management_state& state)
{
  std::array<uint8_t, sizeof(ocudu_dapp_control_header_v1) + OCUDU_DAPP_CONTROL_MAX_PAYLOAD_V1> data{};
  alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(ucred))>     control{};
  iovec  vector{data.data(), data.size()};
  msghdr incoming{};
  incoming.msg_iov        = &vector;
  incoming.msg_iovlen     = 1;
  incoming.msg_control    = control.data();
  incoming.msg_controllen = control.size();
  const ssize_t received  = ::recvmsg(control_fd, &incoming, MSG_CMSG_CLOEXEC);
  if (received < static_cast<ssize_t>(sizeof(ocudu_dapp_control_header_v1)) ||
      (incoming.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
    return false;
  }
  int descriptor = -1;
  for (auto* item = CMSG_FIRSTHDR(&incoming); item != nullptr; item = CMSG_NXTHDR(&incoming, item)) {
    if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_RIGHTS && item->cmsg_len == CMSG_LEN(sizeof(int)))
      std::memcpy(&descriptor, CMSG_DATA(item), sizeof(descriptor));
  }
  ocudu_dapp_control_header_v1 header{};
  std::memcpy(&header, data.data(), sizeof(header));
  if (header.magic != OCUDU_DAPP_CONTROL_MAGIC_V1 || header.version != OCUDU_DAPP_CONTROL_VERSION_V1 ||
      header.reserved != 0 || header.sequence != reference.header.sequence ||
      received != static_cast<ssize_t>(sizeof(header) + header.payload_size) ||
      header.descriptor_count != static_cast<uint16_t>(descriptor >= 0)) {
    if (descriptor >= 0)
      ::close(descriptor);
    return false;
  }
  if (header.type == OCUDU_DAPP_CONTROL_HEARTBEAT_REQUEST_V1) {
    ocudu_dapp_process_heartbeat_v1 heartbeat{};
    std::memcpy(&heartbeat, data.data() + sizeof(header), sizeof(heartbeat));
    if (header.payload_size != sizeof(heartbeat) || header.descriptor_count != 0 ||
        heartbeat.struct_size != sizeof(heartbeat) || heartbeat.flags != 0 ||
        heartbeat.route_generation != reference.header.sequence || heartbeat.challenge == 0 ||
        heartbeat.host_monotonic_ns == 0 || heartbeat.reserved[0] != 0 || heartbeat.reserved[1] != 0)
      return false;
    header.type = OCUDU_DAPP_CONTROL_HEARTBEAT_RESPONSE_V1;
    std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&heartbeat, sizeof(heartbeat)}}};
    msghdr               message{};
    message.msg_iov    = vectors.data();
    message.msg_iovlen = vectors.size();
    return ::sendmsg(control_fd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(header) + sizeof(heartbeat));
  }
  if (header.type != OCUDU_DAPP_CONTROL_MANAGEMENT_REQUEST_V1 ||
      header.payload_size != sizeof(ocudu_dapp_process_management_request_v1)) {
    if (descriptor >= 0)
      ::close(descriptor);
    return false;
  }
  ocudu_dapp_process_management_request_v1 request{};
  std::memcpy(&request, data.data() + sizeof(header), sizeof(request));
  ocudu_dapp_process_management_response_v1 response{};
  response.struct_size      = sizeof(response);
  response.operation        = request.operation;
  response.route_generation = request.route_generation;
  response.request_id       = request.request_id;
  response.status           = OCUDU_DAPP_OK_V1;
  if (request.operation == OCUDU_DAPP_PROCESS_MANAGEMENT_DESCRIBE_CONFIG_V1) {
    response.count = 1;
    auto& output   = response.payload.descriptor;
    std::strcpy(output.key, "enabled");
    std::strcpy(output.description, "Accept fixture publications");
    output.value_kind                  = OCUDU_DAPP_VALUE_BOOL_V1;
    output.default_value.kind          = OCUDU_DAPP_VALUE_BOOL_V1;
    output.default_value.value.boolean = true;
    output.minimum_value.kind          = OCUDU_DAPP_VALUE_BOOL_V1;
    output.maximum_value.kind          = OCUDU_DAPP_VALUE_BOOL_V1;
    output.maximum_value.value.boolean = true;
  } else if (request.operation == OCUDU_DAPP_PROCESS_MANAGEMENT_GET_CONFIG_V1) {
    response.count                = 1;
    response.resulting_generation = state.config_generation;
    std::strcpy(response.payload.entries[0].key, "enabled");
    response.payload.entries[0].value.kind          = OCUDU_DAPP_VALUE_BOOL_V1;
    response.payload.entries[0].value.value.boolean = state.enabled;
  } else if (request.operation == OCUDU_DAPP_PROCESS_MANAGEMENT_SET_CONFIG_V1) {
    if (request.expected_generation != state.config_generation || request.entry_count != 1 ||
        std::strcmp(request.entries[0].key, "enabled") != 0 ||
        request.entries[0].value.kind != OCUDU_DAPP_VALUE_BOOL_V1) {
      response.status = OCUDU_DAPP_CONFLICT_V1;
    } else if ((request.flags & OCUDU_DAPP_PROCESS_MANAGEMENT_DRY_RUN_V1) == 0) {
      state.enabled = request.entries[0].value.value.boolean != 0;
      ++state.config_generation;
    }
    response.resulting_generation = state.config_generation;
  } else if (request.operation == OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_V1) {
    if (request.expected_generation != state.model_generation) {
      response.status = OCUDU_DAPP_CONFLICT_V1;
    } else if ((request.flags & OCUDU_DAPP_PROCESS_MANAGEMENT_DRY_RUN_V1) == 0) {
      ++state.model_generation;
    }
    response.resulting_generation = state.model_generation;
  } else {
    response.status = OCUDU_DAPP_UNSUPPORTED_V1;
  }
  if (descriptor >= 0)
    ::close(descriptor);
  header.type             = OCUDU_DAPP_CONTROL_MANAGEMENT_RESPONSE_V1;
  header.payload_size     = sizeof(response);
  header.descriptor_count = 0;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&response, sizeof(response)}}};
  msghdr               message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  return ::sendmsg(control_fd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(header) + sizeof(response));
}

bool send_consumed_srs_result(int control_fd, const received_offer& reference, uint64_t route_id)
{
  ocudu_dapp_process_srs_isac_result_v1 result{};
  result.struct_size              = sizeof(result);
  result.schema_id                = OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1;
  result.route_id                 = route_id;
  auto& preview                   = result.preview;
  preview.struct_size             = sizeof(preview);
  preview.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  preview.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
  preview.context.struct_size     = sizeof(preview.context);
  preview.context.schema_id       = OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1;
  preview.context.schema_version  = 1;
  preview.context.source_sequence = 1;
  preview.context.cell_id         = 1;
  preview.context.confidence_q16  = 1U << 16U;
  preview.context.created         = {0, 2, 0, 0};
  preview.context.valid_from      = preview.context.created;
  preview.context.valid_until     = preview.context.created;
  preview.source_srs_sequence     = 1;
  preview.tap_count               = 2;
  preview.peak_delay_bin          = 1;
  preview.mean_channel_power      = 2.0F;
  preview.noise_corrected_power   = 1.75F;
  preview.peak_cir_power          = 1.0F;
  preview.noise_variance          = 0.25F;
  preview.tap_power[0]            = 0.25F;
  preview.tap_power[1]            = 1.0F;
  ocudu_dapp_control_header_v1 header{};
  header.magic        = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version      = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type         = OCUDU_DAPP_CONTROL_SRS_ISAC_RESULT_V1;
  header.payload_size = sizeof(result);
  header.sequence     = reference.header.sequence;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&result, sizeof(result)}}};
  msghdr               message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  return ::sendmsg(control_fd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(header) + sizeof(result));
}

int parse_control_fd(std::string_view argument)
{
  constexpr std::string_view prefix = "--control-fd=";
  if (argument.size() < prefix.size() || argument.substr(0, prefix.size()) != prefix) {
    return -1;
  }
  argument.remove_prefix(prefix.size());
  char*      end   = nullptr;
  const long value = std::strtol(argument.data(), &end, 10);
  return end == argument.data() + argument.size() && value >= 0 && value <= INT32_MAX ? static_cast<int>(value) : -1;
}

} // namespace

int main(int argc, char** argv)
{
  int  control_fd           = -1;
  bool exit_after_ready     = false;
  bool no_ready             = false;
  bool ignore_term          = false;
  bool send_result          = false;
  bool send_quiet           = false;
  bool ignore_heartbeat     = false;
  bool heartbeat_capability = true;
  bool apply_no_new_privs   = true;
  bool apply_seccomp_filter = false;
  bool apply_parent_death   = true;
  bool multi_bundle =
#ifdef DAPP_CLASS_C_PROCESS_MULTI_BUNDLE_DEFAULT
      true;
#else
      false;
#endif
  for (int index = 1; index != argc; ++index) {
    const std::string_view argument(argv[index]);
    const int              parsed = parse_control_fd(argument);
    if (parsed >= 0) {
      control_fd = parsed;
    } else if (argument == "--exit-after-ready") {
      exit_after_ready = true;
    } else if (argument == "--no-ready") {
      no_ready = true;
    } else if (argument == "--ignore-term") {
      ignore_term = true;
    } else if (argument == "--send-result") {
      send_result = true;
    } else if (argument == "--send-quiet") {
      send_quiet = true;
    } else if (argument == "--ignore-heartbeat") {
      ignore_heartbeat = true;
    } else if (argument == "--no-heartbeat-capability") {
      heartbeat_capability = false;
    } else if (argument == "--skip-no-new-privileges") {
      apply_no_new_privs = false;
    } else if (argument == "--install-seccomp-filter") {
      apply_seccomp_filter = true;
    } else if (argument == "--skip-parent-death-signal") {
      apply_parent_death = false;
    } else if (argument == "--multi-bundle") {
      multi_bundle = true;
    } else {
      return 2;
    }
  }
  if (control_fd < 0 || (ignore_term && std::signal(SIGTERM, SIG_IGN) == SIG_ERR)) {
    return 2;
  }
  const pid_t parent_pid = ::getppid();
  if (apply_parent_death &&
      (parent_pid <= 1 || ::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0 || ::getppid() != parent_pid)) {
    return 2;
  }
  if (apply_no_new_privs && ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    return 2;
  }
  if (apply_seccomp_filter && !install_test_seccomp_filter()) {
    return 2;
  }

  descriptor_store           descriptors;
  ocudu_dapp_route_bundle_v1 bundle{};
  if (multi_bundle && !receive_bundle(control_fd, parent_pid, bundle)) {
    return 3;
  }
  received_offer reference;
  if (!receive_offer(control_fd, parent_pid, descriptors, reference)) {
    return 3;
  }
  std::array<bool, OCUDU_DAPP_MAX_PRODUCER_SHARDS_V1>     seen{};
  std::array<uint64_t, OCUDU_DAPP_MAX_PRODUCER_SHARDS_V1> route_ids{};
  seen[reference.offer.producer_shard_id]      = true;
  route_ids[reference.offer.producer_shard_id] = reference.offer.route_id;
  for (uint16_t count = 1; count != reference.offer.producer_shard_count; ++count) {
    received_offer next;
    if (!receive_offer(control_fd, parent_pid, descriptors, next) ||
        next.header.sequence != reference.header.sequence || next.offer.interface_id != reference.offer.interface_id ||
        next.offer.metadata_schema_id != reference.offer.metadata_schema_id ||
        next.offer.metadata_schema_version != reference.offer.metadata_schema_version ||
        next.offer.producer_shard_count != reference.offer.producer_shard_count || seen[next.offer.producer_shard_id]) {
      return 3;
    }
    for (uint64_t route_id : route_ids) {
      if (route_id == next.offer.route_id) {
        return 3;
      }
    }
    seen[next.offer.producer_shard_id]      = true;
    route_ids[next.offer.producer_shard_id] = next.offer.route_id;
  }
  if (multi_bundle && (reference.header.sequence != bundle.route_generation ||
                       reference.offer.interface_id != bundle.interfaces[0].interface_id ||
                       reference.offer.producer_shard_count != bundle.route_count)) {
    return 3;
  }
  if (no_ready) {
    for (;;) {
      ::pause();
    }
  }
  const bool ready = multi_bundle ? send_bundle_ready(control_fd, bundle, heartbeat_capability, apply_parent_death)
                                  : send_ready(control_fd, reference, heartbeat_capability, apply_parent_death);
  if (!ready) {
    return 4;
  }
  if (exit_after_ready) {
    return 0;
  }
  if (send_result) {
    const bool sent = reference.offer.interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1
                          ? send_spectrum_result(control_fd, reference, route_ids.front())
                      : reference.offer.interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1
                          ? send_srs_result(control_fd, reference, route_ids.front())
                          : false;
    if (!sent) {
      return 5;
    }
  }
  if (send_quiet && !send_quiet_result(control_fd, reference, route_ids.front())) {
    return 5;
  }
  bool             publication_consumed = send_result || send_quiet;
  management_state management;
  for (;;) {
    std::array<pollfd, 2> polled{
        {{control_fd, POLLIN, 0}, {descriptors.values[OCUDU_DAPP_ROUTE_DESCRIPTOR_NOTIFICATION_V1], POLLIN, 0}}};
    int result;
    do {
      result = ::poll(polled.data(), polled.size(), -1);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
      return 5;
    if ((polled[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0 ||
        ((polled[0].revents & POLLIN) != 0 &&
         (!ignore_heartbeat && !service_control(control_fd, reference, management)))) {
      return 5;
    }
    if (!publication_consumed && (polled[1].revents & POLLIN) != 0) {
      uint64_t ignored = 0;
      if (::read(polled[1].fd, &ignored, sizeof(ignored)) != sizeof(ignored))
        return 5;
      const bool sent = reference.offer.interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1
                            ? send_consumed_spectrum_result(control_fd, reference, route_ids.front())
                        : reference.offer.interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1
                            ? send_consumed_srs_result(control_fd, reference, route_ids.front())
                            : false;
      if (!sent) {
        return 5;
      }
      publication_consumed = true;
    }
  }
}
