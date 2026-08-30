// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "class-c-publication-v1_generated.h"
#include "ocudu/dapp/ipc/class_c_metadata.h"
#include "ocudu/dapp/ipc/class_c_publication_encoder.h"
#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <utility>
#include <vector>

using namespace ocudu::dapp::ipc;
namespace schema = Ocudu::Dapp::ClassC::V1;

namespace {

ocudu_dapp_invocation_v1 invocation()
{
  ocudu_dapp_invocation_v1 value{};
  value.struct_size           = sizeof(value);
  value.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  value.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  value.invocation_id         = 17;
  value.cell_id               = 3;
  value.radio_time.sfn        = 10;
  value.radio_time.slot       = 4;
  value.radio_time.numerology = 1;
  value.input_generation      = 9;
  return value;
}

ocudu_dapp_tensor_view_v1 tensor(void* data, uint32_t element_type, uint64_t element_size)
{
  ocudu_dapp_tensor_view_v1 value{};
  value.struct_size           = sizeof(value);
  value.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  value.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  value.domain                = OCUDU_DAPP_MEM_HOST_V1;
  value.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
  value.element_type          = element_type;
  value.rank                  = 3;
  value.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  value.allocation_id         = 21;
  value.allocation_generation = 2;
  value.native_handle         = reinterpret_cast<uintptr_t>(data);
  value.byte_length           = 8 * element_size;
  value.dimensions[0]         = 1;
  value.dimensions[1]         = 2;
  value.dimensions[2]         = 4;
  value.byte_strides[2]       = element_size;
  value.byte_strides[1]       = 4 * element_size;
  value.byte_strides[0]       = 8 * element_size;
  value.lease_token           = 5;
  return value;
}

template <typename T>
ocudu_dapp_typed_metadata_v1 typed(uint32_t schema_id, T& value)
{
  ocudu_dapp_typed_metadata_v1 result{};
  result.struct_size             = sizeof(result);
  result.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  result.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
  result.metadata_schema_id      = schema_id;
  result.metadata_schema_version = 1;
  result.metadata                = {&value, sizeof(value), 0};
  return result;
}

class_c_encoder_create_result make_encoder()
{
  class_c_encoder_config config{};
  config.producer_instance = {11, 12};
  config.ran_node          = {13, 14};
  return class_c_publication_encoder::create(std::move(config));
}

} // namespace

TEST(dapp_class_c_publication_encoder, encodes_native_bf16_spectrum_as_posix_shared_metadata)
{
  auto encoder = make_encoder();
  ASSERT_TRUE(encoder) << encoder.detail;
  std::array<uint32_t, 8> data{};

  ocudu_dapp_spectrum_slot_metadata_v1 metadata{};
  metadata.struct_size     = sizeof(metadata);
  metadata.abi_major       = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.abi_minor       = OCUDU_DAPP_ABI_MINOR_V1;
  metadata.bandwidth_hz    = 100'000'000;
  metadata.rx_ports        = 1;
  metadata.symbols         = 2;
  metadata.subcarriers     = 4;
  metadata.amplitude_scale = 0.5F;
  ocudu_dapp_spectrum_input_v1 input{};
  input.invocation           = invocation();
  input.uplink_resource_grid = tensor(data.data(), OCUDU_DAPP_ELEMENT_CBF16_V1, sizeof(uint32_t));
  input.slot_metadata        = typed(OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1, metadata);
  input.source_sequence      = 7;

  const auto encoded = encoder.encoder->encode(input, 3, 1000);
  ASSERT_TRUE(encoded) << to_string(encoded.error);
  EXPECT_EQ(validate_class_c_metadata(encoded.metadata), metadata_error::none);
  const auto* envelope = schema::GetPublicationEnvelope(encoded.metadata.data());
  ASSERT_NE(envelope, nullptr);
  EXPECT_EQ(envelope->publication_kind(), schema::PublicationKind::Spectrum);
  EXPECT_EQ(envelope->publication_sequence(), 7U);
  ASSERT_NE(envelope->context(), nullptr);
  EXPECT_EQ(envelope->context()->cell_id(), 3U);
  EXPECT_EQ(envelope->context()->radio_time()->symbol_mask(), 3U);
  ASSERT_NE(envelope->tensor(), nullptr);
  EXPECT_EQ(envelope->tensor()->domain(), schema::MemoryDomain::PosixShared);
  EXPECT_EQ(envelope->tensor()->element_type(), schema::ElementType::ComplexBf16);
  EXPECT_EQ(envelope->tensor()->lease_id()->low(), 3U);
  EXPECT_EQ(envelope->tensor()->lease_id()->high(), 7U);
  EXPECT_EQ(envelope->tensor()->dimensions()->size(), 3U);
  ASSERT_NE(envelope->metadata_as_SpectrumMetadata(), nullptr);
  EXPECT_FLOAT_EQ(envelope->metadata_as_SpectrumMetadata()->scale(), 0.5F);
}

