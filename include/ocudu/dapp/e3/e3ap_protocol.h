// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/e3/telemetry_types.h"
#include "ocudu/dapp/management/types.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ocudu::dapp::e3 {

/// Transport-neutral C++ representation of E3AP-OCUDU-V1 and the two
/// service models it carries. Service-model bodies stay opaque at the E3AP
/// layer: control payloads are APER E3SM-OCUDU-DAPP-MGMT-V1 messages and
/// indication payloads are APER OCUDU-TELEMETRY-SM messages, each possibly
/// split into fragments.
constexpr const char* e3ap_version_string         = "1.0.0";
constexpr uint32_t    e3ap_maximum_payload_bytes  = 16U * 1024U * 1024U;
constexpr uint32_t    e3ap_maximum_fragment_bytes = 64U * 1024U;
constexpr uint32_t    e3ap_maximum_function_data  = 256U * 1024U;
constexpr uint32_t    e3ap_payload_protocol_id    = 0x00E3A001U;
constexpr uint16_t    e3ap_procedure_stream       = 0;
constexpr uint16_t    e3ap_indication_stream      = 1;
constexpr uint16_t    e3ap_default_port           = 36423;

/// Registered RAN functions.
constexpr uint16_t e3ap_ran_function_telemetry  = 10;
constexpr uint16_t e3ap_ran_function_management = 11;
/// Control identifier of the management RAN function.
constexpr uint16_t e3ap_management_control_request = 1;

enum class e3ap_response_code : uint8_t { positive, negative };

enum class e3ap_cause : uint8_t {
  unspecified,
  protocol_error,
  setup_required,
  setup_already_complete,
  unsupported_protocol_version,
  unsupported_ran_function,
  unsupported_identifier,
  invalid_request,
  not_found,
  duplicate,
  resource_exhausted,
  authorization_rejected,
  payload_too_large,
  transport_overrun,
  shutting_down
};

const char* to_string(e3ap_cause cause) noexcept;

/// Requested tensor delivery for a subscription.
enum class e3ap_delivery_mode : uint8_t { in_band, by_reference };

/// Memory domain of a by-reference tensor. Values mirror the Class-C OCC3
/// metadata (\c MemoryDomain in class-c-publication-v1.fbs).
enum class e3ap_memory_domain : uint8_t {
  host,
  host_pinned,
  accelerator_device,
  managed_coherent,
  posix_shared,
  accelerator_ipc_export
};

/// Element type of a by-reference tensor. Values mirror the Class-C OCC3
/// metadata (\c ElementType in class-c-publication-v1.fbs).
enum class e3ap_element_type : uint8_t {
  int8,
  uint8,
  int16,
  uint16,
  int32,
  fp16,
  bf16,
  fp32,
  complex_int16,
  complex_bf16,
  complex_fp16,
  complex_fp32
};

/// One POSIX shared-memory region mapped by the RAN for a by-reference
/// subscription and advertised once in the subscription response.
struct e3ap_shared_region {
  e3ap_memory_domain memory_domain{e3ap_memory_domain::posix_shared};
  /// POSIX shared-memory name (\c shm_open), e.g. "/ocudu-e3-1234-1-1".
  std::string region_name;
  uint64_t    region_bytes{};
  uint16_t    header_bytes{64};
  uint64_t    row_bytes{};
  uint16_t    nof_rows{};
  uint16_t    layout_version{1};
};

/// Tensor located in a shared region row (CPU consumers).
struct e3ap_posix_shared_location {
  std::string region_name;
  uint16_t    row_index{};
  /// Offset of the tensor bytes from the start of the region.
  uint64_t byte_offset{};
};

/// Tensor located in an accelerator IPC export (GPU consumers). The handles
/// mirror the Class-C CUDA export slot: opaque 64-byte memory and ready-event
/// handles interpreted only by the matching accelerator runtime.
struct e3ap_accelerator_ipc_location {
  uint16_t                               device_ordinal{};
  uint64_t                               allocation_id{};
  std::array<uint8_t, 64>                memory_handle{};
  std::optional<std::array<uint8_t, 64>> ready_event_handle;
  uint64_t                               byte_offset{};
};

using e3ap_tensor_location = std::variant<e3ap_posix_shared_location, e3ap_accelerator_ipc_location>;

