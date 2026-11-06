#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/coding.h"

namespace cartwheel::storage::lsm {

// Every write gets a sequence number, which gives MVCC for free. A reader
// that captured `seq` ignores newer entries, so a lock-free read sees a
// consistent snapshot while writers and compactions keep going.
using SequenceNumber = uint64_t;
inline constexpr SequenceNumber kMaxSequence = (uint64_t{1} << 56) - 1;

enum class ValueType : uint8_t { kDeletion = 0, kValue = 1 };
// When seeking, use the highest type so the seek lands on the newest entry at `seq`.
inline constexpr ValueType kValueTypeForSeek = ValueType::kValue;

// Internal key = user_key || fixed64(seq << 8 | type).
// Ordering: user key ascending, then sequence DESCENDING (newest first).
inline constexpr size_t kInternalKeyTrailer = 8;

[[nodiscard]] inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType t) noexcept {
  return (seq << 8) | static_cast<uint64_t>(t);
}

inline void AppendInternalKey(std::string* dst, std::string_view user_key, SequenceNumber seq,
                              ValueType t) {
  dst->append(user_key.data(), user_key.size());
  PutFixed64(dst, PackSequenceAndType(seq, t));
}

[[nodiscard]] inline std::string MakeInternalKey(std::string_view user_key, SequenceNumber seq,
                                                 ValueType t) {
  std::string k;
  k.reserve(user_key.size() + kInternalKeyTrailer);
  AppendInternalKey(&k, user_key, seq, t);
  return k;
}

struct ParsedInternalKey {
  std::string_view user_key;
  SequenceNumber sequence = 0;
  ValueType type = ValueType::kValue;
};

[[nodiscard]] inline bool ParseInternalKey(std::string_view ikey, ParsedInternalKey* out) noexcept {
  if (ikey.size() < kInternalKeyTrailer) return false;
  const uint64_t tag = DecodeFixed64(ikey.data() + ikey.size() - kInternalKeyTrailer);
  const auto type = static_cast<uint8_t>(tag & 0xFF);
  if (type > static_cast<uint8_t>(ValueType::kValue)) return false;
  out->user_key = ikey.substr(0, ikey.size() - kInternalKeyTrailer);
  out->sequence = tag >> 8;
  out->type = static_cast<ValueType>(type);
  return true;
}

[[nodiscard]] inline std::string_view ExtractUserKey(std::string_view ikey) noexcept {
  return ikey.substr(0, ikey.size() - kInternalKeyTrailer);
}

[[nodiscard]] inline uint64_t ExtractTag(std::string_view ikey) noexcept {
  return DecodeFixed64(ikey.data() + ikey.size() - kInternalKeyTrailer);
}

// Three-way comparison of internal keys.
[[nodiscard]] inline int CompareInternalKey(std::string_view a, std::string_view b) noexcept {
  const int r = ExtractUserKey(a).compare(ExtractUserKey(b));
  if (r != 0) return r < 0 ? -1 : 1;
  const uint64_t ta = ExtractTag(a);
  const uint64_t tb = ExtractTag(b);
  if (ta == tb) return 0;
  return ta > tb ? -1 : 1;  // larger seq sorts first
}

struct InternalKeyLess {
  bool operator()(std::string_view a, std::string_view b) const noexcept {
    return CompareInternalKey(a, b) < 0;
  }
};

// A key for point lookups at snapshot `seq`: internal key (user_key, seq, kValueTypeForSeek).
class LookupKey {
 public:
  LookupKey(std::string_view user_key, SequenceNumber seq)
      : ikey_(MakeInternalKey(user_key, seq, kValueTypeForSeek)) {}
  [[nodiscard]] std::string_view internal_key() const noexcept { return ikey_; }
  [[nodiscard]] std::string_view user_key() const noexcept { return ExtractUserKey(ikey_); }

 private:
  std::string ikey_;
};

}  // namespace cartwheel::storage::lsm
