#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

namespace cartwheel::gateway {

// One io_context per thread ("one loop per thread"). The acceptor hands each
// new connection to the next context round-robin, and the connection lives
// there for its whole life. Its coroutine never migrates between threads, so
// per-connection state needs no locks, and cores do not contend on one shared
// run queue.
class IoContextPool {
 public:
  explicit IoContextPool(size_t threads);
  ~IoContextPool();
  IoContextPool(const IoContextPool&) = delete;
  IoContextPool& operator=(const IoContextPool&) = delete;

  void Start();
  void Stop();  // stops every loop and joins; idempotent

  boost::asio::io_context& Next() noexcept;
  [[nodiscard]] size_t size() const noexcept { return contexts_.size(); }

 private:
  using Guard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
  std::vector<std::unique_ptr<boost::asio::io_context>> contexts_;
  std::vector<Guard> guards_;
  std::vector<std::thread> threads_;
  std::atomic<size_t> next_{0};
};

}  // namespace cartwheel::gateway