/// Locates one published tensor delivered by reference.
struct e3ap_tensor_descriptor {
  e3ap_memory_domain   memory_domain{e3ap_memory_domain::posix_shared};
  e3ap_tensor_location location;
  uint64_t             byte_length{};
  /// Row (or slot) generation at publication; readers that observe a different
  /// value after copying have read a torn row and must discard it.
  uint64_t                         generation{};
  std::optional<e3ap_element_type> element_type;
  std::vector<uint32_t>            dimensions;
  std::vector<uint64_t>            byte_strides;
  std::optional<uint16_t>          layout_id;
  std::optional<uint64_t>          lease_expiry_ns;
};

struct e3ap_ran_function_definition {
  uint16_t              ran_function_identifier{};
  std::vector<uint16_t> telemetry_identifiers;
  std::vector<uint16_t> control_identifiers;
  std::vector<uint8_t>  ran_function_data;
  std::string           ran_function_name;
  std::string           ran_function_version;
};

/// First PDU on a new association: identifies the dApp. A major-compatible
/// protocol version ("1.x") is required; every other procedure before setup
/// fails with \ref e3ap_cause::setup_required.
struct e3ap_setup_request {
  std::string e3ap_protocol_version{e3ap_version_string};
  std::string dapp_name;
  std::string dapp_version;
  std::string vendor;
  /// Optional stable identity a reconnecting dApp presents.
  std::optional<management::identifier> dapp_instance;
};

/// Positive responses assign the dApp identifier the client must echo on
/// every later procedure and advertise the RAN-function catalog together with
/// the server's payload/fragment bounds.
struct e3ap_setup_response {
  uint32_t           request_id{};
  e3ap_response_code response_code{e3ap_response_code::negative};
  /// Server identity (configured \c ran_identifier, defaults to the gNB name).
  std::string                ran_identifier;
  std::optional<std::string> e3ap_protocol_version;
  /// Identifier assigned to this dApp for the life of the association.
  std::optional<uint16_t>                   dapp_identifier;
  std::vector<e3ap_ran_function_definition> ran_functions;
  bool                                      ran_functions_present{};
  std::optional<e3ap_cause>                 cause;
  std::string                               detail;
  std::optional<management::identifier>     ran_instance;
  /// Largest reassembled service-model payload the server accepts or produces
  /// (4 MiB by default, bounded by the portable store's maximum object size;
  /// the protocol ceiling is \ref e3ap_maximum_payload_bytes).
  std::optional<uint32_t> maximum_payload_bytes;
  /// Fragmentation threshold (64 KiB); larger payloads arrive in fragments.
  std::optional<uint32_t> maximum_fragment_bytes;
};

/// Subscribes to telemetry streams of one RAN function. Only RAN function 10
/// (telemetry) accepts subscriptions and \c control_identifiers must stay
/// empty. Subscriptions live until deleted, released, or the association
/// closes.
struct e3ap_subscription_request {
  uint16_t              dapp_identifier{};
  uint16_t              ran_function_identifier{};
  std::vector<uint16_t> telemetry_identifiers;
  std::vector<uint16_t> control_identifiers;
  /// Requested subscription duration in seconds (wire range 0..86400).
  /// Carried for E3AP compatibility; the embedded agent does not expire
  /// subscriptions on it.
  std::optional<uint32_t> subscription_time_s;
  /// Requested per-RNTI capture interval in milliseconds for every telemetry
  /// in the request (0 = every event; absent = the stream's advertised default
  /// floor). A per-telemetry \c minimum_interval_ms in \c subscription_data
  /// overrides it; the configured hard floor bounds the stream as a whole.
  std::optional<uint32_t> periodicity_ms;
  /// Opaque APER Telemetry-SubscriptionData with per-telemetry filters.
  std::vector<uint8_t> subscription_data;
  /// Ask for bulk tensors by reference through a shared region.
  std::optional<e3ap_delivery_mode> delivery_mode;
  /// Objects whose tensor is smaller than this stay in band.
  std::optional<uint32_t> minimum_by_reference_bytes;
};

struct e3ap_subscription_delete {
  uint16_t dapp_identifier{};
  uint16_t subscription_id{};
};

struct e3ap_subscription_response {
  uint32_t                             request_id{};
  uint16_t                             dapp_identifier{};
  e3ap_response_code                   response_code{e3ap_response_code::negative};
  std::optional<uint16_t>              subscription_id;
  std::optional<std::vector<uint16_t>> telemetry_identifiers;
  std::optional<std::vector<uint16_t>> control_identifiers;
  std::optional<e3ap_cause>            cause;
  std::string                          detail;
  /// Present when by-reference delivery was granted.
  std::optional<e3ap_shared_region> shared_region;
};

