#pragma once

// The lightning-deal pipeline talks to three external systems: Redis (the
// inventory gate), Redpanda/Kafka (the order log) and Postgres (the source of
// truth). Each sits behind one of the interfaces below. Production wires the
// real clients (redis_gate.h, kafka_log.h, pg_store.h); unit tests and the
// `memory` backend wire memory_ports.h, so every rule in the pipeline is
// tested without infrastructure.

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "cartwheel/deal/v1/deal.pb.h"
#include "common/status.h"

namespace cartwheel::deal {

// Catalog row (Postgres `deals`).
struct DealRecord {
  std::string deal_id;
  std::string sku;
  std::string title;
  int64_t price_cents = 0;
  int64_t list_price_cents = 0;
  int32_t quantity = 0;
  int32_t sold = 0;
  int64_t generation = 1;
  int64_t starts_at_ms = 0;
  int64_t ends_at_ms = 0;
};

// Committed order row (Postgres `orders`).
struct OrderRecord {
  std::string order_id;
  std::string deal_id;
  std::string user_id;
  int64_t price_cents = 0;
  int64_t generation = 0;
  int64_t reserved_at_ms = 0;
  bool confirmed = false;
  std::string reason;  // why it was rejected
};

// --- Inventory gate (Redis) -------------------------------------------------

struct Reservation {
  enum class Kind : uint8_t {
    kReserved,        // took a unit; `order_id` is the caller's new id
    kAlreadyClaimed,  // user already holds a unit; `order_id` is theirs
    kSoldOut,
    kNotArmed,  // no stock key for this generation (never armed, or expired)
  };
  Kind kind = Kind::kSoldOut;
  std::string order_id;
  int64_t remaining = 0;
};

class InventoryGate {
 public:
  virtual ~InventoryGate() = default;

  // Atomically: if `user` already holds a unit of (deal, generation), return
  // it; otherwise take one unit if any is left and record user -> order_id.
  virtual Result<Reservation> Reserve(const std::string& deal_id, int64_t generation,
                                      const std::string& user_id, const std::string& order_id) = 0;

  // Every (user, order id) holding a unit of (deal, generation). The hold
  // sweeper compares these with committed orders. A hold is never released:
  // it is a promise, and the sweeper re-publishes until it is an order.
  virtual Result<std::vector<std::pair<std::string, std::string>>> Holds(const std::string& deal_id,
                                                                         int64_t generation) = 0;

  // Sets the stock for a generation. With only_if_absent, an existing stock
  // key is left alone (restarts must not refill a deal in progress).
  virtual Status Arm(const std::string& deal_id, int64_t generation, int64_t quantity,
                     bool only_if_absent) = 0;

  virtual Result<int64_t> Remaining(const std::string& deal_id, int64_t generation) = 0;

  // The order id `user` holds for (deal, generation), if any.
  virtual Result<std::optional<std::string>> HeldOrder(const std::string& deal_id,
                                                       int64_t generation,
                                                       const std::string& user_id) = 0;
};

// --- Order log (Redpanda / Kafka) --------------------------------------------

enum class PublishOutcome : uint8_t {
  kDelivered,  // acknowledged by all in-sync replicas
  kFailed,     // definitely not written
  kUnknown,    // timed out after sending: it may or may not be in the log
};

class OrderLog {
 public:
  using Done = std::function<void(PublishOutcome, const std::string& error)>;
  virtual ~OrderLog() = default;
  // Asynchronous; `done` runs exactly once, on a producer thread.
  virtual void Publish(const v1::OrderPlaced& event, Done done) = 0;
};

// Consumer side of the log (order-worker). At-least-once: records polled but
// not committed are delivered again after a restart or rebalance.
class OrderSource {
 public:
  virtual ~OrderSource() = default;
  virtual std::vector<v1::OrderPlaced> Poll(size_t max, int64_t wait_ms) = 0;
  // Marks everything polled so far as processed.
  virtual Status Commit() = 0;
};

// --- Order store (Postgres) --------------------------------------------------

struct BatchResult {
  std::vector<OrderRecord> confirmed;
  std::vector<OrderRecord> rejected;
  // Already committed: a redelivery after a crash. Returned with their stored
  // outcome so the caller can re-send notifications (at-least-once).
  std::vector<OrderRecord> duplicates;
};

class OrderStore {
 public:
  virtual ~OrderStore() = default;

  virtual Status UpsertDeal(const DealRecord& deal) = 0;
  virtual Result<std::vector<DealRecord>> LoadDeals() = 0;
  virtual Result<std::vector<OrderRecord>> OrdersForUser(const std::string& user_id) = 0;
  // Which of these order ids are stored (confirmed or rejected).
  virtual Result<std::set<std::string>> KnownOrderIds(const std::vector<std::string>& ids) = 0;

  // Commits a batch of reservations in one transaction. Under a row lock on
  // each deal it re-checks generation, one-per-user and quantity, so the
  // database alone guarantees sold <= quantity whatever the gate did.
  // Idempotent: an order id already committed is counted as a duplicate.
  virtual Result<BatchResult> ApplyBatch(const std::vector<v1::OrderPlaced>& events) = 0;

  // New generation: sold = 0, new window. Old orders stay for the record.
  virtual Result<DealRecord> ResetDeal(const std::string& deal_id, int32_t quantity,
                                       int64_t starts_at_ms, int64_t ends_at_ms) = 0;
};

// --- Notifications (Redis pub/sub -> gateway WebSocket) ----------------------

class OrderNotifier {
 public:
  virtual ~OrderNotifier() = default;
  // Best effort: clients that miss a push still see the order via ListOrders.
  virtual void OrderUpdated(const v1::Order& order) = 0;
};

// Shared conversions.
[[nodiscard]] v1::Deal ToProto(const DealRecord& d, int64_t remaining);
[[nodiscard]] v1::Order ToProto(const OrderRecord& o);
[[nodiscard]] std::string NotificationChannel(const std::string& user_id);

}  // namespace cartwheel::deal
