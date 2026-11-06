#include "kv/fault_injection.h"

#include <functional>
#include <vector>

#include <gtest/gtest.h>

namespace cartwheel::kv {
namespace {

class RecordingTransport final : public cluster::Transport {
 public:
  void Call(const cluster::NodeId& to, cluster::MessageType, std::string, int64_t,
            Done done) override {
    sent.push_back(to);
    done(std::string("pong"));
  }
  std::vector<std::string> sent;
};

class QueueScheduler final : public cluster::Scheduler {
 public:
  void After(int64_t delay_ns, std::function<void()> fn) override {
    delays.push_back(delay_ns);
    tasks.push_back(std::move(fn));
  }
  std::vector<int64_t> delays;
  std::vector<std::function<void()>> tasks;
};

class EchoHandler final : public cluster::RequestHandler {
 public:
  void HandleRequest(const cluster::NodeId&, cluster::MessageType, std::string payload,
                     Reply reply) override {
    reply(std::move(payload));
  }
};

TEST(FaultInjection, IsolationCutsPeerTrafficBothWaysAndExpires) {
  ManualClock clock(1'000'000'000);
  FaultSwitch fault(&clock);
  RecordingTransport inner;
  QueueScheduler scheduler;
  IsolatingTransport transport(&inner, &scheduler, &fault);
  EchoHandler echo;
  IsolatingHandler handler(&echo, &fault);

  Result<std::string> out = Status::Internal("unset");
  transport.Call("n2", cluster::v1::MESSAGE_TYPE_GOSSIP, "x", 300'000'000,
                 [&](Result<std::string> r) { out = std::move(r); });
  EXPECT_TRUE(out.ok());
  EXPECT_EQ(inner.sent.size(), 1U);

  fault.IsolateFor(10'000);
  EXPECT_TRUE(fault.Isolated());
  EXPECT_EQ(fault.RemainingMs(), 10'000);

  // Outgoing: not sent; times out after the call's own timeout, like a partition.
  out = Status::Internal("unset");
  transport.Call("n2", cluster::v1::MESSAGE_TYPE_GOSSIP, "x", 300'000'000,
                 [&](Result<std::string> r) { out = std::move(r); });
  EXPECT_EQ(inner.sent.size(), 1U);
  ASSERT_EQ(scheduler.tasks.size(), 1U);
  EXPECT_EQ(scheduler.delays[0], 300'000'000);
  scheduler.tasks[0]();
  EXPECT_EQ(out.status().code(), StatusCode::kDeadlineExceeded);

  // Incoming: refused.
  Result<std::string> in = std::string();
  handler.HandleRequest("n3", cluster::v1::MESSAGE_TYPE_GOSSIP, "hi",
                        [&](Result<std::string> r) { in = std::move(r); });
  EXPECT_EQ(in.status().code(), StatusCode::kUnavailable);

  // Healed.
  clock.AdvanceMillis(10'001);
  EXPECT_FALSE(fault.Isolated());
  handler.HandleRequest("n3", cluster::v1::MESSAGE_TYPE_GOSSIP, "hi",
                        [&](Result<std::string> r) { in = std::move(r); });
  ASSERT_TRUE(in.ok());
  EXPECT_EQ(*in, "hi");
}

}  // namespace
}  // namespace cartwheel::kv
