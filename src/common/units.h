#pragma once

#include <cstddef>

namespace cartwheel {

// Byte-size literals: 4_MiB is a size_t computed in size_t, so no int
// multiplication can overflow before the widening (bugprone-implicit-widening).
constexpr size_t operator""_KiB(unsigned long long n) {
  return static_cast<size_t>(n) * 1024;
}
constexpr size_t operator""_MiB(unsigned long long n) {
  return static_cast<size_t>(n) * 1024 * 1024;
}

}  // namespace cartwheel
