#include "kv/local_store.h"

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kv/in_process_kv.h"
#include "kv/kv_service.h"

namespace cartwheel::kv {
namespace {

using cluster::VectorClock;

std::set<std::string> LiveValues(const cluster::SiblingSet& s) {
  std::set<std::string> out;
  for (const auto& sib : s.siblings()) {
    if (!sib.tombstone) out.insert(sib.value);
  }
  return out;
}

TEST(LocalStoreTest, ReadYourWrite) {
  LocalStore store("n1", storage::NewMemEngine());
  auto w = store.CoordinateWrite("t", "k", {}, "v1", false);
  ASSERT_TRUE(w.ok());
  auto r = store.Read("t", "k");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(LiveValues(r->siblings), (std::set<std::string>{"v1"}));
  EXPECT_EQ(r->siblings.Join().ToString(), "{n1:1}");
}

TEST(LocalStoreTest, TablesAreIsolated) {
  LocalStore store("n1", storage::NewMemEngine());
  ASSERT_TRUE(store.CoordinateWrite("a", "k", {}, "in-a", false).ok());
  auto r = store.Read("b", "k");
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->siblings.empty());
}

// 16 threads read the same version and all write concurrently. With plain
// vector clocks all but one write would be lost. With DVV every write
// survives as a sibling.
TEST(LocalStoreTest, ConcurrentWritersFromSameContextAllSurvive) {
  LocalStore store("n1", storage::NewMemEngine());
  ASSERT_TRUE(store.CoordinateWrite("t", "k", {}, "base", false).ok());
  const VectorClock ctx = store.Read("t", "k")->siblings.Join();

  constexpr int kWriters = 16;
  std::vector<std::thread> threads;
  threads.reserve(kWriters);
  for (int i = 0; i < kWriters; ++i) {
    threads.emplace_back([&store, &ctx, i] {
      ASSERT_TRUE(store.CoordinateWrite("t", "k", ctx, "w" + std::to_string(i), false).ok());
    });
  }
  for (auto& t : threads) t.join();

  const auto set = store.Read("t", "k")->siblings;
  EXPECT_EQ(set.LiveCount(), static_cast<size_t>(kWriters));
  EXPECT_EQ(LiveValues(set).count("base"), 0U) << "base was read by every writer";
  EXPECT_EQ(set.Join().Get("n1"), static_cast<uint64_t>(kWriters + 1));
}

TEST(LocalStoreTest, MergeIsIdempotentAndSkipsNoopWrites) {
  LocalStore a("n1", storage::NewMemEngine());
  LocalStore b("n2", storage::NewMemEngine());
  auto wa = a.CoordinateWrite("t", "k", {}, "from-a", false);
  auto wb = b.CoordinateWrite("t", "k", {}, "from-b", false);
  ASSERT_TRUE(wa.ok() && wb.ok());
  ASSERT_TRUE(a.Merge("t", "k", wb->siblings).ok());
  ASSERT_TRUE(a.Merge("t", "k", wb->siblings).ok());
  EXPECT_EQ(LiveValues(a.Read("t", "k")->siblings), (std::set<std::string>{"from-a", "from-b"}));
}

class InProcessKvTest : public ::testing::Test {
 protected:
  LocalStore store_{"n1", storage::NewMemEngine()};
  InProcessKv kv_{&store_};
};

TEST_F(InProcessKvTest, GetPutDeleteRoundTrip) {
  auto missing = kv_.Get("carts", "u1");
  ASSERT_TRUE(missing.ok());
  EXPECT_FALSE(missing->found());

  auto put = kv_.Put("carts", "u1", "v1", "");
  ASSERT_TRUE(put.ok());
  EXPECT_EQ(put->values, (std::vector<std::string>{"v1"}));

  auto got = kv_.Get("carts", "u1");
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got->values, (std::vector<std::string>{"v1"}));

  auto del = kv_.Delete("carts", "u1", got->context);
  ASSERT_TRUE(del.ok());
  EXPECT_FALSE(del->found());
  auto after = kv_.Get("carts", "u1");
  EXPECT_FALSE(after->found());
  EXPECT_FALSE(after->context.empty()) << "tombstone context lets a writer supersede the delete";
}

// The bug the design note in local_store.h describes: the response context
// must not let a writer supersede a sibling it was never shown.
TEST_F(InProcessKvTest, PutResponseShowsConcurrentSiblingsItsContextCovers) {
  ASSERT_TRUE(kv_.Put("t", "k", "base", "").ok());
  const std::string ctx = kv_.Get("t", "k")->context;
  ASSERT_TRUE(kv_.Put("t", "k", "alice", ctx).ok());
  auto bob = kv_.Put("t", "k", "bob", ctx);
  ASSERT_TRUE(bob.ok());
  // Bob is told about Alice's concurrent write...
  EXPECT_EQ(bob->values, (std::vector<std::string>{"alice", "bob"}));
  // ...so overwriting with Bob's context only supersedes what Bob was shown.
  auto merged = kv_.Put("t", "k", "alice+bob", bob->context);
  ASSERT_TRUE(merged.ok());
  EXPECT_EQ(merged->values, (std::vector<std::string>{"alice+bob"}));
}

TEST_F(InProcessKvTest, ValidatesInputs) {
  EXPECT_EQ(kv_.Get("Bad Table", "k").status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(kv_.Get("t", "").status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(kv_.Get("t", std::string(kMaxKeyBytes + 1, 'k')).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(kv_.Put("t", "k", std::string(kMaxValueBytes + 1, 'v'), "").status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(kv_.Put("t", "k", "v", "\x07garbage").status().code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace cartwheel::kv
