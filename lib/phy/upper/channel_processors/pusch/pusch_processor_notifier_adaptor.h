// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "pusch_uci_decoder_notifier.h"
#include "ocudu/ocuduvec/copy.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_decoder_notifier.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_decoder_result.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator_notifier.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_processor_result_notifier.h"
#include "ocudu/phy/upper/diagnostics/phy_diagnostics.h"
#include "ocudu/ran/slot_point.h"
#include <complex>
#include <limits>
#include <vector>

namespace ocudu {

namespace detail {

/// \brief PUSCH processor notifier adaptor internal callback.
///
/// Interfaces the different notifiers with the notifier adaptor.
class pusch_processor_notifier_callback
{
public:
  /// Default destructor.
  virtual ~pusch_processor_notifier_callback() = default;

  virtual void on_sch_data(const pusch_decoder_result& result) = 0;

  /// Notifies HARQ-ACK.
  virtual void on_harq_ack(const pusch_uci_field& field) = 0;

  /// Notifies CSI Part 1.
  virtual void on_csi_part1(const pusch_uci_field& field) = 0;

  /// Notifies CSI Part 2.
  virtual void on_csi_part2(const pusch_uci_field& field) = 0;
};

} // namespace detail

class pusch_processor_csi_part1_feedback
{
public:
  virtual ~pusch_processor_csi_part1_feedback() = default;

  virtual void on_csi_part1(const uci_payload_type& part1) = 0;
};

/// \brief Adapts the notifiers of each PUSCH decoder to the PUSCH processor notifier.
///
/// The UCI fields notifier getters set flags of their respective fields as pending.
class pusch_processor_notifier_adaptor : private detail::pusch_processor_notifier_callback
{
public:
  /// Creates a notifier adaptor from a PUSCH processor notifier.
  pusch_processor_notifier_adaptor() :
    pusch_demod_notifier(uci_payload.csi),
    sch_data_notifier(*this),
    harq_ack_notifier(*this),
    csi_part_1_notifier(*this),
    csi_part_2_notifier(*this)
  {
  }

  /// \brief Configures the notifier for a new transmission.
  /// \param[in] notifier_            PUSCH processor result notifier.
  /// \param[in] csi_part_1_feedback_ Uplink control information field CSI Part 1 feedback notifier.
  /// \param[in] csi                  Channel state information.
  /// \return A PUSCH processor notifier adaptor.
  void new_transmission(pusch_processor_result_notifier&    notifier_,
                        pusch_processor_csi_part1_feedback& csi_part_1_feedback_,
                        const channel_state_information&    csi)
  {
    notifier            = &notifier_;
    csi_part_1_feedback = &csi_part_1_feedback_;
    uci_payload.csi     = csi;

    uci_payload.harq_ack.clear();
    uci_payload.csi_part1.clear();
    uci_payload.csi_part2.clear();
  }

  /// Configures CRC diagnostic forwarding for the next transmission. The sink is non-owning.
  void set_crc_diag(phy_diagnostic_sink* sink, slot_point slot, uint16_t rnti)
  {
    crc_sink_ = sink;
    crc_slot_ = slot;
    crc_rnti_ = rnti;
  }

  /// Configures PUSCH equalized-symbol + KPI diagnostic forwarding (publish from on_sch_data).
  void set_pusch_sym_diag(phy_diagnostic_sink*            sink,
                          slot_point                      slot,
                          uint16_t                        rnti,
                          uint16_t                        nof_prb,
                          uint16_t                        nof_data_symbols,
                          uint8_t                         nof_layers,
                          uint8_t                         mod_bits,
                          float                           target_code_rate,
                          span<const std::complex<float>> symbols)
  {
    sym_sink_             = sink;
    sym_slot_             = slot;
    sym_rnti_             = rnti;
    sym_nof_prb_          = nof_prb;
    sym_nof_data_symbols_ = nof_data_symbols;
    sym_nof_layers_       = nof_layers;
    sym_mod_bits_         = mod_bits;
    sym_target_code_rate_ = target_code_rate;
    sym_buffer_.assign(symbols.begin(), symbols.end());
    sym_grant_valid_ = false;
    sym_truncated_   = false;
  }

  /// Attaches the grant parameters to the pending PUSCH symbol diagnostic. Completeness is judged
  /// when the event is emitted (accelerated paths deliver the symbols late through
  /// update_pusch_sym_diag_symbols): the capture must hold exactly \c expected_symbols and DM-RS
  /// symbols must carry no data for it to be a whole codeword.
  void set_pusch_sym_grant(const pusch_grant_diagnostic& grant, std::size_t expected_symbols, bool incomplete)
  {
    sym_grant_            = grant;
    sym_grant_valid_      = true;
    sym_expected_symbols_ = expected_symbols;
    sym_truncated_        = incomplete;
  }

