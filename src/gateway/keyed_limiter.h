#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/clock.h"

namespace cartwheel::gateway {

struct KeyedLimiterOptions {
  double rate_per_sec = 1.0;
  double burst = 3.0;
  size_t shards = 64;
  // Bound on tracked keys per shard. A key whose bucket has refilled carries
  // no information, so those are evicted first.
  size_t max_keys_per_shard = 8192;
};

// One GCRA bucket per key (user, client IP...), the same algorithm as
// TokenBucket but with each key's state reduced to its theoretical arrival
// time in a sharded hash map. 64 shards keep lock hold times to a map lookup
// and spread contention across cores.
class KeyedRateLimiter {
 public:
  explicit KeyedRateLimiter(KeyedLimiterOptions options,
                            const Clock* clock = SystemClock::Default());

  // 0 if admitted; otherwise nanoseconds until the request would be.
  [[nodiscard]] int64_t Admit(std::string_view key, double cost = 1.0);

  [[nodiscard]] size_t tracked_keys() const;

 private:
  // Transparent hashing: look up by string_view without allocating.
  struct KeyHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept {
      return std::hash<std::string_view>{}(s);
    }
  };
  struct Shard {
    mutable std::mutex mu;
    std::unordered_map<std::string, int64_t, KeyHash, std::equal_to<>> tat;
  };
  void EvictLocked(Shard& s, int64_t now) const;

  const KeyedLimiterOptions options_;
  const Clock* clock_;
  const int64_t interval_ns_;
  const int64_t tolerance_ns_;
  std::vector<Shard> shards_;
};

}  // namespace cartwheel::gateway
