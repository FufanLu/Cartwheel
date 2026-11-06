#pragma once

// In-process implementations of the deal ports. They follow the same rules as
// the real systems (atomic reservation, idempotent batch commit under a lock)
// and add fault injection, so unit tests can exercise lost publishes and
// redelivery. `deal-svc --backend=memory` runs the whole pipeline on them for
// demos without Redis, Kafka or Postgres.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "services/deal/deal_ports.h"

namespace cartwheel::deal {

class MemoryInventoryGate final : public InventoryGate {
 public:
  Result<Reservation> Reserve(const std::string& deal_id, int64_t generation,
                              const std::string& user_id, const std::string& order_id) override;
  Result<std::vector<std::pair<std::string, std::string>>> Holds(const std::string& deal_id,
                                                                 int64_t generation) override;
  Status Arm(const std::string& deal_id, int64_t generation, int64_t quantity,
             bool only_if_absent) override;
  Result<int64_t> Remaining(const std::string& deal_id, int64_t generation) override;
  Result<std::optional<std::string>> HeldOrder(const std::string& deal_id, int64_t generation,
                                               const std::string& user_id) override;

  // Simulates Redis losing its data (failover to a replica that missed
  // writes): every hold is forgotten and stock is set to `stock`.
  void ForgetEverything(const std::string& deal_id, int64_t generation, int64_t stock);

 private:
  struct Slot {
    int64_t stock = 0;
    std::unordered_map<std::string, std::string> holds;  // user -> order id
  };
  std::mutex mu_;
  std::map<std::pair<std::string, int64_t>, Slot> slots_;
};

// A partitioned, replayable log with at-least-once consumption.
class MemoryOrderLog final : public OrderLog, public OrderSource {
 public:
  void Publish(const v1::OrderPlaced& event, Done done) override;

  // Fault injection for the next publishes. With kUnknown the record is
  // written unless `lost` (the ack was lost vs. the request was lost).
  void FailNext(int n, PublishOutcome outcome, bool lost = false);

  // Consumer side: up to `max` uncommitted events, waiting up to `wait_ms`.
  // The events stay in the log until Commit; Rewind redelivers them (what a
  // consumer crash before commit looks like).
  std::vector<v1::OrderPlaced> Poll(size_t max, int64_t wait_ms) override;
  Status Commit() override;
  void Rewind();
  void Close();

  [[nodiscard]] size_t size() const;
  [[nodiscard]] size_t committed() const;

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<v1::OrderPlaced> log_;
  size_t committed_ = 0;  // offset of the first uncommitted record
  size_t position_ = 0;   // consumer read position
  int fail_next_ = 0;
  PublishOutcome fail_outcome_ = PublishOutcome::kFailed;
  bool fail_lost_ = false;
  bool closed_ = false;
};

class MemoryOrderStore final : public OrderStore {
 public:
  Status UpsertDeal(const DealRecord& deal) override;
  Result<std::vector<DealRecord>> LoadDeals() override;
  Result<std::vector<OrderRecord>> OrdersForUser(const std::string& user_id) override;
  Result<std::set<std::string>> KnownOrderIds(const std::vector<std::string>& ids) override;
  Result<BatchResult> ApplyBatch(const std::vector<v1::OrderPlaced>& events) override;
  Result<DealRecord> ResetDeal(const std::string& deal_id, int32_t quantity, int64_t starts_at_ms,
                               int64_t ends_at_ms) override;

  void FailNextBatches(int n);
  [[nodiscard]] size_t ConfirmedCount(const std::string& deal_id, int64_t generation) const;
  [[nodiscard]] std::atomic<uint64_t>& load_calls() { return load_calls_; }

 private:
  mutable std::mutex mu_;
  std::map<std::string, DealRecord> deals_;
  std::map<std::string, OrderRecord> orders_;  // by order id
  int fail_batches_ = 0;
  std::atomic<uint64_t> load_calls_{0};
};

class MemoryNotifier final : public OrderNotifier {
 public:
  void OrderUpdated(const v1::Order& order) override;
  [[nodiscard]] std::vector<v1::Order> Drain();

 private:
  std::mutex mu_;
  std::vector<v1::Order> sent_;
};

}  // namespace cartwheel::deal
