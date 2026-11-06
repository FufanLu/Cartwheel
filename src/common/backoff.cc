#include "common/backoff.h"

#include <algorithm>
#include <limits>

namespace cartwheel {
namespace {

// base * 2^attempt without overflow.
int64_t ExponentialCeiling(int64_t base, int64_t cap, int attempt) noexcept {
  if (attempt >= 62) return cap;
  const int64_t factor = int64_t{1} << attempt;
  if (base > cap / factor) return cap;
  return std::min(cap, base * factor);
}

}  // namespace

Backoff::Backoff(BackoffPolicy policy, uint64_t seed)
    : policy_(policy), rng_(seed), prev_micros_(policy.base_micros) {}

void Backoff::Reset() noexcept {
  attempt_ = 0;
  prev_micros_ = policy_.base_micros;
}

int64_t Backoff::Uniform(int64_t lo, int64_t hi) {
  if (hi <= lo) return lo;
  return std::uniform_int_distribution<int64_t>(lo, hi)(rng_);
}

std::optional<int64_t> Backoff::NextDelayMicros() {
  if (attempt_ >= policy_.max_attempts) return std::nullopt;
  const int64_t ceiling = ExponentialCeiling(policy_.base_micros, policy_.cap_micros, attempt_);
  ++attempt_;

  int64_t delay = 0;
  switch (policy_.jitter) {
    case JitterMode::kNone: delay = ceiling; break;
    case JitterMode::kFull: delay = Uniform(0, ceiling); break;
    case JitterMode::kEqual: delay = ceiling / 2 + Uniform(0, ceiling / 2); break;
    case JitterMode::kDecorrelated: {
      const int64_t hi = prev_micros_ > std::numeric_limits<int64_t>::max() / 3 ? policy_.cap_micros
                                                                                : prev_micros_ * 3;
      delay = std::min(policy_.cap_micros, Uniform(policy_.base_micros, hi));
      break;
    }
  }
  prev_micros_ = std::max(delay, policy_.base_micros);
  return delay;
}

}  // namespace cartwheel
