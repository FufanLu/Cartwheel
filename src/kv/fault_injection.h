#pragma once

#include <atomic>
#include <cstdint>

#include "cluster/runtime.h"

namespace cartwheel::kv {

// A switch that cuts this node off from its peers for a while: a network
// partition you can trigger from the ops console. Clients can still reach
// the node, which is exactly the interesting case: it can no longer reach a
// quorum, and the rest of the cluster declares it dead and routes around it.
// Enabled only with debug.fault_injection = true.
class FaultSwitch {
 public:
  explicit FaultSwitch(const Clock* clock) : clock_(clock) {}

  void IsolateFor(int64_t ms) {
    until_ns_.store(clock_->NowNanos() + ms * 1'000'000, std::memory_order_release);
  }
  [[nodiscard]] bool Isolated() const { return RemainingMs() > 0; }
  [[nodiscard]] int64_t RemainingMs() const {
    const int64_t left = until_ns_.load(std::memory_order_acquire) - clock_->NowNanos();
    return left > 0 ? left / 1'000'000 : 0;
  }

 private:
  const Clock* clock_;
  std::atomic<int64_t> until_ns_{0};
};

// Outgoing side: while isolated, peer calls time out (after their own
// timeout, like a real partition) instead of being sent.
class IsolatingTransport final : public cluster::Transport {
 public:
  IsolatingTransport(cluster::Transport* inner, cluster::Scheduler* scheduler,
                     const FaultSwitch* fault)
      : inner_(inner), scheduler_(scheduler), fault_(fault) {}

  void Call(const cluster::NodeId& to, cluster::MessageType type, std::string payload,
            int64_t timeout_ns, Done done) override {
    if (!fault_->Isolated()) {
      inner_->Call(to, type, std::move(payload), timeout_ns, std::move(done));
      return;
    }
    scheduler_->After(timeout_ns, [done = std::move(done)] {
      done(Status::DeadlineExceeded("isolated by fault injection"));
    });
  }

 private:
  cluster::Transport* inner_;
  cluster::Scheduler* scheduler_;
  const FaultSwitch* fault_;
};

// Incoming side: while isolated, peer requests (gossip, replica traffic,
// forwards) are refused.
class IsolatingHandler final : public cluster::RequestHandler {
 public:
  IsolatingHandler(cluster::RequestHandler* inner, const FaultSwitch* fault)
      : inner_(inner), fault_(fault) {}

  void HandleRequest(const cluster::NodeId& from, cluster::MessageType type, std::string payload,
                     Reply reply) override {
    if (fault_->Isolated()) {
      reply(Status::Unavailable("isolated by fault injection"));
      return;
    }
    inner_->HandleRequest(from, type, std::move(payload), std::move(reply));
  }

 private:
  cluster::RequestHandler* inner_;
  const FaultSwitch* fault_;
};

}  // namespace cartwheel::kv
