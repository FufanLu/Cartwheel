#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace cartwheel {

// Binary encoding helpers shared by the WAL, SSTables, vector clocks, and
// sibling sets. Fixed-width integers are little-endian on every platform, so
// files written on an arm64 Mac read back on x86_64 Linux. Varints use the
// LEB128 / Protobuf format: 7 bits per byte, high bit means "more".

inline void EncodeFixed32(char* dst, uint32_t v) noexcept {
  for (int i = 0; i < 4; ++i) dst[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

inline void EncodeFixed64(char* dst, uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) dst[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

[[nodiscard]] inline uint32_t DecodeFixed32(const char* p) noexcept {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i)
    v |= static_cast<uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}

[[nodiscard]] inline uint64_t DecodeFixed64(const char* p) noexcept {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}

inline void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  EncodeFixed32(buf, v);
  dst->append(buf, sizeof(buf));
}

inline void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  EncodeFixed64(buf, v);
  dst->append(buf, sizeof(buf));
}

[[nodiscard]] constexpr int VarintLength(uint64_t v) noexcept {
  int len = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++len;
  }
  return len;
}

inline void PutVarint64(std::string* dst, uint64_t v) {
  while (v >= 0x80) {
    dst->push_back(static_cast<char>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  dst->push_back(static_cast<char>(v));
}

inline void PutVarint32(std::string* dst, uint32_t v) {
  PutVarint64(dst, v);
}

inline void PutLengthPrefixed(std::string* dst, std::string_view s) {
  PutVarint64(dst, s.size());
  dst->append(s.data(), s.size());
}

// Cursor over an encoded buffer. Every Get* returns false and leaves the
// cursor unchanged on truncated or malformed input, so callers can map
// failures to Status::Corruption without ever reading out of bounds.
class Decoder {
 public:
  explicit Decoder(std::string_view in) noexcept : in_(in) {}

  [[nodiscard]] bool GetFixed32(uint32_t* v) noexcept {
    if (in_.size() < 4) return false;
    *v = DecodeFixed32(
        in_.data());  // NOLINT(bugprone-suspicious-stringview-data-usage): size checked
    in_.remove_prefix(4);
    return true;
  }

  [[nodiscard]] bool GetFixed64(uint64_t* v) noexcept {
    if (in_.size() < 8) return false;
    *v = DecodeFixed64(
        in_.data());  // NOLINT(bugprone-suspicious-stringview-data-usage): size checked
    in_.remove_prefix(8);
    return true;
  }

  [[nodiscard]] bool GetVarint64(uint64_t* v) noexcept {
    uint64_t result = 0;
    for (size_t i = 0; i < in_.size() && i < 10; ++i) {
      const auto byte = static_cast<unsigned char>(in_[i]);
      // The 10th byte may only contribute the top bit of a 64-bit value.
      if (i == 9 && byte > 1) return false;
      result |= static_cast<uint64_t>(byte & 0x7F) << (7 * i);
      if ((byte & 0x80) == 0) {
        *v = result;
        in_.remove_prefix(i + 1);
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool GetVarint32(uint32_t* v) noexcept {
    std::string_view saved = in_;
    uint64_t wide = 0;
    if (!GetVarint64(&wide) || wide > UINT32_MAX) {
      in_ = saved;
      return false;
    }
    *v = static_cast<uint32_t>(wide);
    return true;
  }

  [[nodiscard]] bool GetBytes(size_t n, std::string_view* out) noexcept {
    if (in_.size() < n) return false;
    *out = in_.substr(0, n);
    in_.remove_prefix(n);
    return true;
  }

  [[nodiscard]] bool GetLengthPrefixed(std::string_view* out) noexcept {
    std::string_view saved = in_;
    uint64_t len = 0;
    if (!GetVarint64(&len) || len > in_.size()) {
      in_ = saved;
      return false;
    }
    *out = in_.substr(0, static_cast<size_t>(len));
    in_.remove_prefix(static_cast<size_t>(len));
    return true;
  }

  [[nodiscard]] bool empty() const noexcept { return in_.empty(); }
  [[nodiscard]] size_t remaining() const noexcept { return in_.size(); }
  [[nodiscard]] std::string_view rest() const noexcept { return in_; }

 private:
  std::string_view in_;
};

}  // namespace cartwheel
