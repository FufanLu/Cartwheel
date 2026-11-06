#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cartwheel {

// Fixed-size worker pool with a *bounded* queue.
//
// The bound is the point: an unbounded queue turns overload into unbounded
// latency and memory growth ("Avoiding insurmountable queue backlogs",
// Builders' Library). TrySubmit fails fast when the queue is full so callers
// can shed load (return ResourceExhausted / HTTP 503) instead of queueing work
// whose caller will have timed out by the time it runs.
class ThreadPool {
 public:
  using Task = std::function<void()>;

  struct Options {
    size_t threads = std::thread::hardware_concurrency();
    size_t max_queue = 4096;
    std::string name = "pool";  // used for OS thread names
  };

  explicit ThreadPool(Options options);
  ~ThreadPool();  // Shutdown(kDrain)

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Enqueues without blocking; false if the queue is full or the pool is
  // shutting down.
  [[nodiscard]] bool TrySubmit(Task task);

  // Blocks while the queue is full; false only if the pool is shutting down.
  bool Submit(Task task);

  enum class ShutdownMode : uint8_t { kDrain, kDiscard };
  // Stops accepting work, then either runs or drops queued tasks, and joins.
  // Idempotent. Must not be called from a worker thread or concurrently.
  void Shutdown(ShutdownMode mode = ShutdownMode::kDrain);

  [[nodiscard]] size_t queue_depth() const;
  [[nodiscard]] size_t threads() const noexcept { return workers_.size(); }
  [[nodiscard]] uint64_t rejected() const;
  [[nodiscard]] uint64_t completed() const;

 private:
  void WorkerLoop(size_t index);

  const Options options_;
  mutable std::mutex mu_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<Task> queue_;
  bool stopping_ = false;
  std::atomic<uint64_t> rejected_{0};
  std::atomic<uint64_t> completed_{0};
  std::vector<std::thread> workers_;
};

// Best-effort OS thread name (shows up in top -H, gdb, Instruments).
void SetCurrentThreadName(const std::string& name);

}  // namespace cartwheel
