#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "common/status.h"

namespace cartwheel::storage::lsm {

// Forward iterator over internal keys (user_key || seq || type). The memtable,
// SSTable blocks, whole tables, and levels all implement it, and
// MergingIterator combines them into one sorted stream. Only forward
// iteration exists: the store's consumers (point reads, range scans,
// Merkle-tree builds in M3) never need Prev().
class InternalIterator {
 public:
  virtual ~InternalIterator() = default;
  [[nodiscard]] virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void Seek(std::string_view internal_target) = 0;  // first key >= target
  virtual void Next() = 0;
  [[nodiscard]] virtual std::string_view key() const = 0;
  [[nodiscard]] virtual std::string_view value() const = 0;
  [[nodiscard]] virtual Status status() const = 0;
};

// Merges N sorted children into one sorted stream (min-heap, O(log N) per
// step). Equal internal keys never occur, because sequence numbers are unique.
std::unique_ptr<InternalIterator> NewMergingIterator(
    std::vector<std::unique_ptr<InternalIterator>> children);

// Always-empty iterator carrying a status (e.g. a table that failed to open).
std::unique_ptr<InternalIterator> NewErrorIterator(Status s);

}  // namespace cartwheel::storage::lsm
