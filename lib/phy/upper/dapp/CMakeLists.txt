# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

add_library(ocudu_phy_dapp_adapters STATIC
        native_pusch_dapp_demodulator_target.cpp
        native_receiver_resident_adapter.cpp
        pusch_dapp_channel_estimator.cpp
        pusch_dapp_demodulator.cpp
        pusch_resident_dapp.cpp
        resource_grid_tensor_adapter.cpp
        srs_dapp_channel_estimator.cpp
        upper_phy_dapp_data_router.cpp
        upper_phy_dapp_hook_provider.cpp
        upper_phy_dapp_process_target.cpp)
if(ENABLE_E3)
    target_sources(ocudu_phy_dapp_adapters PRIVATE upper_phy_dapp_portable_e3_target.cpp)
endif()
target_link_libraries(ocudu_phy_dapp_adapters PUBLIC
        OCUDU::DAppSDK
        ocudu_dapp_runtime
        ocudu_channel_processors
        ocudu_channel_modulation
        ocudu_signal_processors
        ocudu_phy_support)
target_link_libraries(ocudu_phy_dapp_adapters PRIVATE ocudu_native_control_context)
if(CUDA_ACCEL_FOUND)
    target_sources(ocudu_phy_dapp_adapters PRIVATE
            class_c_cuda_export_pool.cpp
            srs_channel_estimate_cuda.cu
            srs_dapp_channel_estimator_cuda.cu
            srs_channel_estimate_tensor_adapter.cpp)
    target_compile_definitions(ocudu_phy_dapp_adapters PRIVATE ENABLE_CUDA)
    target_link_libraries(ocudu_phy_dapp_adapters PUBLIC CUDA::cudart CUDA::cuda_driver)
    # Each CUDA translation unit is self-contained; keep the static archive
    # directly linkable from ordinary C++ targets without a device-link step.
    set_target_properties(ocudu_phy_dapp_adapters PROPERTIES CUDA_SEPARABLE_COMPILATION OFF)
endif()