TEST(dapp_class_c_publication_encoder, marks_host_tensor_as_embedded_for_portable_e3_payload)
{
  class_c_encoder_config config{};
  config.producer_instance   = {11, 12};
  config.host_object_profile = class_c_host_object_profile::embedded_payload;
  auto encoder               = class_c_publication_encoder::create(std::move(config));
  ASSERT_TRUE(encoder) << encoder.detail;
  std::array<uint32_t, 8> data{};

  ocudu_dapp_spectrum_slot_metadata_v1 metadata{};
  metadata.struct_size     = sizeof(metadata);
  metadata.abi_major       = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.bandwidth_hz    = 100'000'000;
  metadata.rx_ports        = 1;
  metadata.symbols         = 2;
  metadata.subcarriers     = 4;
  metadata.amplitude_scale = 1.0F;
  ocudu_dapp_spectrum_input_v1 input{};
  input.invocation           = invocation();
  input.uplink_resource_grid = tensor(data.data(), OCUDU_DAPP_ELEMENT_CBF16_V1, sizeof(uint32_t));
  input.slot_metadata        = typed(OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1, metadata);
  input.source_sequence      = 7;

  const auto encoded = encoder.encoder->encode(input, 3, 1000);
  ASSERT_TRUE(encoded) << to_string(encoded.error);
  EXPECT_EQ(validate_class_c_metadata(encoded.metadata), metadata_error::none);
  const auto* descriptor = schema::GetPublicationEnvelope(encoded.metadata.data())->tensor();
  ASSERT_NE(descriptor, nullptr);
  EXPECT_EQ(descriptor->domain(), schema::MemoryDomain::Host);
  EXPECT_EQ(descriptor->backend_id(), OCUDU_DAPP_BACKEND_HOST_V1);
  EXPECT_EQ(descriptor->capability_id(), nullptr);
}

TEST(dapp_class_c_publication_encoder, encodes_cuda_spectrum_as_an_accelerator_export_capability)
{
  auto encoder = make_encoder();
  ASSERT_TRUE(encoder) << encoder.detail;
  std::array<uint32_t, 8> storage{};

  ocudu_dapp_spectrum_slot_metadata_v1 metadata{};
  metadata.struct_size     = sizeof(metadata);
  metadata.abi_major       = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.bandwidth_hz    = 100'000'000;
  metadata.rx_ports        = 1;
  metadata.symbols         = 2;
  metadata.subcarriers     = 4;
  metadata.amplitude_scale = 1.0F;
  ocudu_dapp_spectrum_input_v1 input{};
  input.invocation                            = invocation();
  input.uplink_resource_grid                  = tensor(storage.data(), OCUDU_DAPP_ELEMENT_CBF16_V1, sizeof(uint32_t));
  input.uplink_resource_grid.domain           = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
  input.uplink_resource_grid.backend_id       = OCUDU_DAPP_BACKEND_CUDA_V1;
  input.uplink_resource_grid.device_ordinal   = 2;
  input.uplink_resource_grid.access           = OCUDU_DAPP_ACCESS_READ_V1;
  input.uplink_resource_grid.ready_token      = 41;
  input.uplink_resource_grid.completion_token = 42;
  input.slot_metadata                         = typed(OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1, metadata);
  input.source_sequence                       = 7;

  const auto encoded = encoder.encoder->encode(input, 3, 1000);
  ASSERT_TRUE(encoded) << to_string(encoded.error);
  EXPECT_EQ(validate_class_c_metadata(encoded.metadata), metadata_error::none);
  const auto* descriptor = schema::GetPublicationEnvelope(encoded.metadata.data())->tensor();
  ASSERT_NE(descriptor, nullptr);
  EXPECT_EQ(descriptor->domain(), schema::MemoryDomain::AcceleratorIpcExport);
  EXPECT_EQ(descriptor->backend_id(), OCUDU_DAPP_BACKEND_CUDA_V1);
  EXPECT_EQ(descriptor->device_ordinal(), 2U);
  ASSERT_NE(descriptor->capability_id(), nullptr);
  EXPECT_EQ(descriptor->capability_id()->low(), 3U);
  EXPECT_EQ(descriptor->capability_id()->high(), input.uplink_resource_grid.allocation_id);
}