  /// Late-stage symbol update for async D2H (GPU paths).
  void update_pusch_sym_diag_symbols(span<const std::complex<float>> symbols, bool clipped = false)
  {
    if (sym_sink_ == nullptr) {
      return;
    }
    sym_buffer_.assign(symbols.begin(), symbols.end());
    sym_truncated_ = sym_truncated_ || clipped;
  }

  /// Gets the PUSCH demodulator notifier.
  pusch_demodulator_notifier& get_demodulator_notifier() { return pusch_demod_notifier; }

  /// Gets the SCH data notifier.
  pusch_decoder_notifier& get_sch_data_notifier() { return sch_data_notifier; }

  /// Gets the HARQ-ACK notifier and sets the field as pending.
  pusch_uci_decoder_notifier& get_harq_ack_notifier()
  {
    ocudu_assert(!pending_harq_ack, "HARQ-ACK has already been requested.");
    pending_harq_ack = true;
    return harq_ack_notifier;
  }

  /// Gets the CSI Part 1 notifier and sets the field as pending.
  pusch_uci_decoder_notifier& get_csi_part1_notifier()
  {
    ocudu_assert(!pending_csi_part1, "CSI Part 1 has already been requested.");
    pending_csi_part1 = true;
    return csi_part_1_notifier;
  }

  /// Gets the CSI Part 2 notifier and sets the field as pending.
  pusch_uci_decoder_notifier& get_csi_part2_notifier()
  {
    ocudu_assert(!pending_csi_part2, "CSI Part 2 has already been requested.");
    pending_csi_part2 = true;
    return csi_part_2_notifier;
  }

  /// Default destructor - verifies that the previous fields are valid.
  ~pusch_processor_notifier_adaptor() override
  {
    ocudu_assert(!pending_harq_ack, "HARQ-ACK has not been notified.");
    ocudu_assert(!pending_csi_part1, "CSI Part 1 has not been notified.");
    ocudu_assert(!pending_csi_part2, "CSI Part 2 has not been notified.");
  }

private:
  /// Implements the PUSCH demodulator statistics notifier.
  class pusch_demodulator_notifier_impl : public pusch_demodulator_notifier
  {
  public:
    pusch_demodulator_notifier_impl(channel_state_information& csi_) : csi(csi_) {}

    // See interface for documentation.
    void on_provisional_stats(unsigned i_symbol, const demodulation_stats& stats) override
    {
      if (stats.sinr_dB.has_value()) {
        csi.set_sinr_dB(channel_state_information::sinr_type::post_equalization, *stats.sinr_dB);
      }
      if (stats.channel_estimator_sinr_dB.has_value()) {
        csi.set_sinr_dB(channel_state_information::sinr_type::channel_estimator, *stats.channel_estimator_sinr_dB);
      }
      if (stats.evm.has_value()) {
        csi.set_symbol_evm(i_symbol, *stats.evm);
        csi.set_sinr_dB(channel_state_information::sinr_type::evm, -20.0F * log10f(*stats.evm) - 3.7F);
      }
      if (stats.epre_dB.has_value()) {
        csi.set_epre(*stats.epre_dB);
      }
      if (stats.rsrp_dB.has_value()) {
        csi.set_rsrp_dB(*stats.rsrp_dB);
      }
      if (stats.time_alignment_s.has_value()) {
        csi.set_time_alignment(phy_time_unit::from_seconds(static_cast<double>(*stats.time_alignment_s)));
      }
      if (stats.cfo_Hz.has_value()) {
        csi.set_cfo(*stats.cfo_Hz);
      }
    }

    // See interface for documentation.
    void on_end_stats(const demodulation_stats& stats) override
    {
      if (stats.sinr_dB.has_value()) {
        csi.set_sinr_dB(channel_state_information::sinr_type::post_equalization, *stats.sinr_dB);
      }
      if (stats.channel_estimator_sinr_dB.has_value()) {
        csi.set_sinr_dB(channel_state_information::sinr_type::channel_estimator, *stats.channel_estimator_sinr_dB);
      }
      if (stats.evm.has_value()) {
        csi.set_total_evm(*stats.evm);
        csi.set_sinr_dB(channel_state_information::sinr_type::evm, -20.0F * log10f(*stats.evm) - 3.7F);
      }
      if (stats.epre_dB.has_value()) {
        csi.set_epre(*stats.epre_dB);
      }
      if (stats.rsrp_dB.has_value()) {
        csi.set_rsrp_dB(*stats.rsrp_dB);
      }
      if (stats.time_alignment_s.has_value()) {
        csi.set_time_alignment(phy_time_unit::from_seconds(static_cast<double>(*stats.time_alignment_s)));
      }
      if (stats.cfo_Hz.has_value()) {
        csi.set_cfo(*stats.cfo_Hz);
      }
    }

