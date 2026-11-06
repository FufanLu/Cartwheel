#include "common/metrics.h"

#include <exception>

#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/text_serializer.h>

#include "common/build_info.h"

namespace cartwheel {

std::shared_ptr<prometheus::Registry> GlobalMetricsRegistry() {
  static const auto kRegistry = std::make_shared<prometheus::Registry>();
  return kRegistry;
}

std::string SerializeMetrics(const prometheus::Registry& registry) {
  return prometheus::TextSerializer().Serialize(registry.Collect());
}

prometheus::Histogram::BucketBoundaries LatencyBucketsSeconds() {
  // 50us * 2^i for i = 0..18: 50us .. 13.1s.
  prometheus::Histogram::BucketBoundaries b;
  constexpr int kBuckets = 19;
  b.reserve(kBuckets);
  double v = 50e-6;
  for (int i = 0; i < kBuckets; ++i, v *= 2.0) b.push_back(v);
  return b;
}

MetricsServer::MetricsServer(std::unique_ptr<prometheus::Exposer> exposer, int port)
    : exposer_(std::move(exposer)), port_(port) {}

MetricsServer::~MetricsServer() = default;

Result<std::unique_ptr<MetricsServer>> MetricsServer::Start(
    const std::string& bind_address, const std::shared_ptr<prometheus::Registry>& registry) {
  std::unique_ptr<prometheus::Exposer> exposer;
  try {
    exposer = std::make_unique<prometheus::Exposer>(bind_address, /*num_threads=*/2);
  } catch (const std::exception& e) {
    return Status::Unavailable("metrics: cannot listen on " + bind_address + ": " + e.what());
  }
  exposer->RegisterCollectable(registry);
  const auto ports = exposer->GetListeningPorts();
  const int port = ports.empty() ? 0 : ports.front();
  return std::unique_ptr<MetricsServer>(new MetricsServer(std::move(exposer), port));
}

void RegisterBuildInfo(prometheus::Registry& registry, const std::string& service) {
  prometheus::BuildGauge()
      .Name("cartwheel_build_info")
      .Help("Build metadata; value is always 1")
      .Register(registry)
      .Add({{"version", std::string(kVersion)},
            {"git_sha", std::string(kGitSha)},
            {"service", service}})
      .Set(1);
}

}  // namespace cartwheel