TEST(dapp_class_c_publication_encoder, embeds_and_validates_the_expected_ul_use_plan)
{
  auto encoder = make_encoder();
  ASSERT_TRUE(encoder) << encoder.detail;
  std::array<uint32_t, 48> grid{};
  std::array<uint8_t, 4>   plan{{OCUDU_DAPP_UL_USE_REFERENCE_SIGNAL_V1,
                                 OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1,
                                 OCUDU_DAPP_UL_USE_PUSCH_V1,
                                 OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1}};

  ocudu_dapp_spectrum_slot_metadata_v1 metadata{};
  metadata.struct_size            = sizeof(metadata);
  metadata.abi_major              = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.bandwidth_hz           = 100'000'000;
  metadata.rx_ports               = 1;
  metadata.symbols                = 2;
  metadata.subcarriers            = 24;
  metadata.amplitude_scale        = 1.0F;
  metadata.flags                  = OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1;
  metadata.source_plan_generation = 23;

  ocudu_dapp_spectrum_input_v1 input{};
  input.invocation                              = invocation();
  input.uplink_resource_grid                    = tensor(grid.data(), OCUDU_DAPP_ELEMENT_CBF16_V1, sizeof(uint32_t));
  input.uplink_resource_grid.byte_length        = sizeof(grid);
  input.uplink_resource_grid.dimensions[1]      = 2;
  input.uplink_resource_grid.dimensions[2]      = 24;
  input.uplink_resource_grid.byte_strides[0]    = 48 * sizeof(uint32_t);
  input.uplink_resource_grid.byte_strides[1]    = 24 * sizeof(uint32_t);
  input.scheduled_re_mask.struct_size           = sizeof(input.scheduled_re_mask);
  input.scheduled_re_mask.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  input.scheduled_re_mask.domain                = OCUDU_DAPP_MEM_HOST_V1;
  input.scheduled_re_mask.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
  input.scheduled_re_mask.element_type          = OCUDU_DAPP_ELEMENT_U8_V1;
  input.scheduled_re_mask.rank                  = 2;
  input.scheduled_re_mask.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  input.scheduled_re_mask.allocation_id         = 22;
  input.scheduled_re_mask.allocation_generation = metadata.source_plan_generation;
  input.scheduled_re_mask.native_handle         = reinterpret_cast<uintptr_t>(plan.data());
  input.scheduled_re_mask.byte_length           = plan.size();
  input.scheduled_re_mask.dimensions[0]         = 2;
  input.scheduled_re_mask.dimensions[1]         = 2;
  input.scheduled_re_mask.byte_strides[0]       = 2;
  input.scheduled_re_mask.byte_strides[1]       = 1;
  input.scheduled_re_mask.layout_id             = OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1;
  input.scheduled_re_mask.layout_version        = 1;
  input.scheduled_re_mask.lease_token           = 23;
  input.slot_metadata                           = typed(OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1, metadata);
  input.source_sequence                         = 7;

  auto encoded = encoder.encoder->encode(input, 3, 1000);
  ASSERT_TRUE(encoded) << to_string(encoded.error);
  const auto* item = schema::GetPublicationEnvelope(encoded.metadata.data())->metadata_as_SpectrumMetadata();
  ASSERT_NE(item, nullptr);
  ASSERT_NE(item->occupancy_annotations(), nullptr);
  EXPECT_EQ(item->plan_generation(), 23U);
  EXPECT_TRUE(item->occupancy_complete());
  EXPECT_EQ(item->occupancy_granularity(), schema::GridMaskGranularity::SymbolPrb);
  EXPECT_TRUE(std::equal(plan.begin(), plan.end(), item->occupancy_annotations()->begin()));

  plan[0] = UINT8_MAX;
  EXPECT_EQ(encoder.encoder->encode(input, 3, 1001).error, class_c_encode_error::invalid_input);
}

