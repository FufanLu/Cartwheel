#include "services/deal/pg_store.h"

#include <algorithm>
#include <chrono>

#include <fmt/format.h>
#include <pqxx/pqxx>

#include "common/logging.h"
#include "services/deal/batch_rules.h"

namespace cartwheel::deal {
namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS deals (
  deal_id          text PRIMARY KEY,
  sku              text NOT NULL,
  title            text NOT NULL,
  price_cents      bigint NOT NULL CHECK (price_cents >= 0),
  list_price_cents bigint NOT NULL,
  quantity         integer NOT NULL CHECK (quantity >= 0),
  sold             integer NOT NULL DEFAULT 0,
  generation       bigint NOT NULL DEFAULT 1,
  starts_at        timestamptz NOT NULL,
  ends_at          timestamptz NOT NULL,
  -- The last line of defence against overselling, whatever the gate did.
  CONSTRAINT deals_never_oversold CHECK (sold >= 0 AND sold <= quantity)
);

CREATE TABLE IF NOT EXISTS orders (
  order_id     text PRIMARY KEY,
  deal_id      text NOT NULL REFERENCES deals (deal_id),
  generation   bigint NOT NULL,
  user_id      text NOT NULL,
  price_cents  bigint NOT NULL,
  status       text NOT NULL CHECK (status IN ('CONFIRMED', 'REJECTED')),
  reason       text,
  reserved_at  timestamptz NOT NULL,
  committed_at timestamptz NOT NULL DEFAULT now()
);

-- One confirmed unit per customer per deal generation.
CREATE UNIQUE INDEX IF NOT EXISTS orders_one_per_customer
  ON orders (deal_id, generation, user_id) WHERE status = 'CONFIRMED';
CREATE INDEX IF NOT EXISTS orders_by_user ON orders (user_id, reserved_at DESC);
)sql";

constexpr const char* kOrderColumns =
    "order_id, deal_id, user_id, price_cents, generation, "
    "(extract(epoch from reserved_at) * 1000)::bigint, status, coalesce(reason, '')";

constexpr const char* kDealColumns =
    "deal_id, sku, title, price_cents, list_price_cents, quantity, sold, generation, "
    "(extract(epoch from starts_at) * 1000)::bigint, (extract(epoch from ends_at) * 1000)::bigint";

template <typename Row>
OrderRecord ReadOrder(const Row& r) {
  return OrderRecord{.order_id = r[0].template as<std::string>(),
                     .deal_id = r[1].template as<std::string>(),
                     .user_id = r[2].template as<std::string>(),
                     .price_cents = r[3].template as<int64_t>(),
                     .generation = r[4].template as<int64_t>(),
                     .reserved_at_ms = r[5].template as<int64_t>(),
                     .confirmed = r[6].template as<std::string>() == "CONFIRMED",
                     .reason = r[7].template as<std::string>()};
}

template <typename Row>
DealRecord ReadDeal(const Row& r) {
  return DealRecord{.deal_id = r[0].template as<std::string>(),
                    .sku = r[1].template as<std::string>(),
                    .title = r[2].template as<std::string>(),
                    .price_cents = r[3].template as<int64_t>(),
                    .list_price_cents = r[4].template as<int64_t>(),
                    .quantity = r[5].template as<int32_t>(),
                    .sold = r[6].template as<int32_t>(),
                    .generation = r[7].template as<int64_t>(),
                    .starts_at_ms = r[8].template as<int64_t>(),
                    .ends_at_ms = r[9].template as<int64_t>()};
}

}  // namespace

// --- PgPool -------------------------------------------------------------------

PgPool::PgPool(std::string url, size_t size, int64_t acquire_timeout_ms)
    : url_(std::move(url)), acquire_timeout_ms_(acquire_timeout_ms), free_slots_(size) {}

PgPool::~PgPool() = default;

PgPool::Lease::Lease(PgPool* pool, std::unique_ptr<pqxx::connection> conn)
    : pool_(pool), conn_(std::move(conn)) {}

