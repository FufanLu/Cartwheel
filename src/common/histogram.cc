#include "common/histogram.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

#include <fmt/format.h>

namespace cartwheel {
namespace {

void AtomicMin(std::atomic<int64_t>& target, int64_t value) noexcept {
  int64_t cur = target.load(std::memory_order_relaxed);
  while (value < cur && !target.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
  }
}

void AtomicMax(std::atomic<int64_t>& target, int64_t value) noexcept {
  int64_t cur = target.load(std::memory_order_relaxed);
  while (value > cur && !target.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
  }
}

}  // namespace

Histogram::Histogram() noexcept
    : min_(std::numeric_limits<int64_t>::max()), max_(std::numeric_limits<int64_t>::min()) {
  for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
}

size_t Histogram::BucketIndex(int64_t value) noexcept {
  const auto v = static_cast<uint64_t>(std::clamp<int64_t>(value, 0, kMaxTrackable));
  if (v < kExactBuckets) return static_cast<size_t>(v);
  // msb >= kSubBucketBits here. Shift so the top kSubBucketBits bits remain;
  // the leading 1 makes the result land in [2^(S-1), 2^S).
  const int msb = std::bit_width(v) - 1;
  const int group = msb - kSubBucketBits + 1;  // 1-based
  const auto sub = static_cast<size_t>(v >> group) - kSubBucketsPerGroup;
  return kExactBuckets + static_cast<size_t>(group - 1) * kSubBucketsPerGroup + sub;
}

int64_t Histogram::BucketLowerBound(size_t index) noexcept {
  if (index < kExactBuckets) return static_cast<int64_t>(index);
  const size_t j = index - kExactBuckets;
  const auto group = static_cast<int>(j / kSubBucketsPerGroup) + 1;
  const size_t sub = j % kSubBucketsPerGroup;
  return static_cast<int64_t>(static_cast<uint64_t>(kSubBucketsPerGroup + sub) << group);
}

int64_t Histogram::BucketUpperBound(size_t index) noexcept {
  if (index < kExactBuckets) return static_cast<int64_t>(index);
  const auto group = static_cast<int>((index - kExactBuckets) / kSubBucketsPerGroup) + 1;
  return BucketLowerBound(index) + (int64_t{1} << group) - 1;
}

void Histogram::RecordN(int64_t value, uint64_t n) noexcept {
  if (n == 0) return;
  buckets_[BucketIndex(value)].fetch_add(n, std::memory_order_relaxed);
  count_.fetch_add(n, std::memory_order_relaxed);
  sum_.fetch_add(static_cast<uint64_t>(std::max<int64_t>(value, 0)) * n, std::memory_order_relaxed);
  AtomicMin(min_, value);
  AtomicMax(max_, value);
}

void Histogram::Reset() noexcept {
  for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
  count_.store(0, std::memory_order_relaxed);
  sum_.store(0, std::memory_order_relaxed);
  min_.store(std::numeric_limits<int64_t>::max(), std::memory_order_relaxed);
  max_.store(std::numeric_limits<int64_t>::min(), std::memory_order_relaxed);
}

HistogramSnapshot Histogram::Snapshot() const {
  // Not an atomic cut across buckets: concurrent records may land in a bucket
  // but not yet in count_. We recompute count from the buckets so percentiles
  // stay self-consistent.
  HistogramSnapshot snap;
  uint64_t total = 0;
  for (size_t i = 0; i < kBucketCount; ++i) {
    snap.buckets_[i] = buckets_[i].load(std::memory_order_relaxed);
    total += snap.buckets_[i];
  }
  snap.count_ = total;
  snap.sum_ = sum_.load(std::memory_order_relaxed);
  snap.min_ = min_.load(std::memory_order_relaxed);
  snap.max_ = max_.load(std::memory_order_relaxed);
  return snap;
}

HistogramSnapshot::HistogramSnapshot()
    : buckets_(Histogram::kBucketCount, 0),
      min_(std::numeric_limits<int64_t>::max()),
      max_(std::numeric_limits<int64_t>::min()) {}

double HistogramSnapshot::mean() const noexcept {
  return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
}

int64_t HistogramSnapshot::Percentile(double p) const noexcept {
  if (count_ == 0) return 0;
  p = std::clamp(p, 0.0, 100.0);
  const auto rank = std::max<uint64_t>(
      1, static_cast<uint64_t>(std::ceil(p / 100.0 * static_cast<double>(count_))));
  uint64_t seen = 0;
  for (size_t i = 0; i < buckets_.size(); ++i) {
    seen += buckets_[i];
    if (seen >= rank) return std::clamp(Histogram::BucketUpperBound(i), min_, max_);
  }
  return max_;
}

void HistogramSnapshot::Merge(const HistogramSnapshot& other) {
  for (size_t i = 0; i < buckets_.size(); ++i) buckets_[i] += other.buckets_[i];
  count_ += other.count_;
  sum_ += other.sum_;
  min_ = std::min(min_, other.min_);
  max_ = std::max(max_, other.max_);
}

HistogramSnapshot HistogramSnapshot::Minus(const HistogramSnapshot& older) const {
  HistogramSnapshot d;
  d.count_ = 0;
  for (size_t i = 0; i < buckets_.size(); ++i) {
    d.buckets_[i] = buckets_[i] - std::min(buckets_[i], older.buckets_[i]);
    d.count_ += d.buckets_[i];
  }
  d.sum_ = sum_ - std::min(sum_, older.sum_);
  for (size_t i = 0; i < d.buckets_.size(); ++i) {
    if (d.buckets_[i] > 0) {
      d.min_ = Histogram::BucketLowerBound(i);
      break;
    }
  }
  for (size_t i = d.buckets_.size(); i-- > 0;) {
    if (d.buckets_[i] > 0) {
      d.max_ = Histogram::BucketUpperBound(i);
      break;
    }
  }
  return d;
}

std::string HistogramSnapshot::SummaryNanos() const {
  return fmt::format("n={} mean={} p50={} p90={} p99={} p99.9={} max={}", count_,
                     FormatNanos(static_cast<int64_t>(mean())), FormatNanos(Percentile(50)),
                     FormatNanos(Percentile(90)), FormatNanos(Percentile(99)),
                     FormatNanos(Percentile(99.9)), FormatNanos(max()));
}

std::string FormatNanos(int64_t nanos) {
  const auto v = static_cast<double>(nanos);
  if (nanos < 1'000) return fmt::format("{}ns", nanos);
  if (nanos < 1'000'000) return fmt::format("{:.2f}us", v / 1e3);
  if (nanos < 1'000'000'000) return fmt::format("{:.2f}ms", v / 1e6);
  return fmt::format("{:.2f}s", v / 1e9);
}

}  // namespace cartwheel
