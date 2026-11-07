#pragma once

#include <atomic>
#include <thread>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>

#include "kv/cluster_node.h"

namespace cartwheel::kv {

// Exports a cluster node's coordinator and repair counters, hint backlog and
// membership view to Prometheus, sampled once a second. Counters are advanced
// by the delta since the last sample, so they stay monotonic for rate().
class ClusterMetrics {
 public:
  ClusterMetrics(prometheus::Registry& registry, ClusterNode* node);
  ~ClusterMetrics();
  ClusterMetrics(const ClusterMetrics&) = delete;
  ClusterMetrics& operator=(const ClusterMetrics&) = delete;

  void Start();
  void Stop();
  void SampleOnce();

 private:
  ClusterNode* node_;
  ClusterStats last_;
  prometheus::Family<prometheus::Counter>& events_;
  prometheus::Gauge& hints_pending_;
  prometheus::Family<prometheus::Gauge>& members_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace cartwheel::kv
