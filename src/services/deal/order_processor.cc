#include "services/deal/order_processor.h"

#include <chrono>
#include <thread>

#include "common/logging.h"
#include "common/metrics.h"

namespace cartwheel::deal {

OrderProcessor::OrderProcessor(OrderStore* store, OrderNotifier* notifier,
                               prometheus::Registry& registry)
    : store_(store),
      notifier_(notifier),
      batch_size_(prometheus::BuildHistogram()
                      .Name("cartwheel_order_batch_size")
                      .Help("Reservations per database transaction")
                      .Register(registry)
                      .Add({}, prometheus::Histogram::BucketBoundaries{1, 2, 5, 10, 25, 50, 100,
                                                                       250, 500, 1000})),
      commit_latency_(prometheus::BuildHistogram()
                          .Name("cartwheel_order_commit_duration_seconds")
                          .Help("Duration of one batch transaction")
                          .Register(registry)
                          .Add({}, LatencyBucketsSeconds())),
      orders_(prometheus::BuildCounter()
                  .Name("cartwheel_orders_total")
                  .Help("Reservations processed by result (confirmed, rejected, duplicate)")
                  .Register(registry)),
      failed_batches_(prometheus::BuildCounter()
                          .Name("cartwheel_order_batch_failures_total")
                          .Help("Batch transactions that failed and will be retried")
                          .Register(registry)
                          .Add({})) {}

Status OrderProcessor::Process(const std::vector<v1::OrderPlaced>& batch) {
  if (batch.empty()) return Status::Ok();
  const auto start = std::chrono::steady_clock::now();
  auto result = store_->ApplyBatch(batch);
  if (!result.ok()) {
    failed_batches_.Increment();
    return result.status();
  }
  commit_latency_.Observe(
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
  batch_size_.Observe(static_cast<double>(batch.size()));
  orders_.Add({{"result", "confirmed"}}).Increment(static_cast<double>(result->confirmed.size()));
  orders_.Add({{"result", "rejected"}}).Increment(static_cast<double>(result->rejected.size()));
  orders_.Add({{"result", "duplicate"}}).Increment(static_cast<double>(result->duplicates.size()));
  for (const auto& o : result->rejected) {
    CW_LOG_WARN("order {} for {} on {} rejected: {}", o.order_id, o.user_id, o.deal_id, o.reason);
  }
  if (notifier_ != nullptr) {
    for (const auto* group : {&result->confirmed, &result->rejected, &result->duplicates}) {
      for (const auto& o : *group) notifier_->OrderUpdated(ToProto(o));
    }
  }
  return Status::Ok();
}

void RunWorkerLoop(OrderSource* source, OrderProcessor* processor, const std::atomic<bool>* stop,
                   const WorkerLoopOptions& options) {
  while (!stop->load(std::memory_order_acquire)) {
    auto batch = source->Poll(options.max_batch, options.poll_wait_ms);
    if (batch.empty()) continue;
    Backoff backoff(options.retry);
    bool done = false;
    while (!stop->load(std::memory_order_acquire)) {
      auto s = processor->Process(batch);
      if (s.ok()) {
        done = true;
        break;
      }
      const int64_t delay = backoff.NextDelayMicros().value_or(options.retry.cap_micros);
      CW_LOG_WARN("batch of {} failed ({}); retrying in {} ms", batch.size(), s.ToString(),
                  delay / 1000);
      std::this_thread::sleep_for(std::chrono::microseconds(delay));
    }
    if (!done) return;  // stopping: the batch is redelivered after restart
    if (auto s = source->Commit(); !s.ok()) {
      // The records are in Postgres; a redelivery is harmless (duplicates).
      CW_LOG_WARN("offset commit failed: {}", s.ToString());
    }
  }
}

}  // namespace cartwheel::deal
