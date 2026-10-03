// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// The hand-written E3AP Protobuf codec against libprotobuf built from the two
// schemas it implements:
//  - schemas/e3/interop/libe3/e3ap-1.0.0.proto (libe3, vendored verbatim), and
//  - schemas/e3/protobuf/e3ap-ocudu-v2.proto (the OCUDU wire superset).
// Checks that the superset schema really is one (every libe3 field unchanged,
// every addition numbered >= 100), that random PDUs of every procedure encode
// exactly as libprotobuf serialises them, and that a libe3 parser reading an
// OCUDU-profile PDU recovers exactly the libe3-profile PDU.

#include "e3ap-1.0.0.pb.h"
#include "e3ap-ocudu-v2.pb.h"
#include "e3ap_pdu_generator.h"
#include "ocudu/dapp/e3/e3ap_protobuf_codec.h"
#include <gtest/gtest.h>
#include <random>

using namespace ocudu::dapp::e3;
namespace gpb = google::protobuf;
using ocudu::dapp::e3::testing::pdu_generator;

namespace {

std::string to_hex(const std::string& bytes)
{
  static const char digits[] = "0123456789abcdef";
  std::string       text;
  for (const char byte : bytes) {
    text.push_back(digits[static_cast<uint8_t>(byte) >> 4]);
    text.push_back(digits[static_cast<uint8_t>(byte) & 15]);
  }
  return text;
}

std::string as_string(const std::vector<uint8_t>& bytes)
{
  return std::string(bytes.begin(), bytes.end());
}

/// libprotobuf's canonical serialisation (deterministic, so maps and unknowns are stable).
std::string serialise(const gpb::Message& message)
{
  std::string                 out;
  gpb::io::StringOutputStream stream(&out);
  gpb::io::CodedOutputStream  coded(&stream);
  coded.SetSerializationDeterministic(true);
  EXPECT_TRUE(message.SerializeToCodedStream(&coded));
  coded.Trim();
  return out;
}


const gpb::FileDescriptor* libe3_file()
{
  return libe3::e3ap::v1::E3Pdu::descriptor()->file();
}

const gpb::FileDescriptor* ocudu_file()
{
  return ocudu::e3ap::v2::E3Pdu::descriptor()->file();
}

} // namespace

TEST(dapp_e3ap_protobuf_conformance, ocudu_schema_is_a_wire_superset_of_the_libe3_schema)
{
  const auto* upstream = libe3_file();
  const auto* superset = ocudu_file();
  ASSERT_EQ(upstream->syntax(), superset->syntax());
  for (int index = 0; index != upstream->enum_type_count(); ++index) {
    const auto* theirs = upstream->enum_type(index);
    const auto* ours   = superset->FindEnumTypeByName(theirs->name());
    ASSERT_NE(ours, nullptr) << theirs->name();
    ASSERT_EQ(ours->value_count(), theirs->value_count()) << theirs->name();
    for (int value = 0; value != theirs->value_count(); ++value) {
      EXPECT_EQ(ours->FindValueByNumber(theirs->value(value)->number())->name(), theirs->value(value)->name());
    }
  }
  int libe3_fields = 0;
  for (int index = 0; index != upstream->message_type_count(); ++index) {
    const auto* theirs = upstream->message_type(index);
    const auto* ours   = superset->FindMessageTypeByName(theirs->name());
    ASSERT_NE(ours, nullptr) << theirs->name();
    for (int item = 0; item != theirs->field_count(); ++item, ++libe3_fields) {
      const auto* field = theirs->field(item);
      const auto* match = ours->FindFieldByNumber(field->number());
      ASSERT_NE(match, nullptr) << theirs->name() << "." << field->name();
      SCOPED_TRACE(theirs->full_name() + "." + field->name());
      EXPECT_EQ(match->name(), field->name());
      EXPECT_EQ(match->type(), field->type());
      EXPECT_EQ(match->label(), field->label());
      EXPECT_EQ(match->has_presence(), field->has_presence());
      EXPECT_EQ(match->is_packed(), field->is_packed());
      EXPECT_EQ(match->containing_oneof() != nullptr, field->containing_oneof() != nullptr);
      if (field->message_type() != nullptr) {
        EXPECT_EQ(match->message_type()->name(), field->message_type()->name());
      }
      if (field->enum_type() != nullptr) {
        EXPECT_EQ(match->enum_type()->name(), field->enum_type()->name());
      }
    }
    // Everything OCUDU adds to a libe3 message is numbered from 100.
    for (int item = 0; item != ours->field_count(); ++item) {
      const auto* field = ours->field(item);
      if (theirs->FindFieldByNumber(field->number()) == nullptr) {
        EXPECT_GE(field->number(), 100) << ours->name() << "." << field->name() << " collides with libe3's range";
      }
    }
  }
  EXPECT_EQ(libe3_fields, 58) << "libe3 0.2.1 schema changed: re-vendor and review the superset";
}

