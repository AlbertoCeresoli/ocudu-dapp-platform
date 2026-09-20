// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief PUSCH demodulator interface.

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/phy/upper/acceleration/phy_acceleration.h"
#include "ocudu/phy/upper/channel_processors/uci/uci_status.h"
#include "ocudu/phy/upper/dmrs_mapping.h"
#include "ocudu/phy/upper/log_likelihood_ratio.h"
#include "ocudu/phy/upper/signal_processors/pusch/dmrs_pusch_estimator.h"
#include "ocudu/ran/dmrs/dmrs.h"
#include "ocudu/ran/rnti.h"
#include "ocudu/ran/sch/modulation_scheme.h"
#include "ocudu/ran/slot_point.h"
#include <complex>
#include <memory>

namespace ocudu {

class pusch_resource_loan_provider;

class pusch_codeword_buffer;
class pusch_demodulator_notifier;
class resource_grid_reader;
class pusch_resident_dapp;

/// \brief PUSCH demodulator interface.
///
/// The demodulation of a PUSCH consists of:
/// - extracting allocated REs from the resource grid,
/// - equalizing of the extracted RE,
/// - soft-demodulation of the complex data, and
/// - descrambling.
class pusch_demodulator
{
public:
  /// Parameters defining the demodulation procedure of a PUSCH transmission.
  struct configuration {
    /// DU-local cell identifier used for dApp invocation attribution.
    uint32_t cell_id = 0;
    /// Radio Network Temporary Identifier, see parameter \f$n_{RNTI}\f$ in TS38.211 Section 6.3.1.1.
    rnti_t rnti;
    /// Allocation CRB list: the entries set to true are used for transmission.
    crb_bitmap rb_mask;
    /// Modulation scheme used for transmission.
    modulation_scheme modulation;
    /// Time domain allocation within a slot: start symbol index {0, ..., 12}.
    unsigned start_symbol_index;
    /// Time domain allocation within a slot: number of symbols {1, ..., 14}.
    unsigned nof_symbols;
    /// OFDM symbols containing DM-RS: boolean mask.
    symbol_slot_mask dmrs_symb_pos;
    /// DM-RS configuration type.
    dmrs_config_type dmrs_type;
    /// Number of DM-RS CDM groups without data.
    unsigned nof_cdm_groups_without_data;
    /// Scrambling identifier, see parameter \f$n_{ID}\f$ in TS38.211 Section 6.3.1.1. Range is {0, ..., 1023}.
    unsigned n_id;
    /// DMRS scrambling identifier, see parameter \f$N^{n_{SCID}}_{ID}\f$ in TS38.211 Section 6.4.1.1.1.1. Range is
    /// {0, ..., 65535}.
    unsigned dmrs_scrambling_id;
    /// DMRS sequence initialization, see parameter \f$n_{SCID}\f$ in TS38.211 Section 6.4.1.1.1.1. Values {0, 1}.
    bool n_scid;
    /// DM-RS amplitude relative to PUSCH data symbols.
    float dmrs_scaling = 1.0F;
    /// Cyclic prefix used to derive the number of symbols per slot.
    cyclic_prefix c_prefix = cyclic_prefix::NORMAL;
    /// Number of transmit layers.
    unsigned nof_tx_layers;
    /// \brief Direct current position.
    ///
    /// It may be empty if it falls outside the bandwidth or if not configured.
    std::optional<unsigned> dc_position;
    /// Set to true for enabling transform precoding.
    bool enable_transform_precoding;
    /// \brief Low-PAPR sequence reference signal ID for transform precoding DMRS.
    ///
    /// Only used when enable_transform_precoding is true. Range {0, ..., 1007}.
    /// Corresponds to parameter \f$n^{RS}_{ID}\f$ from TS38.211 Section 6.4.1.1.2.
    std::optional<unsigned> n_rs_id;
    /// Slot timing information for DMRS c_init computation.
    slot_point slot;
    /// Receive antenna port indices the PUSCH transmission is mapped to.
    static_vector<uint8_t, MAX_PORTS> rx_ports;
    /// \brief Random Access Preamble Index (RAPID) associated with msgA on PUSCH (Release 16). Possible values
    /// are {0, ..., 63}.
    ///
    /// During 2-step RACH, the PUSCH carrying msgA is alternatively scrambled using an initialization value that
    /// depends on the sequence index of the detected RACH preamble.
    ///
    /// The presence of this parameter indicates the PUSCH is a msgA and the scrambling sequence is initialized as:
    /// \f$c_{init} = n_{RNTI} \cdot 2^{16} + n_{RAPID} \cdot 2^{10} + n_{ID}\f$
    /// as specified in TS 38.211, Section 6.3.1.1.
    std::optional<unsigned> n_rapid;
    /// Resident accelerator handoff options.
    pusch_resident_demodulation_config resident;
    /// Borrowed only during demodulate(); acquire a loan before any deferred use of host-owned resources.
    pusch_resource_loan_provider* resource_owner = nullptr;
  };

  /// Default destructor.
  virtual ~pusch_demodulator() = default;

