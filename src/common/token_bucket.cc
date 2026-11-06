#include "common/token_bucket.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace cartwheel {
namespace {

constexpr double kNanosPerSecond = 1e9;

int64_t TokensToNanos(double tokens, int64_t interval) noexcept {
  return static_cast<int64_t>(std::llround(tokens * static_cast<double>(interval)));
}

int64_t IntervalFor(double rate_per_sec) noexcept {
  return std::max<int64_t>(1, static_cast<int64_t>(std::llround(kNanosPerSecond / rate_per_sec)));
}

}  // namespace

TokenBucket::TokenBucket(double rate_per_sec, double burst, const Clock* clock) : clock_(clock) {
  assert(rate_per_sec > 0.0 && burst >= 1.0);
  const int64_t interval = IntervalFor(rate_per_sec);
  interval_.store(interval, std::memory_order_relaxed);
  tolerance_.store(TokensToNanos(burst, interval), std::memory_order_relaxed);
  // Start full: TAT <= now means all `burst` tokens are available immediately.
  tat_.store(clock_->NowNanos(), std::memory_order_relaxed);
}

void TokenBucket::SetRate(double rate_per_sec, double burst) noexcept {
  assert(rate_per_sec > 0.0 && burst >= 1.0);
  const int64_t interval = IntervalFor(rate_per_sec);
  const int64_t old_interval = interval_.exchange(interval, std::memory_order_relaxed);
  tolerance_.store(TokensToNanos(burst, interval), std::memory_order_relaxed);

  // TAT - now is the bucket's deficit, measured in time at the *old* rate.
  // Rescale it so the deficit in tokens is preserved. Otherwise raising the
  // rate from 1/s to 1000/s would still make callers wait out a full second.
  const int64_t now = clock_->NowNanos();
  int64_t tat = tat_.load(std::memory_order_relaxed);
  for (;;) {
    const int64_t backlog = std::max<int64_t>(0, tat - now);
    const auto scaled =
        static_cast<int64_t>(static_cast<double>(backlog) * static_cast<double>(interval) /
                             static_cast<double>(old_interval));
    if (tat_.compare_exchange_weak(tat, now + scaled, std::memory_order_relaxed)) return;
  }
}

bool TokenBucket::TryAcquire(double tokens) noexcept {
  const int64_t interval = interval_.load(std::memory_order_relaxed);
  const int64_t tolerance = tolerance_.load(std::memory_order_relaxed);
  const int64_t cost = TokensToNanos(tokens, interval);
  const int64_t now = clock_->NowNanos();

  int64_t tat = tat_.load(std::memory_order_relaxed);
  for (;;) {
    const int64_t new_tat = std::max(tat, now) + cost;
    if (new_tat - now > tolerance) return false;  // would overflow the bucket
    // relaxed is enough: the limiter guards no other memory, only its own counter.
    if (tat_.compare_exchange_weak(tat, new_tat, std::memory_order_relaxed)) return true;
    // `tat` reloaded by the failed CAS; recompute against the new value.
  }
}

int64_t TokenBucket::NanosUntilAvailable(double tokens) const noexcept {
  const int64_t interval = interval_.load(std::memory_order_relaxed);
  const int64_t tolerance = tolerance_.load(std::memory_order_relaxed);
  const int64_t now = clock_->NowNanos();
  const int64_t tat = tat_.load(std::memory_order_relaxed);
  const int64_t new_tat = std::max(tat, now) + TokensToNanos(tokens, interval);
  return std::max<int64_t>(0, new_tat - now - tolerance);
}

double TokenBucket::Available() const noexcept {
  const int64_t interval = interval_.load(std::memory_order_relaxed);
  const int64_t tolerance = tolerance_.load(std::memory_order_relaxed);
  const int64_t now = clock_->NowNanos();
  const int64_t backlog = std::max<int64_t>(0, tat_.load(std::memory_order_relaxed) - now);
  // Shrinking burst below the current deficit can make this negative; clamp.
  return std::max(0.0, static_cast<double>(tolerance - backlog) / static_cast<double>(interval));
}

double TokenBucket::rate() const noexcept {
  return kNanosPerSecond / static_cast<double>(interval_.load(std::memory_order_relaxed));
}

double TokenBucket::burst() const noexcept {
  return static_cast<double>(tolerance_.load(std::memory_order_relaxed)) /
         static_cast<double>(interval_.load(std::memory_order_relaxed));
}

}  // namespace cartwheel
