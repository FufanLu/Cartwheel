#pragma once

#include <atomic>
#include <cstddef>

#include <prometheus/counter.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include "common/backoff.h"
#include "services/deal/deal_ports.h"

namespace cartwheel::deal {

// order-worker's core: turns batches of reservations into committed orders.
//
// Delivery is at-least-once (offsets are committed only after the database
// transaction), and ApplyBatch is idempotent, so the combination is
// effectively-once: a crash anywhere replays records the database already
// has, and they come back as duplicates. Notifications are sent for
// duplicates too, so a crash between COMMIT and PUBLISH cannot lose one.
class OrderProcessor {
 public:
  OrderProcessor(OrderStore* store, OrderNotifier* notifier, prometheus::Registry& registry);

  Status Process(const std::vector<v1::OrderPlaced>& batch);

 private:
  OrderStore* store_;
  OrderNotifier* notifier_;
  prometheus::Histogram& batch_size_;
  prometheus::Histogram& commit_latency_;
  prometheus::Family<prometheus::Counter>& orders_;
  prometheus::Counter& failed_batches_;
};

struct WorkerLoopOptions {
  size_t max_batch = 500;
  int64_t poll_wait_ms = 20;
  BackoffPolicy retry{.base_micros = 50'000, .cap_micros = 2'000'000, .max_attempts = 1'000'000};
};

// Poll -> Process -> Commit until `stop`. A failed batch is retried with
// backoff and never skipped: offsets only move once the batch is in Postgres.
void RunWorkerLoop(OrderSource* source, OrderProcessor* processor, const std::atomic<bool>* stop,
                   const WorkerLoopOptions& options = {});

}  // namespace cartwheel::deal
