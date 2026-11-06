// The lightning-deal pipeline on in-memory ports: claims, the gate,
// compensation, the worker's commit rules and redelivery.

#include <atomic>
#include <future>
#include <set>
#include <thread>

#include <gtest/gtest.h>
#include <prometheus/registry.h>

#include "services/deal/batch_rules.h"
#include "services/deal/deal_catalog.h"
#include "services/deal/deal_engine.h"
#include "services/deal/memory_ports.h"
#include "services/deal/order_processor.h"

namespace cartwheel::deal {
namespace {

constexpr int64_t kStart = 1'800'000'000'000;  // unix millis

DealRecord Deal(const std::string& id, int32_t quantity) {
  return {.deal_id = id,
          .sku = "CW-1",
          .title = id,
          .price_cents = 1000,
          .list_price_cents = 2000,
          .quantity = quantity,
          .starts_at_ms = kStart,
          .ends_at_ms = kStart + 60'000};
}

class DealPipeline : public ::testing::Test {
 protected:
  DealPipeline()
      : clock_(kStart * 1'000'000),
        catalog_(&store_, &clock_, 1000),
        engine_(&catalog_, &gate_, &log_, &store_, &clock_, registry_),
        processor_(&store_, &notifier_, registry_) {}

  void AddDeal(const DealRecord& d) {
    ASSERT_TRUE(store_.UpsertDeal(d).ok());
    catalog_.Invalidate();
    ASSERT_TRUE(engine_.ArmAll().ok());
  }

  Result<v1::ClaimResponse> ClaimSync(const std::string& deal, const std::string& user) {
    std::promise<Result<v1::ClaimResponse>> p;
    engine_.Claim(deal, user, [&](Result<v1::ClaimResponse> r) { p.set_value(std::move(r)); });
    return p.get_future().get();
  }

  // Drains the log into the store, like order-worker.
  void RunWorker() {
    for (;;) {
      auto batch = log_.Poll(100, 0);
      if (batch.empty()) return;
      ASSERT_TRUE(processor_.Process(batch).ok());
      ASSERT_TRUE(log_.Commit().ok());
    }
  }

  prometheus::Registry registry_;
  ManualClock clock_;
  MemoryOrderStore store_;
  MemoryInventoryGate gate_;
  MemoryOrderLog log_;
  MemoryNotifier notifier_;
  DealCatalog catalog_;
  DealEngine engine_;
  OrderProcessor processor_;
};

TEST_F(DealPipeline, ReserveThenCommit) {
  AddDeal(Deal("d1", 2));
  auto r = ClaimSync("d1", "ada");
  ASSERT_TRUE(r.ok()) << r.status();
  EXPECT_EQ(r->outcome(), v1::ClaimResponse::OUTCOME_RESERVED);
  EXPECT_EQ(r->remaining(), 1);
  EXPECT_TRUE(r->order_id().starts_with("ord_"));

  // Pending until the worker commits it.
  auto orders = engine_.ListOrders("ada");
  ASSERT_TRUE(orders.ok());
  ASSERT_EQ(orders->orders_size(), 1);
  EXPECT_EQ(orders->orders(0).status(), v1::Order::STATUS_PENDING);

  RunWorker();
  orders = engine_.ListOrders("ada");
  ASSERT_EQ(orders->orders_size(), 1);
  EXPECT_EQ(orders->orders(0).status(), v1::Order::STATUS_CONFIRMED);
  EXPECT_EQ(orders->orders(0).order_id(), r->order_id());
  auto sent = notifier_.Drain();
  ASSERT_EQ(sent.size(), 1U);
  EXPECT_EQ(sent[0].status(), v1::Order::STATUS_CONFIRMED);
}

TEST_F(DealPipeline, RetryReturnsTheSameOrder) {
  AddDeal(Deal("d1", 5));
  auto first = ClaimSync("d1", "ada");
  auto again = ClaimSync("d1", "ada");
  ASSERT_TRUE(first.ok() && again.ok());
  EXPECT_EQ(again->outcome(), v1::ClaimResponse::OUTCOME_ALREADY_CLAIMED);
  EXPECT_EQ(again->order_id(), first->order_id());
  EXPECT_EQ(log_.size(), 1U);  // one record, not two
}

TEST_F(DealPipeline, WindowIsEnforced) {
  auto d = Deal("d1", 5);
  d.starts_at_ms = kStart + 1000;
  AddDeal(d);
  EXPECT_EQ(ClaimSync("d1", "ada")->outcome(), v1::ClaimResponse::OUTCOME_NOT_STARTED);
  clock_.AdvanceMillis(1000);
  EXPECT_EQ(ClaimSync("d1", "ada")->outcome(), v1::ClaimResponse::OUTCOME_RESERVED);
  clock_.AdvanceMillis(60'000);
  EXPECT_EQ(ClaimSync("d1", "grace")->outcome(), v1::ClaimResponse::OUTCOME_ENDED);
  EXPECT_EQ(ClaimSync("nope", "ada").status().code(), StatusCode::kNotFound);
}

TEST_F(DealPipeline, ConcurrentClaimsNeverOversell) {
  constexpr int kQuantity = 100;
  constexpr int kUsers = 1000;
  AddDeal(Deal("hot", kQuantity));
  std::atomic<int> reserved{0};
  std::atomic<int> sold_out{0};
  std::vector<std::thread> threads;
  threads.reserve(16);
  for (int t = 0; t < 16; ++t) {
    threads.emplace_back([&, t] {
      for (int u = t; u < kUsers; u += 16) {
        auto r = ClaimSync("hot", "user-" + std::to_string(u));
        if (!r.ok()) continue;
        if (r->outcome() == v1::ClaimResponse::OUTCOME_RESERVED) reserved.fetch_add(1);
        if (r->outcome() == v1::ClaimResponse::OUTCOME_SOLD_OUT) sold_out.fetch_add(1);
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(reserved.load(), kQuantity);
  EXPECT_EQ(sold_out.load(), kUsers - kQuantity);
  RunWorker();
  EXPECT_EQ(store_.ConfirmedCount("hot", 1), static_cast<size_t>(kQuantity));
  EXPECT_EQ(*gate_.Remaining("hot", 1), 0);
}

TEST_F(DealPipeline, FailedPublishKeepsTheHoldAndRedrives) {
  AddDeal(Deal("d1", 1));
  log_.FailNext(1, PublishOutcome::kFailed);
  auto r = ClaimSync("d1", "ada");
  EXPECT_EQ(r.status().code(), StatusCode::kUnavailable);
  // Not released: ada's retry may already have been told ALREADY_CLAIMED.
  EXPECT_EQ(*gate_.Remaining("d1", 1), 0);
  auto again = ClaimSync("d1", "ada");
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(again->outcome(), v1::ClaimResponse::OUTCOME_ALREADY_CLAIMED);
  for (int i = 0; i < 100 && log_.size() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  RunWorker();
  EXPECT_EQ(store_.ConfirmedCount("d1", 1), 1U);  // the order id she was given is real
}

TEST_F(DealPipeline, SweeperRepublishesHoldsWithoutOrders) {
  AddDeal(Deal("d1", 5));
  // deal-svc crashed between Reserve and Publish: a hold and no record.
  const std::string id = engine_.NewOrderId();
  ASSERT_EQ(gate_.Reserve("d1", 1, "ada", id)->kind, Reservation::Kind::kReserved);
  ASSERT_TRUE(ClaimSync("d1", "grace").ok());  // a normal, published one
  RunWorker();
  // Too young: left alone (it may still be in flight).
  EXPECT_EQ(*engine_.SweepOnce(30'000), 0);
  clock_.AdvanceMillis(31'000);
  EXPECT_EQ(*engine_.SweepOnce(30'000), 1);  // grace's is committed, ada's is not
  for (int i = 0; i < 100 && log_.size() < 2; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  RunWorker();
  EXPECT_EQ(store_.ConfirmedCount("d1", 1), 2U);
  EXPECT_EQ(*engine_.SweepOnce(30'000), 0);  // nothing left to do
}

TEST_F(DealPipeline, UnknownPublishOutcomeKeepsTheHold) {
  AddDeal(Deal("d1", 1));
  log_.FailNext(1, PublishOutcome::kUnknown);  // ack lost, record in the log
  auto r = ClaimSync("d1", "ada");
  EXPECT_EQ(r.status().code(), StatusCode::kUnavailable);
  // Releasing would let grace buy the same unit: it must stay held.
  EXPECT_EQ(*gate_.Remaining("d1", 1), 0);
  EXPECT_EQ(ClaimSync("d1", "grace")->outcome(), v1::ClaimResponse::OUTCOME_SOLD_OUT);
  // ada's retry finds her order, and the worker confirms it. The redrive's
  // second copy is a duplicate, not a second order.
  EXPECT_EQ(ClaimSync("d1", "ada")->outcome(), v1::ClaimResponse::OUTCOME_ALREADY_CLAIMED);
  for (int i = 0; i < 100 && log_.size() < 2; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  RunWorker();
  EXPECT_EQ(store_.ConfirmedCount("d1", 1), 1U);
}

TEST_F(DealPipeline, UnknownOutcomeIsRedrivenUntilTheLogHasIt) {
  AddDeal(Deal("d1", 1));
  // The request never reached the broker, but the producer cannot tell.
  log_.FailNext(2, PublishOutcome::kUnknown, /*lost=*/true);
  EXPECT_EQ(ClaimSync("d1", "ada").status().code(), StatusCode::kUnavailable);
  EXPECT_EQ(log_.size(), 0U);
  // The redrive queue re-publishes the same order id (the first retry is
  // also lost) until the log acknowledges it.
  for (int i = 0; i < 100 && log_.size() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_EQ(log_.size(), 1U);
  EXPECT_EQ(engine_.redrive_pending(), 0U);
  RunWorker();
  EXPECT_EQ(store_.ConfirmedCount("d1", 1), 1U);  // the held unit became an order
  auto again = ClaimSync("d1", "ada");
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(again->outcome(), v1::ClaimResponse::OUTCOME_ALREADY_CLAIMED);
}

TEST_F(DealPipeline, RedeliveryIsIdempotentAndRenotifies) {
  AddDeal(Deal("d1", 10));
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(ClaimSync("d1", "u" + std::to_string(i)).ok());
  auto batch = log_.Poll(100, 0);
  ASSERT_EQ(batch.size(), 5U);
  ASSERT_TRUE(processor_.Process(batch).ok());
  // Crash before the offset commit: the same records come again.
  log_.Rewind();
  auto replay = log_.Poll(100, 0);
  ASSERT_EQ(replay.size(), 5U);
  ASSERT_TRUE(processor_.Process(replay).ok());
  EXPECT_EQ(store_.ConfirmedCount("d1", 1), 5U);
  // Notifications go out again for duplicates (the first ones may have been
  // lost with the crash); clients dedupe by order id.
  EXPECT_EQ(notifier_.Drain().size(), 10U);
}

TEST_F(DealPipeline, DatabaseBackstopsAGateThatLostItsData) {
  AddDeal(Deal("d1", 3));
  for (const char* u : {"a", "b", "c"}) ASSERT_TRUE(ClaimSync("d1", u).ok());
  // Redis fails over to a replica that missed every write: stock is full
  // again and nobody holds anything.
  gate_.ForgetEverything("d1", 1, 3);
  for (const char* u : {"d", "e", "a"}) ASSERT_TRUE(ClaimSync("d1", u).ok());
  RunWorker();
  // Six reservations, three units: the database confirms exactly three, and
  // refuses the second order of user "a".
  EXPECT_EQ(store_.ConfirmedCount("d1", 1), 3U);
  int rejected = 0;
  for (const auto& o : notifier_.Drain()) {
    if (o.status() == v1::Order::STATUS_REJECTED) ++rejected;
  }
  EXPECT_EQ(rejected, 3);
}

TEST_F(DealPipeline, ResetStartsAFreshGeneration) {
  AddDeal(Deal("d1", 1));
  ASSERT_TRUE(ClaimSync("d1", "ada").ok());
  RunWorker();
  v1::ResetDealRequest req;
  req.set_deal_id("d1");
  req.set_quantity(2);
  req.set_starts_in_ms(0);
  auto d = engine_.Reset(req);
  ASSERT_TRUE(d.ok()) << d.status();
  EXPECT_EQ(d->generation(), 2);
  EXPECT_EQ(d->remaining(), 2);
  // ada may buy again in the new generation.
  EXPECT_EQ(ClaimSync("d1", "ada")->outcome(), v1::ClaimResponse::OUTCOME_RESERVED);
  RunWorker();
  EXPECT_EQ(store_.ConfirmedCount("d1", 2), 1U);
  EXPECT_EQ(store_.ConfirmedCount("d1", 1), 1U);
}

TEST_F(DealPipeline, GateRearmsFromTheDatabase) {
  AddDeal(Deal("d1", 5));
  ASSERT_TRUE(ClaimSync("d1", "ada").ok());
  RunWorker();  // sold = 1
  catalog_.Invalidate();
  MemoryInventoryGate fresh;  // a Redis with no keys at all
  DealEngine engine(&catalog_, &fresh, &log_, &store_, &clock_, registry_);
  std::promise<Result<v1::ClaimResponse>> p;
  engine.Claim("d1", "grace", [&](Result<v1::ClaimResponse> r) { p.set_value(std::move(r)); });
  auto r = p.get_future().get();
  ASSERT_TRUE(r.ok()) << r.status();
  EXPECT_EQ(r->outcome(), v1::ClaimResponse::OUTCOME_RESERVED);
  EXPECT_EQ(r->remaining(), 3);  // 5 - 1 sold - this one
}

TEST(BatchRules, DecideInLogOrder) {
  std::map<std::string, DealState> deals{{"d", {.generation = 2, .quantity = 2, .sold = 1}}};
  std::map<std::string, OrderRecord> existing{{"o-old", OrderRecord{.order_id = "o-old",
                                                                    .deal_id = "d",
                                                                    .user_id = "x",
                                                                    .price_cents = 0,
                                                                    .generation = 2,
                                                                    .reserved_at_ms = 0,
                                                                    .confirmed = true,
                                                                    .reason = {}}}};
  std::set<UserKey> users{{"d", 2, "taken"}};
  auto ev = [](const char* id, const char* user, int64_t gen, const char* deal = "d") {
    v1::OrderPlaced e;
    e.set_order_id(id);
    e.set_user_id(user);
    e.set_deal_id(deal);
    e.set_generation(gen);
    return e;
  };
  auto r =
      DecideBatch({ev("o-old", "x", 2), ev("o1", "taken", 2), ev("o2", "x", 1), ev("o3", "y", 2),
                   ev("o3", "y", 2), ev("o4", "z", 2), ev("o5", "q", 2, "?")},
                  existing, &deals, &users);
  ASSERT_EQ(r.confirmed.size(), 1U);
  EXPECT_EQ(r.confirmed[0].order_id, "o3");
  ASSERT_EQ(r.rejected.size(), 4U);
  EXPECT_EQ(r.rejected[0].reason, "one_per_customer");
  EXPECT_EQ(r.rejected[1].reason, "stale_generation");
  EXPECT_EQ(r.rejected[2].reason, "sold_out");
  EXPECT_EQ(r.rejected[3].reason, "unknown_deal");
  ASSERT_EQ(r.duplicates.size(), 2U);  // already stored, and repeated in-batch
  EXPECT_EQ(deals["d"].sold, 2);
}

TEST(Catalog, SingleflightCollapsesConcurrentMisses) {
  class SlowStore final : public OrderStore {
   public:
    Status UpsertDeal(const DealRecord&) override { return Status::Ok(); }
    Result<std::vector<DealRecord>> LoadDeals() override {
      calls.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (fail.load()) return Status::Unavailable("db down");
      return std::vector<DealRecord>{Deal("d1", 1)};
    }
    Result<std::vector<OrderRecord>> OrdersForUser(const std::string&) override {
      return std::vector<OrderRecord>{};
    }
    Result<std::set<std::string>> KnownOrderIds(const std::vector<std::string>&) override {
      return std::set<std::string>{};
    }
    Result<BatchResult> ApplyBatch(const std::vector<v1::OrderPlaced>&) override {
      return BatchResult{};
    }
    Result<DealRecord> ResetDeal(const std::string&, int32_t, int64_t, int64_t) override {
      return Status::Unimplemented("");
    }
    std::atomic<int> calls{0};
    std::atomic<bool> fail{false};
  } store;
  ManualClock clock(0);
  DealCatalog catalog(&store, &clock, 1000);
  std::vector<std::thread> threads;
  threads.reserve(32);
  std::atomic<int> ok{0};
  for (int i = 0; i < 32; ++i) {
    threads.emplace_back([&] {
      if (catalog.Get().ok()) ok.fetch_add(1);
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(ok.load(), 32);
  EXPECT_EQ(store.calls.load(), 1);

  // Expired and the database is down: the stale snapshot keeps serving.
  clock.AdvanceMillis(2000);
  store.fail = true;
  auto stale = catalog.Get();
  ASSERT_TRUE(stale.ok());
  EXPECT_EQ((*stale)->size(), 1U);
  EXPECT_EQ(store.calls.load(), 2);
}

TEST(WorkerLoop, RetriesAFailedBatchWithoutSkippingIt) {
  prometheus::Registry registry;
  MemoryOrderStore store;
  ASSERT_TRUE(store.UpsertDeal(Deal("d1", 10)).ok());
  MemoryOrderLog log;
  for (int i = 0; i < 3; ++i) {
    v1::OrderPlaced e;
    e.set_order_id("o" + std::to_string(i));
    e.set_deal_id("d1");
    e.set_user_id("u" + std::to_string(i));
    e.set_generation(1);
    log.Publish(e, [](PublishOutcome, const std::string&) {});
  }
  store.FailNextBatches(2);
  OrderProcessor processor(&store, nullptr, registry);
  std::atomic<bool> stop{false};
  WorkerLoopOptions opts;
  opts.poll_wait_ms = 5;
  opts.retry = {.base_micros = 1000, .cap_micros = 2000, .max_attempts = 100};
  std::thread worker([&] { RunWorkerLoop(&log, &processor, &stop, opts); });
  for (int i = 0; i < 200 && log.committed() < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  stop = true;
  log.Close();
  worker.join();
  EXPECT_EQ(log.committed(), 3U);
  EXPECT_EQ(store.ConfirmedCount("d1", 1), 3U);
}

}  // namespace
}  // namespace cartwheel::deal
