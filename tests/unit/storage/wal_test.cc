#include "storage/wal.h"

#include <fstream>
#include <random>

#include <gtest/gtest.h>

#include "storage/crc32c.h"
#include "storage_test_util.h"

namespace cartwheel::storage {
namespace {

using testing::TempDir;

TEST(Crc32cTest, KnownVectors) {
  EXPECT_EQ(crc32c::Value("123456789"), 0xE3069283U);  // the standard check value
  EXPECT_EQ(crc32c::Value(""), 0U);
  const std::string zeros(32, '\0');
  EXPECT_EQ(crc32c::Value(zeros), 0x8A9136AAU);  // RFC 3720 B.4
}

TEST(Crc32cTest, HardwareMatchesPortable) {
  std::mt19937_64 rng(3);
  for (int i = 0; i < 2000; ++i) {
    std::string s(rng() % 300, '\0');
    for (auto& c : s) c = static_cast<char>(rng());
    ASSERT_EQ(crc32c::Value(s), crc32c::ExtendPortable(0, s.data(), s.size()))
        << "len " << s.size();
  }
}

TEST(Crc32cTest, ExtendIsConcatenation) {
  EXPECT_EQ(crc32c::Value("hello world"), crc32c::Extend(crc32c::Value("hello "), "world", 5));
}

TEST(Crc32cTest, MaskRoundTrip) {
  const uint32_t c = crc32c::Value("foo");
  EXPECT_NE(crc32c::Mask(c), c);
  EXPECT_EQ(crc32c::Unmask(crc32c::Mask(c)), c);
}

class WalTest : public ::testing::Test {
 protected:
  void Write(const std::vector<std::string>& records) {
    auto f = Env::Default()->NewWritableFile(path_);
    ASSERT_TRUE(f.ok());
    wal::Writer w(std::move(f).value());
    for (const auto& r : records) ASSERT_TRUE(w.AddRecord(r).ok());
    ASSERT_TRUE(w.Close().ok());
  }

  std::vector<std::string> ReadAll(wal::ReadStats* stats = nullptr, bool paranoid = false) {
    auto f = Env::Default()->NewSequentialFile(path_);
    EXPECT_TRUE(f.ok());
    wal::Reader r(std::move(f).value());
    std::vector<std::string> out;
    std::string rec;
    while (r.ReadRecord(&rec, paranoid)) out.push_back(rec);
    if (stats != nullptr) *stats = r.stats();
    return out;
  }

  void CorruptByte(uint64_t offset) {
    std::fstream f(path_, std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(static_cast<std::streamoff>(offset));
    char c = 0;
    f.read(&c, 1);
    c = static_cast<char>(c ^ 0x5A);
    f.seekp(static_cast<std::streamoff>(offset));
    f.write(&c, 1);
  }

  void Truncate(uint64_t size) {
    ASSERT_EQ(::truncate(path_.c_str(), static_cast<off_t>(size)), 0);
  }

  TempDir dir_;
  std::string path_ = dir_.File("000001.log");
};

TEST_F(WalTest, RoundTripSmallLargeAndEmpty) {
  const std::vector<std::string> records = {"a", "", std::string(100'000, 'x'), "tail",
                                            std::string(wal::kBlockSize - wal::kHeaderSize, 'y')};
  Write(records);
  EXPECT_EQ(ReadAll(), records);
}

TEST_F(WalTest, BlockPaddingWhenHeaderDoesNotFit) {
  // Leave exactly 3 bytes in the first block: the writer must pad, not split a header.
  const std::string first(wal::kBlockSize - wal::kHeaderSize - 3, 'p');
  Write({first, "next"});
  EXPECT_EQ(ReadAll(), (std::vector<std::string>{first, "next"}));
}

TEST_F(WalTest, CorruptionSkipsOnlyTheDamagedBlock) {
  std::vector<std::string> records;
  records.reserve(2000);
  for (int i = 0; i < 2000; ++i)
    records.push_back("record-" + std::to_string(i) + std::string(40, 'z'));
  Write(records);
  CorruptByte(wal::kBlockSize + 100);  // inside the second block
  wal::ReadStats stats;
  const auto got = ReadAll(&stats);
  EXPECT_GT(stats.dropped_bytes, 0U);
  EXPECT_LE(stats.dropped_bytes, wal::kBlockSize) << "damage is contained to one 32 KiB block";
  // Each record is ~58 bytes on disk, so one block holds at most ~565 of them.
  const size_t lost = records.size() - got.size();
  EXPECT_GT(lost, 0U);
  EXPECT_LE(lost, wal::kBlockSize / 50 + 2);
  EXPECT_EQ(got.front(), records.front());
  EXPECT_EQ(got.back(), records.back());
}

TEST_F(WalTest, ParanoidStopsAtCorruption) {
  Write({"one", "two", "three"});
  CorruptByte(wal::kHeaderSize + 1);  // payload of the first record
  wal::ReadStats stats;
  EXPECT_TRUE(ReadAll(&stats, /*paranoid=*/true).empty());
}

TEST_F(WalTest, TornTailIsIgnoredNotCorruption) {
  Write({"committed-1", "committed-2", std::string(50'000, 'q')});
  const uint64_t size = Env::Default()->FileSize(path_).value();
  Truncate(size - 1000);  // power lost in the middle of the last record
  wal::ReadStats stats;
  EXPECT_EQ(ReadAll(&stats), (std::vector<std::string>{"committed-1", "committed-2"}));
  EXPECT_TRUE(stats.truncated_tail);
}

TEST_F(WalTest, AppendAfterReopenContinuesBlockAlignment) {
  Write({"before"});
  {
    auto f = Env::Default()->NewAppendableFile(path_);
    ASSERT_TRUE(f.ok());
    const uint64_t len = (*f)->Size();
    wal::Writer w(std::move(f).value(), len);
    ASSERT_TRUE(w.AddRecord(std::string(wal::kBlockSize, 'b')).ok());
    ASSERT_TRUE(w.Close().ok());
  }
  const auto got = ReadAll();
  ASSERT_EQ(got.size(), 2U);
  EXPECT_EQ(got[1].size(), wal::kBlockSize);
}

}  // namespace
}  // namespace cartwheel::storage
