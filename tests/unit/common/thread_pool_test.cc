#include "common/thread_pool.h"

#include <atomic>
#include <chrono>
#include <future>
#include <latch>

#include <gtest/gtest.h>

namespace cartwheel {
namespace {

TEST(ThreadPoolTest, RunsAllSubmittedTasks) {
  std::atomic<int> sum{0};
  {
    ThreadPool pool({.threads = 4, .max_queue = 1024, .name = "t"});
    for (int i = 1; i <= 1000; ++i) {
      ASSERT_TRUE(pool.Submit([&sum, i] { sum.fetch_add(i, std::memory_order_relaxed); }));
    }
  }  // destructor drains
  EXPECT_EQ(sum.load(), 500'500);
}

TEST(ThreadPoolTest, TrySubmitShedsWhenQueueFull) {
  ThreadPool pool({.threads = 1, .max_queue = 2, .name = "t"});
  std::promise<void> release;
  std::shared_future<void> gate = release.get_future().share();
  std::latch started(1);

  // Occupy the only worker, then fill the queue.
  ASSERT_TRUE(pool.TrySubmit([&] {
    started.count_down();
    gate.wait();
  }));
  started.wait();
  ASSERT_TRUE(pool.TrySubmit([gate] { gate.wait(); }));
  ASSERT_TRUE(pool.TrySubmit([gate] { gate.wait(); }));
  EXPECT_EQ(pool.queue_depth(), 2U);

  EXPECT_FALSE(pool.TrySubmit([] {}));
  EXPECT_EQ(pool.rejected(), 1U);

  release.set_value();
  pool.Shutdown();
  EXPECT_EQ(pool.completed(), 3U);
}

TEST(ThreadPoolTest, DiscardShutdownDropsQueuedWork) {
  ThreadPool pool({.threads = 1, .max_queue = 100, .name = "t"});
  std::promise<void> release;
  std::shared_future<void> gate = release.get_future().share();
  std::latch started(1);
  std::atomic<int> ran{0};
  ASSERT_TRUE(pool.TrySubmit([&] {
    started.count_down();
    gate.wait();
  }));
  started.wait();
  for (int i = 0; i < 10; ++i) ASSERT_TRUE(pool.TrySubmit([&ran] { ran.fetch_add(1); }));

  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    release.set_value();
  });
  pool.Shutdown(ThreadPool::ShutdownMode::kDiscard);
  releaser.join();
  EXPECT_EQ(ran.load(), 0);
}

TEST(ThreadPoolTest, RejectsAfterShutdown) {
  ThreadPool pool({.threads = 2, .max_queue = 8, .name = "t"});
  pool.Shutdown();
  EXPECT_FALSE(pool.TrySubmit([] {}));
  EXPECT_FALSE(pool.Submit([] {}));
  pool.Shutdown();  // idempotent
}

}  // namespace
}  // namespace cartwheel
