#include "common/backoff.h"

#include <vector>

#include <gtest/gtest.h>

namespace cartwheel {
namespace {

std::vector<int64_t> Drain(Backoff& b) {
  std::vector<int64_t> out;
  while (auto d = b.NextDelayMicros()) out.push_back(*d);
  return out;
}

TEST(BackoffTest, NoJitterDoublesUntilCap) {
  Backoff b({.base_micros = 10, .cap_micros = 100, .max_attempts = 6, .jitter = JitterMode::kNone});
  EXPECT_EQ(Drain(b), (std::vector<int64_t>{10, 20, 40, 80, 100, 100}));
  EXPECT_EQ(b.attempts(), 6);
}

TEST(BackoffTest, FullJitterStaysWithinExponentialCeiling) {
  for (uint64_t seed = 0; seed < 200; ++seed) {
    Backoff b({.base_micros = 1000,
               .cap_micros = 50'000,
               .max_attempts = 10,
               .jitter = JitterMode::kFull},
              seed);
    int attempt = 0;
    while (auto d = b.NextDelayMicros()) {
      const int64_t ceiling = std::min<int64_t>(50'000, 1000LL << attempt);
      EXPECT_GE(*d, 0);
      EXPECT_LE(*d, ceiling);
      ++attempt;
    }
  }
}

TEST(BackoffTest, EqualJitterKeepsHalf) {
  Backoff b({.base_micros = 1000,
             .cap_micros = 1'000'000,
             .max_attempts = 8,
             .jitter = JitterMode::kEqual},
            3);
  int attempt = 0;
  while (auto d = b.NextDelayMicros()) {
    const int64_t ceiling = 1000LL << attempt;
    EXPECT_GE(*d, ceiling / 2);
    EXPECT_LE(*d, ceiling);
    ++attempt;
  }
}

TEST(BackoffTest, DecorrelatedRespectsBaseAndCap) {
  Backoff b({.base_micros = 100,
             .cap_micros = 5000,
             .max_attempts = 50,
             .jitter = JitterMode::kDecorrelated},
            9);
  for (const int64_t d : Drain(b)) {
    EXPECT_GE(d, 100);
    EXPECT_LE(d, 5000);
  }
}

TEST(BackoffTest, SameSeedSameDelays) {
  const BackoffPolicy p{
      .base_micros = 500, .cap_micros = 100'000, .max_attempts = 8, .jitter = JitterMode::kFull};
  Backoff a(p, 1234);
  Backoff b(p, 1234);
  EXPECT_EQ(Drain(a), Drain(b));
}

TEST(BackoffTest, ResetRestartsSequence) {
  Backoff b(
      {.base_micros = 10, .cap_micros = 1000, .max_attempts = 2, .jitter = JitterMode::kNone});
  EXPECT_EQ(Drain(b).size(), 2U);
  EXPECT_FALSE(b.NextDelayMicros().has_value());
  b.Reset();
  EXPECT_EQ(b.NextDelayMicros(), 10);
}

TEST(BackoffTest, HugeAttemptCountDoesNotOverflow) {
  Backoff b({.base_micros = 1'000'000,
             .cap_micros = 30'000'000,
             .max_attempts = 100,
             .jitter = JitterMode::kNone});
  for (const int64_t d : Drain(b)) EXPECT_LE(d, 30'000'000);
}

}  // namespace
}  // namespace cartwheel
