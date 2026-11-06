#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace cartwheel::storage::lsm {

// Bump allocator for memtable entries and skiplist nodes. Everything is freed
// at once when the memtable dies, so there is no per-entry malloc/free and no
// fragmentation. MemoryUsage() is read by other threads to decide when to
// flush, so it is atomic.
//
// Not thread-safe for allocation: only the memtable's single writer allocates.
class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  char* Allocate(size_t bytes);
  char* AllocateAligned(size_t bytes);

  [[nodiscard]] size_t MemoryUsage() const noexcept {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  static constexpr size_t kBlockSize = 4096;
  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t bytes);

  char* alloc_ptr_ = nullptr;
  size_t alloc_bytes_remaining_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<size_t> memory_usage_{0};
};

}  // namespace cartwheel::storage::lsm
