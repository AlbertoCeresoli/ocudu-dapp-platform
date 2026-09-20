// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include "ocudu/phy/upper/channel_processors/pusch/pusch_processor_result_notifier.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_resource_loan.h"
#include <memory>

namespace ocudu {

/// Retains the actual dependency entry AND its pool and originating processor/slot. No hot-path control-block
/// allocation: the dependency clone uses the bounded pool's cold-allocated intrusive reference counter.
template <typename Pool>
class pooled_pusch_resource_provider final : public pusch_resource_loan_provider
{
public:
  pooled_pusch_resource_provider(const std::shared_ptr<Pool>&     pool_,
                                 const typename Pool::ptr&        dependencies_,
                                 pusch_processor_result_notifier& notifier_) noexcept :
    pool(pool_), dependencies(dependencies_), notifier(notifier_)
  {
  }

  pusch_resource_loan acquire() noexcept override
  {
    if (!pool || !dependencies.belongs_to(*pool)) {
      return {};
    }
    auto activity = notifier.acquire_processing_activity();
    if (!activity) {
      return {};
    }
    return pusch_resource_loan(retained{pool, std::move(activity), dependencies.clone()});
  }

private:
  struct retained {
    // Destruction order matters: return the dependency before allowing processor/slot reuse, and keep its
    // parent pool alive through both releases. Pool destruction belongs on the control/reclamation path.
    std::shared_ptr<Pool>     pool;
    pusch_processing_activity activity;
    typename Pool::ptr        dependency;
    explicit                  operator bool() const noexcept { return pool && activity && dependency; }
  };
  const std::shared_ptr<Pool>&     pool;
  const typename Pool::ptr&        dependencies;
  pusch_processor_result_notifier& notifier;
};
} // namespace ocudu
