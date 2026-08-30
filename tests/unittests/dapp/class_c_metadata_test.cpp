// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "class-c-publication-v1_generated.h"
#include "ocudu/dapp/ipc/class_c_metadata.h"
#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>
#include <random>

using namespace ocudu::dapp::ipc;
namespace schema = Ocudu::Dapp::ClassC::V1;

namespace {

std::vector<uint8_t> spectrum_envelope(const char*             schema_id             = class_c_schema_id,
                                       bool                    tensor_present        = true,
                                       uint64_t                allocation_generation = 1,
                                       uint8_t                 symbols               = 14,
                                       schema::PublicationKind kind = schema::PublicationKind::Spectrum)
{
  flatbuffers::FlatBufferBuilder builder;
  const schema::Id128            identity(1, 2);
  const schema::Id128            lease_id(3, 4);
  const schema::RadioTime        time(10, 2, 1, 0x3fff);
  const auto context = schema::CreateObservationContextDirect(builder, &time, 7, &identity, "monotonic", 100);
  std::vector<uint64_t>                         dimensions{1, 14, 12};
  std::vector<uint64_t>                         strides{1344, 96, 8};
  flatbuffers::Offset<schema::TensorDescriptor> tensor;
  if (tensor_present) {
    tensor = schema::CreateTensorDescriptorDirect(builder,
                                                  &identity,
                                                  allocation_generation,
                                                  schema::MemoryDomain::Host,
                                                  0,
                                                  schema::ElementType::ComplexFp32,
                                                  "rx-grid",
                                                  1,
                                                  0,
                                                  0,
                                                  1344,
                                                  &dimensions,
                                                  &strides,
                                                  65535,
                                                  nullptr,
                                                  &lease_id,
                                                  1000,
                                                  true);
  }
  const auto metadata = schema::CreateSpectrumMetadata(builder, 100'000'000, 1, symbols, 12);
  const auto envelope = schema::CreatePublicationEnvelopeDirect(builder,
                                                                schema_id,
                                                                1,
                                                                0,
                                                                kind,
                                                                1,
                                                                &identity,
                                                                context,
                                                                tensor,
                                                                schema::PublicationMetadata::SpectrumMetadata,
                                                                metadata.Union());
  schema::FinishPublicationEnvelopeBuffer(builder, envelope);
  return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

metadata_error validate(const std::vector<uint8_t>& buffer)
{
  return validate_class_c_metadata({buffer.data(), buffer.size()});
}

} // namespace

TEST(dapp_class_c_metadata, accepts_valid_spectrum_envelope)
{
  EXPECT_EQ(validate(spectrum_envelope()), metadata_error::none);
}

TEST(dapp_class_c_metadata, rejects_structural_schema_and_union_errors)
{
  auto truncated = spectrum_envelope();
  truncated.resize(truncated.size() / 2);
  EXPECT_EQ(validate(truncated), metadata_error::malformed_buffer);
  EXPECT_EQ(validate(spectrum_envelope("wrong.schema")), metadata_error::unsupported_schema);
  EXPECT_EQ(validate(spectrum_envelope(class_c_schema_id, true, 1, 14, schema::PublicationKind::SrsIsac)),
            metadata_error::metadata_mismatch);
}

TEST(dapp_class_c_metadata, enforces_tensor_and_use_case_semantics)
{
  EXPECT_EQ(validate(spectrum_envelope(class_c_schema_id, false)), metadata_error::tensor_required);
  EXPECT_EQ(validate(spectrum_envelope(class_c_schema_id, true, 0)), metadata_error::invalid_tensor);
  EXPECT_EQ(validate(spectrum_envelope(class_c_schema_id, true, 1, 0)), metadata_error::invalid_metadata);
}

TEST(dapp_class_c_metadata, bounded_mutation_corpus_is_rejected_or_safely_validated)
{
  const auto seed = spectrum_envelope();
  for (std::size_t index = 0; index != seed.size(); ++index) {
    auto mutation = seed;
    mutation[index] ^= static_cast<uint8_t>(1U << (index % 8U));
    static_cast<void>(validate(mutation));
  }

  std::mt19937 random(0xc1a55c01U);
  for (std::size_t size = 0; size != 1024; ++size) {
    std::vector<uint8_t> bytes(size);
    for (auto& value : bytes) {
      value = static_cast<uint8_t>(random());
    }
    static_cast<void>(validate(bytes));
  }
}
