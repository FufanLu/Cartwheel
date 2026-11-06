// wheel-bench: YCSB-style load generator for a CartwheelKV cluster.
//
//   wheel-bench --targets 127.0.0.1:7101,127.0.0.1:7102 --load 100000
//   wheel-bench --targets ... --workload A --threads 32 --duration 30
//
// Workloads (YCSB core): A = 50% read / 50% update, B = 95/5, C = read-only.
// An update is read-modify-write: a Get, then a Put carrying the Get's
// context. Blind writes would never supersede each other under DVV, so
// siblings would pile up. Keys follow a Zipfian distribution (theta 0.99,
// YCSB's default), so a few keys are hot. Prints one line per second, which
// makes a node kill during the run visible in the timeline.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include "client/kv_client.h"
#include "common/histogram.h"

namespace {

using cartwheel::FormatNanos;
using cartwheel::Histogram;
using cartwheel::HistogramSnapshot;
using Clock = std::chrono::steady_clock;

// Zipfian over [0, n) via an inverse-CDF table (YCSB's theta = 0.99).
class Zipf {
 public:
  Zipf(uint64_t n, double theta) : cdf_(n) {
    double sum = 0;
    for (uint64_t i = 0; i < n; ++i) sum += 1.0 / std::pow(static_cast<double>(i + 1), theta);
    double acc = 0;
    for (uint64_t i = 0; i < n; ++i) {
      acc += 1.0 / std::pow(static_cast<double>(i + 1), theta) / sum;
      cdf_[i] = acc;
    }
  }
  template <typename Rng>
  uint64_t Next(Rng& rng) const {
    const double u = std::uniform_real_distribution<double>(0, 1)(rng);
    return static_cast<uint64_t>(std::lower_bound(cdf_.begin(), cdf_.end(), u) - cdf_.begin());
  }