PgPool::Lease::Lease(Lease&& other) noexcept : pool_(other.pool_), conn_(std::move(other.conn_)) {
  other.pool_ = nullptr;
}

void PgPool::Lease::Discard() {
  conn_.reset();
}

PgPool::Lease::~Lease() {
  if (pool_ != nullptr) pool_->Return(std::move(conn_));
}

Result<PgPool::Lease> PgPool::Acquire() {
  std::unique_lock lock(mu_);
  const bool got = available_.wait_for(lock, std::chrono::milliseconds(acquire_timeout_ms_),
                                       [&] { return !idle_.empty() || free_slots_ > 0; });
  if (!got) return Status::ResourceExhausted("postgres pool exhausted");
  if (!idle_.empty()) {
    auto conn = std::move(idle_.back());
    idle_.pop_back();
    return Lease(this, std::move(conn));
  }
  --free_slots_;
  lock.unlock();
  try {
    auto conn = std::make_unique<pqxx::connection>(url_);
    return Lease(this, std::move(conn));
  } catch (const std::exception& e) {
    Return(nullptr);  // give the slot back
    return Status::Unavailable(fmt::format("postgres connect: {}", e.what()));
  }
}

void PgPool::Return(std::unique_ptr<pqxx::connection> conn) {
  {
    std::lock_guard lock(mu_);
    if (conn && conn->is_open()) {
      idle_.push_back(std::move(conn));
    } else {
      ++free_slots_;  // broken or discarded: a fresh one will be opened
    }
  }
  available_.notify_one();
}

// --- PgOrderStore -------------------------------------------------------------

PgOrderStore::PgOrderStore(std::shared_ptr<PgPool> pool) : pool_(std::move(pool)) {}

const char* PgOrderStore::Schema() {
  return kSchema;
}

template <typename Fn>
auto PgOrderStore::WithConnection(const char* what, Fn&& fn)
    -> decltype(fn(std::declval<pqxx::connection&>())) {
  auto lease = pool_->Acquire();
  if (!lease.ok()) return lease.status();
  try {
    return fn(**lease);
  } catch (const pqxx::broken_connection& e) {
    lease->Discard();
    return Status::Unavailable(fmt::format("postgres {}: connection lost: {}", what, e.what()));
  } catch (const pqxx::transaction_rollback& e) {
    // Serialization failure or deadlock: safe to retry the whole batch.
    return Status::Aborted(fmt::format("postgres {}: {}", what, e.what()));
  } catch (const pqxx::sql_error& e) {
    return Status::Internal(fmt::format("postgres {}: {} [{}]", what, e.what(), e.sqlstate()));
  } catch (const std::exception& e) {
    lease->Discard();
    return Status::Internal(fmt::format("postgres {}: {}", what, e.what()));
  }
}

Status PgOrderStore::Migrate() {
  auto r = WithConnection("migrate", [](pqxx::connection& c) -> Result<bool> {
    pqxx::work tx(c);
    // Several services may start at once; serialize their migrations.
    tx.exec("SELECT pg_advisory_xact_lock(7243105)");
    tx.exec(kSchema);
    tx.commit();
    return true;
  });
  return r.status();
}

Status PgOrderStore::UpsertDeal(const DealRecord& d) {
  auto r = WithConnection("upsert deal", [&](pqxx::connection& c) -> Result<bool> {
    pqxx::work tx(c);
    tx.exec(
        "INSERT INTO deals (deal_id, sku, title, price_cents, list_price_cents, quantity, sold, "
        "generation, starts_at, ends_at) VALUES ($1, $2, $3, $4, $5, $6, 0, $7, "
        "to_timestamp($8 / 1000.0), to_timestamp($9 / 1000.0)) "
        "ON CONFLICT (deal_id) DO UPDATE SET sku = EXCLUDED.sku, title = EXCLUDED.title, "
        "price_cents = EXCLUDED.price_cents, list_price_cents = EXCLUDED.list_price_cents",
        pqxx::params{tx, d.deal_id, d.sku, d.title, d.price_cents, d.list_price_cents, d.quantity,
                     d.generation, d.starts_at_ms, d.ends_at_ms});
    tx.commit();
    return true;
  });
  return r.status();
}

