#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "storage/lsm/block.h"

namespace cartwheel::storage::lsm {

// Sharded LRU cache of decoded data blocks, keyed by (file number, offset).
//
// 16 shards, each with its own mutex, so readers on different cores rarely
// contend (chapter 00 showed what one hot cache line costs). Entries are
// shared_ptr<const Block>: an iterator holding a block keeps it alive even
// after eviction, so eviction never invalidates a reader.
class BlockCache {
 public:
  explicit BlockCache(size_t capacity_bytes);

  [[nodiscard]] std::shared_ptr<const Block> Lookup(uint64_t file, uint64_t offset);
  void Insert(uint64_t file, uint64_t offset, std::shared_ptr<const Block> block);
  void EraseFile(uint64_t file);  // after compaction deletes a table

  [[nodiscard]] uint64_t hits() const { return hits_.load(std::memory_order_relaxed); }
  [[nodiscard]] uint64_t misses() const { return misses_.load(std::memory_order_relaxed); }
  [[nodiscard]] size_t usage() const;

 private:
  static constexpr size_t kShards = 16;
  struct Key {
    uint64_t file;
    uint64_t offset;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    size_t operator()(const Key& k) const noexcept {
      return std::hash<uint64_t>{}(k.file * 0x9E3779B97F4A7C15ULL ^ k.offset);
    }
  };
  struct Shard {
    std::mutex mu;
    std::list<std::pair<Key, std::shared_ptr<const Block>>> lru;  // front = most recent
    std::unordered_map<Key, decltype(lru)::iterator, KeyHash> index;
    size_t usage = 0;
  };
  Shard& ShardFor(const Key& k) { return shards_[KeyHash{}(k) % kShards]; }

  const size_t per_shard_capacity_;
  mutable std::array<Shard, kShards> shards_;
  std::atomic<uint64_t> hits_{0};
  std::atomic<uint64_t> misses_{0};
};

}  // namespace cartwheel::storage::lsm
