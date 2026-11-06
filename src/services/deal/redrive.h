#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>

#include "services/deal/deal_ports.h"

namespace cartwheel::deal {

// Re-publishes reservations the order log did not acknowledge.
//
// When the broker stops answering, a record may or may not be in the log.
// Releasing the unit could sell it twice (or void an order id a retrying
// buyer was already given); keeping the hold without a record would strand it
// (the buyer sees PENDING forever). The way out is the idempotent consumer:
// publishing the same order id again is harmless, because order-worker counts
// a second copy as a duplicate. So every unacknowledged publish is retried
// here, with backoff, until the log has it.
class RedriveQueue {
 public:
  RedriveQueue(OrderLog* log, prometheus::Registry& registry);
  // Waits up to 10 s for publishes in flight; anything still queued is left
  // to the hold sweeper of the next deal-svc.
  ~RedriveQueue();
  RedriveQueue(const RedriveQueue&) = delete;
  RedriveQueue& operator=(const RedriveQueue&) = delete;

  void Add(const v1::OrderPlaced& event);
  [[nodiscard]] bool Contains(const std::string& order_id) const;
  [[nodiscard]] size_t pending() const;

 private:
  struct Item {
    v1::OrderPlaced event;
    std::chrono::steady_clock::time_point due;
    std::chrono::milliseconds backoff;
  };
  void Run();

  OrderLog* log_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Item> items_;
  std::set<std::string> ids_;  // queued or in flight
  size_t inflight_ = 0;
  bool stopping_ = false;
  prometheus::Counter& redriven_;
  prometheus::Gauge& pending_gauge_;
  std::thread thread_;
};

}  // namespace cartwheel::deal
