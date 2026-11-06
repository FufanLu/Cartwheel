#include "gateway/ws_hub.h"

#include <chrono>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <fmt/format.h>
#include <sw/redis++/redis++.h>

#include "common/backoff.h"
#include "common/logging.h"

namespace cartwheel::gateway {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;

WsHub::Connection::Connection(beast::tcp_stream stream)
    : ws(std::move(stream)), wake(ws.get_executor()) {
  wake.expires_at(asio::steady_timer::time_point::max());
}

WsHub::WsHub(prometheus::Registry& registry)
    : connections_gauge_(prometheus::BuildGauge()
                             .Name("cartwheel_ws_connections")
                             .Help("Open WebSocket connections")
                             .Register(registry)
                             .Add({})),
      pushed_(prometheus::BuildCounter()
                  .Name("cartwheel_ws_messages_total")
                  .Help("Messages pushed to WebSocket clients")
                  .Register(registry)
                  .Add({})),
      dropped_slow_(prometheus::BuildCounter()
                        .Name("cartwheel_ws_slow_disconnects_total")
                        .Help("Clients disconnected because their send queue was full")
                        .Register(registry)
                        .Add({})) {}

asio::awaitable<void> WsHub::Upgrade(beast::tcp_stream stream, Request req,
                                     const Authenticator* auth) {
  std::string token;
  std::string_view protocols = req[http::field::sec_websocket_protocol];
  while (!protocols.empty()) {
    const size_t comma = protocols.find(',');
    std::string_view item = protocols.substr(0, comma);
    while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
    while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
    if (!item.empty() && item != "cartwheel.v1") token = std::string(item);
    if (comma == std::string_view::npos) break;
    protocols.remove_prefix(comma + 1);
  }
  auto user = token.empty() ? Result<std::string>(Status::Unauthenticated("no token"))
                            : auth->UserFromToken(token);
  if (!user.ok()) {
    Response res{http::status::unauthorized, req.version()};
    res.set(http::field::content_type, "application/json");
    res.body() = R"({"error":{"code":"Unauthenticated","message":"token required"}})";
    res.prepare_payload();
    res.keep_alive(false);
    co_await http::async_write(stream, res, asio::as_tuple(asio::use_awaitable));
    co_return;
  }
  co_await Serve(std::move(stream), std::move(req), *user);
}

asio::awaitable<void> WsHub::Serve(beast::tcp_stream stream, Request req, std::string user) {
  auto c = std::make_shared<Connection>(std::move(stream));
  beast::get_lowest_layer(c->ws).expires_never();
  // Beast's server defaults: idle timeout with keepalive pings.
  c->ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
  c->ws.set_option(websocket::stream_base::decorator([](websocket::response_type& res) {
    res.set(http::field::sec_websocket_protocol, "cartwheel.v1");
  }));
  auto [aec] = co_await c->ws.async_accept(req, asio::as_tuple(asio::use_awaitable));
  if (aec) co_return;

  Add(user, c);
  c->queue.push_back(fmt::format(R"({{"type":"hello","user":"{}"}})", user));
  asio::co_spawn(c->ws.get_executor(), WriteLoop(c), asio::detached);

  beast::flat_buffer buffer;
  for (;;) {
    auto [ec, n] = co_await c->ws.async_read(buffer, asio::as_tuple(asio::use_awaitable));
    if (ec) break;      // closed by the client, timed out, or torn down
    buffer.consume(n);  // clients have nothing to say yet; drain and ignore
  }
  c->closed = true;
  c->wake.cancel();
  Remove(user, c);
}

asio::awaitable<void> WsHub::WriteLoop(std::shared_ptr<Connection> c) {
  while (!c->closed) {
    if (c->queue.empty()) {
      // Cancelled by Publish (new message) or by the reader (closing).
      [[maybe_unused]] auto [ec] = co_await c->wake.async_wait(asio::as_tuple(asio::use_awaitable));
      c->wake.expires_at(asio::steady_timer::time_point::max());
      continue;
    }
    std::string msg = std::move(c->queue.front());
    c->queue.pop_front();
    c->ws.text(true);
    auto [ec, n] =
        co_await c->ws.async_write(asio::buffer(msg), asio::as_tuple(asio::use_awaitable));
    if (ec) {
      c->closed = true;
      break;
    }
    pushed_.Increment();
  }
}

void WsHub::Publish(const std::string& user, const std::string& json) {
  std::vector<std::shared_ptr<Connection>> targets;
  {
    std::lock_guard lock(mu_);
    auto it = by_user_.find(user);
    if (it == by_user_.end()) return;
    targets = it->second;
  }
  for (auto& c : targets) {
    asio::post(c->ws.get_executor(), [this, c, json] {
      if (c->closed) return;
      if (c->queue.size() >= kMaxQueued) {
        // Too slow to keep up: close rather than buffer without bound.
        dropped_slow_.Increment();
        c->closed = true;
        beast::get_lowest_layer(c->ws).close();
        return;
      }
      c->queue.push_back(json);
      c->wake.cancel();
    });
  }
}

void WsHub::Add(const std::string& user, const std::shared_ptr<Connection>& c) {
  std::lock_guard lock(mu_);
  by_user_[user].push_back(c);
  ++count_;
  connections_gauge_.Increment();
}

void WsHub::Remove(const std::string& user, const std::shared_ptr<Connection>& c) {
  std::lock_guard lock(mu_);
  auto it = by_user_.find(user);
  if (it == by_user_.end()) return;
  std::erase(it->second, c);
  if (it->second.empty()) by_user_.erase(it);
  --count_;
  connections_gauge_.Decrement();
}

size_t WsHub::connections() const {
  std::lock_guard lock(mu_);
  return count_;
}

// --- OrderFeed ----------------------------------------------------------------

OrderFeed::OrderFeed(std::string redis_url, WsHub* hub)
    : redis_url_(std::move(redis_url)), hub_(hub) {}

OrderFeed::~OrderFeed() {
  Stop();
}

void OrderFeed::Start() {
  running_.store(true);
  thread_ = std::thread([this] { Run(); });
}

void OrderFeed::Stop() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

void OrderFeed::Run() {
  Backoff backoff({.base_micros = 100'000, .cap_micros = 5'000'000, .max_attempts = 1 << 30});
  constexpr std::string_view kPrefix = "orders:";
  while (running_.load()) {
    try {
      // consume() returns every second, so Stop() is noticed promptly.
      const char sep = redis_url_.find('?') == std::string::npos ? '?' : '&';
      sw::redis::Redis redis(redis_url_ + sep + "socket_timeout=1s");
      auto sub = redis.subscriber();
      sub.on_pmessage([this, kPrefix](const std::string& /*pattern*/, const std::string& channel,
                                      const std::string& msg) {
        if (!std::string_view(channel).starts_with(kPrefix)) return;
        hub_->Publish(channel.substr(kPrefix.size()),
                      fmt::format(R"({{"type":"order","order":{}}})", msg));
      });
      sub.psubscribe("orders:*");
      CW_LOG_INFO("order feed subscribed to orders:* on {}", redis_url_);
      backoff.Reset();
      while (running_.load()) {
        try {
          sub.consume();
        } catch (const sw::redis::TimeoutError&) {
          continue;  // an idle second: loop to check running_
        }
      }
    } catch (const sw::redis::Error& e) {
      const int64_t delay = backoff.NextDelayMicros().value_or(5'000'000);
      CW_LOG_WARN("order feed: {}; reconnecting in {} ms", e.what(), delay / 1000);
      for (int64_t waited = 0; waited < delay && running_.load(); waited += 50'000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
  }
}

}  // namespace cartwheel::gateway
