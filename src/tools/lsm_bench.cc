// lsm-bench: db_bench-style workloads against CartwheelKV's LSM engine and,
// when built with -DCARTWHEEL_BENCH_ROCKSDB=ON, against RocksDB with matching
// settings (4 MiB memtable, 4 KiB blocks, 10-bit bloom, no compression).
//
//   lsm-bench --engine lsm --num 1000000 --benchmarks fillseq,fillrandom,readrandom
//   lsm-bench --engine rocksdb ...

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include "common/histogram.h"
#include "storage/lsm/db.h"

#if defined(CARTWHEEL_HAVE_ROCKSDB)
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#endif

namespace {

using cartwheel::Histogram;
using cartwheel::HistogramSnapshot;
using Clock = std::chrono::steady_clock;

// Minimal engine facade so both stores run the same loops.
class BenchEngine {
 public:
  virtual ~BenchEngine() = default;
  virtual bool Put(const std::string& k, const std::string& v, bool sync) = 0;
  virtual bool Get(const std::string& k, std::string* v) = 0;
  virtual uint64_t Scan() = 0;  // returns entries visited
  virtual std::string StatsLine() = 0;
  virtual void Settle() {}  // wait for flushes/compactions
};

class CartwheelEngine final : public BenchEngine {
 public:
  explicit CartwheelEngine(const std::string& dir) {
    cartwheel::storage::lsm::Options o;
    o.block_cache_bytes = 32 << 20;
    db_ = std::move(cartwheel::storage::lsm::DB::Open(o, dir)).value();
  }
  bool Put(const std::string& k, const std::string& v, bool sync) override {
    return db_->Put({.sync = sync}, k, v).ok();
  }
  bool Get(const std::string& k, std::string* v) override {
    auto r = db_->Get(k);
    if (!r.ok()) return false;
    *v = std::move(r).value();
    return true;
  }
  uint64_t Scan() override {
    uint64_t n = 0;
    auto it = db_->NewIterator();
    for (it->SeekToFirst(); it->Valid(); it->Next()) ++n;
    return n;
  }
  void Settle() override { static_cast<void>(db_->WaitForCompactions()); }
  std::string StatsLine() override {
    const auto s = db_->Stats();
    uint64_t bytes = 0;
    for (const auto b : s.level_bytes) bytes += b;
    return fmt::format(
        "{}  size={:.1f} MiB  write-amp={:.2f}  flushes={} compactions={} "
        "(trivial {})  stall={:.1f} ms  slowdowns={}  cache hit={:.1f}%",
        s.levels, static_cast<double>(bytes) / (1 << 20), s.WriteAmplification(), s.flushes,
        s.compactions, s.trivial_moves, static_cast<double>(s.stall_micros) / 1000.0, s.slowdowns,
        100.0 * static_cast<double>(s.block_cache_hits) /
            static_cast<double>(std::max<uint64_t>(1, s.block_cache_hits + s.block_cache_misses)));
  }

 private:
  std::unique_ptr<cartwheel::storage::lsm::DB> db_;
};

#if defined(CARTWHEEL_HAVE_ROCKSDB)
class RocksEngine final : public BenchEngine {
 public:
  explicit RocksEngine(const std::string& dir) {
    rocksdb::Options o;
    o.create_if_missing = true;
    o.write_buffer_size = 4 << 20;
    o.compression = rocksdb::kNoCompression;
    o.max_background_jobs = 2;  // ours: one flush/compaction thread + the writer
    rocksdb::BlockBasedTableOptions t;
    t.block_size = 4096;
    t.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
    t.block_cache = rocksdb::NewLRUCache(32 << 20);
    o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(t));
    const auto s = rocksdb::DB::Open(o, dir, &db_);
    if (!s.ok()) throw std::runtime_error(s.ToString());
  }
  bool Put(const std::string& k, const std::string& v, bool sync) override {
    rocksdb::WriteOptions w;
    w.sync = sync;
    return db_->Put(w, k, v).ok();
  }
  bool Get(const std::string& k, std::string* v) override {
    return db_->Get(rocksdb::ReadOptions(), k, v).ok();
  }
  uint64_t Scan() override {
    uint64_t n = 0;
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
    for (it->SeekToFirst(); it->Valid(); it->Next()) ++n;
    return n;
  }
  void Settle() override {
    static_cast<void>(db_->WaitForCompact(rocksdb::WaitForCompactOptions()));
  }
  std::string StatsLine() override {
    std::string levels;
    db_->GetProperty("rocksdb.levelstats", &levels);
    std::replace(levels.begin(), levels.end(), '\n', ' ');
    return levels.substr(0, 200);
  }

 private:
  std::unique_ptr<rocksdb::DB> db_;
};
#endif

std::string KeyFor(uint64_t i) {
  return fmt::format("{:016d}", i);
}

struct Result {
  std::string name;
  uint64_t ops = 0;
  double seconds = 0;
  HistogramSnapshot latency;
};

