#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "cluster/runtime.h"

namespace cartwheel::kv {

// Production cluster::Scheduler: one thread and a deadline-ordered heap.
// Timer callbacks (gossip ticks, hint delivery, anti-entropy rounds) are
// short and only start async work, so a single thread suffices.
class TimerScheduler final : public cluster::Scheduler {
 public:
  TimerScheduler();
  ~TimerScheduler() override;
  TimerScheduler(const TimerScheduler&) = delete;
  TimerScheduler& operator=(const TimerScheduler&) = delete;

  void After(int64_t delay_ns, std::function<void()> fn) override;
  void Stop();

 private:
  struct Timer {
    std::chrono::steady_clock::time_point at;
    uint64_t seq;
    std::function<void()> fn;
    bool operator>(const Timer& o) const { return at != o.at ? at > o.at : seq > o.seq; }
  };
  void Loop();

  std::mutex mu_;
  std::condition_variable cv_;
  std::priority_queue<Timer, std::vector<Timer>, std::greater<>> timers_;
  uint64_t seq_ = 0;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace cartwheel::kv
