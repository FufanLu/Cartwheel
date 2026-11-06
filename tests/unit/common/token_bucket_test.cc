#include "common/token_bucket.h"

#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace cartwheel {
namespace {

constexpr int64_t kMs = 1'000'000;

TEST(TokenBucketTest, StartsFullAndAllowsExactlyBurst) {
  ManualClock clock(1'000 * kMs);
  TokenBucket b(10.0, 5.0, &clock);
  EXPECT_DOUBLE_EQ(b.Available(), 5.0);
  for (int i = 0; i < 5; ++i) EXPECT_TRUE(b.TryAcquire()) << i;
  EXPECT_FALSE(b.TryAcquire());
  EXPECT_NEAR(b.Available(), 0.0, 1e-9);
}

TEST(TokenBucketTest, RefillsAtRate) {
  ManualClock clock(0);
  TokenBucket b(10.0, 2.0, &clock);  // one token per 100ms
  ASSERT_TRUE(b.TryAcquire());
  ASSERT_TRUE(b.TryAcquire());
  ASSERT_FALSE(b.TryAcquire());

  clock.AdvanceMillis(99);
  EXPECT_FALSE(b.TryAcquire());
  clock.AdvanceMillis(1);
  EXPECT_TRUE(b.TryAcquire());
  EXPECT_FALSE(b.TryAcquire());

  // Idle time refills only up to burst, never beyond.
  clock.AdvanceMillis(10'000);
  EXPECT_DOUBLE_EQ(b.Available(), 2.0);
}

TEST(TokenBucketTest, NanosUntilAvailableMatchesRefill) {
  ManualClock clock(0);
  TokenBucket b(4.0, 1.0, &clock);  // 250ms per token
  ASSERT_TRUE(b.TryAcquire());
  EXPECT_EQ(b.NanosUntilAvailable(), 250 * kMs);
  clock.AdvanceMillis(100);
  EXPECT_EQ(b.NanosUntilAvailable(), 150 * kMs);
  clock.AdvanceMillis(150);
  EXPECT_EQ(b.NanosUntilAvailable(), 0);
}

TEST(TokenBucketTest, MultiTokenRequests) {
  ManualClock clock(0);
  TokenBucket b(100.0, 10.0, &clock);
  EXPECT_TRUE(b.TryAcquire(7));
  EXPECT_FALSE(b.TryAcquire(4));  // only 3 left; a rejected request takes nothing
  EXPECT_TRUE(b.TryAcquire(3));
}

TEST(TokenBucketTest, SetRatePreservesFillLevelInTokens) {
  ManualClock clock(0);
  TokenBucket b(10.0, 10.0, &clock);
  for (int i = 0; i < 6; ++i) ASSERT_TRUE(b.TryAcquire());
  EXPECT_NEAR(b.Available(), 4.0, 1e-9);
  b.SetRate(100.0, 10.0);  // 10x faster refill, same capacity
  EXPECT_NEAR(b.Available(), 4.0, 1e-9);
  clock.AdvanceMillis(30);  // 3 tokens at the new rate
  EXPECT_NEAR(b.Available(), 7.0, 1e-9);
}

TEST(TokenBucketTest, ShrinkingBurstClampsAvailable) {
  ManualClock clock(0);
  TokenBucket b(10.0, 10.0, &clock);
  for (int i = 0; i < 9; ++i) ASSERT_TRUE(b.TryAcquire());
  b.SetRate(10.0, 2.0);
  EXPECT_GE(b.Available(), 0.0);
  EXPECT_FALSE(b.TryAcquire());
}

TEST(TokenBucketTest, SetRateTakesEffect) {
  ManualClock clock(0);
  TokenBucket b(1.0, 1.0, &clock);
  ASSERT_TRUE(b.TryAcquire());
  b.SetRate(1000.0, 10.0);
  clock.AdvanceMillis(1);
  EXPECT_TRUE(b.TryAcquire());
  EXPECT_DOUBLE_EQ(b.rate(), 1000.0);
  EXPECT_DOUBLE_EQ(b.burst(), 10.0);
}

// Under contention the CAS loop must never admit more than burst + refill.
TEST(TokenBucketTest, ConcurrentAcquireNeverOverAdmits) {
  ManualClock clock(0);  // frozen time: no refill during the test
  TokenBucket b(1.0, 1000.0, &clock);
  std::atomic<int> admitted{0};
  std::vector<std::thread> threads;
  threads.reserve(8);
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 10'000; ++i) {
        if (b.TryAcquire()) admitted.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(admitted.load(), 1000);
}

}  // namespace
}  // namespace cartwheel