TEST(dapp_class_c_publication_encoder, default_capacity_holds_a_maximum_symbol_prb_plan)
{
  auto encoder = make_encoder();
  ASSERT_TRUE(encoder) << encoder.detail;
  constexpr uint32_t    symbols     = 14;
  constexpr uint32_t    subcarriers = OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 * 12;
  std::vector<uint32_t> grid(symbols * subcarriers);
  std::vector<uint8_t>  plan(symbols * OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1, OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1);

  ocudu_dapp_spectrum_slot_metadata_v1 metadata{};
  metadata.struct_size            = sizeof(metadata);
  metadata.abi_major              = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.bandwidth_hz           = 100'000'000;
  metadata.rx_ports               = 1;
  metadata.symbols                = symbols;
  metadata.subcarriers            = subcarriers;
  metadata.amplitude_scale        = 1.0F;
  metadata.flags                  = OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1;
  metadata.source_plan_generation = 31;

  ocudu_dapp_spectrum_input_v1 input{};
  input.invocation                              = invocation();
  input.uplink_resource_grid                    = tensor(grid.data(), OCUDU_DAPP_ELEMENT_CBF16_V1, sizeof(uint32_t));
  input.uplink_resource_grid.byte_length        = grid.size() * sizeof(uint32_t);
  input.uplink_resource_grid.dimensions[0]      = 1;
  input.uplink_resource_grid.dimensions[1]      = symbols;
  input.uplink_resource_grid.dimensions[2]      = subcarriers;
  input.uplink_resource_grid.byte_strides[0]    = symbols * subcarriers * sizeof(uint32_t);
  input.uplink_resource_grid.byte_strides[1]    = subcarriers * sizeof(uint32_t);
  input.scheduled_re_mask.struct_size           = sizeof(input.scheduled_re_mask);
  input.scheduled_re_mask.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  input.scheduled_re_mask.domain                = OCUDU_DAPP_MEM_HOST_V1;
  input.scheduled_re_mask.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
  input.scheduled_re_mask.element_type          = OCUDU_DAPP_ELEMENT_U8_V1;
  input.scheduled_re_mask.rank                  = 2;
  input.scheduled_re_mask.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  input.scheduled_re_mask.allocation_id         = 30;
  input.scheduled_re_mask.allocation_generation = metadata.source_plan_generation;
  input.scheduled_re_mask.native_handle         = reinterpret_cast<uintptr_t>(plan.data());
  input.scheduled_re_mask.byte_length           = plan.size();
  input.scheduled_re_mask.dimensions[0]         = symbols;
  input.scheduled_re_mask.dimensions[1]         = OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1;
  input.scheduled_re_mask.byte_strides[0]       = OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1;
  input.scheduled_re_mask.byte_strides[1]       = 1;
  input.scheduled_re_mask.layout_id             = OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1;
  input.scheduled_re_mask.layout_version        = 1;
  input.scheduled_re_mask.lease_token           = metadata.source_plan_generation;
  input.slot_metadata                           = typed(OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1, metadata);
  input.source_sequence                         = 7;

  const auto encoded = encoder.encoder->encode(input, 3, 1000);
  ASSERT_TRUE(encoded) << to_string(encoded.error);
  EXPECT_LE(encoded.metadata.size(), class_c_encoder_config{}.metadata_capacity);
  EXPECT_EQ(validate_class_c_metadata(encoded.metadata), metadata_error::none);
}

