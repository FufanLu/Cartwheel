#include "storage/lsm/bloom.h"

#include <algorithm>
#include <cstring>

namespace cartwheel::storage::lsm {

uint32_t BloomHash(std::string_view data) noexcept {
  constexpr uint32_t kSeed = 0xbc9f1d34U;
  constexpr uint32_t kM = 0xc6a4a793U;
  constexpr int kR = 24;
  const auto* p = reinterpret_cast<const unsigned char*>(data.data());
  size_t n = data.size();
  uint32_t h = kSeed ^ (static_cast<uint32_t>(n) * kM);
  while (n >= 4) {
    uint32_t w = 0;
    std::memcpy(&w, p, 4);
    h += w;
    h *= kM;
    h ^= (h >> 16);
    p += 4;
    n -= 4;
  }
  switch (n) {
    case 3: h += static_cast<uint32_t>(p[2]) << 16; [[fallthrough]];
    case 2: h += static_cast<uint32_t>(p[1]) << 8; [[fallthrough]];
    case 1:
      h += p[0];
      h *= kM;
      h ^= (h >> kR);
      break;
    default: break;
  }
  return h;
}

BloomFilterBuilder::BloomFilterBuilder(int bits_per_key) : bits_per_key_(bits_per_key) {}

void BloomFilterBuilder::AddKey(std::string_view user_key) {
  const uint32_t h = BloomHash(user_key);
  // Consecutive duplicate keys (several versions of one key) add nothing.
  if (hashes_.empty() || hashes_.back() != h) hashes_.push_back(h);
}

std::string BloomFilterBuilder::Finish() {
  // k = bits_per_key * ln(2) minimises the false-positive rate.
  const int k = std::clamp(static_cast<int>(bits_per_key_ * 0.69), 1, 30);
  size_t bits = std::max<size_t>(64, hashes_.size() * static_cast<size_t>(bits_per_key_));
  const size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;
  std::string out(bytes, '\0');
  for (uint32_t h : hashes_) {
    const uint32_t delta = (h >> 17) | (h << 15);  // double hashing: h + i*delta
    for (int j = 0; j < k; ++j) {
      const size_t bit = h % bits;
      out[bit / 8] =
          static_cast<char>(static_cast<unsigned char>(out[bit / 8]) | (1U << (bit % 8)));
      h += delta;
    }
  }
  out.push_back(static_cast<char>(k));
  return out;
}

bool BloomMayContain(std::string_view filter, std::string_view user_key) noexcept {
  if (filter.size() < 2) return true;  // missing or degenerate filter: cannot exclude
  const size_t bytes = filter.size() - 1;
  const size_t bits = bytes * 8;
  const int k = static_cast<unsigned char>(filter.back());
  if (k > 30) return true;  // reserved for future encodings
  uint32_t h = BloomHash(user_key);
  const uint32_t delta = (h >> 17) | (h << 15);
  for (int j = 0; j < k; ++j) {
    const size_t bit = h % bits;
    if ((static_cast<unsigned char>(filter[bit / 8]) & (1U << (bit % 8))) == 0) return false;
    h += delta;
  }
  return true;
}

}  // namespace cartwheel::storage::lsm
