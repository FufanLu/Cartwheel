#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "storage/lsm/arena.h"
#include "storage/lsm/dbformat.h"
#include "storage/lsm/iterator.h"
#include "storage/lsm/skiplist.h"

namespace cartwheel::storage::lsm {

class MemTableIterator;

// In-memory write buffer: a skiplist of entries laid out in an arena as
//   varint32 ikey_len | internal key | varint32 value_len | value
// One writer (the group-commit leader) inserts; readers are lock-free.
// Shared via shared_ptr: readers and the flush thread keep an immutable
// memtable alive after the DB has switched to a fresh one.
class MemTable {
 public:
  MemTable();
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  void Add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value);

  enum class GetResult : uint8_t { kNotFound, kFound, kDeleted };
  // Newest entry for key.user_key() with sequence <= the lookup's sequence.
  [[nodiscard]] GetResult Get(const LookupKey& key, std::string* value) const;

  // The iterator keeps the memtable alive (it holds a shared_ptr to it).
  [[nodiscard]] static std::unique_ptr<InternalIterator> NewIterator(
      std::shared_ptr<const MemTable> mem);

  [[nodiscard]] size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }
  [[nodiscard]] size_t entries() const { return entries_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::vector<size_t> SkipListLevelCounts() const { return table_.LevelCounts(); }

  // Decodes an arena entry; exposed for the iterator.
  static std::string_view EntryKey(const char* entry);
  static std::string_view EntryValue(const char* entry);

 private:
  friend class MemTableIterator;
  struct EntryComparator {
    int operator()(const char* a, const char* b) const;
  };
  using Table = SkipList<const char*, EntryComparator>;

  Arena arena_;
  Table table_;
  std::atomic<size_t> entries_{0};
};

}  // namespace cartwheel::storage::lsm
