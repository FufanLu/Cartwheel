#pragma once

#include <memory>
#include <string>

#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include "common/status.h"

namespace prometheus {
class Exposer;
}

namespace cartwheel {

// Process-wide Prometheus registry. Components register their families here;
// MetricsServer exposes it at GET /metrics.
std::shared_ptr<prometheus::Registry> GlobalMetricsRegistry();

// Text exposition format of a registry (what /metrics returns).
[[nodiscard]] std::string SerializeMetrics(const prometheus::Registry& registry);

// Latency buckets in seconds: 50us .. ~13s, roughly x2 per bucket. One shared
// layout keeps histograms aggregatable across services in PromQL.
[[nodiscard]] prometheus::Histogram::BucketBoundaries LatencyBucketsSeconds();

// HTTP endpoint serving /metrics. Owns the civetweb listener thread.
class MetricsServer {
 public:
  // bind_address like "0.0.0.0:9100"; port 0 picks a free port.
  static Result<std::unique_ptr<MetricsServer>> Start(
      const std::string& bind_address,
      const std::shared_ptr<prometheus::Registry>& registry = GlobalMetricsRegistry());
  ~MetricsServer();

  MetricsServer(const MetricsServer&) = delete;
  MetricsServer& operator=(const MetricsServer&) = delete;

  [[nodiscard]] int port() const noexcept { return port_; }

 private:
  MetricsServer(std::unique_ptr<prometheus::Exposer> exposer, int port);
  std::unique_ptr<prometheus::Exposer> exposer_;
  int port_;
};

// Registers cartwheel_build_info{version, git_sha, service} = 1.
void RegisterBuildInfo(prometheus::Registry& registry, const std::string& service);

}  // namespace cartwheel
