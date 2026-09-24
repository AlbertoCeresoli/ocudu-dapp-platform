// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "fixtures/fixture_common.h"
#include "ocudu/dapp/runtime/abi_validation.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/pusch_metadata.h"
#include "ocudu/dapp/use_cases/v1/srs_channel_estimator.h"
#include <gtest/gtest.h>
#include <limits>

using namespace ocudu::dapp;

namespace {

ocudu_dapp_tensor_view_v1 valid_tensor()
{
  ocudu_dapp_tensor_view_v1 view{};
  view.struct_size           = sizeof(view);
  view.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  view.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  view.domain                = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
  view.element_type          = OCUDU_DAPP_ELEMENT_CF16_V1;
  view.rank                  = 2;
  view.access                = OCUDU_DAPP_ACCESS_READ_V1;
  view.allocation_id         = 3;
  view.allocation_generation = 7;
  view.byte_length           = 48;
  view.dimensions[0]         = 3;
  view.dimensions[1]         = 4;
  view.byte_strides[0]       = 16;
  view.byte_strides[1]       = 4;
  view.lease_token           = 11;
  return view;
}

ocudu_dapp_status_v1 create_worker(void* instance, uint32_t, void** output)
{
  *output = instance;
  return OCUDU_DAPP_OK_V1;
}

void destroy_worker(void*, void*) {}

ocudu_dapp_status_v1
invoke_channel_estimator(void*, const ocudu_dapp_channel_estimator_input_v1*, ocudu_dapp_channel_estimator_output_v1*)
{
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 invoke_srs_channel_estimator(void*,
                                                  const ocudu_dapp_srs_channel_estimator_input_v1*,
                                                  ocudu_dapp_srs_channel_estimator_output_v1*)
{
  return OCUDU_DAPP_OK_V1;
}

} // namespace

TEST(dapp_abi_validation, accepts_bounded_strided_tensor)
{
  EXPECT_EQ(validate_tensor_view(valid_tensor()), validation_error::none);
}

TEST(dapp_abi_validation, rejects_stale_or_unleased_allocation)
{
  auto view                  = valid_tensor();
  view.allocation_generation = 0;
  EXPECT_EQ(validate_tensor_view(view), validation_error::missing_generation);
}

TEST(dapp_abi_validation, rejects_tensor_extent_outside_lease)
{
  auto view        = valid_tensor();
  view.byte_length = 47;
  EXPECT_EQ(validate_tensor_view(view), validation_error::invalid_byte_range);
}

TEST(dapp_abi_validation, detects_stride_arithmetic_overflow)
{
  auto view            = valid_tensor();
  view.dimensions[0]   = 3;
  view.byte_strides[0] = std::numeric_limits<uint64_t>::max();
  EXPECT_EQ(validate_tensor_view(view), validation_error::arithmetic_overflow);
}

TEST(dapp_abi_validation, caller_owned_storage_identity_excludes_only_completion_publication)
{
  const auto expected     = valid_tensor();
  auto       actual       = expected;
  actual.completion_token = 19;
  EXPECT_TRUE(same_tensor_storage(expected, actual));
  ++actual.allocation_generation;
  EXPECT_FALSE(same_tensor_storage(expected, actual));
}

TEST(dapp_abi_validation, absent_typed_metadata_requires_the_all_zero_representation)
{
  ocudu_dapp_typed_metadata_v1 metadata{};
  EXPECT_TRUE(empty_typed_metadata(metadata));

  metadata.struct_size = sizeof(metadata);
  EXPECT_FALSE(empty_typed_metadata(metadata));
  metadata               = {};
  metadata.metadata.data = &metadata;
  EXPECT_FALSE(empty_typed_metadata(metadata));
  metadata                   = {};
  metadata.metadata.reserved = 1;
  EXPECT_FALSE(empty_typed_metadata(metadata));
}

TEST(dapp_abi_validation, class_a_and_b_are_in_process_only)
{
  EXPECT_EQ(validate_interface_placement(OCUDU_DAPP_CLASS_A_INLINE_V1,
                                         OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1,
                                         OCUDU_DAPP_THREADING_PER_WORKER_V1),
            validation_error::forbidden_placement);
  EXPECT_EQ(validate_interface_placement(OCUDU_DAPP_CLASS_B_DEADLINE_V1,
                                         OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1,
                                         OCUDU_DAPP_THREADING_REENTRANT_SHARED_V1),
            validation_error::forbidden_placement);
  EXPECT_EQ(validate_interface_placement(OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1,
                                         OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1,
                                         OCUDU_DAPP_THREADING_SERIALIZED_V1),
            validation_error::none);
}

TEST(dapp_abi_validation, serialized_threading_is_rejected_on_deadline_paths)
{
  EXPECT_EQ(validate_interface_placement(
                OCUDU_DAPP_CLASS_A_INLINE_V1, OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1, OCUDU_DAPP_THREADING_SERIALIZED_V1),
            validation_error::forbidden_threading);
}

TEST(dapp_abi_validation, accepts_complete_optional_management_extension)
{
  ocudu_dapp_module_v1 module{};
  populate_valid_module(&module);
  EXPECT_EQ(validate_module_descriptor(&module), validation_error::none);
}

TEST(dapp_abi_validation, rejects_truncated_or_incomplete_management_extension)
{
  ocudu_dapp_module_v1 module{};
  populate_valid_module(&module);
  ocudu_dapp_management_v1 management = *module.management;
  module.management                   = &management;
  management.struct_size              = offsetof(ocudu_dapp_management_v1, reserved) - 1;
  EXPECT_EQ(validate_module_descriptor(&module), validation_error::structure_too_small);

  management.struct_size = sizeof(management);
  management.set_config  = nullptr;
  EXPECT_EQ(validate_module_descriptor(&module), validation_error::missing_callback);
}

TEST(dapp_abi_validation, validates_optional_class_a_pusch_profile)
{
  ocudu_dapp_module_v1 module{};
  populate_valid_module(&module);
  auto interface       = module.interfaces[0];
  module.interfaces    = &interface;
  interface.dapp_class = OCUDU_DAPP_CLASS_A_INLINE_V1;
  interface.threading  = OCUDU_DAPP_THREADING_PER_WORKER_V1;

  ocudu_dapp_pusch_profile_v1 profile{};
  profile.struct_size        = sizeof(profile);
  profile.abi_major          = OCUDU_DAPP_ABI_MAJOR_V1;
  profile.profile_id         = OCUDU_DAPP_PUSCH_PROFILE_V1;
  profile.max_nof_prb        = 275;
  profile.max_nof_rx_ports   = 64;
  profile.max_nof_layers     = 2;
  profile.modulation_mask    = OCUDU_DAPP_PUSCH_MODULATION_QPSK_V1 | OCUDU_DAPP_PUSCH_MODULATION_QAM16_V1;
  profile.dmrs_type_mask     = OCUDU_DAPP_PUSCH_DMRS_TYPE1_V1;
  interface.use_case_profile = &profile;
  EXPECT_EQ(validate_module_descriptor(&module), validation_error::none);

  profile.max_nof_layers = 0;
  EXPECT_EQ(validate_module_descriptor(&module), validation_error::invalid_dimensions);
  profile.max_nof_layers  = 2;
  profile.modulation_mask = UINT32_C(1) << 31U;
  EXPECT_EQ(validate_module_descriptor(&module), validation_error::invalid_dimensions);

  profile.modulation_mask = OCUDU_DAPP_PUSCH_MODULATION_QPSK_V1;
  interface.dapp_class    = OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1;
  EXPECT_EQ(validate_module_descriptor(&module), validation_error::invalid_dimensions);
}

TEST(dapp_abi_validation, validates_known_interface_worker_and_invoke_vtables)
{
  ocudu_dapp_worker_api_v1 worker{};
  worker.struct_size            = sizeof(worker);
  worker.abi_major              = OCUDU_DAPP_ABI_MAJOR_V1;
  worker.create_worker_context  = create_worker;
  worker.destroy_worker_context = destroy_worker;
  ocudu_dapp_channel_estimator_interface_v1 api{worker, invoke_channel_estimator, {}};

  ocudu_dapp_module_v1 module{};
  populate_valid_module(&module);
  auto interface             = module.interfaces[0];
  module.interfaces          = &interface;
  interface.interface_id     = OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1;
  interface.interface_major  = 1;
  interface.interface_vtable = &api;
  EXPECT_EQ(validate_module_descriptor(&module), validation_error::none);

  api.worker.struct_size = offsetof(ocudu_dapp_worker_api_v1, reserved) - 1;
  EXPECT_EQ(validate_module_descriptor(&module), validation_error::structure_too_small);
  api.worker.struct_size = sizeof(api.worker);
  api.invoke             = nullptr;
  EXPECT_EQ(validate_module_descriptor(&module), validation_error::missing_callback);
  api.invoke                = invoke_channel_estimator;
  interface.interface_major = 2;
  EXPECT_EQ(validate_module_descriptor(&module), validation_error::incompatible_abi);
}

TEST(dapp_abi_validation, srs_channel_estimator_requires_class_a_and_its_own_profile)
{
  ocudu_dapp_worker_api_v1 worker{};
  worker.struct_size            = sizeof(worker);
  worker.abi_major              = OCUDU_DAPP_ABI_MAJOR_V1;
  worker.create_worker_context  = create_worker;
  worker.destroy_worker_context = destroy_worker;
  ocudu_dapp_srs_channel_estimator_interface_v1 api{worker, invoke_srs_channel_estimator, {}};

  ocudu_dapp_srs_profile_v1 profile{};
  profile.struct_size         = sizeof(profile);
  profile.abi_major           = OCUDU_DAPP_ABI_MAJOR_V1;
  profile.profile_id          = OCUDU_DAPP_SRS_PROFILE_V1;
  profile.max_nof_rx_ports    = 4;
  profile.max_nof_tx_ports    = 2;
  profile.max_sequence_length = 816;
  profile.comb_mask           = OCUDU_DAPP_SRS_COMB_2_V1 | OCUDU_DAPP_SRS_COMB_4_V1;

  ocudu_dapp_module_v1 module{};
  populate_valid_module(&module);
  auto interface             = module.interfaces[0];
  module.interfaces          = &interface;
  interface.interface_id     = OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1;
  interface.interface_major  = 1;
  interface.interface_vtable = &api;
  interface.dapp_class       = OCUDU_DAPP_CLASS_A_INLINE_V1;
  interface.threading        = OCUDU_DAPP_THREADING_PER_WORKER_V1;
  interface.use_case_profile = &profile;
  EXPECT_EQ(validate_module_descriptor(&module), validation_error::none);
  EXPECT_TRUE(is_class_a_interface(OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1));

  // The profile is mandatory, and it is the SRS profile.
  interface.use_case_profile = nullptr;
  EXPECT_EQ(validate_module_descriptor(&module), validation_error::invalid_dimensions);
  interface.use_case_profile = &profile;
  for (const auto corrupt : {+[](ocudu_dapp_srs_profile_v1& value) { value.profile_id = OCUDU_DAPP_PUSCH_PROFILE_V1; },
                             +[](ocudu_dapp_srs_profile_v1& value) { value.max_nof_rx_ports = 9; },
                             +[](ocudu_dapp_srs_profile_v1& value) { value.max_nof_tx_ports = 0; },
                             +[](ocudu_dapp_srs_profile_v1& value) { value.max_sequence_length = 1633; },
                             +[](ocudu_dapp_srs_profile_v1& value) { value.comb_mask = 4; },
                             +[](ocudu_dapp_srs_profile_v1& value) { value.flags = 1; }}) {
    auto invalid = profile;
    corrupt(invalid);
    interface.use_case_profile = &invalid;
    EXPECT_EQ(validate_module_descriptor(&module), validation_error::invalid_dimensions);
  }
  interface.use_case_profile = &profile;
  interface.dapp_class       = OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1;
  EXPECT_EQ(validate_module_descriptor(&module), validation_error::invalid_dimensions);

  interface.dapp_class = OCUDU_DAPP_CLASS_A_INLINE_V1;
  api.invoke           = nullptr;
  EXPECT_EQ(validate_module_descriptor(&module), validation_error::missing_callback);
}
