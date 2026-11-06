#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/status.h"
#include "storage/lsm/dbformat.h"

namespace cartwheel::storage::lsm {

class MemTable;

// An atomic group of updates and the exact payload of one WAL record:
//
//   fixed64 sequence | fixed32 count | { u8 type | lp(key) [| lp(value)] }*
//
// The group-commit leader concatenates many callers' batches into one, writes
// them as a single WAL record (one write, at most one fsync), then applies
// them to the memtable with consecutive sequence numbers.
class WriteBatch {
 public:
  WriteBatch();

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();
  void Append(const WriteBatch& other);

  [[nodiscard]] uint32_t Count() const;
  [[nodiscard]] size_t ByteSize() const { return rep_.size(); }

  [[nodiscard]] SequenceNumber Sequence() const;
  void SetSequence(SequenceNumber seq);

  [[nodiscard]] std::string_view Contents() const { return rep_; }
  [[nodiscard]] static Result<WriteBatch> FromContents(std::string_view contents);

  // Calls fn(type, key, value) per record in order; Corruption on bad bytes.
  template <typename Fn>
  Status Iterate(Fn&& fn) const;

  // Inserts every record into `mem` with sequence numbers Sequence() + i.
  Status InsertInto(MemTable* mem) const;

 private:
  static constexpr size_t kHeader = 12;
  void SetCount(uint32_t n);
  std::string rep_;
};

template <typename Fn>
Status WriteBatch::Iterate(Fn&& fn) const {
  if (rep_.size() < kHeader) return Status::Corruption("write batch: too small");
  Decoder d(std::string_view(rep_).substr(kHeader));
  uint32_t found = 0;
  while (!d.empty()) {
    std::string_view tag;
    std::string_view key;
    std::string_view value;
    if (!d.GetBytes(1, &tag) || !d.GetLengthPrefixed(&key)) {
      return Status::Corruption("write batch: truncated record");
    }
    const auto type = static_cast<ValueType>(tag[0]);
    if (type == ValueType::kValue) {
      if (!d.GetLengthPrefixed(&value)) return Status::Corruption("write batch: truncated value");
    } else if (type != ValueType::kDeletion) {
      return Status::Corruption("write batch: unknown record type");
    }
    fn(type, key, value);
    ++found;
  }
  if (found != Count()) return Status::Corruption("write batch: count mismatch");
  return Status::Ok();
}

}  // namespace cartwheel::storage::lsm
