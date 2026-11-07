#include "kv/cluster_metrics.h"

#include <chrono>
#include <map>
#include <string>

namespace cartwheel::kv {

ClusterMetrics::ClusterMetrics(prometheus::Registry& registry, ClusterNode* node)
    : node_(node),
      events_(prometheus::BuildCounter()
                  .Name("cartwheel_cluster_events_total")
                  .Help("Coordinator and repair events on this node, by kind")
                  .Register(registry)),
      hints_pending_(prometheus::BuildGauge()
                         .Name("cartwheel_cluster_hints_pending")
                         .Help("Hinted writes held for unreachable peers")
                         .Register(registry)
                         .Add({})),
      members_(prometheus::BuildGauge()
                   .Name("cartwheel_cluster_members")
                   .Help("Members by state, as this node's failure detector sees them")
                   .Register(registry)) {}

ClusterMetrics::~ClusterMetrics() {
  Stop();
}

void ClusterMetrics::Start() {
  running_ = true;
  thread_ = std::thread([this] {
    while (running_.load()) {
      SampleOnce();
      for (int i = 0; i < 10 && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  });
}

void ClusterMetrics::Stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

void ClusterMetrics::SampleOnce() {
  const ClusterStats s = node_->Stats();
  const auto bump = [this](const char* kind, uint64_t now, uint64_t before) {
    auto& c = events_.Add({{"kind", kind}});
    if (now > before) c.Increment(static_cast<double>(now - before));
  };
  bump("put", s.puts, last_.puts);
  bump("get", s.gets, last_.gets);
  bump("forwarded", s.forwarded, last_.forwarded);
  bump("quorum_failure", s.quorum_failures, last_.quorum_failures);
  bump("throttled", s.throttled, last_.throttled);
  bump("read_repair", s.read_repairs, last_.read_repairs);
  bump("hint_stored", s.hints_stored, last_.hints_stored);
  bump("hint_delivered", s.hints_delivered, last_.hints_delivered);
  bump("anti_entropy_round", s.anti_entropy_rounds, last_.anti_entropy_rounds);
  bump("anti_entropy_key_repaired", s.anti_entropy_keys_repaired, last_.anti_entropy_keys_repaired);
  last_ = s;
  hints_pending_.Set(static_cast<double>(s.hints_pending));

  std::map<std::string, int> by_state{{"ALIVE", 0}, {"SUSPECT", 0}, {"DEAD", 0}, {"LEFT", 0}};
  for (const auto& m : node_->membership().Snapshot()) {
    const auto name = cluster::v1::Member::State_Name(m.state());
    const auto pos = name.rfind('_');
    ++by_state[pos == std::string::npos ? name : name.substr(pos + 1)];
  }
  for (const auto& [state, n] : by_state) members_.Add({{"state", state}}).Set(n);
}

}  // namespace cartwheel::kv
