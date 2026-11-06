#include "cluster/ring.h"

#include <algorithm>
#include <map>
#include <set>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "cluster/merkle.h"

namespace cartwheel::cluster {
namespace {

Ring FiveNodes(int vnodes = 64) {
  Ring r(vnodes);
  for (int i = 1; i <= 5; ++i) r.AddNode(fmt::format("wheel-{}", i));
  return r;
}

TEST(RingTest, PreferenceListIsDistinctAndDeterministic) {
  const Ring a = FiveNodes();
  const Ring b = FiveNodes();
  for (int i = 0; i < 1000; ++i) {
    const auto key = fmt::format("key{}", i);
    const auto pl = a.PreferenceList(key, 3);
    ASSERT_EQ(pl.size(), 3U);
    ASSERT_EQ(std::set<std::string>(pl.begin(), pl.end()).size(), 3U);
    ASSERT_EQ(pl, b.PreferenceList(key, 3)) << "placement must not depend on insertion order";
  }
}

TEST(RingTest, VnodesBalanceOwnership) {
  for (const int vnodes : {1, 8, 64}) {
    const auto share = FiveNodes(vnodes).Ownership();
    double lo = 1;
    double hi = 0;
    double total = 0;
    for (const auto& [n, s] : share) {
      lo = std::min(lo, s);
      hi = std::max(hi, s);
      total += s;
    }
    EXPECT_NEAR(total, 1.0, 1e-9);
    RecordProperty(fmt::format("vnodes_{}_spread", vnodes), fmt::format("{:.3f}-{:.3f}", lo, hi));
    if (vnodes == 64) {
      EXPECT_GT(lo, 0.15) << "with 64 vnodes no node should own much less than 1/5";
      EXPECT_LT(hi, 0.25);
    }
  }
}

TEST(RingTest, AddingANodeMovesAboutOneSixthOfKeysAllToTheNewNode) {
  Ring before = FiveNodes();
  Ring after = FiveNodes();
  after.AddNode("wheel-6");
  int moved = 0;
  constexpr int kKeys = 20000;
  for (int i = 0; i < kKeys; ++i) {
    const auto key = fmt::format("user{}", i);
    const auto p0 = before.PreferenceList(key, 1).front();
    const auto p1 = after.PreferenceList(key, 1).front();
    if (p0 != p1) {
      ++moved;
      ASSERT_EQ(p1, "wheel-6") << "a key may only move to the node that joined";
    }
  }
  const double frac = static_cast<double>(moved) / kKeys;
  EXPECT_NEAR(frac, 1.0 / 6.0, 0.04) << "consistent hashing moves ~1/n of the keys";
}

TEST(RingTest, SkippingDeadNodesExtendsTheListClockwise) {
  const Ring r = FiveNodes();
  const auto home = r.PreferenceList("k", 3);
  const auto sloppy = r.PreferenceList("k", 3, {home[1]});
  ASSERT_EQ(sloppy.size(), 3U);
  EXPECT_EQ(sloppy[0], home[0]);
  EXPECT_EQ(sloppy[1], home[2]);
  EXPECT_EQ(std::count(sloppy.begin(), sloppy.end(), home[1]), 0);
  EXPECT_EQ(std::count(home.begin(), home.end(), sloppy[2]), 0) << "the stand-in is a new node";
}

TEST(RingTest, PartitionReplicasMatchPreferenceList) {
  const Ring r = FiveNodes();
  for (int i = 0; i < 200; ++i) {
    const auto key = fmt::format("k{}", i);
    EXPECT_EQ(r.PartitionReplicas(r.PartitionOf(key), 3), r.PreferenceList(key, 3));
  }
}

TEST(MerkleTest, EqualDataEqualRootRegardlessOfOrder) {
  MerkleTree a;
  MerkleTree b;
  for (int i = 0; i < 500; ++i) a.Add(fmt::format("k{}", i), fmt::format("v{}", i));
  for (int i = 499; i >= 0; --i) b.Add(fmt::format("k{}", i), fmt::format("v{}", i));
  a.Finalize();
  b.Finalize();
  EXPECT_EQ(a.root(), b.root());
}

TEST(MerkleTest, OneStaleKeyChangesExactlyOneLeafAndItsAncestors) {
  MerkleTree a;
  MerkleTree b;
  for (int i = 0; i < 500; ++i) {
    a.Add(fmt::format("k{}", i), "v");
    b.Add(fmt::format("k{}", i), i == 123 ? "stale" : "v");
  }
  a.Finalize();
  b.Finalize();
  EXPECT_NE(a.root(), b.root());
  int diff_leaves = 0;
  int diff_mid = 0;
  for (uint32_t i = 0; i < MerkleTree::kLeaves; ++i) diff_leaves += a.Hash(2, i) != b.Hash(2, i);
  for (uint32_t i = 0; i < MerkleTree::NodesAt(1); ++i) diff_mid += a.Hash(1, i) != b.Hash(1, i);
  EXPECT_EQ(diff_leaves, 1);
  EXPECT_EQ(diff_mid, 1);
  const uint32_t bucket = MerkleTree::BucketOf("k123");
  EXPECT_NE(a.Hash(2, bucket), b.Hash(2, bucket));
}

}  // namespace
}  // namespace cartwheel::cluster
