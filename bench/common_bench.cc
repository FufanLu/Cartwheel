// Micro benchmarks for hot-path primitives. Run with the release preset:
//   build/release/bench/common_bench --benchmark_min_time=0.5s
#include <algorithm>
#include <mutex>

#include <benchmark/benchmark.h>

#include "common/clock.h"
#include "common/histogram.h"
#include "common/token_bucket.h"

namespace cartwheel {
namespace {

// Textbook mutex-protected token bucket, as the baseline GCRA replaces.
class MutexTokenBucket {
 public:
  MutexTokenBucket(double rate, double burst, const Clock* clock)
      : clock_(clock),
        rate_per_ns_(rate / 1e9),
        burst_(burst),
        tokens_(burst),
        last_(clock->NowNanos()) {}

  bool TryAcquire() {
    std::lock_guard lock(mu_);
    const int64_t now = clock_->NowNanos();
    tokens_ = std::min(burst_, tokens_ + static_cast<double>(now - last_) * rate_per_ns_);
    last_ = now;
    if (tokens_ < 1.0) return false;
    tokens_ -= 1.0;
    return true;
  }

 private:
  std::mutex mu_;
  const Clock* clock_;
  double rate_per_ns_;
  double burst_;
  double tokens_;
  int64_t last_;
};

// High rate so the benchmark measures admission cost, not rejection.
constexpr double kRate = 1e9;
constexpr double kBurst = 1e6;

void BM_TokenBucketGcra(benchmark::State& state) {
  static TokenBucket bucket(kRate, kBurst);
  for (auto _ : state) benchmark::DoNotOptimize(bucket.TryAcquire());
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TokenBucketGcra)->ThreadRange(1, 8)->UseRealTime();

void BM_TokenBucketMutex(benchmark::State& state) {
  static MutexTokenBucket bucket(kRate, kBurst, SystemClock::Default());
  for (auto _ : state) benchmark::DoNotOptimize(bucket.TryAcquire());
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TokenBucketMutex)->ThreadRange(1, 8)->UseRealTime();

// Per-client limiters: each thread owns its bucket, as the gateway shards
// limits by client and CartwheelKV by table. No shared cache line.
void BM_TokenBucketGcraSharded(benchmark::State& state) {
  TokenBucket bucket(kRate, kBurst);
  for (auto _ : state) benchmark::DoNotOptimize(bucket.TryAcquire());
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TokenBucketGcraSharded)->ThreadRange(1, 8)->UseRealTime();

void BM_HistogramRecordShared(benchmark::State& state) {
  static Histogram hist;
  int64_t v = 1'000 + state.thread_index() * 7919;
  for (auto _ : state) {
    hist.Record(v);
    v = (v * 1103515245 + 12345) & 0xFFFFFF;  // cheap pseudo-random latencies
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HistogramRecordShared)->ThreadRange(1, 8)->UseRealTime();

void BM_HistogramRecordPerThread(benchmark::State& state) {
  Histogram hist;  // one per thread: what wheel-bench does
  int64_t v = 1'000 + state.thread_index() * 7919;
  for (auto _ : state) {
    hist.Record(v);
    v = (v * 1103515245 + 12345) & 0xFFFFFF;
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HistogramRecordPerThread)->ThreadRange(1, 8)->UseRealTime();

void BM_SystemClockNow(benchmark::State& state) {
  const Clock* clock = SystemClock::Default();
  for (auto _ : state) benchmark::DoNotOptimize(clock->NowNanos());
}
BENCHMARK(BM_SystemClockNow);

}  // namespace
}  // namespace cartwheel
