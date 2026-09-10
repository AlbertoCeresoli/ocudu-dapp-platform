// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/dapp/runtime/job_dispatcher.h"

namespace ocudu::dapp::e3 {

/// In-process E3 control core. Protocol decoding and transport remain outside
/// this class; no method is used on a dApp real-time invocation path.
class embedded_agent
{
public:
  explicit embedded_agent(instance_manager& instances, std::size_t maximum_queued = 64, std::size_t retained = 1024) :
    instance_owner(instances), jobs(maximum_queued, retained)
  {
  }

  job_submit_result submit(management::load_request request);
  job_submit_result submit(management::catalog_admit_request request);
  job_submit_result submit(management::lifecycle_request request);
  job_submit_result submit(management::config_set_request request);
  job_submit_result submit(management::model_request request);

  management::inventory_snapshot             inventory() const { return instance_owner.inventory(); }
  std::optional<management::instance_record> instance(const management::identifier& id) const
  {
    return instance_owner.instance(id);
  }
  management::query_result<std::vector<management::config_descriptor>> config_schema(const management::identifier& id)
  {
    return instance_owner.config_schema(id);
  }
  management::query_result<management::config_snapshot> config(const management::identifier& id)
  {
    return instance_owner.config(id);
  }
  management::query_result<std::vector<management::metric_descriptor>> metric_schema(const management::identifier& id)
  {
    return instance_owner.metric_schema(id);
  }
  management::query_result<std::vector<management::metric_sample>> metrics(const management::identifier& id)
  {
    return instance_owner.metrics(id);
  }
  std::vector<incident_record> incidents(const incident_filter& filter = {})
  {
    instance_owner.drain_incidents();
    return instance_owner.incidents().query(filter);
  }
  incident_health_summary health()
  {
    instance_owner.drain_incidents();
    return instance_owner.incidents().health();
  }
  std::size_t clear_incidents_through(uint64_t acknowledged_id)
  {
    instance_owner.drain_incidents();
    return instance_owner.incidents().clear_through(acknowledged_id);
  }

  std::optional<management::job_record> job(const management::identifier& id) const { return jobs.get(id); }
  std::vector<management::job_record>   list_jobs() const { return jobs.list(); }
  bool                                  cancel(const management::identifier& id) { return jobs.cancel(id); }
  std::optional<management::job_record> wait(const management::identifier&         id,
                                             std::chrono::steady_clock::time_point deadline) const
  {
    return jobs.wait(id, deadline);
  }
  void stop() { jobs.stop(); }

private:
  instance_manager& instance_owner;
  job_dispatcher    jobs;
};

} // namespace ocudu::dapp::e3