struct e3ap_indication_message {
  uint16_t                dapp_identifier{};
  uint16_t                ran_function_identifier{};
  std::vector<uint8_t>    protocol_data;
  std::optional<uint16_t> subscription_id;
  std::optional<uint16_t> telemetry_identifier;
  std::optional<uint64_t> sequence;
  uint16_t                fragment_index{};
  uint32_t                fragment_count{1};
  std::optional<uint32_t> payload_bytes;
  /// Present when the record's bulk field was delivered by reference.
  std::optional<e3ap_tensor_descriptor> by_reference;
};

struct e3ap_dapp_control_action {
  uint16_t                dapp_identifier{};
  uint16_t                ran_function_identifier{};
  uint16_t                control_identifier{};
  std::vector<uint8_t>    action_data;
  uint16_t                fragment_index{};
  uint32_t                fragment_count{1};
  std::optional<uint32_t> payload_bytes;
};

struct e3ap_control_response {
  uint32_t                  request_id{};
  uint16_t                  dapp_identifier{};
  uint16_t                  ran_function_identifier{};
  uint16_t                  control_identifier{};
  e3ap_response_code        response_code{e3ap_response_code::negative};
  std::optional<e3ap_cause> cause;
  std::string               detail;
  std::vector<uint8_t>      control_outcome;
  uint16_t                  fragment_index{};
  uint32_t                  fragment_count{1};
  uint32_t                  payload_bytes{};
};

struct e3ap_dapp_report {
  uint16_t             dapp_identifier{};
  uint16_t             ran_function_identifier{};
  std::vector<uint8_t> report_data;
};

struct e3ap_xapp_control_action {
  uint16_t             dapp_identifier{};
  uint16_t             ran_function_identifier{};
  std::vector<uint8_t> xapp_control_data;
};

struct e3ap_release_message {
  uint16_t dapp_identifier{};
};

struct e3ap_message_ack {
  uint32_t                  request_id{};
  e3ap_response_code        response_code{e3ap_response_code::negative};
  std::optional<e3ap_cause> cause;
  std::string               detail;
};

using e3ap_message = std::variant<e3ap_setup_request,
                                  e3ap_setup_response,
                                  e3ap_subscription_request,
                                  e3ap_subscription_delete,
                                  e3ap_subscription_response,
                                  e3ap_indication_message,
                                  e3ap_dapp_control_action,
                                  e3ap_dapp_report,
                                  e3ap_xapp_control_action,
                                  e3ap_release_message,
                                  e3ap_message_ack,
                                  e3ap_control_response>;

struct e3ap_pdu {
  uint32_t id{1};
  /// Producer realtime clock at send time in nanoseconds since the Unix epoch
  /// (libe3-compatible envelope field); absent means no producer reference.
  std::optional<int64_t> timestamp_ns;
  e3ap_message           message;
};

// ---------------------------------------------------------------------------
// OCUDU-TELEMETRY-SM
// ---------------------------------------------------------------------------

enum class telemetry_drop_policy : uint8_t { drop_newest, drop_oldest, coalesce_latest };

struct telemetry_stream_record {
  uint16_t                telemetry_id{};
  std::string             name;
  std::string             description;
  bool                    class_c_portable{};
  uint32_t                interface_id{};
  std::optional<uint64_t> minimum_interval_us;
  std::vector<uint16_t>   cell_ids;
  /// Floor no subscriber can go below; absent means full rate is available.
  std::optional<uint64_t> hard_floor_interval_us;
};

struct telemetry_ran_function_data {
  std::string                          name{"OCUDU Telemetry"};
  uint16_t                             version{1};
  std::string                          description{"PUSCH, SRS, DM-RS and gNB metrics observation streams"};
  std::vector<telemetry_stream_record> streams;
  uint32_t                             maximum_object_bytes{};
  uint32_t                             maximum_subscriptions{};
};

struct telemetry_subscription_config {
  uint16_t                telemetry_id{};
  std::optional<uint16_t> cell_id;
  uint16_t                sample_every_n{1};
  telemetry_drop_policy   drop_policy{telemetry_drop_policy::coalesce_latest};
  /// Requested per-RNTI capture interval; zero asks for every event. Absent
  /// keeps the advertised default floor.
  std::optional<uint32_t> minimum_interval_ms;
  std::vector<uint16_t>   rnti_filter;
};

struct telemetry_subscription_data {
  std::vector<telemetry_subscription_config> configs;
};

struct telemetry_radio_slot_value {
  uint16_t sfn{};
  uint8_t  slot{};
  uint8_t  numerology{};
};

