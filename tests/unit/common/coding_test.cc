#include "common/coding.h"

#include <limits>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace cartwheel {
namespace {

TEST(CodingTest, FixedIsLittleEndian) {
  std::string buf;
  PutFixed32(&buf, 0x04030201);
  PutFixed64(&buf, 0x0807060504030201ULL);
  ASSERT_EQ(buf.size(), 12U);
  EXPECT_EQ(buf.substr(0, 4), std::string("\x01\x02\x03\x04", 4));
  EXPECT_EQ(DecodeFixed32(buf.data()), 0x04030201U);
  EXPECT_EQ(DecodeFixed64(buf.data() + 4), 0x0807060504030201ULL);
}

TEST(CodingTest, VarintRoundTripAndLength) {
  const std::vector<uint64_t> values = {
      0, 1, 127, 128, 300, 16'383, 16'384, UINT32_MAX, 1ULL << 35, UINT64_MAX, UINT64_MAX - 1};
  std::string buf;
  for (const uint64_t v : values) {
    const size_t before = buf.size();
    PutVarint64(&buf, v);
    EXPECT_EQ(static_cast<int>(buf.size() - before), VarintLength(v)) << v;
  }
  Decoder d(buf);
  for (const uint64_t v : values) {
    uint64_t got = 0;
    ASSERT_TRUE(d.GetVarint64(&got));
    EXPECT_EQ(got, v);
  }
  EXPECT_TRUE(d.empty());
}

TEST(CodingTest, VarintKnownBytes) {
  std::string buf;
  PutVarint64(&buf, 300);  // 0b1_0010_1100 -> 0xAC 0x02
  EXPECT_EQ(buf, std::string("\xAC\x02", 2));
}

TEST(CodingTest, TruncatedInputFailsWithoutConsuming) {
  std::string buf;
  PutVarint64(&buf, 1ULL << 40);
  buf.pop_back();
  Decoder d(buf);
  uint64_t v = 0;
  EXPECT_FALSE(d.GetVarint64(&v));
  EXPECT_EQ(d.remaining(), buf.size());

  std::string lp;
  PutLengthPrefixed(&lp, "hello");
  lp.pop_back();
  Decoder d2(lp);
  std::string_view s;
  EXPECT_FALSE(d2.GetLengthPrefixed(&s));
  EXPECT_EQ(d2.remaining(), lp.size());
}

TEST(CodingTest, OverlongVarintRejected) {
  const std::string eleven(11, '\xFF');
  Decoder d(eleven);
  uint64_t v = 0;
  EXPECT_FALSE(d.GetVarint64(&v));

  // 10 bytes where the last carries more than the single remaining bit.
  std::string ten(9, '\xFF');
  ten.push_back('\x02');
  Decoder d2(ten);
  EXPECT_FALSE(d2.GetVarint64(&v));
}

TEST(CodingTest, Varint32RejectsWideValues) {
  std::string buf;
  PutVarint64(&buf, static_cast<uint64_t>(UINT32_MAX) + 1);
  Decoder d(buf);
  uint32_t v = 0;
  EXPECT_FALSE(d.GetVarint32(&v));
  EXPECT_EQ(d.remaining(), buf.size());
}

TEST(CodingTest, LengthPrefixedSequence) {
  std::string buf;
  PutLengthPrefixed(&buf, "cart");
  PutLengthPrefixed(&buf, "");
  PutLengthPrefixed(&buf, std::string(1000, 'x'));
  Decoder d(buf);
  std::string_view a;
  std::string_view b;
  std::string_view c;
  ASSERT_TRUE(d.GetLengthPrefixed(&a));
  ASSERT_TRUE(d.GetLengthPrefixed(&b));
  ASSERT_TRUE(d.GetLengthPrefixed(&c));
  EXPECT_EQ(a, "cart");
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(c.size(), 1000U);
  EXPECT_TRUE(d.empty());
}

TEST(CodingTest, RandomBytesNeverCrashDecoder) {
  std::mt19937_64 rng(5);
  for (int i = 0; i < 20'000; ++i) {
    std::string junk(rng() % 24, '\0');
    for (auto& c : junk) c = static_cast<char>(rng());
    Decoder d(junk);
    uint64_t v = 0;
    std::string_view s;
    while (d.GetVarint64(&v) && d.GetLengthPrefixed(&s)) {
    }
  }
  SUCCEED();
}

}  // namespace
}  // namespace cartwheel
