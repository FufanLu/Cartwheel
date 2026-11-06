#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace cartwheel {

// Time source abstraction. Production code uses SystemClock; tests and the
// deterministic cluster simulator inject ManualClock so time only moves when
// the test says so. All values are nanoseconds.
class Clock {
 public:
  virtual ~Clock() = default;

  // Monotonic time; only differences are meaningful.
  [[nodiscard]] virtual int64_t NowNanos() const noexcept = 0;
  // Wall-clock time since the Unix epoch (for logs, TTLs, hint expiry).
  [[nodiscard]] virtual int64_t WallNanos() const noexcept = 0;

  [[nodiscard]] int64_t NowMicros() const noexcept { return NowNanos() / 1000; }
  [[nodiscard]] int64_t WallMillis() const noexcept { return WallNanos() / 1'000'000; }
};

class SystemClock final : public Clock {
 public:
  [[nodiscard]] int64_t NowNanos() const noexcept override {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  [[nodiscard]] int64_t WallNanos() const noexcept override {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  // Process-wide instance (stateless, so destruction order at exit is harmless).
  static const SystemClock* Default() noexcept {
    static const SystemClock kClock;
    return &kClock;
  }
};

// Thread-safe manually advanced clock.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(int64_t start_nanos = 0) noexcept : now_(start_nanos) {}

  [[nodiscard]] int64_t NowNanos() const noexcept override {
    return now_.load(std::memory_order_acquire);
  }
  [[nodiscard]] int64_t WallNanos() const noexcept override { return NowNanos(); }

  void Advance(int64_t nanos) noexcept { now_.fetch_add(nanos, std::memory_order_acq_rel); }
  void AdvanceMicros(int64_t micros) noexcept { Advance(micros * 1000); }
  void AdvanceMillis(int64_t millis) noexcept { Advance(millis * 1'000'000); }
  void Set(int64_t nanos) noexcept { now_.store(nanos, std::memory_order_release); }

 private:
  std::atomic<int64_t> now_;
};

}  // namespace cartwheel