/// Grant parameters carried with PUSCH records so an offline receiver can
/// regenerate the DM-RS, descramble, de-multiplex UCI and LDPC-decode.
struct pusch_grant_parameters {
  uint8_t harq_id{};
  /// Data scrambling identity (TS 38.211 dataScramblingIdentityPUSCH).
  uint16_t n_id{};
  /// Transport block size in bytes.
  uint32_t tbs_bytes{};
  /// Redundancy version (0..3).
  uint8_t rv{};
  /// New-data indicator: true for a first transmission.
  bool    new_data{true};
  uint8_t ldpc_base_graph{1}; ///< 1 or 2.
  /// TBS assumed for limited buffer rate matching, in bytes.
  uint32_t                tbs_lbrm_bytes{};
  std::optional<uint8_t>  mcs_index; ///< TS 38.214 MCS row; absent when unknown.
  std::optional<uint8_t>  mcs_table; ///< 0 qam64, 1 qam256, 2 qam64LowSe.
  bool                    transform_precoding{};
  std::optional<uint16_t> n_rs_id;         ///< DFT-s-OFDM low-PAPR sequence group id.
  uint8_t                 cyclic_prefix{}; ///< 0 normal, 1 extended.
  /// Time allocation: first OFDM symbol and symbol count within the slot.
  uint8_t start_symbol{};
  uint8_t nof_symbols{};
  /// Bit s set when OFDM symbol s of the slot carries DM-RS.
  uint16_t dmrs_symbol_mask{};
  uint8_t  dmrs_type{1}; ///< DM-RS configuration type, 1 or 2.
  uint8_t  nof_cdm_groups_without_data{2};
  uint16_t dmrs_scrambling_id{};
  bool     n_scid{};
  /// BWP position as CRB start and size in RBs.
  uint16_t                bwp_start_rb{};
  uint16_t                bwp_size_rb{};
  std::array<uint8_t, 35> prb_mask{}; ///< CRB mask, 275 bits, LSB-first per byte.
  uint8_t                 nof_rx_ports{};
  uint8_t                 nof_layers{};
  /// Transmitter DC subcarrier position within the grid; absent when unknown.
  std::optional<uint16_t> dc_position;
  /// UCI multiplexed on PUSCH: payload bit counts and TS 38.213 scaling.
  uint16_t nof_harq_ack_bits{};
  uint16_t nof_csi_part1_bits{};
  bool     csi_part2_expected{};
  double   alpha_scaling{1.0};
  double   beta_offset_harq_ack{};
  double   beta_offset_csi_part1{};
  double   beta_offset_csi_part2{};
};

struct pusch_symbols_record {
  uint16_t                              rnti{};
  telemetry_radio_slot_value            radio_slot{};
  uint16_t                              nof_prb{};
  uint8_t                               nof_data_symbols{};
  uint8_t                               nof_layers{};
  uint8_t                               modulation_bits{};
  std::optional<double>                 sinr_db;
  std::optional<double>                 epre_db;
  std::optional<double>                 rsrp_db;
  std::optional<double>                 evm;
  std::optional<double>                 time_alignment_us;
  std::optional<double>                 cfo_hz;
  std::optional<double>                 target_code_rate;
  std::vector<uint8_t>                  equalized_symbols; // interleaved binary32 (re, im)
  std::optional<pusch_grant_parameters> grant;
  std::optional<bool>                   crc_ok;
  bool                                  symbols_truncated{};
};

/// Raw (pre-equalization) PUSCH resource grid for one grant, DM-RS included.
struct pusch_grid_record {
  uint16_t                   rnti{};
  telemetry_radio_slot_value radio_slot{};
  uint16_t                   nof_prb{};
  uint8_t                    nof_rx_ports{};
  uint8_t                    start_symbol{};
  uint8_t                    nof_symbols{};
  pusch_grant_parameters     grant;
  std::vector<uint8_t>       resource_elements; // interleaved binary32 (re, im)
};

struct pusch_crc_record {
  uint16_t                   rnti{};
  telemetry_radio_slot_value radio_slot{};
  bool                       crc_ok{};
};