 private:
  std::vector<double> cdf_;
};

// Scramble so hot keys are spread over the ring, not adjacent.
std::string KeyFor(uint64_t rank) {
  const uint64_t h = (rank + 1) * 0x9E3779B97F4A7C15ULL;
  return fmt::format("user{:016x}", h);
}

struct ThreadStats {
  Histogram latency;
  std::atomic<uint64_t> ops{0};
  std::atomic<uint64_t> errors{0};
  std::atomic<uint64_t> unavailable{0};
};

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"wheel-bench: YCSB workloads against CartwheelKV"};
  std::string targets = "127.0.0.1:7101";
  std::string workload = "A";
  std::string table = "usertable";
  uint64_t keys = 100'000;
  uint64_t load = 0;
  size_t value_size = 100;
  int threads = 32;
  int duration_s = 30;
  int64_t timeout_ms = 500;
  app.add_option("--targets", targets, "comma-separated wheel-node addresses (round-robin)");
  app.add_option("--workload", workload, "A | B | C");
  app.add_option("--table", table);
  app.add_option("--keys", keys, "key space");
  app.add_option("--load", load, "insert this many keys first, then exit unless --duration > 0");
  app.add_option("--value-size", value_size);
  app.add_option("--threads", threads);
  app.add_option("--duration", duration_s, "seconds of workload (0 = load only)");
  app.add_option("--timeout-ms", timeout_ms, "per-attempt deadline");
  CLI11_PARSE(app, argc, argv);

  const std::string target = targets.find(',') == std::string::npos ? targets : "ipv4:" + targets;
  cartwheel::client::KvClient kv({.target = target, .timeout_ms = timeout_ms});
  const std::string value(value_size, 'x');

  if (load > 0) {
    const auto start = Clock::now();
    std::atomic<uint64_t> next{0};
    std::atomic<uint64_t> failed{0};
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads));
    for (int t = 0; t < threads; ++t) {
      pool.emplace_back([&] {
        for (uint64_t i; (i = next.fetch_add(1)) < load;) {
          if (!kv.Put(table, KeyFor(i), value, "").ok()) failed.fetch_add(1);
        }
      });
    }
    for (auto& th : pool) th.join();
    const double secs = std::chrono::duration<double>(Clock::now() - start).count();
    fmt::print("load: {} keys in {:.1f}s = {:.0f} writes/s ({} failed)\n", load, secs,
               static_cast<double>(load) / secs, failed.load());
    if (duration_s == 0) return 0;
  }

  const double read_ratio = workload == "C" ? 1.0 : workload == "B" ? 0.95 : 0.5;
  const Zipf zipf(keys, 0.99);
  std::vector<std::unique_ptr<ThreadStats>> stats;
  stats.reserve(static_cast<size_t>(threads));
  for (int t = 0; t < threads; ++t) stats.push_back(std::make_unique<ThreadStats>());
  std::atomic<bool> stop{false};
  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(threads));
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      std::mt19937_64 rng(1000 + static_cast<uint64_t>(t));
      ThreadStats& st = *stats[static_cast<size_t>(t)];
      while (!stop.load(std::memory_order_relaxed)) {
        const std::string key = KeyFor(zipf.Next(rng));
        const auto s = Clock::now();
        cartwheel::Status status;
        auto read = kv.Get(table, key);
        status = read.status();
        if (status.ok() && std::uniform_real_distribution<double>(0, 1)(rng) >= read_ratio) {
          status = kv.Put(table, key, value, read->context).status();
        }
        st.latency.Record(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - s).count());
        st.ops.fetch_add(1, std::memory_order_relaxed);
        if (!status.ok()) {
          st.errors.fetch_add(1, std::memory_order_relaxed);
          if (status.code() == cartwheel::StatusCode::kUnavailable ||
              status.code() == cartwheel::StatusCode::kDeadlineExceeded) {
            st.unavailable.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }

  fmt::print("workload {} ({:.0f}% reads), {} threads, {} keys zipfian(0.99), targets {}\n",
             workload, read_ratio * 100, threads, keys, targets);
  fmt::print("{:>4} {:>9} {:>7} {:>9} {:>9} {:>9}\n", "t(s)", "ops/s", "errors", "p50", "p99",
             "p99.9");
  HistogramSnapshot prev_total;
  uint64_t prev_ops = 0;
  uint64_t prev_errors = 0;
  HistogramSnapshot grand;
  uint64_t total_ops = 0;
  uint64_t total_errors = 0;
  for (int sec = 1; sec <= duration_s; ++sec) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    HistogramSnapshot now;
    uint64_t ops = 0;
    uint64_t errors = 0;
    for (auto& st : stats) {
      now.Merge(st->latency.Snapshot());
      ops += st->ops.load();
      errors += st->errors.load();
    }
    const HistogramSnapshot interval = now.Minus(prev_total);
    fmt::print("{:>4} {:>9} {:>7} {:>9} {:>9} {:>9}\n", sec, ops - prev_ops, errors - prev_errors,
               FormatNanos(interval.Percentile(50)), FormatNanos(interval.Percentile(99)),
               FormatNanos(interval.Percentile(99.9)));
    std::fflush(stdout);
    prev_total = now;
    prev_ops = ops;
    prev_errors = errors;
    grand = now;
    total_ops = ops;
    total_errors = errors;
  }
  stop = true;
  for (auto& th : pool) th.join();
  uint64_t unavailable = 0;
  for (auto& st : stats) unavailable += st->unavailable.load();
  fmt::print(
      "total: {} ops in {}s = {:.0f} ops/s, {} errors ({} unavailable/timeout), p50 {} p99 {} "
      "p99.9 {}\n",
      total_ops, duration_s, static_cast<double>(total_ops) / duration_s, total_errors, unavailable,
      FormatNanos(grand.Percentile(50)), FormatNanos(grand.Percentile(99)),
      FormatNanos(grand.Percentile(99.9)));
  return 0;
}
