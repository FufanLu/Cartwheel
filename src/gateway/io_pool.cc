#include "gateway/io_pool.h"

#include <algorithm>
#include <string>

#include "common/thread_pool.h"

namespace cartwheel::gateway {

IoContextPool::IoContextPool(size_t threads) {
  const size_t n = std::max<size_t>(1, threads);
  contexts_.reserve(n);
  guards_.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    // concurrency_hint = 1: each context is driven by exactly one thread,
    // which lets Asio skip internal locking on the scheduler.
    contexts_.push_back(std::make_unique<boost::asio::io_context>(1));
    guards_.emplace_back(boost::asio::make_work_guard(*contexts_.back()));
  }
}

IoContextPool::~IoContextPool() {
  Stop();
}

void IoContextPool::Start() {
  threads_.reserve(contexts_.size());
  for (size_t i = 0; i < contexts_.size(); ++i) {
    threads_.emplace_back([this, i] {
      SetCurrentThreadName("io-" + std::to_string(i));
      contexts_[i]->run();
    });
  }
}

void IoContextPool::Stop() {
  for (auto& g : guards_) g.reset();
  for (auto& c : contexts_) c->stop();
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
  threads_.clear();
}

boost::asio::io_context& IoContextPool::Next() noexcept {
  return *contexts_[next_.fetch_add(1, std::memory_order_relaxed) % contexts_.size()];
}

}  // namespace cartwheel::gateway