  private:
    channel_state_information& csi;
  };

  /// Implements the SCH data notifier.
  class sch_data_notifier_impl : public pusch_decoder_notifier
  {
  public:
    sch_data_notifier_impl(detail::pusch_processor_notifier_callback& notifier_) : notifier(notifier_) {}

    // See interface for documentation.
    void on_sch_data(const pusch_decoder_result& result) override { notifier.on_sch_data(result); }

  private:
    detail::pusch_processor_notifier_callback& notifier;
  };

  /// Implements the HARQ-ACK notifier.
  class harq_ack_notifier_impl : public pusch_uci_decoder_notifier
  {
  public:
    harq_ack_notifier_impl(detail::pusch_processor_notifier_callback& callback_) : callback(callback_) {}

    // See interface for documentation.
    void on_uci_decoded(span<const uint8_t> message, const uci_status& status) override
    {
      pusch_uci_field field;
      field.payload = uci_payload_type(message.begin(), message.end());
      field.status  = status;
      callback.on_harq_ack(field);
    }

  private:
    detail::pusch_processor_notifier_callback& callback;
  };

  /// Implements the CSI Part 1 notifier.
  class csi_part1_notifier_impl : public pusch_uci_decoder_notifier
  {
  public:
    csi_part1_notifier_impl(detail::pusch_processor_notifier_callback& callback_) : callback(callback_) {}

    // See interface for documentation.
    void on_uci_decoded(span<const uint8_t> message, const uci_status& status) override
    {
      pusch_uci_field field;
      field.payload = uci_payload_type(message.begin(), message.end());
      field.status  = status;
      callback.on_csi_part1(field);
    }

  private:
    detail::pusch_processor_notifier_callback& callback;
  };

  /// Implements the CSI Part 2 notifier.
  class csi_part2_notifier_impl : public pusch_uci_decoder_notifier
  {
  public:
    csi_part2_notifier_impl(detail::pusch_processor_notifier_callback& callback_) : callback(callback_) {}

    // See interface for documentation.
    void on_uci_decoded(span<const uint8_t> message, const uci_status& status) override
    {
      pusch_uci_field field;
      field.payload = uci_payload_type(message.begin(), message.end());
      field.status  = status;
      callback.on_csi_part2(field);
    }

  private:
    detail::pusch_processor_notifier_callback& callback;
  };

  void on_sch_data(const pusch_decoder_result& result) override
  {
    // MAC first — diagnostics never delay CRC/TB delivery.
    pusch_processor_result_data result_data;
    result_data.data = result;
    result_data.csi  = uci_payload.csi;
    notifier->on_sch(result_data);

    if (crc_sink_ != nullptr) {
      crc_sink_->on_pusch_crc({.rnti = crc_rnti_, .slot = crc_slot_, .crc_ok = result.tb_crc_ok});
      crc_sink_ = nullptr;
    }
    if (sym_sink_ != nullptr) {
      const channel_state_information& csi_now = uci_payload.csi;
      const auto                       sinr    = csi_now.get_sinr_dB();
      const auto                       epre    = csi_now.get_epre_dB();
      const auto                       rsrp    = csi_now.get_rsrp_dB();
      const auto                       evm_opt = csi_now.get_total_evm();
      const auto                       ta_opt  = csi_now.get_time_alignment();
      const auto                       cfo_opt = csi_now.get_cfo_Hz();
      const float                      nanv    = std::numeric_limits<float>::quiet_NaN();
      const float ta_us = ta_opt.has_value() ? static_cast<float>(ta_opt.value().to_seconds() * 1e6) : nanv;
      sym_sink_->on_pusch_symbols(
          {.rnti              = sym_rnti_,
           .slot              = sym_slot_,
           .nof_prb           = sym_nof_prb_,
           .nof_data_symbols  = sym_nof_data_symbols_,
           .nof_layers        = sym_nof_layers_,
           .mod_bits          = sym_mod_bits_,
           .sinr_dB           = sinr.value_or(nanv),
           .epre_dB           = epre.value_or(nanv),
           .rsrp_dB           = rsrp.value_or(nanv),
           .evm               = evm_opt.value_or(nanv),
           .time_alignment_us = ta_us,
           .cfo_Hz            = cfo_opt.value_or(nanv),
           .target_code_rate  = sym_target_code_rate_,
           .equalized_symbols = span<const std::complex<float>>(sym_buffer_.data(), sym_buffer_.size()),
           .grant_valid       = sym_grant_valid_,
           .grant             = sym_grant_,
           .crc_known         = true,
           .crc_ok            = result.tb_crc_ok,
           .symbols_truncated = sym_truncated_ || (sym_grant_valid_ && sym_buffer_.size() != sym_expected_symbols_)});
      sym_sink_ = nullptr;
      sym_buffer_.clear();
    }
  }

