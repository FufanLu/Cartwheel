#include "services/deal/redrive.h"

#include <algorithm>

#include "common/logging.h"

namespace cartwheel::deal {

namespace {
constexpr std::chrono::milliseconds kFirstBackoff{100};
constexpr std::chrono::milliseconds kMaxBackoff{5000};
}  // namespace

RedriveQueue::RedriveQueue(OrderLog* log, prometheus::Registry& registry)
    : log_(log),
      redriven_(prometheus::BuildCounter()
                    .Name("cartwheel_deal_redrives_total")
                    .Help("Re-publishes of reservations whose first publish had an unknown outcome")
                    .Register(registry)
                    .Add({})),
      pending_gauge_(prometheus::BuildGauge()
                         .Name("cartwheel_deal_redrive_pending")
                         .Help("Reservations waiting for the order log to acknowledge them")
                         .Register(registry)
                         .Add({})),
      thread_([this] { Run(); }) {}

RedriveQueue::~RedriveQueue() {
  {
    std::lock_guard lock(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  thread_.join();
  // Wait for publishes in flight: their callbacks touch this object.
  std::unique_lock lock(mu_);
  cv_.wait_for(lock, std::chrono::seconds(10), [&] { return inflight_ == 0; });
  if (!items_.empty()) {
    CW_LOG_ERROR("{} reservations still unconfirmed at shutdown; their holds stay in the gate",
                 items_.size());
  }
}

void RedriveQueue::Add(const v1::OrderPlaced& event) {
  {
    std::lock_guard lock(mu_);
    if (!ids_.insert(event.order_id()).second) return;  // already being redriven
    items_.push_back({event, std::chrono::steady_clock::now() + kFirstBackoff, kFirstBackoff});
    pending_gauge_.Set(static_cast<double>(items_.size() + inflight_));
  }
  cv_.notify_all();
}

bool RedriveQueue::Contains(const std::string& order_id) const {
  std::lock_guard lock(mu_);
  return ids_.contains(order_id);
}

size_t RedriveQueue::pending() const {
  std::lock_guard lock(mu_);
  return items_.size() + inflight_;
}

void RedriveQueue::Run() {
  std::unique_lock lock(mu_);
  while (!stopping_) {
    if (items_.empty()) {
      cv_.wait(lock, [&] { return stopping_ || !items_.empty(); });
      continue;
    }
    auto next = std::min_element(items_.begin(), items_.end(),
                                 [](const Item& a, const Item& b) { return a.due < b.due; });
    if (next->due > std::chrono::steady_clock::now()) {
      cv_.wait_until(lock, next->due);
      continue;
    }
    Item item = std::move(*next);
    items_.erase(next);
    ++inflight_;
    lock.unlock();
    redriven_.Increment();
    log_->Publish(item.event, [this, item](PublishOutcome outcome, const std::string& error) {
      {
        std::lock_guard l(mu_);
        --inflight_;
        if (outcome != PublishOutcome::kDelivered) {
          // Still not acknowledged: keep the hold, try again later.
          Item again = item;
          again.backoff = std::min(item.backoff * 2, kMaxBackoff);
          again.due = std::chrono::steady_clock::now() + again.backoff;
          items_.push_back(std::move(again));
          CW_LOG_DEBUG("redrive {}: {} ({})", item.event.order_id(),
                       outcome == PublishOutcome::kFailed ? "failed" : "unknown", error);
        } else {
          ids_.erase(item.event.order_id());
          CW_LOG_INFO("redrive {}: acknowledged", item.event.order_id());
        }
        pending_gauge_.Set(static_cast<double>(items_.size() + inflight_));
        // Under the lock: once it is released the destructor may finish, and
        // nothing of `this` may be touched after that.
        cv_.notify_all();
      }
    });
    lock.lock();
  }
}

}  // namespace cartwheel::deal
