#include "storage/lsm/memtable.h"

#include <cstring>

namespace cartwheel::storage::lsm {
namespace {

const char* DecodeLengthPrefixed(const char* p, std::string_view* out) {
  // Byte-at-a-time varint32: never reads past the varint itself, so the
  // last entry in an arena block cannot trigger an out-of-bounds read.
  uint32_t len = 0;
  for (int shift = 0; shift <= 28; shift += 7) {
    const auto byte = static_cast<unsigned char>(*p++);
    len |= static_cast<uint32_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) break;
  }
  *out = std::string_view(p, len);
  return p + len;
}

}  // namespace

std::string_view MemTable::EntryKey(const char* entry) {
  std::string_view k;
  DecodeLengthPrefixed(entry, &k);
  return k;
}

std::string_view MemTable::EntryValue(const char* entry) {
  std::string_view k;
  const char* p = DecodeLengthPrefixed(entry, &k);
  std::string_view v;
  DecodeLengthPrefixed(p, &v);
  return v;
}

int MemTable::EntryComparator::operator()(const char* a, const char* b) const {
  // The skiplist head holds a null key; it never takes part in comparisons.
  return CompareInternalKey(EntryKey(a), EntryKey(b));
}

MemTable::MemTable() : table_(EntryComparator{}, &arena_) {}

void MemTable::Add(SequenceNumber seq, ValueType type, std::string_view key,
                   std::string_view value) {
  const size_t ikey_len = key.size() + kInternalKeyTrailer;
  const size_t encoded = static_cast<size_t>(VarintLength(ikey_len)) + ikey_len +
                         static_cast<size_t>(VarintLength(value.size())) + value.size();
  char* buf = arena_.Allocate(encoded);
  std::string scratch;
  scratch.reserve(encoded);
  PutVarint32(&scratch, static_cast<uint32_t>(ikey_len));
  AppendInternalKey(&scratch, key, seq, type);
  PutVarint32(&scratch, static_cast<uint32_t>(value.size()));
  scratch.append(value);
  std::memcpy(buf, scratch.data(), encoded);
  table_.Insert(buf);
  entries_.fetch_add(1, std::memory_order_relaxed);
}

MemTable::GetResult MemTable::Get(const LookupKey& key, std::string* value) const {
  // Seek to the first entry >= (user_key, seq): that is the newest version
  // visible at the lookup's snapshot, if the user key matches.
  std::string probe;
  PutVarint32(&probe, static_cast<uint32_t>(key.internal_key().size()));
  probe.append(key.internal_key());
  Table::Iterator it(&table_);
  it.Seek(probe.data());
  if (!it.Valid()) return GetResult::kNotFound;
  ParsedInternalKey parsed;
  if (!ParseInternalKey(EntryKey(it.key()), &parsed) || parsed.user_key != key.user_key()) {
    return GetResult::kNotFound;
  }
  if (parsed.type == ValueType::kDeletion) return GetResult::kDeleted;
  value->assign(EntryValue(it.key()));
  return GetResult::kFound;
}

class MemTableIterator final : public InternalIterator {
 public:
  explicit MemTableIterator(std::shared_ptr<const MemTable> owner)
      : owner_(std::move(owner)), it_(&owner_->table_) {}

  bool Valid() const override { return it_.Valid(); }
  void SeekToFirst() override { it_.SeekToFirst(); }
  void Seek(std::string_view target) override {
    probe_.clear();
    PutVarint32(&probe_, static_cast<uint32_t>(target.size()));
    probe_.append(target);
    it_.Seek(probe_.data());
  }
  void Next() override { it_.Next(); }
  std::string_view key() const override { return MemTable::EntryKey(it_.key()); }
  std::string_view value() const override { return MemTable::EntryValue(it_.key()); }
  Status status() const override { return Status::Ok(); }

 private:
  std::shared_ptr<const MemTable> owner_;
  MemTable::Table::Iterator it_;
  std::string probe_;  // the skiplist compares encoded entries, so seeks need one
};

std::unique_ptr<InternalIterator> MemTable::NewIterator(std::shared_ptr<const MemTable> mem) {
  return std::make_unique<MemTableIterator>(std::move(mem));
}

}  // namespace cartwheel::storage::lsm
