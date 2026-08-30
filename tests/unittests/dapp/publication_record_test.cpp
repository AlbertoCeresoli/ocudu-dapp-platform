// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/process/v1/publication_record.h"
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace {

std::vector<uint8_t> make_inline_record()
{
  constexpr std::size_t                   tensor_offset = 128;
  std::vector<uint8_t>                    record(tensor_offset + 4);
  ocudu_dapp_publication_record_header_v1 header{};
  header.magic                = OCUDU_DAPP_PUBLICATION_RECORD_MAGIC_V1;
  header.version              = OCUDU_DAPP_PUBLICATION_RECORD_VERSION_V1;
  header.header_size          = sizeof(header);
  header.flags                = OCUDU_DAPP_PUBLICATION_RECORD_INLINE_TENSOR_V1;
  header.metadata_size        = 3;
  header.tensor_offset        = tensor_offset;
  header.tensor_size          = 4;
  header.publication_sequence = 7;
  header.route_epoch          = 2;
  std::memcpy(record.data(), &header, sizeof(header));
  const std::array<uint8_t, 3> metadata{1, 2, 3};
  const std::array<uint8_t, 4> tensor{4, 5, 6, 7};
  std::memcpy(record.data() + sizeof(header), metadata.data(), metadata.size());
  std::memcpy(record.data() + tensor_offset, tensor.data(), tensor.size());
  return record;
}

} // namespace

TEST(dapp_publication_record, exposes_metadata_and_inline_tensor_without_copy)
{
  const auto                            record = make_inline_record();
  ocudu_dapp_publication_record_view_v1 view{};
  ASSERT_EQ(ocudu_dapp_parse_publication_record_v1(record.data(), record.size(), &view),
            OCUDU_DAPP_PUBLICATION_RECORD_OK_V1);
  EXPECT_EQ(view.header.metadata_size, 3U);
  EXPECT_EQ(view.header.tensor_size, 4U);
  EXPECT_EQ(view.metadata, record.data() + sizeof(view.header));
  EXPECT_EQ(view.tensor, record.data() + view.header.tensor_offset);
}

TEST(dapp_publication_record, accepts_external_tensor_metadata_without_embedded_bytes)
{
  std::vector<uint8_t> record(sizeof(ocudu_dapp_publication_record_header_v1) + 2);
  auto*                header  = reinterpret_cast<ocudu_dapp_publication_record_header_v1*>(record.data());
  header->magic                = OCUDU_DAPP_PUBLICATION_RECORD_MAGIC_V1;
  header->version              = OCUDU_DAPP_PUBLICATION_RECORD_VERSION_V1;
  header->header_size          = sizeof(*header);
  header->flags                = OCUDU_DAPP_PUBLICATION_RECORD_EXTERNAL_TENSOR_V1;
  header->metadata_size        = 2;
  header->tensor_offset        = 17;
  header->tensor_size          = 4096;
  header->publication_sequence = 1;
  header->route_epoch          = 1;
  ocudu_dapp_publication_record_view_v1 view{};
  ASSERT_EQ(ocudu_dapp_parse_publication_record_v1(record.data(), record.size(), &view),
            OCUDU_DAPP_PUBLICATION_RECORD_OK_V1);
  EXPECT_EQ(view.tensor, nullptr);
  EXPECT_EQ(view.header.tensor_offset, 17U);
  EXPECT_EQ(view.header.tensor_size, 4096U);

  header->tensor_offset = uint64_t{UINT32_MAX} + 1;
  EXPECT_EQ(ocudu_dapp_parse_publication_record_v1(record.data(), record.size(), &view),
            OCUDU_DAPP_PUBLICATION_RECORD_INVALID_EXTENT_V1);
}

TEST(dapp_publication_record, rejects_conflicting_flags_and_out_of_range_tensor)
{
  auto  record = make_inline_record();
  auto* header = reinterpret_cast<ocudu_dapp_publication_record_header_v1*>(record.data());
  header->flags |= OCUDU_DAPP_PUBLICATION_RECORD_EXTERNAL_TENSOR_V1;
  ocudu_dapp_publication_record_view_v1 view{};
  EXPECT_EQ(ocudu_dapp_parse_publication_record_v1(record.data(), record.size(), &view),
            OCUDU_DAPP_PUBLICATION_RECORD_INVALID_FLAGS_V1);
  header->flags         = OCUDU_DAPP_PUBLICATION_RECORD_INLINE_TENSOR_V1;
  header->tensor_offset = record.size() + 64;
  EXPECT_EQ(ocudu_dapp_parse_publication_record_v1(record.data(), record.size(), &view),
            OCUDU_DAPP_PUBLICATION_RECORD_INVALID_EXTENT_V1);
}

TEST(dapp_publication_record, rejects_nonzero_alignment_padding)
{
  auto record = make_inline_record();
  record[100] = 1;
  ocudu_dapp_publication_record_view_v1 view{};
  EXPECT_EQ(ocudu_dapp_parse_publication_record_v1(record.data(), record.size(), &view),
            OCUDU_DAPP_PUBLICATION_RECORD_NONZERO_PADDING_V1);
}