TEST(dapp_e3ap_protobuf_conformance, random_pdus_encode_exactly_as_libprotobuf_serialises_them)
{
  for (const auto profile : {e3ap_profile::libe3, e3ap_profile::ocudu}) {
    pdu_generator generator(profile == e3ap_profile::ocudu ? 11 : 22, profile);
    for (std::size_t iteration = 0; iteration != 6000; ++iteration) {
      const auto pdu     = generator.next(iteration);
      auto       encoded = e3ap_protobuf_codec::encode(pdu, profile);
      ASSERT_TRUE(encoded) << encoded.detail << " (kind " << iteration % 12 << ")";
      const std::string ours = as_string(*encoded.value);

      ocudu::e3ap::v2::E3Pdu superset;
      ASSERT_TRUE(superset.ParseFromString(ours));
      EXPECT_EQ(superset.GetReflection()->GetUnknownFields(superset).field_count(), 0) << "field not in e3ap-ocudu-v2.proto";
      ASSERT_EQ(to_hex(serialise(superset)), to_hex(ours)) << "not libprotobuf-canonical: " << superset.DebugString();

      // And it reads back to the same PDU.
      auto decoded = e3ap_protobuf_codec::decode(*encoded.value, profile);
      ASSERT_TRUE(decoded) << decoded.detail;
      auto again = e3ap_protobuf_codec::encode(*decoded.value, profile);
      ASSERT_TRUE(again);
      ASSERT_EQ(to_hex(as_string(*again.value)), to_hex(ours));

      // What libe3 makes of it: the libe3 view, never an error, for every libe3 procedure.
      libe3::e3ap::v1::E3Pdu upstream;
      ASSERT_TRUE(upstream.ParseFromString(ours));
      if (std::holds_alternative<e3ap_control_response>(pdu.message)) {
        EXPECT_EQ(upstream.msg_case(), libe3::e3ap::v1::E3Pdu::MSG_NOT_SET);
        continue;
      }
      if (profile == e3ap_profile::libe3) {
        EXPECT_EQ(to_hex(serialise(upstream)), to_hex(ours));
        EXPECT_EQ(upstream.GetReflection()->GetUnknownFields(upstream).field_count(), 0);
      } else {
        // Stripping the OCUDU additions leaves exactly the libe3-profile bytes, whenever the
        // PDU fits the libe3 module at all.
        upstream.DiscardUnknownFields();
        auto plain = e3ap_protobuf_codec::encode(pdu, e3ap_profile::libe3);
        if (plain) {
          EXPECT_EQ(to_hex(serialise(upstream)), to_hex(as_string(*plain.value)));
        }
      }
    }
  }
}

TEST(dapp_e3ap_protobuf_conformance, libprotobuf_output_with_reordered_fields_decodes_to_the_same_pdu)
{
  // libprotobuf may emit fields in any order on the wire (e.g. after merges); a reflection-built
  // message serialised field by field in reverse must decode the same as the canonical form.
  pdu_generator generator(33, e3ap_profile::ocudu);
  for (std::size_t iteration = 0; iteration != 1200; ++iteration) {
    const auto pdu     = generator.next(iteration);
    auto       encoded = e3ap_protobuf_codec::encode(pdu, e3ap_profile::ocudu);
    ASSERT_TRUE(encoded) << encoded.detail;
    ocudu::e3ap::v2::E3Pdu message;
    ASSERT_TRUE(message.ParseFromString(as_string(*encoded.value)));

    // Reverse the top-level fields: the oneof body first, then timestamp, then id.
    const auto*                               reflection = message.GetReflection();
    std::vector<const gpb::FieldDescriptor*> fields;
    reflection->ListFields(message, &fields);
    std::string reversed;
    for (auto it = fields.rbegin(); it != fields.rend(); ++it) {
      ocudu::e3ap::v2::E3Pdu single;
      single.GetReflection()->SwapFields(&single, &message, {*it}); // move one field across
      reversed += serialise(single);
      single.GetReflection()->SwapFields(&single, &message, {*it});
    }
    ASSERT_EQ(reversed.size(), encoded.value->size());
    auto decoded = e3ap_protobuf_codec::decode(std::vector<uint8_t>(reversed.begin(), reversed.end()),
                                               e3ap_profile::ocudu);
    ASSERT_TRUE(decoded) << decoded.detail;
    auto again = e3ap_protobuf_codec::encode(*decoded.value, e3ap_profile::ocudu);
    ASSERT_TRUE(again);
    ASSERT_EQ(to_hex(as_string(*again.value)), to_hex(as_string(*encoded.value)));
  }
}
