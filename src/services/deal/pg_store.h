#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "services/deal/deal_ports.h"

namespace pqxx {
class connection;
}

namespace cartwheel::deal {

// Fixed-size pool of libpqxx connections (a connection is not thread-safe).
// Acquire waits a bounded time, then fails with RESOURCE_EXHAUSTED rather
// than queueing requests behind a saturated database.
class PgPool {
 public:
  PgPool(std::string url, size_t size, int64_t acquire_timeout_ms);
  ~PgPool();

  class Lease {
   public:
    Lease(PgPool* pool, std::unique_ptr<pqxx::connection> conn);
    ~Lease();
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&&) = delete;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    pqxx::connection& operator*() { return *conn_; }
    pqxx::connection* operator->() { return conn_.get(); }
    // The connection failed mid-use; drop it instead of returning it.
    void Discard();

   private:
    PgPool* pool_;
    std::unique_ptr<pqxx::connection> conn_;
  };

  Result<Lease> Acquire();

 private:
  void Return(std::unique_ptr<pqxx::connection> conn);

  const std::string url_;
  const int64_t acquire_timeout_ms_;
  std::mutex mu_;
  std::condition_variable available_;
  std::vector<std::unique_ptr<pqxx::connection>> idle_;
  size_t free_slots_;  // connections that may still be opened
};

// Postgres is the source of truth for deals and orders.
//
//   deals(deal_id PK, ..., quantity, sold, generation, CHECK (sold <= quantity))
//   orders(order_id PK, deal_id, generation, user_id, status, reason, ...)
//     UNIQUE (deal_id, generation, user_id) WHERE status = 'CONFIRMED'
//
// ApplyBatch is one transaction: lock the batch's deal rows (SELECT ... FOR
// UPDATE, in id order so concurrent workers cannot deadlock), read which
// order ids and users already exist, decide with DecideBatch, insert with one
// unnest() statement, update the sold counters, COMMIT. The CHECK and the
// partial unique index stay as constraints of last resort.
class PgOrderStore final : public OrderStore {
 public:
  explicit PgOrderStore(std::shared_ptr<PgPool> pool);

  // Idempotent schema setup (CREATE ... IF NOT EXISTS).
  Status Migrate();

  Status UpsertDeal(const DealRecord& deal) override;
  Result<std::vector<DealRecord>> LoadDeals() override;
  Result<std::vector<OrderRecord>> OrdersForUser(const std::string& user_id) override;
  Result<std::set<std::string>> KnownOrderIds(const std::vector<std::string>& ids) override;
  Result<BatchResult> ApplyBatch(const std::vector<v1::OrderPlaced>& events) override;
  Result<DealRecord> ResetDeal(const std::string& deal_id, int32_t quantity, int64_t starts_at_ms,
                               int64_t ends_at_ms) override;

  // Test helper: removes every deal and order.
  Status TruncateForTest();

  static const char* Schema();

 private:
  template <typename Fn>
  auto WithConnection(const char* what, Fn&& fn) -> decltype(fn(std::declval<pqxx::connection&>()));

  std::shared_ptr<PgPool> pool_;
};

}  // namespace cartwheel::deal
