#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <boost/asio/ip/tcp.hpp>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include "common/status.h"
#include "gateway/http.h"
#include "gateway/io_pool.h"

namespace cartwheel::gateway {

struct HttpServerOptions {
  std::string address = "0.0.0.0";
  uint16_t port = 8080;  // 0 picks a free port
  size_t threads = 4;
  // Concurrent requests across all connections. Beyond this the server
  // answers 503 + Retry-After at once instead of queueing (load shedding).
  int64_t max_inflight = 2048;
  size_t max_body_bytes = size_t{64} * 1024;
  std::chrono::seconds idle_timeout{30};
};

// HTTP/1.1 keep-alive server on Boost.Beast. Each connection is one C++20
// coroutine pinned to one io_context in the pool.
class HttpServer {
 public:
  HttpServer(HttpServerOptions options, const Router* router, prometheus::Registry& registry);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  Status Start();
  void Stop();

  [[nodiscard]] uint16_t port() const noexcept { return bound_port_; }
  [[nodiscard]] int64_t inflight() const noexcept {
    return inflight_.load(std::memory_order_relaxed);
  }

 private:
  boost::asio::awaitable<void> AcceptLoop();
  boost::asio::awaitable<void> Session(boost::asio::ip::tcp::socket socket);
  boost::asio::awaitable<Response> Dispatch(const Request& req, std::string request_id,
                                            std::string* route);

  const HttpServerOptions options_;
  const Router* router_;
  IoContextPool pool_;
  boost::asio::io_context accept_context_{1};
  std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
  std::thread accept_thread_;
  uint16_t bound_port_ = 0;
  std::atomic<int64_t> inflight_{0};
  std::atomic<uint64_t> request_seq_{0};

  prometheus::Family<prometheus::Counter>& requests_;
  prometheus::Family<prometheus::Histogram>& latency_;
  prometheus::Gauge& inflight_gauge_;
  prometheus::Counter& shed_;
};

}  // namespace cartwheel::gateway
