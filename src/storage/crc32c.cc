#include "storage/crc32c.h"

#include <array>
#include <cstring>

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define CW_CRC32C_ARM 1
#elif defined(__x86_64__) && defined(__SSE4_2__)
#include <nmmintrin.h>
#define CW_CRC32C_SSE42 1
#endif

namespace cartwheel::storage::crc32c {
namespace {

constexpr uint32_t kPoly = 0x82f63b78U;  // reflected Castagnoli polynomial

// tables[k][b]: CRC contribution of byte b located k bytes before the end of
// an 8-byte word, for slicing-by-8.
constexpr std::array<std::array<uint32_t, 256>, 8> MakeTables() {
  std::array<std::array<uint32_t, 256>, 8> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1U) ? (c >> 1) ^ kPoly : c >> 1;
    t[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; ++i) {
    for (size_t k = 1; k < 8; ++k) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFFU];
  }
  return t;
}

constexpr auto kTables = MakeTables();

uint64_t LoadLE64(const char* p) noexcept {
  uint64_t v = 0;
  std::memcpy(&v, p, sizeof(v));  // all supported targets are little-endian
  return v;
}

}  // namespace

uint32_t ExtendPortable(uint32_t crc, const char* data, size_t n) noexcept {
  const auto* p = data;
  uint32_t c = ~crc;
  while (n >= 8) {
    const uint64_t word = LoadLE64(p) ^ c;
    c = kTables[7][word & 0xFF] ^ kTables[6][(word >> 8) & 0xFF] ^ kTables[5][(word >> 16) & 0xFF] ^
        kTables[4][(word >> 24) & 0xFF] ^ kTables[3][(word >> 32) & 0xFF] ^
        kTables[2][(word >> 40) & 0xFF] ^ kTables[1][(word >> 48) & 0xFF] ^
        kTables[0][(word >> 56) & 0xFF];
    p += 8;
    n -= 8;
  }
  while (n-- > 0) {
    c = kTables[0][(c ^ static_cast<unsigned char>(*p++)) & 0xFFU] ^ (c >> 8);
  }
  return ~c;
}

bool HardwareAccelerated() noexcept {
#if defined(CW_CRC32C_ARM) || defined(CW_CRC32C_SSE42)
  return true;
#else
  return false;
#endif
}

uint32_t Extend(uint32_t crc, const char* data, size_t n) noexcept {
#if defined(CW_CRC32C_ARM)
  uint32_t c = ~crc;
  while (n >= 8) {
    c = __crc32cd(c, LoadLE64(data));
    data += 8;
    n -= 8;
  }
  while (n-- > 0) c = __crc32cb(c, static_cast<uint8_t>(*data++));
  return ~c;
#elif defined(CW_CRC32C_SSE42)
  uint64_t c = ~crc;
  while (n >= 8) {
    c = _mm_crc32_u64(c, LoadLE64(data));
    data += 8;
    n -= 8;
  }
  auto c32 = static_cast<uint32_t>(c);
  while (n-- > 0) c32 = _mm_crc32_u8(c32, static_cast<uint8_t>(*data++));
  return ~c32;
#else
  return ExtendPortable(crc, data, n);
#endif
}

}  // namespace cartwheel::storage::crc32c
