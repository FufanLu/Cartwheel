#include "common/histogram.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace cartwheel {
namespace {

TEST(HistogramGeometryTest, BucketsAreContiguousAndCoverRange) {
  int64_t expected_lo = 0;
  for (size_t i = 0; i < Histogram::kBucketCount; ++i) {
    ASSERT_EQ(Histogram::BucketLowerBound(i), expected_lo) << "bucket " << i;
    const int64_t hi = Histogram::BucketUpperBound(i);
    ASSERT_GE(hi, expected_lo);
    expected_lo = hi + 1;
  }
  EXPECT_EQ(expected_lo - 1, Histogram::kMaxTrackable);
}

TEST(HistogramGeometryTest, EveryValueMapsIntoItsBucket) {
  std::mt19937_64 rng(1);
  for (int i = 0; i < 100'000; ++i) {
    const auto v = static_cast<int64_t>(rng() % static_cast<uint64_t>(Histogram::kMaxTrackable));
    const size_t idx = Histogram::BucketIndex(v);
    ASSERT_LE(Histogram::BucketLowerBound(idx), v);
    ASSERT_GE(Histogram::BucketUpperBound(idx), v);
  }
}

TEST(HistogramGeometryTest, RelativeErrorBounded) {
  for (size_t i = Histogram::kExactBuckets; i < Histogram::kBucketCount; ++i) {
    const auto lo = static_cast<double>(Histogram::BucketLowerBound(i));
    const auto hi = static_cast<double>(Histogram::BucketUpperBound(i));
    ASSERT_LE((hi - lo) / lo, 1.0 / 128.0) << "bucket " << i;
  }
}

TEST(HistogramGeometryTest, ClampsOutOfRange) {
  EXPECT_EQ(Histogram::BucketIndex(-5), 0U);
  EXPECT_EQ(Histogram::BucketIndex(Histogram::kMaxTrackable + 12345), Histogram::kBucketCount - 1);
}

TEST(HistogramTest, EmptySnapshot) {
  Histogram h;
  const auto s = h.Snapshot();
  EXPECT_EQ(s.count(), 0U);
  EXPECT_EQ(s.Percentile(99), 0);
  EXPECT_EQ(s.min(), 0);
  EXPECT_EQ(s.max(), 0);
}

TEST(HistogramTest, PercentilesWithinOnePercentOfExact) {
  std::mt19937_64 rng(42);
  std::lognormal_distribution<double> dist(std::log(2e6), 0.8);
  Histogram h;
  std::vector<int64_t> exact;
  exact.reserve(100'000);
  for (int i = 0; i < 100'000; ++i) {
    const auto v = static_cast<int64_t>(dist(rng));
    h.Record(v);
    exact.push_back(v);
  }
  std::sort(exact.begin(), exact.end());
  const auto s = h.Snapshot();
  ASSERT_EQ(s.count(), exact.size());
  for (const double p : {1.0, 25.0, 50.0, 90.0, 99.0, 99.9}) {
    const auto rank = static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(exact.size())));
    const auto want = static_cast<double>(exact[rank - 1]);
    const auto got = static_cast<double>(s.Percentile(p));
    EXPECT_NEAR(got, want, want * 0.01) << "p" << p;
  }
  EXPECT_EQ(s.Percentile(100), exact.back());
  EXPECT_EQ(s.min(), exact.front());
}

TEST(HistogramTest, MergeEqualsCombinedRecording) {
  Histogram a;
  Histogram b;
  Histogram both;
  for (int64_t i = 1; i <= 1000; ++i) {
    (i % 2 ? a : b).Record(i * 1000);
    both.Record(i * 1000);
  }
  auto merged = a.Snapshot();
  merged.Merge(b.Snapshot());
  const auto expect = both.Snapshot();
  EXPECT_EQ(merged.count(), expect.count());
  EXPECT_EQ(merged.buckets(), expect.buckets());
  EXPECT_EQ(merged.Percentile(50), expect.Percentile(50));
  EXPECT_DOUBLE_EQ(merged.mean(), expect.mean());
}

TEST(HistogramTest, ConcurrentRecordingLosesNothing) {
  Histogram h;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 50'000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&h, t] {
      for (int i = 0; i < kPerThread; ++i) h.Record(int64_t{t + 1} * 1000 + i % 100);
    });
  }
  for (auto& t : threads) t.join();
  const auto s = h.Snapshot();
  EXPECT_EQ(s.count(), static_cast<uint64_t>(kThreads) * kPerThread);
  EXPECT_EQ(s.min(), 1000);
  EXPECT_EQ(s.max(), int64_t{kThreads} * 1000 + 99);
}

TEST(HistogramTest, FormatNanos) {
  EXPECT_EQ(FormatNanos(950), "950ns");
  EXPECT_EQ(FormatNanos(1'500), "1.50us");
  EXPECT_EQ(FormatNanos(2'345'678), "2.35ms");
  EXPECT_EQ(FormatNanos(3'000'000'000), "3.00s");
}

}  // namespace
}  // namespace cartwheel
