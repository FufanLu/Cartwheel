#include "storage/lsm/write_batch.h"

#include "storage/lsm/memtable.h"

namespace cartwheel::storage::lsm {

WriteBatch::WriteBatch() {
  Clear();
}

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeader, '\0');
}

uint32_t WriteBatch::Count() const {
  return DecodeFixed32(rep_.data() + 8);
}
void WriteBatch::SetCount(uint32_t n) {
  EncodeFixed32(rep_.data() + 8, n);
}
SequenceNumber WriteBatch::Sequence() const {
  return DecodeFixed64(rep_.data());
}
void WriteBatch::SetSequence(SequenceNumber seq) {
  EncodeFixed64(rep_.data(), seq);
}

void WriteBatch::Put(std::string_view key, std::string_view value) {
  SetCount(Count() + 1);
  rep_.push_back(static_cast<char>(ValueType::kValue));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  SetCount(Count() + 1);
  rep_.push_back(static_cast<char>(ValueType::kDeletion));
  PutLengthPrefixed(&rep_, key);
}

void WriteBatch::Append(const WriteBatch& other) {
  SetCount(Count() + other.Count());
  rep_.append(other.rep_, kHeader, std::string::npos);
}

Result<WriteBatch> WriteBatch::FromContents(std::string_view contents) {
  if (contents.size() < kHeader) return Status::Corruption("write batch: too small");
  WriteBatch b;
  b.rep_.assign(contents);
  CW_RETURN_IF_ERROR(b.Iterate([](ValueType, std::string_view, std::string_view) {}));
  return b;
}

Status WriteBatch::InsertInto(MemTable* mem) const {
  SequenceNumber seq = Sequence();
  return Iterate([&](ValueType type, std::string_view key, std::string_view value) {
    mem->Add(seq++, type, key, value);
  });
}

}  // namespace cartwheel::storage::lsm
