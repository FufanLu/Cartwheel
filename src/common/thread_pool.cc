#include "common/thread_pool.h"

#include <algorithm>
#include <pthread.h>
#include <utility>

namespace cartwheel {

void SetCurrentThreadName(const std::string& name) {
  // Linux limits names to 15 chars + NUL.
  const std::string truncated = name.substr(0, 15);
#if defined(__APPLE__)
  pthread_setname_np(truncated.c_str());
#elif defined(__linux__)
  pthread_setname_np(pthread_self(), truncated.c_str());
#endif
}

ThreadPool::ThreadPool(Options options) : options_(std::move(options)) {
  const size_t n = std::max<size_t>(1, options_.threads);
  workers_.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    workers_.emplace_back([this, i] { WorkerLoop(i); });
  }
}

ThreadPool::~ThreadPool() {
  Shutdown(ShutdownMode::kDrain);
}

bool ThreadPool::TrySubmit(Task task) {
  {
    std::lock_guard lock(mu_);
    if (stopping_ || queue_.size() >= options_.max_queue) {
      rejected_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    queue_.push_back(std::move(task));
  }
  not_empty_.notify_one();
  return true;
}

bool ThreadPool::Submit(Task task) {
  {
    std::unique_lock lock(mu_);
    not_full_.wait(lock, [this] { return stopping_ || queue_.size() < options_.max_queue; });
    if (stopping_) {
      rejected_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    queue_.push_back(std::move(task));
  }
  not_empty_.notify_one();
  return true;
}

void ThreadPool::Shutdown(ShutdownMode mode) {
  {
    std::lock_guard lock(mu_);
    if (stopping_ && workers_.empty()) return;
    stopping_ = true;
    if (mode == ShutdownMode::kDiscard) queue_.clear();
  }
  not_empty_.notify_all();
  not_full_.notify_all();
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
  std::lock_guard lock(mu_);
  workers_.clear();
}

void ThreadPool::WorkerLoop(size_t index) {
  SetCurrentThreadName(options_.name + "-" + std::to_string(index));
  for (;;) {
    Task task;
    {
      std::unique_lock lock(mu_);
      not_empty_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) return;  // stopping and drained
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    not_full_.notify_one();
    task();
    completed_.fetch_add(1, std::memory_order_relaxed);
  }
}

size_t ThreadPool::queue_depth() const {
  std::lock_guard lock(mu_);
  return queue_.size();
}

uint64_t ThreadPool::rejected() const {
  return rejected_.load(std::memory_order_relaxed);
}

uint64_t ThreadPool::completed() const {
  return completed_.load(std::memory_order_relaxed);
}

}  // namespace cartwheel
