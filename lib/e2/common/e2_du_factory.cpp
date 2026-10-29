// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/e2/e2_du_factory.h"
#include "e2_entity.h"
#include "e2_impl.h"
#include "e2sm/e2sm_ccc/e2sm_ccc_asn1_packer.h"
#include "e2sm/e2sm_ccc/e2sm_ccc_control_action_du_executor.h"
#include "e2sm/e2sm_ccc/e2sm_ccc_control_service_impl.h"
#include "e2sm/e2sm_ccc/e2sm_ccc_impl.h"
#include "e2sm/e2sm_kpm/e2sm_kpm_asn1_packer.h"
#include "e2sm/e2sm_kpm/e2sm_kpm_du_meas_provider_impl.h"
#include "e2sm/e2sm_kpm/e2sm_kpm_impl.h"
#include "e2sm/e2sm_rc/e2sm_rc_asn1_packer.h"
#include "e2sm/e2sm_rc/e2sm_rc_control_action_du_executor.h"
#include "e2sm/e2sm_rc/e2sm_rc_control_service_impl.h"
#include "e2sm/e2sm_rc/e2sm_rc_impl.h"
#ifdef OCUDU_E2SM_DAPP
#include "e2sm/e2sm_dapp/e2sm_dapp_impl.h" // [srs-sharing]
#include "ocudu/ran/plmn_identity.h"
#endif
#include "ocudu/e2/e2_agent_dependencies.h"

using namespace ocudu;

