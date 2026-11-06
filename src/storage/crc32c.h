#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cartwheel::storage::crc32c {

// CRC-32C (Castagnoli), the checksum on every WAL record and SSTable block.
// Uses the ARMv8 CRC32C instructions or SSE4.2 `crc32` when the CPU has them,
// with a slicing-by-8 table fallback. All three produce identical results
// (crc32c_test checks them against each other).
[[nodiscard]] uint32_t Extend(uint32_t crc, const char* data, size_t n) noexcept;

[[nodiscard]] inline uint32_t Value(const char* data, size_t n) noexcept {
  return Extend(0, data, n);
}
[[nodiscard]] inline uint32_t Value(std::string_view s) noexcept {
  return Value(s.data(), s.size());
}

// Portable implementation, exposed so tests can compare it with the hardware path.
[[nodiscard]] uint32_t ExtendPortable(uint32_t crc, const char* data, size_t n) noexcept;
[[nodiscard]] bool HardwareAccelerated() noexcept;

// Stored CRCs are masked, as in LevelDB. Computing the CRC of data that
// itself contains embedded CRCs (a WAL record holding an SST block, say)
// then does not degenerate.
inline constexpr uint32_t kMaskDelta = 0xa282ead8U;
[[nodiscard]] constexpr uint32_t Mask(uint32_t crc) noexcept {
  return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}
[[nodiscard]] constexpr uint32_t Unmask(uint32_t masked) noexcept {
  const uint32_t rot = masked - kMaskDelta;
  return (rot >> 17) | (rot << 15);
}

}  // namespace cartwheel::storage::crc32c
