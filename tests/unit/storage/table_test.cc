#include "storage/lsm/table.h"

#include <atomic>
#include <map>
#include <random>
#include <set>
#include <thread>

#include <gtest/gtest.h>

#include "storage/lsm/block.h"
#include "storage/lsm/bloom.h"
#include "storage/lsm/memtable.h"
#include "storage/lsm/skiplist.h"
#include "storage_test_util.h"

namespace cartwheel::storage::lsm {
namespace {

using testing::TempDir;

struct IntCmp {
  int operator()(uint64_t a, uint64_t b) const { return a < b ? -1 : (a > b ? 1 : 0); }
};

TEST(SkipListTest, InsertContainsAndOrderedIteration) {
  Arena arena;
  SkipList<uint64_t, IntCmp> list(IntCmp{}, &arena);
  std::mt19937_64 rng(1);
  std::set<uint64_t> model;
  for (int i = 0; i < 5000; ++i) {
    const uint64_t k = rng() % 100000;
    if (model.insert(k).second) list.Insert(k);
  }
  for (const uint64_t k : model) ASSERT_TRUE(list.Contains(k));
  EXPECT_FALSE(list.Contains(100001));
  SkipList<uint64_t, IntCmp>::Iterator it(&list);
  it.SeekToFirst();
  for (const uint64_t k : model) {
    ASSERT_TRUE(it.Valid());
    ASSERT_EQ(it.key(), k);
    it.Next();
  }
  EXPECT_FALSE(it.Valid());
  it.Seek(50000);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), *model.lower_bound(50000));
}

// One writer, four lock-free readers. Readers must only ever see a sorted
// list whose length never shrinks (TSan checks the memory ordering).
TEST(SkipListTest, ConcurrentReadersSeeConsistentPrefix) {
  Arena arena;
  SkipList<uint64_t, IntCmp> list(IntCmp{}, &arena);
  std::atomic<bool> done{false};
  std::atomic<uint64_t> inserted{0};
  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      while (!done.load(std::memory_order_acquire)) {
        const uint64_t floor = inserted.load(std::memory_order_acquire);
        SkipList<uint64_t, IntCmp>::Iterator it(&list);
        uint64_t prev = 0;
        uint64_t n = 0;
        for (it.SeekToFirst(); it.Valid(); it.Next()) {
          ASSERT_GE(it.key(), prev);
          prev = it.key();
          ++n;
        }
        ASSERT_GE(n, floor);
      }
    });
  }
  for (uint64_t i = 1; i <= 20000; ++i) {
    list.Insert(i * 7919 % 1000003);
    inserted.store(i, std::memory_order_release);
  }
  done.store(true, std::memory_order_release);
  for (auto& t : readers) t.join();
}

TEST(MemTableTest, NewestVisibleVersionWins) {
  auto mem = std::make_shared<MemTable>();
  mem->Add(1, ValueType::kValue, "k", "v1");
  mem->Add(2, ValueType::kValue, "k", "v2");
  mem->Add(3, ValueType::kDeletion, "k", "");
  std::string v;
  EXPECT_EQ(mem->Get(LookupKey("k", 1), &v), MemTable::GetResult::kFound);
  EXPECT_EQ(v, "v1");
  EXPECT_EQ(mem->Get(LookupKey("k", 2), &v), MemTable::GetResult::kFound);
  EXPECT_EQ(v, "v2");
  EXPECT_EQ(mem->Get(LookupKey("k", 3), &v), MemTable::GetResult::kDeleted);
  EXPECT_EQ(mem->Get(LookupKey("j", 3), &v), MemTable::GetResult::kNotFound);
  EXPECT_EQ(mem->Get(LookupKey("ka", 3), &v), MemTable::GetResult::kNotFound);
}

TEST(BlockTest, PrefixCompressionRoundTripAndSeek) {
  BlockBuilder b(4);
  std::vector<std::string> keys;
  keys.reserve(100);
  for (int i = 0; i < 100; ++i) {
    keys.push_back(MakeInternalKey("user:" + std::to_string(1000 + i), 7, ValueType::kValue));
  }
  for (const auto& k : keys) b.Add(k, "v" + std::string(ExtractUserKey(k)));
  auto block = std::make_shared<const Block>(std::string(b.Finish()));
  EXPECT_EQ(block->num_restarts(), 25U);
  auto it = Block::NewIterator(block);
  it->SeekToFirst();
  for (const auto& k : keys) {
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(it->key(), k);
    it->Next();
  }
  EXPECT_FALSE(it->Valid());
  it->Seek(MakeInternalKey("user:1050", kMaxSequence, ValueType::kValue));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ExtractUserKey(it->key()), "user:1050");
  it->Seek(MakeInternalKey("user:9999", kMaxSequence, ValueType::kValue));
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

