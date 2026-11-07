// The lightning-deal pipeline against real Redis, Postgres and Kafka.
// Skipped unless CW_TEST_REDIS / CW_TEST_POSTGRES / CW_TEST_KAFKA are set
// (scripts/local_infra.sh env prints them; CI starts the services).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <set>
#include <thread>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <pqxx/pqxx>
#include <prometheus/registry.h>
#include <sw/redis++/redis++.h>

#include "services/deal/deal_catalog.h"
#include "services/deal/deal_engine.h"
#include "services/deal/kafka_log.h"
#include "services/deal/order_processor.h"
#include "services/deal/pg_store.h"
#include "services/deal/redis_gate.h"

namespace cartwheel::deal {
namespace {

std::string Env(const char* name) {
  // Read before any test thread starts; nothing in the process sets env vars.
  const char* v = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
  return v == nullptr ? "" : v;
}

std::string RunId() {
  return fmt::format("{:x}", std::chrono::steady_clock::now().time_since_epoch().count());
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

DealRecord Deal(const std::string& id, int32_t quantity) {
  const int64_t now = NowMs();
  return {.deal_id = id,
          .sku = "CW-TEST",
          .title = "test " + id,
          .price_cents = 999,
          .list_price_cents = 1999,
          .quantity = quantity,
          .starts_at_ms = now - 1000,
          .ends_at_ms = now + int64_t{10} * 60'000};
}

std::shared_ptr<sw::redis::Redis> TestRedis() {
  auto opts = ParseRedisUrl(Env("CW_TEST_REDIS"));
  EXPECT_TRUE(opts.ok());
  opts->pool_size = 32;
  opts->socket_timeout_ms = 2000;
  opts->pool_wait_ms = 2000;
  return ConnectRedis(*opts);
}

#define REQUIRE_ENV(var)                                     \
  if (Env(var).empty()) GTEST_SKIP() << (var) << " not set"; \
  static_assert(true)

TEST(RedisGate, ReservationIsAtomicUnderContention) {
  REQUIRE_ENV("CW_TEST_REDIS");
  auto redis = TestRedis();
  RedisInventoryGate gate(redis);
  const std::string deal = "rg-" + RunId();
  ASSERT_TRUE(gate.Arm(deal, 1, 200, false).ok());

  std::atomic<int> reserved{0};
  std::atomic<int> sold_out{0};
  std::vector<std::thread> threads;
  threads.reserve(32);
  for (int t = 0; t < 32; ++t) {
    threads.emplace_back([&, t] {
      for (int u = t; u < 2000; u += 32) {
        auto r = gate.Reserve(deal, 1, fmt::format("u{}", u), fmt::format("o{}", u));
        ASSERT_TRUE(r.ok()) << r.status();
        if (r->kind == Reservation::Kind::kReserved) reserved.fetch_add(1);
        if (r->kind == Reservation::Kind::kSoldOut) sold_out.fetch_add(1);
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(reserved.load(), 200);
  EXPECT_EQ(sold_out.load(), 1800);
  EXPECT_EQ(*gate.Remaining(deal, 1), 0);
  // holds = 200 users + the armed marker
  EXPECT_EQ(redis->hlen(RedisInventoryGate::HoldsKey(deal, 1)), 201);
}

TEST(RedisGate, HoldsListsEveryReservationButNotTheMarker) {
  REQUIRE_ENV("CW_TEST_REDIS");
  RedisInventoryGate gate(TestRedis());
  const std::string deal = "rh-" + RunId();
  ASSERT_TRUE(gate.Arm(deal, 1, 5, false).ok());
  ASSERT_EQ(gate.Reserve(deal, 1, "ada", "o1")->kind, Reservation::Kind::kReserved);
  ASSERT_EQ(gate.Reserve(deal, 1, "grace", "o2")->kind, Reservation::Kind::kReserved);
  auto holds = gate.Holds(deal, 1);
  ASSERT_TRUE(holds.ok()) << holds.status();
  std::sort(holds->begin(), holds->end());
  ASSERT_EQ(holds->size(), 2U);
  EXPECT_EQ((*holds)[0], std::make_pair(std::string("ada"), std::string("o1")));
  EXPECT_EQ((*holds)[1], std::make_pair(std::string("grace"), std::string("o2")));
  EXPECT_TRUE(gate.Holds(deal, 9)->empty());
}

TEST(RedisGate, SurvivesScriptCacheFlushAndReportsUnarmed) {
  REQUIRE_ENV("CW_TEST_REDIS");
  auto redis = TestRedis();
  RedisInventoryGate gate(redis);
  const std::string deal = "rs-" + RunId();
  ASSERT_TRUE(gate.Arm(deal, 1, 5, false).ok());
  ASSERT_EQ(gate.Reserve(deal, 1, "a", "o1")->kind, Reservation::Kind::kReserved);
  redis->command("SCRIPT", "FLUSH");  // what a Redis restart does to EVALSHA
  EXPECT_EQ(gate.Reserve(deal, 1, "b", "o2")->kind, Reservation::Kind::kReserved);
  EXPECT_EQ(gate.Reserve(deal, 1, "a", "o3")->kind, Reservation::Kind::kAlreadyClaimed);
  EXPECT_EQ(gate.Reserve(deal, 2, "a", "o4")->kind, Reservation::Kind::kNotArmed);
  // only_if_absent keeps a live deal's stock.
  ASSERT_TRUE(gate.Arm(deal, 1, 100, /*only_if_absent=*/true).ok());
  EXPECT_EQ(*gate.Remaining(deal, 1), 3);
}

class PgTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (Env("CW_TEST_POSTGRES").empty()) GTEST_SKIP() << "CW_TEST_POSTGRES not set";
    pool_ = std::make_shared<PgPool>(Env("CW_TEST_POSTGRES"), 8, 2000);
    store_ = std::make_unique<PgOrderStore>(pool_);
    ASSERT_TRUE(store_->Migrate().ok());
  }
  std::shared_ptr<PgPool> pool_;
  std::unique_ptr<PgOrderStore> store_;
};

v1::OrderPlaced Event(const std::string& order, const std::string& deal, const std::string& user,
                      int64_t gen = 1) {
  v1::OrderPlaced e;
  e.set_order_id(order);
  e.set_deal_id(deal);
  e.set_user_id(user);
  e.set_price_cents(999);
  e.set_generation(gen);
  e.set_reserved_at_ms(NowMs());
  return e;
}

TEST_F(PgTest, ApplyBatchIsIdempotentAndNeverOversells) {
  const std::string deal = "pg-" + RunId();
  ASSERT_TRUE(store_->UpsertDeal(Deal(deal, 3)).ok());
  std::vector<v1::OrderPlaced> batch;
  batch.reserve(6);
  for (int i = 0; i < 5; ++i)
    batch.push_back(Event(fmt::format("{}-o{}", deal, i), deal, fmt::format("u{}", i)));
  batch.push_back(Event(deal + "-dup", deal, "u0"));  // second order for u0
  auto r = store_->ApplyBatch(batch);
  ASSERT_TRUE(r.ok()) << r.status();
  EXPECT_EQ(r->confirmed.size(), 3U);
  EXPECT_EQ(r->rejected.size(), 3U);
  EXPECT_TRUE(r->duplicates.empty());

  // Redelivery of the same batch: everything is a duplicate, nothing changes.
  auto again = store_->ApplyBatch(batch);
  ASSERT_TRUE(again.ok());
  EXPECT_TRUE(again->confirmed.empty());
  EXPECT_EQ(again->duplicates.size(), batch.size());

  auto deals = store_->LoadDeals();
  ASSERT_TRUE(deals.ok());
  for (const auto& d : *deals) {
    if (d.deal_id == deal) EXPECT_EQ(d.sold, 3);
  }
  auto known = store_->KnownOrderIds({deal + "-o0", deal + "-dup", "no-such-order"});
  ASSERT_TRUE(known.ok());
  EXPECT_EQ(known->size(), 2U);  // confirmed and rejected both count
  auto orders = store_->OrdersForUser("u0");
  ASSERT_TRUE(orders.ok());
  int u0_confirmed = 0;
  for (const auto& o : *orders) {
    if (o.deal_id == deal && o.confirmed) ++u0_confirmed;
  }
  EXPECT_EQ(u0_confirmed, 1);
}

TEST_F(PgTest, ConcurrentWorkersRespectTheRowLock) {
  const std::string deal = "pgc-" + RunId();
  ASSERT_TRUE(store_->UpsertDeal(Deal(deal, 50)).ok());
  // 8 workers each commit batches of 25 distinct users: 200 reservations
  // race for 50 units.
  std::vector<std::thread> workers;
  workers.reserve(8);
  std::atomic<int> confirmed{0};
  for (int w = 0; w < 8; ++w) {
    workers.emplace_back([&, w] {
      std::vector<v1::OrderPlaced> batch;
      batch.reserve(25);
      for (int i = 0; i < 25; ++i) {
        batch.push_back(
            Event(fmt::format("{}-w{}-{}", deal, w, i), deal, fmt::format("w{}u{}", w, i)));
      }
      for (int attempt = 0; attempt < 5; ++attempt) {
        auto r = store_->ApplyBatch(batch);
        if (r.ok()) {
          confirmed.fetch_add(static_cast<int>(r->confirmed.size()));
          return;
        }
      }
      ADD_FAILURE() << "batch kept failing";
    });
  }
  for (auto& t : workers) t.join();
  EXPECT_EQ(confirmed.load(), 50);
  auto deals = store_->LoadDeals();
  for (const auto& d : *deals) {
    if (d.deal_id == deal) EXPECT_EQ(d.sold, 50);
  }
}

TEST_F(PgTest, CheckConstraintIsTheLastLineOfDefence) {
  const std::string deal = "pgk-" + RunId();
  ASSERT_TRUE(store_->UpsertDeal(Deal(deal, 1)).ok());
  pqxx::connection c(Env("CW_TEST_POSTGRES"));
  pqxx::work tx(c);
  EXPECT_THROW(tx.exec("UPDATE deals SET sold = 2 WHERE deal_id = $1", pqxx::params{tx, deal}),
               pqxx::check_violation);
}

// The whole pipeline: engine -> Redis gate -> Kafka -> 3 workers -> Postgres.
TEST(DealPipelineInfra, SpikeSellsExactlyTheQuantity) {
  for (const char* v : {"CW_TEST_REDIS", "CW_TEST_POSTGRES", "CW_TEST_KAFKA"}) {
    if (Env(v).empty()) GTEST_SKIP() << v << " not set";
  }
  constexpr int kQuantity = 300;
  constexpr int kUsers = 1500;
  const std::string run = RunId();
  const std::string deal = "spike-" + run;
  prometheus::Registry registry;

  auto pool = std::make_shared<PgPool>(Env("CW_TEST_POSTGRES"), 8, 2000);
  PgOrderStore store(pool);
  ASSERT_TRUE(store.Migrate().ok());
  ASSERT_TRUE(store.UpsertDeal(Deal(deal, kQuantity)).ok());
  RedisInventoryGate gate(TestRedis());

  KafkaOptions kopts{
      .brokers = Env("CW_TEST_KAFKA"), .topic = "deal-orders-" + run, .partitions = 6};
  ASSERT_TRUE(EnsureTopic(kopts).ok());
  auto log = KafkaOrderLog::Create(kopts);
  ASSERT_TRUE(log.ok()) << log.status();

  DealCatalog catalog(&store, SystemClock::Default(), 1000);
  DealEngine engine(&catalog, &gate, log->get(), &store, SystemClock::Default(), registry);
  ASSERT_TRUE(engine.ArmAll().ok());

  // Three workers in one consumer group share the partitions.
  std::atomic<bool> stop{false};
  std::vector<std::unique_ptr<KafkaOrderSource>> sources;
  std::vector<std::thread> workers;
  OrderProcessor processor(&store, nullptr, registry);
  sources.reserve(3);
  workers.reserve(3);
  for (int w = 0; w < 3; ++w) {
    auto src = KafkaOrderSource::Create(kopts, "test-" + run);
    ASSERT_TRUE(src.ok()) << src.status();
    sources.push_back(std::move(src).value());
  }
  for (auto& src : sources) {
    workers.emplace_back([&, s = src.get()] { RunWorkerLoop(s, &processor, &stop); });
  }

  std::atomic<int> reserved{0};
  std::atomic<int> sold_out{0};
  std::atomic<int> errors{0};
  std::vector<std::thread> buyers;
  buyers.reserve(32);
  for (int t = 0; t < 32; ++t) {
    buyers.emplace_back([&, t] {
      for (int u = t; u < kUsers; u += 32) {
        // A real client: a retryable error (the log did not confirm in
        // time) is retried; the retry finds the held unit as ALREADY_CLAIMED.
        for (int attempt = 0; attempt < 6; ++attempt) {
          std::promise<Result<v1::ClaimResponse>> p;
          engine.Claim(deal, fmt::format("buyer{}", u),
                       [&](Result<v1::ClaimResponse> r) { p.set_value(std::move(r)); });
          auto r = p.get_future().get();
          if (!r.ok()) {
            if (r.status().retryable() && attempt < 5) {
              std::this_thread::sleep_for(std::chrono::milliseconds(200 << attempt));
              continue;
            }
            errors.fetch_add(1);
          } else if (r->outcome() == v1::ClaimResponse::OUTCOME_RESERVED ||
                     r->outcome() == v1::ClaimResponse::OUTCOME_ALREADY_CLAIMED) {
            reserved.fetch_add(1);
          } else if (r->outcome() == v1::ClaimResponse::OUTCOME_SOLD_OUT) {
            sold_out.fetch_add(1);
          }
          break;
        }
      }
    });
  }
  for (auto& b : buyers) b.join();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(reserved.load(), kQuantity);
  EXPECT_EQ(sold_out.load(), kUsers - kQuantity);

  // Wait for the workers to commit everything. Three consumers joining a new
  // group rebalance several times first, which is slow under TSan.
  int sold = 0;
  for (int i = 0; i < 1200 && sold < kQuantity; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    catalog.Invalidate();
    auto d = catalog.Find(deal);
    if (d.ok()) sold = d->sold;
  }
  stop = true;
  for (auto& w : workers) w.join();

  // The invariant from the plan: committed orders == initial - remaining,
  // never above the quantity, at most one per user.
  EXPECT_EQ(sold, kQuantity);
  EXPECT_EQ(kQuantity - *gate.Remaining(deal, 1), sold);
  pqxx::connection c(Env("CW_TEST_POSTGRES"));
  pqxx::read_transaction tx(c);
  auto row = tx.exec(
                   "SELECT count(*), count(DISTINCT user_id) FROM orders "
                   "WHERE deal_id = $1 AND status = 'CONFIRMED'",
                   pqxx::params{tx, deal})
                 .one_row();
  EXPECT_EQ(row[0].as<int>(), kQuantity);
  EXPECT_EQ(row[1].as<int>(), kQuantity);
}

}  // namespace
}  // namespace cartwheel::deal
