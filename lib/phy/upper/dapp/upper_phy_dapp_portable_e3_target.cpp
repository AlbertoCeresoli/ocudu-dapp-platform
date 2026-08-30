// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/phy/upper/dapp/upper_phy_dapp_portable_e3_target.h"
#include <chrono>
#include <limits>
#include <new>

using namespace ocudu;

namespace {

uint64_t monotonic_ns() noexcept
{
  const auto value =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return value > 0 ? static_cast<uint64_t>(value) : 1;
}

class portable_e3_target final : public upper_phy_dapp_publication_target
{
public:
  portable_e3_target(uint32_t                                                interface_id_,
                     uint64_t                                                route_epoch_,
                     std::unique_ptr<dapp::ipc::class_c_publication_encoder> encoder_,
                     std::shared_ptr<dapp::e3::portable_data_store>          store_) :
    id(interface_id_), route_epoch(route_epoch_), encoder(std::move(encoder_)), store(std::move(store_))
  {
  }

  uint32_t interface_id() const noexcept override { return id; }
  bool     active() const noexcept override { return store->has_active(id); }

  void publish(const ocudu_dapp_spectrum_input_v1& input) noexcept override
  {
    publish(input, input.uplink_resource_grid);
  }

  void publish(const ocudu_dapp_srs_isac_input_v1& input) noexcept override { publish(input, input.channel_estimates); }

private:
  template <typename Input>
  void publish(const Input& input, const ocudu_dapp_tensor_view_v1& tensor) noexcept
  {
    if (!active() || tensor.byte_length == 0 || tensor.byte_length > std::numeric_limits<std::size_t>::max() ||
        tensor.native_handle == 0 || tensor.native_handle > std::numeric_limits<uintptr_t>::max() ||
        tensor.byte_offset > std::numeric_limits<uintptr_t>::max() - tensor.native_handle) {
      return;
    }
    const auto encoded = encoder->encode(input, route_epoch, monotonic_ns());
    if (!encoded) {
      return;
    }
    const auto* bytes =
        reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(tensor.native_handle + tensor.byte_offset));
    dapp::e3::portable_publication_source source;
    source.interface_id         = id;
    source.cell_id              = input.invocation.cell_id;
    source.publication_sequence = input.source_sequence;
    source.route_epoch          = route_epoch;
    source.metadata             = encoded.metadata;
    source.tensor               = {bytes, static_cast<std::size_t>(tensor.byte_length)};
    static_cast<void>(store->publish(source));
  }

  uint32_t                                                id;
  uint64_t                                                route_epoch;
  std::unique_ptr<dapp::ipc::class_c_publication_encoder> encoder;
  std::shared_ptr<dapp::e3::portable_data_store>          store;
};

} // namespace

upper_phy_dapp_portable_e3_target_create_result
ocudu::create_upper_phy_dapp_portable_e3_target(upper_phy_dapp_portable_e3_target_config config) noexcept
{
  if ((config.interface_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 &&
       config.interface_id != OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) ||
      config.route_epoch == 0 || config.store == nullptr ||
      config.encoder.host_object_profile != dapp::ipc::class_c_host_object_profile::embedded_payload) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, "invalid portable E3 target configuration", {}};
  }
  auto encoder = dapp::ipc::class_c_publication_encoder::create(std::move(config.encoder));
  if (!encoder) {
    return {OCUDU_DAPP_UNSUPPORTED_V1, std::move(encoder.detail), {}};
  }
  try {
    return {OCUDU_DAPP_OK_V1,
            {},
            std::make_shared<portable_e3_target>(
                config.interface_id, config.route_epoch, std::move(encoder.encoder), std::move(config.store))};
  } catch (const std::bad_alloc&) {
    return {OCUDU_DAPP_RESOURCE_V1, "unable to allocate portable E3 target", {}};
  } catch (...) {
    return {OCUDU_DAPP_INTERNAL_ERROR_V1, "unknown portable E3 target construction failure", {}};
  }
}
