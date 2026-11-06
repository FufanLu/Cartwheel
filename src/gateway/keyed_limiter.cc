#include "gateway/keyed_limiter.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace cartwheel::gateway {

KeyedRateLimiter::KeyedRateLimiter(KeyedLimiterOptions options, const Clock* clock)
    : options_(options),
      clock_(clock),
      interval_ns_(static_cast<int64_t>(std::llround(1e9 / options.rate_per_sec))),
      tolerance_ns_(static_cast<int64_t>(std::llround(options.burst * 1e9 / options.rate_per_sec))),
      shards_(std::max<size_t>(options.shards, 1)) {}

int64_t KeyedRateLimiter::Admit(std::string_view key, double cost) {
  Shard& s = shards_[std::hash<std::string_view>{}(key) % shards_.size()];
  const int64_t now = clock_->NowNanos();
  const auto increment =
      static_cast<int64_t>(std::llround(cost * static_cast<double>(interval_ns_)));
  std::lock_guard lock(s.mu);
  auto it = s.tat.find(key);
  if (it == s.tat.end()) {
    if (s.tat.size() >= options_.max_keys_per_shard) EvictLocked(s, now);
    it = s.tat.emplace(std::string(key), now).first;
  }
  // Same rule as TokenBucket: admit iff the bucket would not overflow.
  const int64_t new_tat = std::max(it->second, now) + increment;
  const int64_t over = new_tat - now - tolerance_ns_;
  if (over > 0) return over;
  it->second = new_tat;
  return 0;
}

void KeyedRateLimiter::EvictLocked(Shard& s, int64_t now) const {
  std::erase_if(s.tat, [now](const auto& kv) { return kv.second <= now; });
  if (s.tat.size() >= options_.max_keys_per_shard) {
    // Every key is still limited: drop one. It gets a fresh bucket next time,
    // a bounded leak that keeps memory bounded under a key flood.
    s.tat.erase(s.tat.begin());
  }
}

size_t KeyedRateLimiter::tracked_keys() const {
  size_t n = 0;
  for (const auto& s : shards_) {
    std::lock_guard lock(s.mu);
    n += s.tat.size();
  }
  return n;
}

}  // namespace cartwheel::gateway
