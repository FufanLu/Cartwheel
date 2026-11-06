#include "cluster/vector_clock.h"

#include <gtest/gtest.h>

namespace cartwheel::cluster {
namespace {

using Order = VectorClock::Order;

TEST(VectorClockTest, GetSetAdvance) {
  VectorClock c;
  EXPECT_EQ(c.Get("a"), 0U);
  c.Set("b", 2);
  c.Set("a", 1);
  EXPECT_EQ(c.ToString(), "{a:1, b:2}");  // kept sorted
  c.Advance("a", 5);
  c.Advance("b", 1);  // never lowers
  EXPECT_EQ(c.ToString(), "{a:5, b:2}");
  c.Set("a", 0);
  EXPECT_EQ(c.ToString(), "{b:2}");
}

TEST(VectorClockTest, Compare) {
  const VectorClock a{{"n1", 2}, {"n2", 1}};
  const VectorClock b{{"n1", 2}, {"n2", 2}};
  const VectorClock c{{"n1", 3}};
  EXPECT_EQ(VectorClock::Compare(a, a), Order::kEqual);
  EXPECT_EQ(VectorClock::Compare(a, b), Order::kBefore);
  EXPECT_EQ(VectorClock::Compare(b, a), Order::kAfter);
  EXPECT_EQ(VectorClock::Compare(b, c), Order::kConcurrent);
  EXPECT_EQ(VectorClock::Compare(VectorClock{}, a), Order::kBefore);
}

TEST(VectorClockTest, MergeIsPointwiseMax) {
  VectorClock a{{"n1", 2}, {"n3", 1}};
  const VectorClock b{{"n1", 1}, {"n2", 4}};
  a.Merge(b);
  EXPECT_EQ(a.ToString(), "{n1:2, n2:4, n3:1}");
  EXPECT_TRUE(a.Descends(b));
}

TEST(VectorClockTest, EncodeDecodeRoundTrip) {
  const VectorClock c{{"wheel-1", 7}, {"wheel-3", 300}};
  auto decoded = VectorClock::Decode(c.Encode());
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(*decoded, c);
  EXPECT_TRUE(VectorClock::Decode("").value().empty());
}

TEST(VectorClockTest, DecodeRejectsNonCanonical) {
  std::string unsorted;
  PutVarint64(&unsorted, 2);
  PutLengthPrefixed(&unsorted, "b");
  PutVarint64(&unsorted, 1);
  PutLengthPrefixed(&unsorted, "a");
  PutVarint64(&unsorted, 1);
  EXPECT_TRUE(VectorClock::Decode(unsorted).status().IsCorruption());

  std::string zero;
  PutVarint64(&zero, 1);
  PutLengthPrefixed(&zero, "a");
  PutVarint64(&zero, 0);
  EXPECT_TRUE(VectorClock::Decode(zero).status().IsCorruption());

  EXPECT_TRUE(VectorClock::Decode("\x05").status().IsCorruption());
}

}  // namespace
}  // namespace cartwheel::cluster
