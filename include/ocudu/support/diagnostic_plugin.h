// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <stdint.h>

#define OCUDU_DIAGNOSTIC_PLUGIN_ABI_VERSION 102

#ifdef __cplusplus
extern "C" {
#endif

enum ocudu_diagnostic_plugin_stream {
  OCUDU_DIAGNOSTIC_PLUGIN_PUSCH_SYMBOLS      = 0,
  OCUDU_DIAGNOSTIC_PLUGIN_PUSCH_CRC          = 1,
  OCUDU_DIAGNOSTIC_PLUGIN_SRS_CHANNEL        = 2,
  OCUDU_DIAGNOSTIC_PLUGIN_PUSCH_DMRS_CHANNEL = 3
};

typedef struct ocudu_diagnostic_plugin_handle ocudu_diagnostic_plugin_handle;

typedef ocudu_diagnostic_plugin_handle* (*ocudu_diagnostic_plugin_open_fn)(const char* endpoint);
typedef void (*ocudu_diagnostic_plugin_close_fn)(ocudu_diagnostic_plugin_handle* handle);
typedef int (*ocudu_diagnostic_plugin_should_collect_fn)(ocudu_diagnostic_plugin_handle* handle,
                                                         uint8_t                         stream,
                                                         uint16_t                        rnti);
typedef int (*ocudu_diagnostic_plugin_is_collect_due_fn)(ocudu_diagnostic_plugin_handle* handle,
                                                         uint8_t                         stream,
                                                         uint16_t                        rnti);
typedef int (*ocudu_diagnostic_plugin_publish_crc_fn)(ocudu_diagnostic_plugin_handle* handle,
                                                      uint16_t                        rnti,
                                                      uint16_t                        sfn,
                                                      uint16_t                        slot,
                                                      int                             crc_ok);
typedef int (*ocudu_diagnostic_plugin_publish_symbols_fn)(ocudu_diagnostic_plugin_handle* handle,
                                                          uint16_t                        rnti,
                                                          uint16_t                        sfn,
                                                          uint16_t                        slot,
                                                          uint16_t                        nof_prb,
                                                          uint16_t                        nof_data_symbols,
                                                          uint8_t                         nof_layers,
                                                          uint8_t                         mod_bits,
                                                          float                           sinr_db,
                                                          float                           epre_db,
                                                          float                           rsrp_db,
                                                          float                           evm,
                                                          float                           time_alignment_us,
                                                          float                           cfo_hz,
                                                          float                           target_code_rate,
                                                          const float*                    interleaved_cf32,
                                                          uint32_t                        nof_floats);
typedef int (*ocudu_diagnostic_plugin_publish_srs_fn)(ocudu_diagnostic_plugin_handle* handle,
                                                      uint16_t                        rnti,
                                                      uint16_t                        sfn,
                                                      uint16_t                        slot,
                                                      uint16_t                        nof_subcarriers,
                                                      uint8_t                         comb_size,
                                                      uint8_t                         nof_rx_ports,
                                                      uint8_t                         nof_tx_ports,
                                                      uint8_t                         rx_port_index,
                                                      uint8_t                         tx_port_index,
                                                      float                           rsrp_db,
                                                      float                           epre_db,
                                                      float                           noise_variance,
                                                      float                           time_alignment_ns,
                                                      const float*                    interleaved_cf32,
                                                      uint32_t                        nof_floats);
typedef int (*ocudu_diagnostic_plugin_publish_metrics_json_fn)(const char* json, uint32_t len);
typedef int (*ocudu_diagnostic_plugin_abi_version_fn)(void);

ocudu_diagnostic_plugin_handle* ocudu_diagnostic_plugin_open(const char* endpoint);
void                            ocudu_diagnostic_plugin_close(ocudu_diagnostic_plugin_handle* handle);
int ocudu_diagnostic_plugin_should_collect(ocudu_diagnostic_plugin_handle* handle, uint8_t stream, uint16_t rnti);
int ocudu_diagnostic_plugin_is_collect_due(ocudu_diagnostic_plugin_handle* handle, uint8_t stream, uint16_t rnti);
int ocudu_diagnostic_plugin_publish_crc(ocudu_diagnostic_plugin_handle* handle,
                                        uint16_t                        rnti,
                                        uint16_t                        sfn,
                                        uint16_t                        slot,
                                        int                             crc_ok);
int ocudu_diagnostic_plugin_publish_symbols(ocudu_diagnostic_plugin_handle* handle,
                                            uint16_t                        rnti,
                                            uint16_t                        sfn,
                                            uint16_t                        slot,
                                            uint16_t                        nof_prb,
                                            uint16_t                        nof_data_symbols,
                                            uint8_t                         nof_layers,
                                            uint8_t                         mod_bits,
                                            float                           sinr_db,
                                            float                           epre_db,
                                            float                           rsrp_db,
                                            float                           evm,
                                            float                           time_alignment_us,
                                            float                           cfo_hz,
                                            float                           target_code_rate,
                                            const float*                    interleaved_cf32,
                                            uint32_t                        nof_floats);
int ocudu_diagnostic_plugin_publish_srs(ocudu_diagnostic_plugin_handle* handle,
                                        uint16_t                        rnti,
                                        uint16_t                        sfn,
                                        uint16_t                        slot,
                                        uint16_t                        nof_subcarriers,
                                        uint8_t                         comb_size,
                                        uint8_t                         nof_rx_ports,
                                        uint8_t                         nof_tx_ports,
                                        uint8_t                         rx_port_index,
                                        uint8_t                         tx_port_index,
                                        float                           rsrp_db,
                                        float                           epre_db,
                                        float                           noise_variance,
                                        float                           time_alignment_ns,
                                        const float*                    interleaved_cf32,
                                        uint32_t                        nof_floats);
int ocudu_diagnostic_plugin_publish_metrics_json(const char* json, uint32_t len);
int ocudu_diagnostic_plugin_abi_version(void);

#ifdef __cplusplus
}
#endif
