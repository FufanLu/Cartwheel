// cw-tour: drives the common primitives with deterministic inputs and prints
// what they actually did. The output is pasted verbatim into
// docs/illustrated/00-overload-toolbox.md, so the docs never drift from code.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include "common/backoff.h"
#include "common/clock.h"
#include "common/histogram.h"
#include "common/thread_pool.h"
#include "common/token_bucket.h"

namespace cartwheel {
namespace {

constexpr int64_t kMs = 1'000'000;

void TourTokenBucket() {
  fmt::print("## GCRA token bucket: rate=5/s, burst=3\n\n");
  ManualClock clock(0);
  TokenBucket bucket(5.0, 3.0, &clock);
  fmt::print("emission interval T = {} ms, tolerance = burst*T = {} ms\n\n",
             bucket.interval_nanos() / kMs, 3 * bucket.interval_nanos() / kMs);
  fmt::print(
      "| t (ms) | TAT before | new TAT if admitted | new TAT - t | decision | tokens left |\n");
  fmt::print("|---:|---:|---:|---:|:---|---:|\n");
  const std::array<int64_t, 9> arrivals_ms = {0, 0, 0, 0, 100, 200, 250, 900, 900};
  for (const int64_t t : arrivals_ms) {
    clock.Set(t * kMs);
    const int64_t before = bucket.tat_nanos();
    const int64_t candidate = std::max(before, t * kMs) + bucket.interval_nanos();
    const bool ok = bucket.TryAcquire();
    fmt::print("| {} | {} | {} | {} | {} | {:.2f} |\n", t, before / kMs, candidate / kMs,
               (candidate - t * kMs) / kMs, ok ? "admit" : "**reject**", bucket.Available());
  }
  fmt::print("\n");
}

void TourBackoffDelays() {
  fmt::print("## Backoff delays (base=10ms, cap=2s, seed=42)\n\n");
  fmt::print("| attempt | none | full | equal | decorrelated |\n|---:|---:|---:|---:|---:|\n");
  const std::array<JitterMode, 4> modes = {JitterMode::kNone, JitterMode::kFull, JitterMode::kEqual,
                                           JitterMode::kDecorrelated};
  std::vector<Backoff> backoffs;
  backoffs.reserve(modes.size());
  for (const auto m : modes) {
    backoffs.emplace_back(BackoffPolicy{10'000, 2'000'000, 8, m}, 42);
  }
  for (int attempt = 1; attempt <= 8; ++attempt) {
    fmt::print("| {} |", attempt);
    for (auto& b : backoffs) {
      fmt::print(" {:.1f} ms |", static_cast<double>(b.NextDelayMicros().value_or(0)) / 1000.0);
    }
    fmt::print("\n");
  }
  fmt::print("\n");
}

// 1000 clients fail at t=0 and retry up to 4 times. What hurts a recovering
// server is the instantaneous spike: without jitter every client retries in
// the same microsecond. We report the peak number of arrivals in any single
// millisecond, plus a 25 ms-bucket picture of how retries spread out.
void TourRetryStorm() {
  fmt::print("## Retry storm: 1000 clients, first failure at t=0, 4 retries each (base=25ms)\n\n");
  constexpr int kClients = 1000;
  constexpr int64_t kBinUs = 25'000;
  constexpr size_t kBins = 16;  // 0..400 ms
  const std::array<std::pair<const char*, JitterMode>, 3> modes = {
      std::pair{"none", JitterMode::kNone}, std::pair{"full", JitterMode::kFull},
      std::pair{"decorrelated", JitterMode::kDecorrelated}};
  for (const auto& [name, mode] : modes) {
    std::vector<int> bins(kBins, 0);
    std::vector<int> per_ms(4'000, 0);  // 0..4 s
    int total = 0;
    for (int c = 0; c < kClients; ++c) {
      Backoff b(BackoffPolicy{25'000, 2'000'000, 4, mode}, static_cast<uint64_t>(c) + 1);
      int64_t t = 0;
      while (auto d = b.NextDelayMicros()) {
        t += *d;
        ++total;
        if (const auto bin = static_cast<size_t>(t / kBinUs); bin < kBins) ++bins[bin];
        if (const auto ms = static_cast<size_t>(t / 1000); ms < per_ms.size()) ++per_ms[ms];
      }
    }
    const int peak_ms = *std::max_element(per_ms.begin(), per_ms.end());
    fmt::print("jitter={:<13} retries={}  peak arrivals in any 1 ms = {}\n", name, total, peak_ms);
    for (size_t i = 0; i < kBins; ++i) {
      fmt::print("  {:>3}-{:<3}ms {:>5} {}\n", i * 25, (i + 1) * 25, bins[i],
                 std::string(static_cast<size_t>(bins[i] / 20), '#'));
    }
    fmt::print("\n");
  }
}

void TourHistogram() {
  fmt::print("## Histogram bucket geometry (kSubBucketBits=8)\n\n");
  fmt::print("| value (ns) | bucket | range [lo, hi] | width | max rel. error |\n");
  fmt::print("|---:|---:|---|---:|---:|\n");
  const std::array<int64_t, 7> values = {7, 255, 256, 1'000, 123'456, 1'500'000, 987'654'321};
  for (const int64_t v : values) {
    const size_t idx = Histogram::BucketIndex(v);
    const int64_t lo = Histogram::BucketLowerBound(idx);
    const int64_t hi = Histogram::BucketUpperBound(idx);
    fmt::print(
        "| {} | {} | [{}, {}] | {} | {:.3f}% |\n", v, idx, lo, hi, hi - lo + 1,
        100.0 * static_cast<double>(hi - lo) / static_cast<double>(std::max<int64_t>(lo, 1)));
  }
  fmt::print("\nbuckets: {} ({} KiB of atomics per histogram)\n\n", Histogram::kBucketCount,
             Histogram::kBucketCount * sizeof(uint64_t) / 1024);

  fmt::print("## Histogram vs exact percentiles: 200k log-normal latencies (median ~1ms)\n\n");
  std::mt19937_64 rng(7);
  std::lognormal_distribution<double> dist(std::log(1e6), 0.6);
  Histogram h;
  std::vector<int64_t> exact;
  exact.reserve(200'000);
  for (int i = 0; i < 200'000; ++i) {
    const auto v = static_cast<int64_t>(dist(rng));
    h.Record(v);
    exact.push_back(v);
  }
  std::sort(exact.begin(), exact.end());
  const auto snap = h.Snapshot();
  fmt::print("| percentile | exact | histogram | error |\n|---:|---:|---:|---:|\n");
  for (const double p : {50.0, 90.0, 99.0, 99.9, 100.0}) {
    const auto rank = static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(exact.size())));
    const int64_t e = exact[std::max<size_t>(rank, 1) - 1];
    const int64_t got = snap.Percentile(p);
    fmt::print("| p{} | {} | {} | {:+.3f}% |\n", p, FormatNanos(e), FormatNanos(got),
               100.0 * static_cast<double>(got - e) / static_cast<double>(e));
  }
  fmt::print("\n");
}

void TourLoadShedding() {
  fmt::print("## Bounded queue: 2 workers, queue=4, 20 tasks of 20ms submitted at once\n\n");
  ThreadPool pool({.threads = 2, .max_queue = 4, .name = "tour"});
  int accepted = 0;
  int rejected = 0;
  std::string timeline;
  for (int i = 0; i < 20; ++i) {
    const bool ok =
        pool.TrySubmit([] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); });
    ok ? ++accepted : ++rejected;
    timeline += ok ? 'A' : 'r';
  }
  pool.Shutdown();
  fmt::print("submission order (A=accepted, r=rejected): {}\n", timeline);
  fmt::print("accepted={} rejected={} completed={}\n", accepted, rejected, pool.completed());
  fmt::print("worst-case queueing delay is bounded by queue/threads*task = {} ms\n\n", 4 / 2 * 20);
}

}  // namespace
}  // namespace cartwheel

int main(int argc, char** argv) {
  CLI::App app{"cw-tour: print real traces of Cartwheel primitives for the illustrated docs"};
  std::string section = "all";
  app.add_option("--section", section, "token-bucket|backoff|histogram|pool|all");
  CLI11_PARSE(app, argc, argv);

  using namespace cartwheel;
  if (section == "all" || section == "token-bucket") TourTokenBucket();
  if (section == "all" || section == "backoff") {
    TourBackoffDelays();
    TourRetryStorm();
  }
  if (section == "all" || section == "histogram") TourHistogram();
  if (section == "all" || section == "pool") TourLoadShedding();
  return 0;
}
