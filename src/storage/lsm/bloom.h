#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cartwheel::storage::lsm {

// One bloom filter per SSTable over its user keys. A point read checks it
// before touching the index or data blocks, so a miss usually costs zero IO.
// With 10 bits per key and k = 7 probes (double hashing), the theoretical
// false-positive rate is about 0.8%. bloom_test measures the real one.
class BloomFilterBuilder {
 public:
  explicit BloomFilterBuilder(int bits_per_key = 10);
  void AddKey(std::string_view user_key);
  // Layout: bit array | u8 k
  [[nodiscard]] std::string Finish();
  [[nodiscard]] size_t keys() const { return hashes_.size(); }

 private:
  int bits_per_key_;
  std::vector<uint32_t> hashes_;
};

[[nodiscard]] bool BloomMayContain(std::string_view filter, std::string_view user_key) noexcept;

// 32-bit hash (Murmur-style mixing, as in LevelDB's Hash).
[[nodiscard]] uint32_t BloomHash(std::string_view data) noexcept;

}  // namespace cartwheel::storage::lsm
