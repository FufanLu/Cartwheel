#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace cartwheel::cluster {

// 64-bit hash used for ring placement: FNV-1a with a murmur3 finalizer for
// avalanche. Stable across platforms and releases, because key placement
// depends on it.
[[nodiscard]] uint64_t RingHash(std::string_view data) noexcept;

// Consistent-hash ring with virtual nodes (Dynamo §4.2).
//
// Each physical node owns `vnodes` tokens at RingHash("node#i"). A key is
// stored on the first N *distinct physical* nodes clockwise from
// RingHash(key): its preference list. Vnodes spread each node's ranges over
// the ring, so adding or removing a node moves only about 1/n of the keys,
// and the moved keys come from all the other nodes rather than one neighbour.
class Ring {
 public:
  explicit Ring(int vnodes_per_node = 64);

  void AddNode(const std::string& node);
  void RemoveNode(const std::string& node);
  [[nodiscard]] bool Contains(const std::string& node) const { return nodes_.contains(node); }
  [[nodiscard]] const std::set<std::string>& nodes() const { return nodes_; }

  // First `n` distinct nodes clockwise from the key's position. Nodes in
  // `skip` are passed over: callers skip dead nodes to build a *sloppy*
  // preference list, and the skipped nodes' data goes to the next healthy
  // ones as hints.
  [[nodiscard]] std::vector<std::string> PreferenceList(
      std::string_view key, size_t n, const std::set<std::string>& skip = {}) const;

  // Partition = index of the key's vnode token, for per-range Merkle trees.
  [[nodiscard]] uint32_t PartitionOf(std::string_view key) const;
  [[nodiscard]] size_t partitions() const { return tokens_.size(); }
  // Nodes responsible for a partition (its first N distinct owners clockwise).
  [[nodiscard]] std::vector<std::string> PartitionReplicas(uint32_t partition, size_t n) const;

  // Fraction of the hash space each node owns as primary (first replica).
  [[nodiscard]] std::map<std::string, double> Ownership() const;

  struct Token {
    uint64_t position;
    std::string node;
  };
  [[nodiscard]] const std::vector<Token>& tokens() const { return tokens_; }

 private:
  void Rebuild();
  [[nodiscard]] size_t TokenIndexFor(uint64_t hash) const;
  [[nodiscard]] std::vector<std::string> WalkFrom(size_t start, size_t n,
                                                  const std::set<std::string>& skip) const;

  int vnodes_;
  std::set<std::string> nodes_;
  std::vector<Token> tokens_;  // sorted by position
};

}  // namespace cartwheel::cluster
