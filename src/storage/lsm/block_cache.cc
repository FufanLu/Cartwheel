#include "storage/lsm/block_cache.h"

#include <algorithm>

namespace cartwheel::storage::lsm {

BlockCache::BlockCache(size_t capacity_bytes)
    : per_shard_capacity_(std::max<size_t>(1, capacity_bytes / kShards)) {}

std::shared_ptr<const Block> BlockCache::Lookup(uint64_t file, uint64_t offset) {
  const Key k{file, offset};
  Shard& s = ShardFor(k);
  std::lock_guard lock(s.mu);
  auto it = s.index.find(k);
  if (it == s.index.end()) {
    misses_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  s.lru.splice(s.lru.begin(), s.lru, it->second);  // move to front
  hits_.fetch_add(1, std::memory_order_relaxed);
  return it->second->second;
}

void BlockCache::Insert(uint64_t file, uint64_t offset, std::shared_ptr<const Block> block) {
  const Key k{file, offset};
  Shard& s = ShardFor(k);
  const size_t charge = block->size();
  std::lock_guard lock(s.mu);
  if (auto it = s.index.find(k); it != s.index.end()) {
    s.usage -= it->second->second->size();
    s.lru.erase(it->second);
    s.index.erase(it);
  }
  s.lru.emplace_front(k, std::move(block));
  s.index.emplace(k, s.lru.begin());
  s.usage += charge;
  while (s.usage > per_shard_capacity_ && s.lru.size() > 1) {
    auto& victim = s.lru.back();
    s.usage -= victim.second->size();
    s.index.erase(victim.first);
    s.lru.pop_back();
  }
}

void BlockCache::EraseFile(uint64_t file) {
  for (auto& s : shards_) {
    std::lock_guard lock(s.mu);
    for (auto it = s.lru.begin(); it != s.lru.end();) {
      if (it->first.file == file) {
        s.usage -= it->second->size();
        s.index.erase(it->first);
        it = s.lru.erase(it);
      } else {
        ++it;
      }
    }
  }
}

size_t BlockCache::usage() const {
  size_t total = 0;
  for (auto& s : shards_) {
    std::lock_guard lock(s.mu);
    total += s.usage;
  }
  return total;
}

}  // namespace cartwheel::storage::lsm