struct srs_channel_record {
  uint16_t                   rnti{};
  telemetry_radio_slot_value radio_slot{};
  uint16_t                   nof_subcarriers{};
  uint8_t                    comb_size{};
  uint8_t                    nof_rx_ports{};
  uint8_t                    nof_tx_ports{};
  uint8_t                    rx_port_index{};
  uint8_t                    tx_port_index{};
  uint8_t                    start_symbol{};
  uint8_t                    nof_symbols{};
  std::optional<double>      rsrp_db;
  std::optional<double>      epre_db;
  std::optional<double>      noise_variance;
  std::optional<double>      time_alignment_ns;
  std::optional<uint16_t>    mapping_initial_subcarrier;
  std::optional<uint8_t>     comb_offset;
  std::optional<uint8_t>     cyclic_shift;
  std::optional<uint16_t>    sequence_id;
  std::optional<uint16_t>    sequence_group;
  std::optional<uint16_t>    sequence_number;
  std::optional<uint16_t>    cyclic_shift_index;
  std::optional<uint16_t>    cyclic_shift_maximum;
  std::optional<uint8_t>     bandwidth_index;
  std::optional<uint8_t>     frequency_position;
  std::optional<uint16_t>    frequency_shift;
  std::optional<uint8_t>     frequency_hopping;
  std::vector<uint8_t>       coefficients;
  /// CF32 [symbol, pilot]: the received pilots at the transmit port's comb subcarriers; empty when not captured.
  std::vector<uint8_t> received_pilots;
  /// CF32 [pilot]: the transmit port's sequence, cyclic shift included; empty when not captured.
  std::vector<uint8_t> reference_symbols;
};

struct pusch_dmrs_channel_record {
  uint16_t                   rnti{};
  telemetry_radio_slot_value radio_slot{};
  uint16_t                   start_prb{};
  uint16_t                   nof_prb{};
  uint16_t                   nof_subcarriers{};
  uint8_t                    dmrs_symbol_index{};
  uint8_t                    start_symbol{};
  uint8_t                    nof_symbols{};
  uint8_t                    nof_rx_ports{};
  uint8_t                    nof_tx_layers{};
  uint8_t                    rx_port_index{};
  uint8_t                    tx_layer_index{};
  uint8_t                    dmrs_type{1};
  uint8_t                    nof_cdm_groups_without_data{};
  bool                       transform_precoding{};
  uint16_t                   scrambling_id{};
  bool                       n_scid{};
  std::optional<double>      sinr_db;
  std::optional<double>      epre_db;
  std::optional<double>      rsrp_db;
  std::optional<double>      noise_variance;
  std::optional<double>      time_alignment_us;
  std::optional<double>      cfo_hz;
  std::optional<double>      target_code_rate;
  std::vector<uint8_t>       estimates;
};

struct gnb_metrics_json_record {
  std::vector<uint8_t> json;
};

struct class_c_portable_record {
  uint32_t             interface_id{};
  std::vector<uint8_t> metadata;
  std::vector<uint8_t> tensor;
};

using telemetry_object = std::variant<pusch_symbols_record,
                                      pusch_crc_record,
                                      srs_channel_record,
                                      pusch_dmrs_channel_record,
                                      gnb_metrics_json_record,
                                      class_c_portable_record,
                                      pusch_grid_record>;

struct telemetry_indication {
  uint16_t         telemetry_id{};
  uint16_t         cell_id{};
  uint64_t         sequence{};
  uint64_t         route_epoch{};
  uint64_t         monotonic_ns{};
  uint64_t         dropped_objects{};
  telemetry_object object;
};

// ---------------------------------------------------------------------------
// OCUDU-DAPP-MGMT-CARRIER-SM
// ---------------------------------------------------------------------------

struct management_ran_function_data {
  std::string name{"OCUDU dApp Management"};
  uint16_t    version{1};
  std::string description{"Inventory, lifecycle, configuration, models, metrics and incidents of hosted dApps"};
  std::string schema_module{"E3SM-OCUDU-DAPP-MGMT-V1"};
  uint32_t    maximum_bytes{2U * 1024U * 1024U};
};

/// Telemetry identifiers of RAN function 10 and their store interfaces.
constexpr uint16_t telemetry_id_pusch_symbols      = 1;
constexpr uint16_t telemetry_id_pusch_crc          = 2;
constexpr uint16_t telemetry_id_srs_channel        = 3;
constexpr uint16_t telemetry_id_pusch_dmrs_channel = 4;
constexpr uint16_t telemetry_id_gnb_metrics_json   = 5;
constexpr uint16_t telemetry_id_class_c_spectrum   = 6;
constexpr uint16_t telemetry_id_class_c_srs_isac   = 7;
constexpr uint16_t telemetry_id_pusch_grid         = 8;

std::optional<uint32_t> telemetry_interface_for(uint16_t telemetry_id) noexcept;
std::optional<uint16_t> telemetry_id_for(uint32_t interface_id) noexcept;

/// Built-in stream catalog advertised in the telemetry RAN-function data.
std::vector<telemetry_stream_record> default_telemetry_streams();

} // namespace ocudu::dapp::e3
