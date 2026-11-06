#pragma once

// Deterministic simulation of a CartwheelKV cluster (ADR 0003, FoundationDB style).
//
// One thread, one seeded PRNG, one virtual clock. The network, timers and
// node crashes are all events in a single time-ordered queue, so a seed
// reproduces a run exactly: every message, drop, timeout and interleaving.
// A failing invariant prints its seed; `CW_SIM_SEED=<n>` replays it.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "cluster/runtime.h"
#include "common/clock.h"

namespace cartwheel::sim {

using cluster::NodeId;

class World {
 public:
  explicit World(uint64_t seed) : rng(seed), clock(1'000'000'000) {}

  void After(int64_t delay_ns, std::function<void()> fn) {
    queue_.push(
        Event{clock.NowNanos() + std::max<int64_t>(0, delay_ns), next_seq_++, std::move(fn)});
  }
  bool Step() {
    if (queue_.empty()) return false;
    Event e = queue_.top();
    queue_.pop();
    clock.Set(e.at);
    e.fn();
    ++steps_;
    return true;
  }
  void RunFor(int64_t ns) {
    const int64_t until = clock.NowNanos() + ns;
    while (!queue_.empty() && queue_.top().at <= until) Step();
    clock.Set(until);
  }
  [[nodiscard]] int64_t now() const { return clock.NowNanos(); }
  [[nodiscard]] uint64_t steps() const { return steps_; }
  uint64_t Uniform(uint64_t n) { return n == 0 ? 0 : rng() % n; }
  bool Chance(double p) { return std::uniform_real_distribution<double>(0, 1)(rng) < p; }

  std::mt19937_64 rng;
  ManualClock clock;

 private:
  struct Event {
    int64_t at;
    uint64_t seq;  // FIFO among equal timestamps: determinism
    std::function<void()> fn;
    bool operator>(const Event& o) const { return at != o.at ? at > o.at : seq > o.seq; }
  };
  std::priority_queue<Event, std::vector<Event>, std::greater<>> queue_;
  uint64_t next_seq_ = 0;
  uint64_t steps_ = 0;
};

class Network {
 public:
  explicit Network(World* w) : world_(w) {}

  double drop_rate = 0.0;
  // Optional: called for every message event (send, deliver, drop, reply).
  std::function<void(const std::string& event, const NodeId& from, const NodeId& to,
                     cluster::MessageType type)>
      trace;
  int64_t min_latency_ns = 200'000;    // 0.2 ms
  int64_t max_latency_ns = 3'000'000;  // 3 ms
  uint64_t sent = 0;
  uint64_t dropped = 0;

  void Register(const NodeId& id, cluster::RequestHandler* h) {
    handlers_[id] = h;
    up_.insert(id);
  }
  void SetUp(const NodeId& id, bool up) {
    if (up) {
      up_.insert(id);
    } else {
      up_.erase(id);
    }
  }
  [[nodiscard]] bool IsUp(const NodeId& id) const { return up_.contains(id); }
  void Partition(std::set<NodeId> side_a) { side_a_ = std::move(side_a); }
  void Heal() { side_a_.clear(); }
  [[nodiscard]] bool Partitioned() const { return !side_a_.empty(); }
  [[nodiscard]] bool CanTalk(const NodeId& a, const NodeId& b) const {
    if (!IsUp(a) || !IsUp(b)) return false;
    return side_a_.empty() || side_a_.contains(a) == side_a_.contains(b);
  }

  // Transport bound to a sender.
  class Endpoint final : public cluster::Transport {
   public:
    Endpoint(Network* net, NodeId self) : net_(net), self_(std::move(self)) {}
    void Call(const NodeId& to, cluster::MessageType type, std::string payload, int64_t timeout_ns,
              Done done) override {
      net_->Send(self_, to, type, std::move(payload), timeout_ns, std::move(done));
    }

   private:
    Network* net_;
    NodeId self_;
  };

 private:
  int64_t Latency() {
    return min_latency_ns + static_cast<int64_t>(world_->Uniform(
                                static_cast<uint64_t>(max_latency_ns - min_latency_ns)));
  }

  void Send(const NodeId& from, const NodeId& to, cluster::MessageType type, std::string payload,
            int64_t timeout_ns, cluster::Transport::Done done) {
    ++sent;
    auto once = std::make_shared<cluster::Transport::Done>(std::move(done));
    auto fire = [once](Result<std::string> r) {
      if (*once) {
        auto f = std::move(*once);
        *once = nullptr;
        f(std::move(r));
      }
    };
    world_->After(timeout_ns, [fire] { fire(Status::DeadlineExceeded("sim: timeout")); });
    if (!CanTalk(from, to) || world_->Chance(drop_rate)) {
      ++dropped;
      if (trace) trace("lost", from, to, type);
      return;
    }
    if (trace) trace("send", from, to, type);
    world_->After(Latency(), [this, from, to, type, payload = std::move(payload), fire]() mutable {
      if (!CanTalk(from, to)) return;  // went down or partitioned in flight
      auto it = handlers_.find(to);
      if (it == handlers_.end()) return;
      it->second->HandleRequest(
          from, type, std::move(payload), [this, from, to, type, fire](Result<std::string> resp) {
            if (world_->Chance(drop_rate)) {
              ++dropped;
              return;
            }
            world_->After(Latency(),
                          [this, from, to, type, fire, resp = std::move(resp)]() mutable {
                            if (!CanTalk(to, from)) return;
                            if (trace) trace(resp.ok() ? "reply" : "error", to, from, type);
                            fire(std::move(resp));
                          });
          });
    });
  }

  World* world_;
  std::map<NodeId, cluster::RequestHandler*> handlers_;
  std::set<NodeId> up_;
  std::set<NodeId> side_a_;
};

// Timers of one node incarnation: they stop firing once that incarnation crashes.
class NodeScheduler final : public cluster::Scheduler {
 public:
  NodeScheduler(World* w, std::shared_ptr<bool> alive) : world_(w), alive_(std::move(alive)) {}
  void After(int64_t delay_ns, std::function<void()> fn) override {
    world_->After(delay_ns, [alive = alive_, fn = std::move(fn)] {
      if (*alive) fn();
    });
  }

 private:
  World* world_;
  std::shared_ptr<bool> alive_;
};

}  // namespace cartwheel::sim
