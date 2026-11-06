#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket.hpp>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>

#include "gateway/auth.h"
#include "gateway/http.h"

namespace cartwheel::gateway {

// Live order updates over WebSocket, fanned out per user.
//
// Each connection is two coroutines on its io_context thread: a read loop
// (which also answers pings and notices the close) and a write loop fed by a
// bounded queue. Publish may be called from any thread; it posts to the
// connection's executor, so the queue is only ever touched by that thread.
// A client that falls 256 messages behind is disconnected instead of letting
// its queue grow without bound (slow consumers must not cost memory).
class WsHub {
 public:
  static constexpr size_t kMaxQueued = 256;

  explicit WsHub(prometheus::Registry& registry);

  // The upgrade entry point: authenticates the token carried in
  // Sec-WebSocket-Protocol ("cartwheel.v1, <jwt>": browsers cannot set an
  // Authorization header on a WebSocket) and then serves the connection, or
  // answers 401 and closes.
  boost::asio::awaitable<void> Upgrade(boost::beast::tcp_stream stream, Request req,
                                       const Authenticator* auth);

  // Runs one connection until it closes. `req` is the upgrade request.
  boost::asio::awaitable<void> Serve(boost::beast::tcp_stream stream, Request req,
                                     std::string user);

  // Thread-safe. Delivers `json` to every connection of `user`.
  void Publish(const std::string& user, const std::string& json);

  [[nodiscard]] size_t connections() const;

 private:
  struct Connection {
    explicit Connection(boost::beast::tcp_stream stream);
    boost::beast::websocket::stream<boost::beast::tcp_stream> ws;
    boost::asio::steady_timer wake;
    std::deque<std::string> queue;
    bool closed = false;
  };

  boost::asio::awaitable<void> WriteLoop(std::shared_ptr<Connection> c);
  void Add(const std::string& user, const std::shared_ptr<Connection>& c);
  void Remove(const std::string& user, const std::shared_ptr<Connection>& c);

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::vector<std::shared_ptr<Connection>>> by_user_;
  size_t count_ = 0;

  prometheus::Gauge& connections_gauge_;
  prometheus::Counter& pushed_;
  prometheus::Counter& dropped_slow_;
};

// Subscribes to Redis "orders:*" and forwards each message to the hub as
// {"type":"order","order":{...}}. Reconnects with backoff; runs on its own
// thread because the redis++ subscriber is blocking.
class OrderFeed {
 public:
  OrderFeed(std::string redis_url, WsHub* hub);
  ~OrderFeed();
  OrderFeed(const OrderFeed&) = delete;
  OrderFeed& operator=(const OrderFeed&) = delete;

  void Start();
  void Stop();

 private:
  void Run();

  std::string redis_url_;
  WsHub* hub_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace cartwheel::gateway