TEST(dapp_class_c_publication_encoder, encodes_an_advance_ul_plan_without_a_tensor)
{
  auto encoder = make_encoder();
  ASSERT_TRUE(encoder) << encoder.detail;
  std::array<ocudu_dapp_ul_resource_span_v1, 2> spans{};
  spans[0].struct_size      = sizeof(spans[0]);
  spans[0].symbol_start     = 2;
  spans[0].symbol_count     = 4;
  spans[0].first_prb        = 10;
  spans[0].nof_prbs         = 8;
  spans[0].use_category     = OCUDU_DAPP_UL_USE_PUSCH_V1;
  spans[0].dmrs_symbol_mask = 1U << 3U;
  spans[0].allocation_id    = 7;
  spans[1].struct_size      = sizeof(spans[1]);
  spans[1].symbol_start     = 10;
  spans[1].symbol_count     = 2;
  spans[1].first_prb        = 30;
  spans[1].nof_prbs         = 1;
  spans[1].use_category     = OCUDU_DAPP_UL_USE_PUCCH_V1;
  spans[1].allocation_id    = 8;

  ocudu_dapp_ul_resource_plan_v1 plan{};
  plan.struct_size                     = sizeof(plan);
  plan.abi_major                       = OCUDU_DAPP_ABI_MAJOR_V1;
  plan.abi_minor                       = OCUDU_DAPP_ABI_MINOR_V1;
  plan.context.struct_size             = sizeof(plan.context);
  plan.context.schema_id               = OCUDU_DAPP_CONTEXT_UL_RESOURCE_PLAN_V1;
  plan.context.schema_version          = 1;
  plan.context.producer_instance_id[0] = 1;
  plan.context.producer_generation     = 1;
  plan.context.source_sequence         = 17;
  plan.context.cell_id                 = 3;
  plan.context.confidence_q16          = 1U << 16U;
  plan.context.created                 = {10, 4, 1, 0};
  plan.context.valid_from              = plan.context.created;
  plan.context.valid_until             = plan.context.created;
  plan.planned_time                    = plan.context.created;
  plan.plan_generation                 = 17;
  plan.spans                           = spans.data();
  plan.nof_spans                       = spans.size();
  plan.flags                           = OCUDU_DAPP_UL_RESOURCE_PLAN_COMPLETE_V1;
  plan.cell_nof_prbs                   = 52;

  const auto encoded = encoder.encoder->encode(plan, 5, 2000);
  ASSERT_TRUE(encoded) << to_string(encoded.error);
  EXPECT_EQ(validate_class_c_metadata(encoded.metadata), metadata_error::none);
  const auto* envelope = schema::GetPublicationEnvelope(encoded.metadata.data());
  ASSERT_NE(envelope, nullptr);
  EXPECT_EQ(envelope->publication_kind(), schema::PublicationKind::UlResourcePlan);
  EXPECT_EQ(envelope->publication_sequence(), plan.plan_generation);
  EXPECT_EQ(envelope->tensor(), nullptr);
  const auto* metadata = envelope->metadata_as_UlResourcePlanMetadata();
  ASSERT_NE(metadata, nullptr);
  ASSERT_NE(metadata->spans(), nullptr);
  EXPECT_EQ(metadata->plan_generation(), plan.plan_generation);
  EXPECT_EQ(metadata->prb_count(), plan.cell_nof_prbs);
  EXPECT_TRUE(metadata->complete());
  ASSERT_EQ(metadata->spans()->size(), spans.size());
  EXPECT_EQ(metadata->spans()->Get(0)->prb_start(), spans[0].first_prb);
  EXPECT_EQ(metadata->spans()->Get(1)->use_category(), schema::UlUseCategory::Pucch);

  plan.cell_nof_prbs = 0;
  EXPECT_EQ(encoder.encoder->encode(plan, 5, 2001).error, class_c_encode_error::invalid_input);
}