void Print(const Result& r, size_t value_size) {
  const double ops_s = static_cast<double>(r.ops) / r.seconds;
  fmt::print("{:<12} {:>10.0f} ops/s  {:>7.1f} MB/s  p50 {:>8}  p99 {:>8}  p99.9 {:>8}\n", r.name,
             ops_s, ops_s * static_cast<double>(16 + value_size) / 1e6,
             cartwheel::FormatNanos(r.latency.Percentile(50)),
             cartwheel::FormatNanos(r.latency.Percentile(99)),
             cartwheel::FormatNanos(r.latency.Percentile(99.9)));
}

// Runs `op(i)` for i in [0, n) on `threads` threads, timing each call.
template <typename Op>
Result Run(const std::string& name, uint64_t n, int threads, Op&& op) {
  std::vector<std::unique_ptr<Histogram>> hists;
  hists.reserve(static_cast<size_t>(threads));
  for (int t = 0; t < threads; ++t) hists.push_back(std::make_unique<Histogram>());
  std::atomic<uint64_t> next{0};
  const auto start = Clock::now();
  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(threads));
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      for (uint64_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < n;) {
        const auto s = Clock::now();
        op(i, t);
        hists[static_cast<size_t>(t)]->Record(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - s).count());
      }
    });
  }
  for (auto& th : pool) th.join();
  Result r{name, n, std::chrono::duration<double>(Clock::now() - start).count(), {}};
  for (auto& h : hists) r.latency.Merge(h->Snapshot());
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"lsm-bench: db_bench-style workloads"};
  std::string engine_name = "lsm";
  std::string dir = "/tmp/lsm-bench";
  std::string benchmarks = "fillseq,fillrandom,overwrite,readrandom,readmissing,readseq";
  uint64_t num = 1'000'000;
  size_t value_size = 100;
  int threads = 1;
  app.add_option("--engine", engine_name, "lsm | rocksdb");
  app.add_option("--db", dir, "database directory (wiped first)");
  app.add_option("--benchmarks", benchmarks, "comma-separated workloads");
  app.add_option("--num", num, "operations per workload");
  app.add_option("--value-size", value_size, "value bytes");
  app.add_option("--threads", threads, "threads for read workloads");
  CLI11_PARSE(app, argc, argv);

  std::filesystem::remove_all(dir);
  std::unique_ptr<BenchEngine> db;
  if (engine_name == "lsm") {
    db = std::make_unique<CartwheelEngine>(dir);
#if defined(CARTWHEEL_HAVE_ROCKSDB)
  } else if (engine_name == "rocksdb") {
    db = std::make_unique<RocksEngine>(dir);
#endif
  } else {
    fmt::print(stderr, "unknown or unavailable engine '{}'\n", engine_name);
    return 2;
  }

  fmt::print("engine={} num={} value_size={} read_threads={}\n", engine_name, num, value_size,
             threads);
  const std::string value(value_size, 'v');
  std::stringstream list(benchmarks);
  for (std::string b; std::getline(list, b, ',');) {
    Result r;
    if (b == "fillseq") {
      r = Run(b, num, 1, [&](uint64_t i, int) { db->Put(KeyFor(i), value, false); });
    } else if (b == "fillrandom" || b == "overwrite") {
      std::mt19937_64 rng(b == "overwrite" ? 7 : 3);
      std::vector<uint64_t> order(num);
      for (auto& x : order) x = rng() % num;
      r = Run(b, num, 1, [&](uint64_t i, int) { db->Put(KeyFor(order[i]), value, false); });
    } else if (b == "fillsync") {
      r = Run(b, std::min<uint64_t>(num / 100, 2000), 1,
              [&](uint64_t i, int) { db->Put(KeyFor(i), value, true); });
    } else if (b == "readrandom" || b == "readmissing") {
      db->Settle();
      std::vector<std::mt19937_64> rngs;
      rngs.reserve(static_cast<size_t>(threads));
      for (int t = 0; t < threads; ++t) rngs.emplace_back(100 + static_cast<uint64_t>(t));
      const bool missing = b == "readmissing";
      std::atomic<uint64_t> found{0};
      r = Run(b, num, threads, [&](uint64_t, int t) {
        std::string v;
        const uint64_t k = rngs[static_cast<size_t>(t)]() % num;
        if (db->Get(missing ? KeyFor(k) + "." : KeyFor(k), &v)) found.fetch_add(1);
      });
      fmt::print("  ({} of {} found)\n", found.load(), num);
    } else if (b == "readseq") {
      const auto start = Clock::now();
      const uint64_t n = db->Scan();
      r = Result{b, n, std::chrono::duration<double>(Clock::now() - start).count(), {}};
    } else {
      fmt::print(stderr, "unknown benchmark '{}'\n", b);
      continue;
    }
    Print(r, value_size);
  }
  db->Settle();
  fmt::print("stats: {}\n", db->StatsLine());
  return 0;
}