std::unique_ptr<e2_agent>
ocudu::create_e2_du_agent(const e2ap_configuration&                          e2ap_cfg_,
                          e2_connection_client&                              e2_client_,
                          e2_du_metrics_interface*                           e2_metrics_,
                          odu::f1ap_ue_id_translator*                        f1ap_ue_id_translator_,
                          odu::du_configurator*                              du_configurator_,
                          timer_factory                                      timers_,
                          task_executor&                                     e2_exec_,
                          std::unique_ptr<e2_node_component_config_provider> node_component_config_provider_,
                          std::shared_ptr<e2sm_dapp_bridge>                  dapp_bridge_)
{
  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("E2-DU");
  e2_agent_dependencies   dependencies{
      &logger, e2ap_cfg_, &e2_client_, &timers_, &e2_exec_, std::move(node_component_config_provider_)};

  // E2SM-KPM
  if (e2ap_cfg_.e2sm_kpm_enabled) {
    auto e2sm_kpm_meas_provider = std::make_unique<e2sm_kpm_du_meas_provider_impl>(*f1ap_ue_id_translator_);
    std::unique_ptr<e2sm_kpm_asn1_packer> e2sm_kpm_packer =
        std::make_unique<e2sm_kpm_asn1_packer>(*e2sm_kpm_meas_provider);
    std::unique_ptr<e2sm_kpm_impl> e2sm_kpm_iface =
        std::make_unique<e2sm_kpm_impl>(logger, *e2sm_kpm_packer, *e2sm_kpm_meas_provider);
    e2_metrics_->connect_e2_du_meas_provider(std::move(e2sm_kpm_meas_provider));
    dependencies.e2sm_modules.emplace_back(e2sm_module{e2sm_kpm_asn1_packer::ran_func_id,
                                                       e2sm_kpm_asn1_packer::oid,
                                                       std::move(e2sm_kpm_packer),
                                                       std::move(e2sm_kpm_iface)});
  }

  // E2SM-RC
  if (e2ap_cfg_.e2sm_rc_enabled) {
    auto                                  e2sm_rc_packer = std::make_unique<e2sm_rc_asn1_packer>();
    auto                                  e2sm_rc_iface  = std::make_unique<e2sm_rc_impl>(logger, *e2sm_rc_packer);
    int                                   control_service_style_id = 2;
    std::unique_ptr<e2sm_control_service> rc_control_service_style =
        std::make_unique<e2sm_rc_control_service>(control_service_style_id);
    std::unique_ptr<e2sm_control_action_executor> rc_control_action_executor =
        std::make_unique<e2sm_rc_control_action_2_6_du_executor>(*du_configurator_, *f1ap_ue_id_translator_);
    rc_control_service_style->add_e2sm_rc_control_action_executor(std::move(rc_control_action_executor));
    e2sm_rc_packer->add_e2sm_control_service(rc_control_service_style.get());
    e2sm_rc_iface->add_e2sm_control_service(std::move(rc_control_service_style));
    dependencies.e2sm_modules.emplace_back(e2sm_module{e2sm_rc_asn1_packer::ran_func_id,
                                                       e2sm_rc_asn1_packer::oid,
                                                       std::move(e2sm_rc_packer),
                                                       std::move(e2sm_rc_iface)});
  }

  // E2SM-CCC
  if (e2ap_cfg_.e2sm_ccc_enabled) {
    auto                                  e2sm_ccc_packer = std::make_unique<e2sm_ccc_asn1_packer>();
    auto                                  e2sm_ccc_iface  = std::make_unique<e2sm_ccc_impl>(logger, *e2sm_ccc_packer);
    std::unique_ptr<e2sm_control_service> ccc_control_service_style =
        std::make_unique<e2sm_ccc_control_service_style_2>();
    std::unique_ptr<e2sm_control_action_executor> ccc_control_action_executor =
        std::make_unique<e2sm_ccc_control_o_rrm_policy_ratio_executor>(*du_configurator_, e2_exec_);
    ccc_control_service_style->add_e2sm_rc_control_action_executor(std::move(ccc_control_action_executor));
    e2sm_ccc_packer->add_e2sm_control_service(ccc_control_service_style.get());
    e2sm_ccc_iface->add_e2sm_control_service(std::move(ccc_control_service_style));
    dependencies.e2sm_modules.emplace_back(e2sm_module{e2sm_ccc_asn1_packer::ran_func_id,
                                                       e2sm_ccc_asn1_packer::oid,
                                                       std::move(e2sm_ccc_packer),
                                                       std::move(e2sm_ccc_iface)});
  }

#ifdef OCUDU_E2SM_DAPP
  // [srs-sharing] E2SM-DAPP: E3 dApp reports up to xApps and xApp controls down to dApps, through the dApp service's
  // E3 server (dapp_bridge_). Without a bridge (no dApp service, or one without E3) it is not registered, and E2 Setup
  // logs that it cannot list it.
  if (e2ap_cfg_.e2sm_dapp_enabled) {
    if (dapp_bridge_ == nullptr) {
      logger.error("E2SM-DAPP is enabled but the gNB runs no dApp service with an E3 server: not registered");
    } else {
      e2sm_dapp_node_identity node;
      if (auto plmn = plmn_identity::parse(e2ap_cfg_.plmn)) {
        node.plmn = plmn.value().to_bytes();
      }
      node.nb_id = e2ap_cfg_.gnb_id.id;
      if (e2ap_cfg_.gnb_du_id) {
        node.cu_du_id = gnb_du_id_to_int(*e2ap_cfg_.gnb_du_id);
      }
      auto e2sm_dapp_packer = std::make_unique<e2sm_dapp_asn1_packer>();
      auto e2sm_dapp_iface  = std::make_unique<e2sm_dapp_impl>(logger, *e2sm_dapp_packer, dapp_bridge_, node);
      dependencies.e2sm_modules.emplace_back(e2sm_module{e2sm_dapp_asn1_packer::ran_func_id,
                                                         e2sm_dapp_asn1_packer::oid,
                                                         std::move(e2sm_dapp_packer),
                                                         std::move(e2sm_dapp_iface)});
    }
  }
#else
  static_cast<void>(dapp_bridge_);
#endif

  auto e2_ext = std::make_unique<e2_entity>(std::move(dependencies));
  return e2_ext;
}