TEST(dapp_class_c_publication_encoder, reuses_encoder_for_srs_metadata_and_rejects_unsafe_inputs)
{
  auto encoder = make_encoder();
  ASSERT_TRUE(encoder);
  std::array<uint64_t, 8> data{};

  ocudu_dapp_srs_isac_metadata_v1 metadata{};
  metadata.struct_size           = sizeof(metadata);
  metadata.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  metadata.srs_resource_id       = 2;
  metadata.srs_config_id         = 3;
  metadata.rx_ports              = 1;
  metadata.tx_ports              = 2;
  metadata.comb_size             = 2;
  metadata.first_subcarrier      = 12;
  metadata.subcarrier_spacing_hz = 30'000;
  metadata.frequency_points      = 4;
  metadata.noise_variance        = 0.125F;
  metadata.rsrp_db               = -8.0F;
  metadata.epre_db               = -7.0F;
  metadata.timing_advance_ns     = 2000;
  ocudu_dapp_srs_isac_input_v1 input{};
  input.invocation        = invocation();
  input.channel_estimates = tensor(data.data(), OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(uint64_t));
  input.srs_metadata      = typed(OCUDU_DAPP_METADATA_SRS_ISAC_V1, metadata);
  input.source_sequence   = 9;

  auto encoded = encoder.encoder->encode(input, 4, 2000);
  ASSERT_TRUE(encoded) << to_string(encoded.error);
  const auto* envelope = schema::GetPublicationEnvelope(encoded.metadata.data());
  EXPECT_EQ(envelope->publication_kind(), schema::PublicationKind::SrsIsac);
  EXPECT_EQ(envelope->tensor()->element_type(), schema::ElementType::ComplexFp32);
  ASSERT_NE(envelope->metadata_as_SrsIsacMetadata(), nullptr);
  EXPECT_EQ(envelope->metadata_as_SrsIsacMetadata()->timing_advance_ns(), 2000);

  input.channel_estimates.domain = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
  EXPECT_EQ(encoder.encoder->encode(input, 4, 2001).error, class_c_encode_error::invalid_input);
  input.channel_estimates.backend_id       = OCUDU_DAPP_BACKEND_CUDA_V1;
  input.channel_estimates.access           = OCUDU_DAPP_ACCESS_READ_V1;
  input.channel_estimates.device_ordinal   = 1;
  input.channel_estimates.ready_token      = 17;
  input.channel_estimates.completion_token = 18;
  encoded                                  = encoder.encoder->encode(input, 4, 2002);
  ASSERT_TRUE(encoded) << to_string(encoded.error);
  envelope = schema::GetPublicationEnvelope(encoded.metadata.data());
  ASSERT_NE(envelope, nullptr);
  EXPECT_EQ(envelope->publication_kind(), schema::PublicationKind::SrsIsac);
  EXPECT_EQ(envelope->tensor()->domain(), schema::MemoryDomain::AcceleratorIpcExport);
  EXPECT_EQ(envelope->tensor()->backend_id(), OCUDU_DAPP_BACKEND_CUDA_V1);
  ASSERT_NE(envelope->metadata_as_SrsIsacMetadata(), nullptr);
  EXPECT_EQ(envelope->metadata_as_SrsIsacMetadata()->srs_resource_id(), metadata.srs_resource_id);

  input.channel_estimates.domain       = OCUDU_DAPP_MEM_HOST_V1;
  input.channel_estimates.backend_id   = OCUDU_DAPP_BACKEND_HOST_V1;
  input.channel_estimates.element_type = OCUDU_DAPP_ELEMENT_I8_V1;
  EXPECT_EQ(encoder.encoder->encode(input, 4, 2003).error, class_c_encode_error::unsupported_tensor);

  class_c_encoder_config invalid{};
  invalid.producer_instance = {};
  EXPECT_FALSE(class_c_publication_encoder::create(std::move(invalid)));
}
