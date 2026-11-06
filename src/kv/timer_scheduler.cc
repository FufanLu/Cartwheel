#include "kv/timer_scheduler.h"

#include "common/thread_pool.h"

namespace cartwheel::kv {

TimerScheduler::TimerScheduler() : thread_([this] { Loop(); }) {}

TimerScheduler::~TimerScheduler() {
  Stop();
}

void TimerScheduler::Stop() {
  {
    std::lock_guard lock(mu_);
    if (stopping_) return;
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void TimerScheduler::After(int64_t delay_ns, std::function<void()> fn) {
  {
    std::lock_guard lock(mu_);
    if (stopping_) return;
    timers_.push(Timer{std::chrono::steady_clock::now() + std::chrono::nanoseconds(delay_ns),
                       seq_++, std::move(fn)});
  }
  cv_.notify_one();
}

void TimerScheduler::Loop() {
  SetCurrentThreadName("cluster-timer");
  std::unique_lock lock(mu_);
  while (!stopping_) {
    if (timers_.empty()) {
      cv_.wait(lock);
      continue;
    }
    const auto next = timers_.top().at;
    if (std::chrono::steady_clock::now() < next) {
      cv_.wait_until(lock, next);
      continue;
    }
    auto fn = timers_.top().fn;
    timers_.pop();
    lock.unlock();
    fn();
    lock.lock();
  }
}

}  // namespace cartwheel::kv