Result<std::vector<DealRecord>> PgOrderStore::LoadDeals() {
  return WithConnection("load deals", [](pqxx::connection& c) -> Result<std::vector<DealRecord>> {
    pqxx::read_transaction tx(c);
    std::vector<DealRecord> out;
    for (const auto& row :
         tx.exec(fmt::format("SELECT {} FROM deals ORDER BY starts_at, deal_id", kDealColumns))) {
      out.push_back(ReadDeal(row));
    }
    return out;
  });
}

Result<std::vector<OrderRecord>> PgOrderStore::OrdersForUser(const std::string& user_id) {
  return WithConnection(
      "orders for user", [&](pqxx::connection& c) -> Result<std::vector<OrderRecord>> {
        pqxx::read_transaction tx(c);
        std::vector<OrderRecord> out;
        for (const auto& row : tx.exec(fmt::format("SELECT {} FROM orders WHERE user_id = $1 "
                                                   "ORDER BY reserved_at DESC LIMIT 100",
                                                   kOrderColumns),
                                       pqxx::params{tx, user_id})) {
          out.push_back(ReadOrder(row));
        }
        return out;
      });
}

Result<std::set<std::string>> PgOrderStore::KnownOrderIds(const std::vector<std::string>& ids) {
  if (ids.empty()) return std::set<std::string>();
  return WithConnection(
      "known order ids", [&](pqxx::connection& c) -> Result<std::set<std::string>> {
        pqxx::read_transaction tx(c);
        std::set<std::string> out;
        for (const auto& row : tx.exec("SELECT order_id FROM orders WHERE order_id = ANY($1)",
                                       pqxx::params{tx, ids})) {
          out.insert(row[0].as<std::string>());
        }
        return out;
      });
}