  /// \brief Demodulates a PUSCH transmission.
  ///
  /// Computes log-likelihood ratios from channel samples by reversing all the operations described in TS38.211 Section
  /// 6.3.1.
  ///
  /// \remarks
  /// - The size of \c data determines the codeword size in soft bits.
  /// - The total number of LLR must be consistent with the number of RE allocated to the transmission.
  ///
  /// \param[out] codeword_buffer Codeword buffer.
  /// \param[in]  notifier        Demodulation statistics notifier.
  /// \param[in]  grid            Resource grid for the current slot.
  /// \param[in]  est_results     Interface to access the channel estimates for the REs allocated to the PUSCH
  ///                             transmission.
  /// \param[in]  config          Configuration parameters.
  virtual void demodulate(pusch_codeword_buffer&              codeword_buffer,
                          pusch_demodulator_notifier&         notifier,
                          const resource_grid_reader&         grid,
                          const dmrs_pusch_estimator_results& est_results,
                          const configuration&                config) = 0;

  /// \brief Checks if resident accelerator mode is supported by this demodulator.
  virtual bool supports_resident_mode() const { return false; }

  /// \brief Enables resident accelerator mode for accelerated SCH handoff.
  virtual void enable_resident_mode() {}

  /// \brief Disables resident accelerator mode.
  virtual void disable_resident_mode() {}

  /// \brief Checks if resident accelerator mode is currently enabled.
  virtual bool is_resident_mode_enabled() const { return false; }

  /// Installs one pre-bound resident Class-A adapter during dependency construction.
  virtual bool install_resident_dapp(std::shared_ptr<pusch_resident_dapp>) { return false; }

  /// \brief Gets resident softbit buffer information from the last demodulation.
  virtual resident_softbit_buffer get_resident_softbits() const { return {}; }
  /// After synchronous resident decoder return and CPU diagnostics; retain the originating dependency through this call.
  virtual void finish_resident_consumers() {}

  /// \brief Returns true if the last resident-mode demodulation recovered through the host codeword buffer.
  virtual bool used_host_codeword_fallback() const { return false; }

  /// True only for a failed inline replacement whose producer/side streams were successfully drained.
  /// No softbits were submitted to the decoder. Caller must settle UCI/diagnostics and discard the configured TB.
  /// Not permission to retire a native completion ticket or release independently owned resources.
  virtual bool discarded_inline_input() const { return false; }

  /// Gets compact UCI LLRs from the last demodulation.
  virtual resident_uci_buffer get_resident_uci() const { return {}; }

  /// Finalizes compact UCI payloads/LLRs after the resident handoff stream has been synchronized downstream.
  virtual void finalize_resident_uci() {}

  /// \brief Get last grid staging time in microseconds (gap profiling).
  virtual float get_last_grid_staging_us() const { return 0; }

  /// \brief Returns true when the last demodulation consumed the CUDA-visible grid directly.
  virtual bool used_device_grid() const { return false; }

  /// \brief Returns true when the last demodulation consumed the immutable producer-owned snapshot.
  virtual bool used_owned_device_grid() const { return false; }

  /// \brief Get last demod sync time in microseconds (gap profiling).
  virtual float get_last_demod_sync_us() const { return 0; }

  /// \brief Report deferred SINR/EVM stats after decoder sync.
  ///
  /// In resident mode, the demodulator can defer synchronization during \ref demodulate. After the downstream decoder
  /// synchronizes the resident work, this method reads scheduler-visible metrics and calls on_end_stats().
  /// \param[in] notifier The demodulator notifier to report stats to.
  /// \return SINR readback time in microseconds.
  virtual float report_deferred_sinr(pusch_demodulator_notifier& notifier) { return 0; }

  /// Reports the completed resident dApp boundary after accelerator synchronization.
  virtual void report_deferred_dapp_completion() {}

  /// Enables opt-in resident dApp stage diagnostics. This must be configured before concurrent processing starts.
  virtual void enable_resident_dapp_diagnostic_timing(bool) {}

  /// Returns opaque stage timing tokens for a fixed resident completion slot.
  virtual resident_dapp_diagnostic_timing get_resident_dapp_diagnostic_timing(unsigned) const { return {}; }

  /// \brief Enable equalized-symbol capture for the next demodulation (constellation diagnostics).
  ///
  /// GPU-resident paths may leave the buffer empty — diagnostics are best-effort.
  virtual void enable_symbol_capture() {}

  /// Disable equalized-symbol capture.
  virtual void disable_symbol_capture() {}

  /// Captured equalized symbols from the last demodulate() when capture was enabled.
  /// Non-blocking: may return empty if async D2H is still in flight (call again after
  /// \ref wait_captured_symbols from the decoder pre-join path).
  virtual span<const std::complex<float>> get_captured_symbols() const { return {}; }

  /// Wait for any in-flight constellation D2H to finish. Safe from decoder pre-join
  /// (off the slot-critical path). Default is a no-op for CPU paths.
  virtual void wait_captured_symbols() {}
};

} // namespace ocudu