// Regression for a block_fuzz finding: a restart offset past the restart
// array used to underflow a length and read out of bounds.
TEST(BlockTest, CorruptRestartOffsetIsCorruptionNotCrash) {
  BlockBuilder b(2);
  for (int i = 0; i < 6; ++i)
    b.Add(MakeInternalKey("k" + std::to_string(i), 1, ValueType::kValue), "v");
  std::string bytes(b.Finish());
  // Point the second restart far beyond the data.
  const size_t num_restarts = DecodeFixed32(bytes.data() + bytes.size() - 4);
  EncodeFixed32(bytes.data() + bytes.size() - 4 - 4 * (num_restarts - 1), 0xFFFFFF00U);
  auto it = Block::NewIterator(std::make_shared<const Block>(bytes));
  it->Seek(MakeInternalKey("k5", kMaxSequence, ValueType::kValue));
  EXPECT_TRUE(it->status().IsCorruption());
  auto tiny = Block::NewIterator(std::make_shared<const Block>(std::string("\x01", 1)));
  tiny->SeekToFirst();
  EXPECT_FALSE(tiny->Valid());
}

TEST(BloomTest, NoFalseNegativesAndLowFalsePositives) {
  BloomFilterBuilder b(10);
  for (int i = 0; i < 10000; ++i) b.AddKey("present-" + std::to_string(i));
  const std::string filter = b.Finish();
  for (int i = 0; i < 10000; ++i)
    ASSERT_TRUE(BloomMayContain(filter, "present-" + std::to_string(i)));
  int fp = 0;
  for (int i = 0; i < 100000; ++i)
    fp += BloomMayContain(filter, "absent-" + std::to_string(i)) ? 1 : 0;
  const double rate = fp / 100000.0;
  EXPECT_LT(rate, 0.02) << "false-positive rate " << rate;
  RecordProperty("bloom_fp_rate", std::to_string(rate));
}

class TableTest : public ::testing::Test {
 protected:
  std::shared_ptr<Table> Build(const std::map<std::string, std::string>& kv, BlockCache* cache) {
    const std::string path = dir_.File("000009.sst");
    {
      auto f = Env::Default()->NewWritableFile(path);
      EXPECT_TRUE(f.ok());
      TableBuilder tb(TableOptions{}, f->get());
      SequenceNumber seq = 1;
      for (const auto& [k, v] : kv) tb.Add(MakeInternalKey(k, seq++, ValueType::kValue), v);
      EXPECT_TRUE(tb.Finish().ok());
      EXPECT_TRUE((*f)->Close().ok());
    }
    auto file = Env::Default()->NewRandomAccessFile(path);
    EXPECT_TRUE(file.ok());
    auto t = Table::Open(std::move(file).value(), 9, cache);
    EXPECT_TRUE(t.ok()) << t.status();
    return t.ok() ? std::move(t).value() : nullptr;
  }
  TempDir dir_;
};

TEST_F(TableTest, PointLookupsScanAndCache) {
  std::map<std::string, std::string> kv;
  std::mt19937_64 rng(9);
  for (int i = 0; i < 20000; ++i)
    kv["key-" + testing::RandomString(rng, 12)] = testing::RandomString(rng, 50);
  BlockCache cache(1 << 20);
  auto table = Build(kv, &cache);
  ASSERT_NE(table, nullptr);

  std::string v;
  for (const auto& [k, want] : kv) {
    auto r = table->Get(LookupKey(k, kMaxSequence), &v);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(*r, MemTable::GetResult::kFound) << k;
    ASSERT_EQ(v, want);
  }
  for (int i = 0; i < 1000; ++i) {
    auto r = table->Get(LookupKey("missing-" + std::to_string(i), kMaxSequence), &v);
    ASSERT_EQ(*r, MemTable::GetResult::kNotFound);
  }
  EXPECT_GT(table->bloom_skips(), 950U) << "bloom filter should reject most misses without IO";
  EXPECT_GT(cache.hits(), 0U);

  auto it = Table::NewIterator(table);
  auto expect = kv.begin();
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++expect) {
    ASSERT_EQ(ExtractUserKey(it->key()), expect->first);
    ASSERT_EQ(it->value(), expect->second);
  }
  EXPECT_EQ(expect, kv.end());
}

TEST_F(TableTest, DetectsCorruptBlock) {
  std::map<std::string, std::string> kv;
  for (int i = 0; i < 2000; ++i) kv["k" + std::to_string(10000 + i)] = std::string(100, 'v');
  auto table = Build(kv, nullptr);
  ASSERT_NE(table, nullptr);
  {
    std::FILE* f = std::fopen(dir_.File("000009.sst").c_str(), "r+b");
    std::fseek(f, 200, SEEK_SET);
    std::fputc(0x7F, f);
    std::fclose(f);
  }
  auto file = Env::Default()->NewRandomAccessFile(dir_.File("000009.sst"));
  auto reopened = Table::Open(std::move(file).value(), 10, nullptr);
  ASSERT_TRUE(reopened.ok());
  std::string v;
  auto r = (*reopened)->Get(LookupKey("k10000", kMaxSequence), &v);
  EXPECT_TRUE(r.status().IsCorruption()) << r.status();
}

}  // namespace
}  // namespace cartwheel::storage::lsm
