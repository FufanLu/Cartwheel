#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cartwheel::cluster {

// Fixed-shape Merkle tree over one ring partition's keys (Dynamo §4.7).
//
//   level 0: 1 root  ->  level 1: 16 nodes  ->  level 2: 256 leaf buckets
//
// A key goes into leaf RingHash(key) % 256, and a leaf's hash is the sum of
// hash(key || encoded siblings) over its keys. The sum is order-independent,
// so the tree builds in one scan without sorting. Each inner node hashes its
// 16 children. Two replicas compare top-down and descend only into subtrees
// that differ, so syncing one stale key costs 1 + 16 + 16 hashes plus the
// keys of a single bucket, not a full key exchange.
class MerkleTree {
 public:
  static constexpr int kFanout = 16;
  static constexpr int kLevels = 3;
  static constexpr uint32_t kLeaves = 256;

  MerkleTree();
  void Add(std::string_view storage_key, std::string_view value);
  void Finalize();

  [[nodiscard]] uint64_t Hash(int level, uint32_t index) const;
  [[nodiscard]] uint64_t root() const { return Hash(0, 0); }
  [[nodiscard]] size_t keys() const { return keys_; }
  [[nodiscard]] static uint32_t BucketOf(std::string_view storage_key);
  [[nodiscard]] static uint32_t NodesAt(int level);

 private:
  std::array<std::vector<uint64_t>, kLevels> levels_;
  size_t keys_ = 0;
};

}  // namespace cartwheel::cluster
