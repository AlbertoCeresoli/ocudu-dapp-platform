// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// [srs-sharing] E2SM-DAPP service model (not in upstream OCUDU): E3 dApp reports up as E2 indications (report
// style 1), the dApp subscription map (report style 2), and E2 controls down to dApps (control style 1). The E3 side
// is reached only through e2sm_dapp_bridge (include/ocudu/e2/e2sm/e2sm_dapp.h).

#include "e2sm_dapp_asn1_packer.h"
#include "ocudu/e2/e2sm/e2sm.h"
#include "ocudu/e2/e2sm/e2sm_dapp.h"
#include "ocudu/ocudulog/logger.h"
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace ocudu {

/// Bounded queue of E3 dApp reports for one report-style-1 action.
class e2sm_dapp_report_queue
{
public:
  /// Reports kept while waiting for the next indication tick; older ones are dropped beyond this.
  static constexpr std::size_t capacity = 256;

  void                            push(const e2sm_dapp_report& report);
  std::optional<e2sm_dapp_report> pop();
  uint64_t                        dropped() const;

private:
  mutable std::mutex           mutex;
  std::deque<e2sm_dapp_report> reports;
  uint64_t                     nof_dropped = 0;
};

/// Fans every E3 dApp report out to the queues of the active report-style-1 actions. The bridge calls publish() on
/// the E3 server thread; queues register and unregister on the E2 executor.
class e2sm_dapp_report_hub
{
public:
  void publish(const e2sm_dapp_report& report);
  void add(e2sm_dapp_report_queue* queue);
  void remove(e2sm_dapp_report_queue* queue);

private:
  std::mutex                           mutex;
  std::vector<e2sm_dapp_report_queue*> queues;
};

class e2sm_dapp_impl : public e2sm_interface
{
public:
  e2sm_dapp_impl(ocudulog::basic_logger&           logger_,
                 e2sm_dapp_asn1_packer&            packer_,
                 std::shared_ptr<e2sm_dapp_bridge> bridge_,
                 e2sm_dapp_node_identity           node_);

  e2sm_handler&                        get_e2sm_packer() override { return packer; }
  bool                                 action_supported(const asn1::e2ap::ric_action_to_be_setup_item_s& ric_action) override;
  std::unique_ptr<e2sm_report_service> get_e2sm_report_service(const byte_buffer& action_definition) override;
  bool                  add_e2sm_control_service(std::unique_ptr<e2sm_control_service> control_service) override;
  e2sm_control_service* get_e2sm_control_service(const e2sm_ric_control_request& request) override;

private:
  ocudulog::basic_logger&               logger;
  e2sm_dapp_asn1_packer&                packer;
  /// Shared with the dApp service, so the bridge stays valid for as long as this module and its services use it.
  std::shared_ptr<e2sm_dapp_bridge>     bridge;
  e2sm_dapp_node_identity               node;
  std::shared_ptr<e2sm_dapp_report_hub> hub;
  std::unique_ptr<e2sm_control_service> control_service;
};

} // namespace ocudu