  // See detail::pusch_processor_notifier_callback for documentation.
  void on_harq_ack(const pusch_uci_field& field) override
  {
    // Check HARQ ACK is pending.
    ocudu_assert(pending_harq_ack, "HARQ ACK is not pending.");

    // Set HARQ ACK field.
    uci_payload.harq_ack = field;

    // HARQ ACK is no longer pending.
    pending_harq_ack = false;

    // Check if the rest of fields are pending and notify the results.
    check_and_notify_uci();
  }

  // See detail::pusch_processor_notifier_callback for documentation.
  void on_csi_part1(const pusch_uci_field& field) override
  {
    // Check CSI Part 1 is pending.
    ocudu_assert(pending_csi_part1, "CSI Part 1 is not pending.");

    if (field.status == uci_status::valid) {
      ocudu_assert(csi_part_1_feedback != nullptr, "Invalid CSI Part 1 feedback.");
      csi_part_1_feedback->on_csi_part1(field.payload);
    }

    // Set CSI Part 1 field.
    uci_payload.csi_part1 = field;

    // CSI Part 1 is no longer pending.
    pending_csi_part1 = false;

    // Check if the rest of fields are pending and notify the results.
    check_and_notify_uci();
  }

  // See detail::pusch_processor_notifier_callback for documentation.
  void on_csi_part2(const pusch_uci_field& field) override
  {
    // Check CSI Part 2 is pending.
    ocudu_assert(pending_csi_part2, "CSI Part 2 is not pending.");

    // Set CSI Part 2 field.
    uci_payload.csi_part2 = field;

    // CSI Part 2 is no longer pending.
    pending_csi_part2 = false;

    // Check if the rest of fields are pending and notify the results.
    check_and_notify_uci();
  }

  /// Check if the no UCI fields are pending and notify the results.
  void check_and_notify_uci()
  {
    ocudu_assert(notifier != nullptr, "Invalid notifier.");
    if (!pending_harq_ack && !pending_csi_part1 && !pending_csi_part2) {
      notifier->on_uci(uci_payload);
    }
  }

  /// Set to true if the HARQ-ACK was requested but it has not notified back.
  bool pending_harq_ack = false;
  /// Set to true if the CSI Part 1 was requested but it has not notified back.
  bool pending_csi_part1 = false;
  /// Set to true if the CSI Part 2 was requested but it has not notified back.
  bool pending_csi_part2 = false;
  /// Stored UCI payload.
  pusch_processor_result_control uci_payload;
  /// Reference to PUSCH processor notifier.
  pusch_processor_result_notifier* notifier;
  /// CSI Part 1 feedback.
  pusch_processor_csi_part1_feedback* csi_part_1_feedback;
  /// Channel state information notifier.
  pusch_demodulator_notifier_impl pusch_demod_notifier;
  /// SCH data notifier.
  sch_data_notifier_impl sch_data_notifier;
  /// HARQ-ACK notifier.
  harq_ack_notifier_impl harq_ack_notifier;
  /// CSI Part 1 notifier.
  csi_part1_notifier_impl csi_part_1_notifier;
  /// CSI Part 2 notifier.
  csi_part2_notifier_impl csi_part_2_notifier;

  /// Optional CRC diagnostic sink (non-owning, reset after each transmission).
  phy_diagnostic_sink* crc_sink_ = nullptr;
  slot_point           crc_slot_;
  uint16_t             crc_rnti_ = 0;

  /// Optional PUSCH symbol/KPI diagnostic sink.
  phy_diagnostic_sink*             sym_sink_ = nullptr;
  slot_point                       sym_slot_;
  uint16_t                         sym_rnti_             = 0;
  uint16_t                         sym_nof_prb_          = 0;
  uint16_t                         sym_nof_data_symbols_ = 0;
  uint8_t                          sym_nof_layers_       = 0;
  uint8_t                          sym_mod_bits_         = 0;
  float                            sym_target_code_rate_ = 0.0f;
  std::vector<std::complex<float>> sym_buffer_;
  pusch_grant_diagnostic           sym_grant_{};
  bool                             sym_grant_valid_      = false;
  bool                             sym_truncated_        = false;
  std::size_t                      sym_expected_symbols_ = 0;
};

} // namespace ocudu
