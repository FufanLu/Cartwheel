#pragma once

#include <atomic>
#include <cstdint>

#include "common/clock.h"

namespace cartwheel {

// Lock-free rate limiter implementing GCRA (Generic Cell Rate Algorithm).
//
// GCRA behaves exactly like a token bucket with `rate` tokens/s and capacity
// `burst`, but its whole state is a single 64-bit "theoretical arrival time"
// (TAT): the instant at which the bucket would be full again. One CAS per
// admission keeps it cheap on hot paths (gateway per-client limits, per-table
// provisioned throughput in CartwheelKV) with no mutex convoy under contention.
//
//   emission interval T = 1s / rate        (time one token takes to refill)
//   admit n tokens at time `now`:
//     new_tat = max(tat, now) + n*T
//     admit iff new_tat - now <= burst*T   (i.e. the bucket does not overflow)
//
// See docs/illustrated/00-overload-toolbox.md for a traced example.
class TokenBucket {
 public:
  // rate_per_sec > 0, burst >= 1.
  TokenBucket(double rate_per_sec, double burst, const Clock* clock = SystemClock::Default());

  TokenBucket(const TokenBucket&) = delete;
  TokenBucket& operator=(const TokenBucket&) = delete;

  // Takes `tokens` if available. Never blocks.
  [[nodiscard]] bool TryAcquire(double tokens = 1.0) noexcept;

  // Nanoseconds until `tokens` could be acquired (0 if available now).
  // Useful for Retry-After headers and client-side pacing.
  [[nodiscard]] int64_t NanosUntilAvailable(double tokens = 1.0) const noexcept;

  // Tokens that could be taken right now, in [0, burst].
  [[nodiscard]] double Available() const noexcept;

  // Changes rate and burst, keeping the current fill level (in tokens). An
  // admission racing with SetRate may use either the old or new parameters,
  // which is acceptable for rate limiting.
  void SetRate(double rate_per_sec, double burst) noexcept;

  [[nodiscard]] double rate() const noexcept;
  [[nodiscard]] double burst() const noexcept;

  // Exposed for the illustrated docs and tests.
  [[nodiscard]] int64_t tat_nanos() const noexcept { return tat_.load(std::memory_order_relaxed); }
  [[nodiscard]] int64_t interval_nanos() const noexcept {
    return interval_.load(std::memory_order_relaxed);
  }

 private:
  const Clock* clock_;
  std::atomic<int64_t> tat_{0};        // theoretical arrival time
  std::atomic<int64_t> interval_{0};   // T, nanoseconds per token
  std::atomic<int64_t> tolerance_{0};  // burst * T
};

}  // namespace cartwheel
