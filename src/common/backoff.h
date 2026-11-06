#pragma once

#include <cstdint>
#include <optional>
#include <random>

namespace cartwheel {

// Retry delays with jitter, following "Exponential Backoff And Jitter"
// (AWS Architecture Blog) and the Builders' Library article "Timeouts,
// retries, and backoff with jitter". Jitter spreads retries from many clients
// so a brief outage does not turn into synchronized retry storms.
enum class JitterMode : uint8_t {
  kNone,          // base * 2^n, capped (synchronized retries; for comparison only)
  kFull,          // uniform(0, min(cap, base * 2^n))
  kEqual,         // half fixed + half random
  kDecorrelated,  // min(cap, uniform(base, prev * 3))
};

struct BackoffPolicy {
  int64_t base_micros = 10'000;    // 10 ms
  int64_t cap_micros = 2'000'000;  // 2 s
  int max_attempts = 5;            // retries, not counting the first try
  JitterMode jitter = JitterMode::kFull;
};

// Not thread-safe: one instance per logical request.
class Backoff {
 public:
  // `seed` makes delays reproducible in tests and the cluster simulator.
  explicit Backoff(BackoffPolicy policy, uint64_t seed = std::random_device{}());

  // Delay before the next retry, or nullopt once max_attempts is exhausted.
  [[nodiscard]] std::optional<int64_t> NextDelayMicros();

  [[nodiscard]] int attempts() const noexcept { return attempt_; }
  void Reset() noexcept;

 private:
  [[nodiscard]] int64_t Uniform(int64_t lo, int64_t hi);  // inclusive bounds

  BackoffPolicy policy_;
  std::mt19937_64 rng_;
  int attempt_ = 0;
  int64_t prev_micros_;
};

}  // namespace cartwheel
