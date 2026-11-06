#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cartwheel {

class HistogramSnapshot;

// Concurrent log-linear histogram (HdrHistogram-style) for latencies.
//
// Values below 2^kSubBucketBits get one exact bucket each. Above that, every
// power-of-two range [2^m, 2^(m+1)) is split into 2^(kSubBucketBits-1) equal
// sub-buckets, so the relative error is at most 2^-(kSubBucketBits-1) = 0.78%
// no matter how large the value. Recording is one relaxed atomic increment,
// with no allocation and no locks.
//
// Values are unitless int64; Cartwheel records nanoseconds. Values >= 2^40
// (~18 minutes in ns) are clamped into the last bucket.
class Histogram {
 public:
  static constexpr int kSubBucketBits = 8;
  static constexpr int kMaxValueBits = 40;
  static constexpr int64_t kMaxTrackable = (int64_t{1} << kMaxValueBits) - 1;
  static constexpr size_t kExactBuckets = size_t{1} << kSubBucketBits;
  static constexpr size_t kSubBucketsPerGroup = size_t{1} << (kSubBucketBits - 1);
  static constexpr size_t kGroups = kMaxValueBits - kSubBucketBits;
  static constexpr size_t kBucketCount = kExactBuckets + kGroups * kSubBucketsPerGroup;

  Histogram() noexcept;
  Histogram(const Histogram&) = delete;
  Histogram& operator=(const Histogram&) = delete;

  void Record(int64_t value) noexcept { RecordN(value, 1); }
  void RecordN(int64_t value, uint64_t n) noexcept;

  [[nodiscard]] HistogramSnapshot Snapshot() const;
  void Reset() noexcept;

  // Bucket geometry, exposed for tests and docs.
  [[nodiscard]] static size_t BucketIndex(int64_t value) noexcept;
  [[nodiscard]] static int64_t BucketLowerBound(size_t index) noexcept;
  [[nodiscard]] static int64_t BucketUpperBound(size_t index) noexcept;  // inclusive

 private:
  std::array<std::atomic<uint64_t>, kBucketCount> buckets_;
  std::atomic<uint64_t> count_{0};
  std::atomic<uint64_t> sum_{0};
  std::atomic<int64_t> min_;
  std::atomic<int64_t> max_;
};

// Immutable, mergeable copy of a Histogram. Merge per-thread snapshots to
// avoid cross-core contention in load generators.
class HistogramSnapshot {
 public:
  HistogramSnapshot();

  [[nodiscard]] uint64_t count() const noexcept { return count_; }
  [[nodiscard]] int64_t min() const noexcept { return count_ ? min_ : 0; }
  [[nodiscard]] int64_t max() const noexcept { return count_ ? max_ : 0; }
  [[nodiscard]] double mean() const noexcept;

  // p in [0, 100]. Returns the highest value equivalent to the bucket holding
  // the rank, clamped to [min, max], so p100 == max exactly.
  [[nodiscard]] int64_t Percentile(double p) const noexcept;

  void Merge(const HistogramSnapshot& other);
  // The samples recorded between `older` and this snapshot of the same
  // histogram (per-interval percentiles from a cumulative histogram). min and
  // max are approximated by the outermost non-empty buckets.
  [[nodiscard]] HistogramSnapshot Minus(const HistogramSnapshot& older) const;

  // "n=1000 mean=1.2ms p50=1.1ms p90=… p99=… p99.9=… max=…", values in ns.
  [[nodiscard]] std::string SummaryNanos() const;

  [[nodiscard]] const std::vector<uint64_t>& buckets() const noexcept { return buckets_; }

 private:
  friend class Histogram;
  std::vector<uint64_t> buckets_;
  uint64_t count_ = 0;
  uint64_t sum_ = 0;
  int64_t min_ = 0;
  int64_t max_ = 0;
};

// 1234567 -> "1.23ms"
[[nodiscard]] std::string FormatNanos(int64_t nanos);

}  // namespace cartwheel
