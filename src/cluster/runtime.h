#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "cartwheel/cluster/v1/cluster.pb.h"
#include "common/clock.h"
#include "common/status.h"

namespace cartwheel::cluster {

// The three seams of ADR 0003. Cluster logic touches the outside world only
// through them, so the same code runs over gRPC + Asio in production and
// inside a single-threaded, seeded simulator in tests.

using NodeId = std::string;
using MessageType = v1::MessageType;

// Request/response messaging between nodes. Implementations must invoke
// `done` exactly once: with the response payload, with the remote error, or
// with DeadlineExceeded when the timeout fires first.
class Transport {
 public:
  using Done = std::function<void(Result<std::string> response)>;
  virtual ~Transport() = default;
  virtual void Call(const NodeId& to, MessageType type, std::string payload, int64_t timeout_ns,
                    Done done) = 0;
};

// The receiving side. A node handles a request and calls `reply` exactly once,
// possibly later (e.g. a forwarded write waits for its own quorum).
class RequestHandler {
 public:
  using Reply = std::function<void(Result<std::string> response)>;
  virtual ~RequestHandler() = default;
  virtual void HandleRequest(const NodeId& from, MessageType type, std::string payload,
                             Reply reply) = 0;
};

// Timers. Callbacks may run on any thread in production, and in simulator
// order in tests.
class Scheduler {
 public:
  virtual ~Scheduler() = default;
  virtual void After(int64_t delay_ns, std::function<void()> fn) = 0;
};

// Everything a node needs from its environment.
struct Runtime {
  Transport* transport = nullptr;
  Scheduler* scheduler = nullptr;
  const Clock* clock = nullptr;
  uint64_t seed = 1;  // every random choice (gossip peers, jitter) derives from it
};

}  // namespace cartwheel::cluster
