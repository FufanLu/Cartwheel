#include "storage/lsm/db.h"

#include <atomic>
#include <map>
#include <random>
#include <thread>

#include <gtest/gtest.h>

#include "common/units.h"
#include "fault_env.h"
#include "storage/engine.h"
#include "storage_test_util.h"

namespace cartwheel::storage::lsm {
namespace {

using testing::FaultInjectionEnv;
using testing::TempDir;

// Small buffers so tests exercise flushes and multi-level compactions quickly.
Options SmallOptions(Env* env = Env::Default()) {
  Options o;
  o.env = env;
  o.write_buffer_size = 64_KiB;
  o.max_file_size = 32_KiB;
  o.levels.l1_target_bytes = 128_KiB;
  o.levels.l0_compaction_trigger = 2;
  o.block_cache_bytes = 256_KiB;
  return o;
}

std::unique_ptr<DB> OpenOrDie(const Options& o, const std::string& dir) {
  auto db = DB::Open(o, dir);
  EXPECT_TRUE(db.ok()) << db.status();
  return db.ok() ? std::move(db).value() : nullptr;
}

std::map<std::string, std::string> Scan(DB* db) {
  std::map<std::string, std::string> out;
  auto it = db->NewIterator();
  for (it->SeekToFirst(); it->Valid(); it->Next()) out.emplace(it->key(), it->value());
  EXPECT_TRUE(it->status().ok());
  return out;
}

TEST(LsmDbTest, PutGetDeleteOverwrite) {
  TempDir dir;
  auto db = OpenOrDie(SmallOptions(), dir.path());
  ASSERT_TRUE(db->Put({}, "a", "1").ok());
  ASSERT_TRUE(db->Put({}, "b", "2").ok());
  ASSERT_TRUE(db->Put({}, "a", "3").ok());
  ASSERT_TRUE(db->Delete({}, "b").ok());
  EXPECT_EQ(db->Get("a").value(), "3");
  EXPECT_TRUE(db->Get("b").status().IsNotFound());
  EXPECT_TRUE(db->Get("zzz").status().IsNotFound());
}

TEST(LsmDbTest, RecoversFromWalAfterReopen) {
  TempDir dir;
  {
    auto db = OpenOrDie(SmallOptions(), dir.path());
    for (int i = 0; i < 500; ++i)
      ASSERT_TRUE(db->Put({}, "k" + std::to_string(i), "v" + std::to_string(i)).ok());
    ASSERT_TRUE(db->Delete({}, "k7").ok());
  }  // no flush: everything lives in the WAL
  auto db = OpenOrDie(SmallOptions(), dir.path());
  EXPECT_EQ(db->Get("k499").value(), "v499");
  EXPECT_TRUE(db->Get("k7").status().IsNotFound());
  EXPECT_GE(db->Stats().level_files[0], 1U) << "recovery flushes the replayed WAL into L0";
}

TEST(LsmDbTest, FlushAndCompactionsKeepEveryKeyReadable) {
  TempDir dir;
  auto db = OpenOrDie(SmallOptions(), dir.path());
  std::mt19937_64 rng(4);
  std::map<std::string, std::string> model;
  for (int i = 0; i < 20000; ++i) {
    const std::string k = "user" + std::to_string(rng() % 4000);
    const std::string v = testing::RandomString(rng, 60);
    ASSERT_TRUE(db->Put({}, k, v).ok());
    model[k] = v;
  }
  ASSERT_TRUE(db->Flush().ok());
  ASSERT_TRUE(db->WaitForCompactions().ok());
  const DbStats s = db->Stats();
  EXPECT_GT(s.flushes, 5U);
  EXPECT_GT(s.compactions, 0U);
  EXPECT_GT(s.level_files[1] + s.level_files[2], 0U) << s.levels;
  for (const auto& [k, v] : model) ASSERT_EQ(db->Get(k).value(), v) << k;
  EXPECT_EQ(Scan(db.get()), model);
  // Overwrites were compacted away: the tables hold close to one entry per key.
  EXPECT_LT(s.table_entries, model.size() * 3) << s.levels;
}

TEST(LsmDbTest, IteratorIsASnapshot) {
  TempDir dir;
  auto db = OpenOrDie(SmallOptions(), dir.path());
  ASSERT_TRUE(db->Put({}, "a", "old").ok());
  ASSERT_TRUE(db->Put({}, "b", "old").ok());
  auto it = db->NewIterator();
  ASSERT_TRUE(db->Put({}, "a", "new").ok());
  ASSERT_TRUE(db->Delete({}, "b").ok());
  ASSERT_TRUE(db->Put({}, "c", "new").ok());
  ASSERT_TRUE(db->Flush().ok());  // even across a flush
  std::map<std::string, std::string> seen;
  for (it->SeekToFirst(); it->Valid(); it->Next()) seen.emplace(it->key(), it->value());
  EXPECT_EQ(seen, (std::map<std::string, std::string>{{"a", "old"}, {"b", "old"}}));
  EXPECT_EQ(Scan(db.get()), (std::map<std::string, std::string>{{"a", "new"}, {"c", "new"}}));
}

TEST(LsmDbTest, SeekLandsOnFirstKeyAtOrAfterTarget) {
  TempDir dir;
  auto db = OpenOrDie(SmallOptions(), dir.path());
  for (const char* k : {"apple", "banana", "cherry"}) ASSERT_TRUE(db->Put({}, k, k).ok());
  ASSERT_TRUE(db->Delete({}, "banana").ok());
  auto it = db->NewIterator();
  it->Seek("b");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "cherry");
}

// Randomized differential test against std::map, with reopen, flush and
// compaction mixed in. Any divergence prints the seed for exact replay.
TEST(LsmDbTest, DifferentialAgainstStdMap) {
  for (uint64_t seed = 1; seed <= 4; ++seed) {
    TempDir dir;
    std::mt19937_64 rng(seed);
    std::map<std::string, std::string> model;
    auto db = OpenOrDie(SmallOptions(), dir.path());
    for (int op = 0; op < 6000; ++op) {
      const uint64_t dice = rng() % 100;
      const std::string k = "k" + std::to_string(rng() % 800);
      if (dice < 60) {
        const std::string v = testing::RandomString(rng, rng() % 200);
        ASSERT_TRUE(db->Put({}, k, v).ok());
        model[k] = v;
      } else if (dice < 80) {
        ASSERT_TRUE(db->Delete({}, k).ok());
        model.erase(k);
      } else if (dice < 97) {
        auto got = db->Get(k);
        auto want = model.find(k);
        if (want == model.end()) {
          ASSERT_TRUE(got.status().IsNotFound()) << "seed " << seed << " op " << op << " key " << k;
        } else {
          ASSERT_TRUE(got.ok()) << "seed " << seed << " op " << op << " " << got.status();
          ASSERT_EQ(*got, want->second) << "seed " << seed << " op " << op;
        }
      } else if (dice < 98) {
        ASSERT_TRUE(db->Flush().ok());
      } else if (dice < 99) {
        ASSERT_TRUE(db->WaitForCompactions().ok());
      } else {
        db.reset();
        db = OpenOrDie(SmallOptions(), dir.path());
      }
    }
    ASSERT_EQ(Scan(db.get()), model) << "seed " << seed;
  }
}

// Power-loss durability: everything acknowledged with sync=true survives a
// crash that drops all unsynced bytes. Unsynced writes may be lost, but the
// DB must still open cleanly and show a consistent prefix of them.
TEST(LsmDbTest, SyncedWritesSurvivePowerLoss) {
  TempDir dir;
  FaultInjectionEnv env(Env::Default());
  const Options o = SmallOptions(&env);
  std::map<std::string, std::string> durable;
  {
    auto db = OpenOrDie(o, dir.path());
    for (int i = 0; i < 3000; ++i) {
      const std::string k = "key" + std::to_string(i);
      const bool sync = i % 10 == 0;
      ASSERT_TRUE(db->Put(WriteOptions{.sync = sync}, k, "v" + std::to_string(i)).ok());
      if (sync) {
        // A synced write also makes every earlier write durable (same WAL, in order).
        for (int j = 0; j <= i; ++j) durable["key" + std::to_string(j)] = "v" + std::to_string(j);
      }
    }
    // Crash: let background work go idle, then drop every unsynced byte.
    // Closing afterwards writes nothing (the WAL buffer is already flushed
    // to the OS, and shutdown never flushes the memtable), so what
    // remains on disk is exactly what a power cut would leave.
    ASSERT_TRUE(db->WaitForCompactions().ok());
    env.DropUnsyncedData();
  }
  auto db = OpenOrDie(SmallOptions(), dir.path());
  for (const auto& [k, v] : durable) ASSERT_EQ(db->Get(k).value(), v) << k;
}

TEST(LsmDbTest, ConcurrentWritersAreGroupCommitted) {
  TempDir dir;
  auto db = OpenOrDie(SmallOptions(), dir.path());
  constexpr int kThreads = 8;
  constexpr int kPerThread = 2000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&db, t] {
      for (int i = 0; i < kPerThread; ++i) {
        ASSERT_TRUE(db->Put({}, "t" + std::to_string(t) + "-" + std::to_string(i), "x").ok());
      }
    });
  }
  // Concurrent readers on a moving target.
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    while (!stop.load()) {
      auto r = db->Get("t0-0");
      ASSERT_TRUE(r.ok() || r.status().IsNotFound());
    }
  });
  for (auto& t : threads) t.join();
  stop = true;
  reader.join();
  const DbStats s = db->Stats();
  EXPECT_EQ(s.writes, static_cast<uint64_t>(kThreads * kPerThread));
  EXPECT_LT(s.write_groups, s.writes) << "followers should ride along in a leader's WAL record";
  for (int t = 0; t < kThreads; ++t) ASSERT_TRUE(db->Get("t" + std::to_string(t) + "-1999").ok());
}

TEST(LsmDbTest, SecondOpenOfSameDirectoryIsRefused) {
  TempDir dir;
  auto db = OpenOrDie(SmallOptions(), dir.path());
  auto again = DB::Open(SmallOptions(), dir.path());
  EXPECT_EQ(again.status().code(), StatusCode::kFailedPrecondition);
}

TEST(LsmEngineTest, ImplementsTheEngineInterface) {
  TempDir dir;
  auto engine = NewLsmEngine(SmallOptions(), dir.path());
  ASSERT_TRUE(engine.ok());
  Engine& e = **engine;
  EXPECT_EQ(e.Name(), "lsm");
  ASSERT_TRUE(e.Put("x", "1").ok());
  ASSERT_TRUE(e.Put("y", "2").ok());
  ASSERT_TRUE(e.Delete("x").ok());
  EXPECT_TRUE(e.Get("x").status().IsNotFound());
  auto it = e.NewIterator();
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "y");
}

}  // namespace
}  // namespace cartwheel::storage::lsm
