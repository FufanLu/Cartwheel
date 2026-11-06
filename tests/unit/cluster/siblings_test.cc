#include "cluster/siblings.h"

#include <random>

#include <gtest/gtest.h>

namespace cartwheel::cluster {
namespace {

std::vector<std::string> Values(const SiblingSet& s) {
  std::vector<std::string> out;
  for (const auto& sib : s.siblings()) out.push_back(sib.tombstone ? "<del>" : sib.value);
  return out;
}

TEST(SiblingSetTest, SequentialWritesReplace) {
  SiblingSet s;
  s.Update("n1", {}, "v1", false, 1);
  s.Update("n1", s.Join(), "v2", false, 2);
  s.Update("n1", s.Join(), "v3", false, 3);
  EXPECT_EQ(Values(s), (std::vector<std::string>{"v3"}));
  EXPECT_EQ(s.Join().ToString(), "{n1:3}");
}

// The case plain vector clocks get wrong: two clients read the same version
// and write through the same coordinator. Both writes must survive.
TEST(SiblingSetTest, ConcurrentWritesSameCoordinatorBothSurvive) {
  SiblingSet s;
  s.Update("n1", {}, "base", false, 1);
  const VectorClock read_by_both = s.Join();  // {n1:1}

  s.Update("n1", read_by_both, "alice", false, 2);  // dot (n1,2)
  s.Update("n1", read_by_both, "bob", false, 3);    // dot (n1,3)

  EXPECT_EQ(Values(s), (std::vector<std::string>{"alice", "bob"}));
  EXPECT_EQ(s.siblings()[0].dot, (Dot{"n1", 2}));
  EXPECT_EQ(s.siblings()[1].dot, (Dot{"n1", 3}));

  // A writer who read both siblings reconciles them into one.
  s.Update("n1", s.Join(), "alice+bob", false, 4);
  EXPECT_EQ(Values(s), (std::vector<std::string>{"alice+bob"}));
}

TEST(SiblingSetTest, BlindWriteBecomesSibling) {
  SiblingSet s;
  s.Update("n1", {}, "a", false, 1);
  s.Update("n2", {}, "b", false, 2);  // empty context: saw nothing
  EXPECT_EQ(s.LiveCount(), 2U);
}

TEST(SiblingSetTest, DeleteIsATombstoneVersion) {
  SiblingSet s;
  s.Update("n1", {}, "a", false, 1);
  s.Update("n1", s.Join(), "", true, 2);
  EXPECT_TRUE(s.IsDeleted());
  EXPECT_EQ(s.siblings().size(), 1U);

  // A concurrent write that did not see the delete survives next to it.
  SiblingSet other;
  other.Update("n2", {}, "late", false, 3);
  s.Sync(other);
  EXPECT_FALSE(s.IsDeleted());
  EXPECT_EQ(Values(s), (std::vector<std::string>{"<del>", "late"}));
}

TEST(SiblingSetTest, SyncDropsWhatOtherSideSuperseded) {
  // Replica A and B both start with v1 from n1.
  SiblingSet a;
  a.Update("n1", {}, "v1", false, 1);
  SiblingSet b = a;

  // B gets an update that read v1.
  b.Update("n2", b.Join(), "v2", false, 2);
  a.Sync(b);
  EXPECT_EQ(Values(a), (std::vector<std::string>{"v2"}));
}

TEST(SiblingSetTest, SyncIsCommutativeAssociativeIdempotent) {
  std::mt19937_64 rng(11);
  const std::vector<std::string> nodes = {"n1", "n2", "n3"};
  for (int trial = 0; trial < 300; ++trial) {
    // Three replicas take random writes, some reading their local state.
    std::vector<SiblingSet> r(3);
    for (int step = 0; step < 12; ++step) {
      const size_t i = rng() % 3;
      const VectorClock ctx = (rng() % 3 == 0) ? VectorClock{} : r[i].Join();
      r[i].Update(nodes[i], ctx, "w" + std::to_string(step), rng() % 5 == 0, step);
      if (rng() % 4 == 0) r[(i + 1) % 3].Sync(r[i]);  // partial replication
    }
    SiblingSet ab = r[0];
    ab.Sync(r[1]);
    SiblingSet ba = r[1];
    ba.Sync(r[0]);
    ASSERT_EQ(ab.Encode(), ba.Encode()) << "commutative, trial " << trial;

    SiblingSet ab_c = ab;
    ab_c.Sync(r[2]);
    SiblingSet bc = r[1];
    bc.Sync(r[2]);
    SiblingSet a_bc = r[0];
    a_bc.Sync(bc);
    ASSERT_EQ(ab_c.Encode(), a_bc.Encode()) << "associative, trial " << trial;

    SiblingSet again = ab_c;
    again.Sync(ab_c);
    ASSERT_EQ(again.Encode(), ab_c.Encode()) << "idempotent, trial " << trial;
  }
}

TEST(SiblingSetTest, EncodeDecodeRoundTrip) {
  SiblingSet s;
  s.Update("n1", {}, "base", false, 1);
  const VectorClock ctx = s.Join();
  s.Update("n1", ctx, "x", false, 2);
  s.Update("n2", ctx, "", true, 3);
  auto decoded = SiblingSet::Decode(s.Encode());
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(decoded->Encode(), s.Encode());
  EXPECT_EQ(decoded->ToString(), s.ToString());
}

TEST(SiblingSetTest, DecodeRejectsGarbage) {
  EXPECT_TRUE(SiblingSet::Decode("").status().IsCorruption());
  EXPECT_TRUE(SiblingSet::Decode("\x09").status().IsCorruption());
  SiblingSet s;
  s.Update("n1", {}, "v", false, 1);
  std::string enc = s.Encode();
  enc.pop_back();
  EXPECT_TRUE(SiblingSet::Decode(enc).status().IsCorruption());
  EXPECT_TRUE(SiblingSet::Decode(s.Encode() + "x").status().IsCorruption());
}

}  // namespace
}  // namespace cartwheel::cluster