Result<BatchResult> PgOrderStore::ApplyBatch(const std::vector<v1::OrderPlaced>& events) {
  if (events.empty()) return BatchResult{};
  return WithConnection("apply batch", [&](pqxx::connection& c) -> Result<BatchResult> {
    std::vector<std::string> deal_ids;
    std::vector<std::string> order_ids;
    std::vector<std::string> user_ids;
    for (const auto& e : events) {
      deal_ids.push_back(e.deal_id());
      order_ids.push_back(e.order_id());
      user_ids.push_back(e.user_id());
    }
    std::sort(deal_ids.begin(), deal_ids.end());
    deal_ids.erase(std::unique(deal_ids.begin(), deal_ids.end()), deal_ids.end());

    pqxx::work tx(c);
    // 1. Lock the deals in a fixed order: two workers holding batches for
    //    the same deals queue on the first lock instead of deadlocking.
    std::map<std::string, DealState> deals;
    for (const auto& row : tx.exec("SELECT deal_id, generation, quantity, sold FROM deals "
                                   "WHERE deal_id = ANY($1) ORDER BY deal_id FOR UPDATE",
                                   pqxx::params{tx, deal_ids})) {
      deals.emplace(row[0].as<std::string>(), DealState{.generation = row[1].as<int64_t>(),
                                                        .quantity = row[2].as<int32_t>(),
                                                        .sold = row[3].as<int32_t>()});
    }
    // 2. What is already committed: redelivered order ids, and users who
    //    already hold a confirmed unit.
    std::map<std::string, OrderRecord> existing;
    for (const auto& row :
         tx.exec(fmt::format("SELECT {} FROM orders WHERE order_id = ANY($1)", kOrderColumns),
                 pqxx::params{tx, order_ids})) {
      auto o = ReadOrder(row);
      existing.emplace(o.order_id, std::move(o));
    }
    std::set<UserKey> confirmed_users;
    for (const auto& row :
         tx.exec("SELECT deal_id, generation, user_id FROM orders WHERE status = 'CONFIRMED' "
                 "AND deal_id = ANY($1) AND user_id = ANY($2)",
                 pqxx::params{tx, deal_ids, user_ids})) {
      confirmed_users.emplace(row[0].as<std::string>(), row[1].as<int64_t>(),
                              row[2].as<std::string>());
    }

    // 3. Decide, exactly like the in-memory store.
    std::map<std::string, DealState> before = deals;
    BatchResult result = DecideBatch(events, existing, &deals, &confirmed_users);

    // 4. One INSERT for the whole batch.
    std::vector<std::string> ins_order;
    std::vector<std::string> ins_deal;
    std::vector<int64_t> ins_gen;
    std::vector<std::string> ins_user;
    std::vector<int64_t> ins_price;
    std::vector<std::string> ins_status;
    std::vector<std::string> ins_reason;
    std::vector<int64_t> ins_reserved;
    for (const auto* group : {&result.confirmed, &result.rejected}) {
      for (const auto& o : *group) {
        ins_order.push_back(o.order_id);
        ins_deal.push_back(o.deal_id);
        ins_gen.push_back(o.generation);
        ins_user.push_back(o.user_id);
        ins_price.push_back(o.price_cents);
        ins_status.emplace_back(o.confirmed ? "CONFIRMED" : "REJECTED");
        ins_reason.push_back(o.reason);
        ins_reserved.push_back(o.reserved_at_ms);
      }
    }
    if (!ins_order.empty()) {
      // Rejections for an unknown deal cannot reference `deals`; they are
      // reported to the caller but not stored.
      tx.exec(
          "INSERT INTO orders (order_id, deal_id, generation, user_id, price_cents, status, "
          "reason, reserved_at) "
          "SELECT o, d, g, u, p, s, NULLIF(r, ''), to_timestamp(t / 1000.0) "
          "FROM unnest($1::text[], $2::text[], $3::bigint[], $4::text[], $5::bigint[], "
          "$6::text[], $7::text[], $8::bigint[]) AS x(o, d, g, u, p, s, r, t) "
          "WHERE r <> 'unknown_deal' "
          "ON CONFLICT (order_id) DO NOTHING",
          pqxx::params{tx, ins_order, ins_deal, ins_gen, ins_user, ins_price, ins_status,
                       ins_reason, ins_reserved});
    }
    // 5. Counters.
    for (const auto& [id, st] : deals) {
      if (st.sold != before.at(id).sold) {
        tx.exec("UPDATE deals SET sold = $2 WHERE deal_id = $1", pqxx::params{tx, id, st.sold});
      }
    }
    tx.commit();
    return result;
  });
}

Result<DealRecord> PgOrderStore::ResetDeal(const std::string& deal_id, int32_t quantity,
                                           int64_t starts_at_ms, int64_t ends_at_ms) {
  return WithConnection("reset deal", [&](pqxx::connection& c) -> Result<DealRecord> {
    pqxx::work tx(c);
    auto r = tx.exec(fmt::format("UPDATE deals SET generation = generation + 1, sold = 0, "
                                 "quantity = $2, starts_at = to_timestamp($3 / 1000.0), "
                                 "ends_at = to_timestamp($4 / 1000.0) WHERE deal_id = $1 "
                                 "RETURNING {}",
                                 kDealColumns),
                     pqxx::params{tx, deal_id, quantity, starts_at_ms, ends_at_ms});
    if (r.empty()) return Status::NotFound("deal " + deal_id);
    DealRecord d = ReadDeal(r[0]);
    tx.commit();
    return d;
  });
}

Status PgOrderStore::TruncateForTest() {
  auto r = WithConnection("truncate", [](pqxx::connection& c) -> Result<bool> {
    pqxx::work tx(c);
    tx.exec("TRUNCATE orders, deals");
    tx.commit();
    return true;
  });
  return r.status();
}

}  // namespace cartwheel::deal
