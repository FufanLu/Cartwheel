#include "cluster/merkle.h"

#include "cluster/ring.h"
#include "common/coding.h"

namespace cartwheel::cluster {

uint32_t MerkleTree::NodesAt(int level) {
  uint32_t n = 1;
  for (int i = 0; i < level; ++i) n *= kFanout;
  return n;
}

MerkleTree::MerkleTree() {
  for (int l = 0; l < kLevels; ++l) levels_[static_cast<size_t>(l)].assign(NodesAt(l), 0);
}

uint32_t MerkleTree::BucketOf(std::string_view storage_key) {
  // Mix again so bucket choice is independent of the ring position, which
  // already pinned the key to this partition.
  return static_cast<uint32_t>((RingHash(storage_key) >> 17) % kLeaves);
}

void MerkleTree::Add(std::string_view storage_key, std::string_view value) {
  std::string buf(storage_key);
  buf.push_back('\0');
  buf.append(value);
  levels_[kLevels - 1][BucketOf(storage_key)] += RingHash(buf);
  ++keys_;
}

void MerkleTree::Finalize() {
  for (int l = kLevels - 2; l >= 0; --l) {
    auto& parents = levels_[static_cast<size_t>(l)];
    const auto& children = levels_[static_cast<size_t>(l) + 1];
    for (uint32_t i = 0; i < parents.size(); ++i) {
      std::string buf;
      for (int c = 0; c < kFanout; ++c)
        PutFixed64(&buf, children[i * kFanout + static_cast<uint32_t>(c)]);
      parents[i] = RingHash(buf);
    }
  }
}

uint64_t MerkleTree::Hash(int level, uint32_t index) const {
  return levels_[static_cast<size_t>(level)][index];
}

}  // namespace cartwheel::cluster
