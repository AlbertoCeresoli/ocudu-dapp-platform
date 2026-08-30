// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "apps/services/dapp/dapp_service.h"
#include "apps/services/remote_control/remote_server_metrics_gateway.h"
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace ocudu {
namespace app_services {

/// Tees every already-generated JSON metrics report to the optional remote
/// WebSocket gateway and to the dApp E3 telemetry stream `gnb.metrics_json`.
///
/// The gateway is handed to metrics consumers before the DU (and therefore the
/// dApp service) exists, so the sink is bound later; the sink is shared so a
/// report that races DU teardown never touches freed state.
class dapp_metrics_json_fanout final : public remote_server_metrics_gateway
{
public:
  explicit dapp_metrics_json_fanout(remote_server_metrics_gateway* upstream_) : upstream(upstream_) {}

  void bind(std::shared_ptr<dapp_metrics_json_sink> sink_) noexcept
  {
    const std::lock_guard<std::mutex> guard(mutex);
    sink = std::move(sink_);
  }

  bool has_upstream() const noexcept { return upstream != nullptr; }

  void send(std::string metrics) override
  {
    std::shared_ptr<dapp_metrics_json_sink> active;
    {
      const std::lock_guard<std::mutex> guard(mutex);
      active = sink;
    }
    if (active != nullptr) {
      active->publish_metrics_json(metrics);
    }
    if (upstream != nullptr) {
      upstream->send(std::move(metrics));
    }
  }

private:
  remote_server_metrics_gateway*          upstream;
  std::mutex                              mutex;
  std::shared_ptr<dapp_metrics_json_sink> sink;
};

} // namespace app_services
} // namespace ocudu
