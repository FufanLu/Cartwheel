// deal-rush: a lightning-deal spike through the real HTTP edge.
//
//   deal-rush --gateway 127.0.0.1:8080 --deal dl-headphones --users 5000 \
//             --quantity 500 --connections 256 --admin-token dev-admin
//
//   1. resets the deal (new generation, `quantity` units, starts in a few s)
//   2. logs every user in (one JWT each)
//   3. opens WebSockets for a sample of users
//   4. at the start instant, every connection fires claims as fast as it can
//   5. waits for the workers to commit, then checks the invariant:
//        sold == reserved == quantity - remaining, and sold <= quantity
//
// Prints claim latency percentiles, the outcome mix, how long the deal took
// to sell out, how long the database lagged behind the gate, and the
// claim-to-push latency of the WebSocket notifications.

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include "common/histogram.h"

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;
using cartwheel::FormatNanos;
using cartwheel::Histogram;
using Json = nlohmann::json;

int64_t NanosSince(Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count();
}

struct Target {
  std::string host;
  std::string port;
};

struct HttpResult {
  int status = 0;
  std::string body;
};

// One keep-alive HTTP/1.1 connection, reconnected on demand.
class Conn {
 public:
  Conn(const asio::any_io_executor& ex, Target t) : stream_(ex), target_(std::move(t)) {}

  asio::awaitable<HttpResult> Send(http::verb verb, const std::string& path,
                                   const std::string& body, const std::string& token,
                                   const std::string& admin = "") {
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (!connected_) {
        tcp::resolver resolver(stream_.get_executor());
        auto [rec, eps] = co_await resolver.async_resolve(target_.host, target_.port,
                                                          asio::as_tuple(asio::use_awaitable));
        if (rec) co_return HttpResult{0, rec.message()};
        auto [cec, ep] = co_await stream_.async_connect(eps, asio::as_tuple(asio::use_awaitable));
        if (cec) co_return HttpResult{0, cec.message()};
        stream_.socket().set_option(tcp::no_delay(true));
        connected_ = true;
      }
      http::request<http::string_body> req{verb, path, 11};
      req.set(http::field::host, target_.host);
      req.keep_alive(true);
      if (!token.empty()) req.set(http::field::authorization, "Bearer " + token);
      if (!admin.empty()) req.set("X-Admin-Token", admin);
      if (!body.empty()) {
        req.set(http::field::content_type, "application/json");
        req.body() = body;
      }
      req.prepare_payload();
      stream_.expires_after(std::chrono::seconds(10));
      auto [wec, wn] =
          co_await http::async_write(stream_, req, asio::as_tuple(asio::use_awaitable));
      http::response<http::string_body> res;
      beast::error_code rec2 = wec;
      if (!wec) {
        auto [ec, n] =
            co_await http::async_read(stream_, buffer_, res, asio::as_tuple(asio::use_awaitable));
        rec2 = ec;
      }
      if (rec2) {  // stale keep-alive connection: reconnect once
        beast::error_code ignored;
        // Reported through `ignored`; closing a dead socket cannot fail usefully.
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        stream_.socket().close(ignored);
        buffer_.clear();
        connected_ = false;
        continue;
      }
      if (!res.keep_alive()) {
        beast::error_code ignored;
        // Reported through `ignored`; closing a dead socket cannot fail usefully.
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        stream_.socket().close(ignored);
        connected_ = false;
      }
      co_return HttpResult{static_cast<int>(res.result_int()), std::move(res.body())};
    }
    co_return HttpResult{0, "connection failed"};
  }

 private:
  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;
  Target target_;
  bool connected_ = false;
};

struct Stats {
  Histogram claim_latency;
  Histogram push_latency;
  std::mutex mu;
  std::map<std::string, int> outcomes;  // by outcome or HTTP status
  std::atomic<int64_t> first_claim_ns{0};
  std::atomic<int64_t> last_reserved_ns{0};
  std::atomic<int> reserved{0};
  std::map<std::string, Clock::time_point> claim_sent;  // order id -> when its claim went out
  std::atomic<int> pushes{0};
  std::atomic<int> retries{0};
  std::vector<std::string> acknowledged;  // order ids the buyers were given
};

// Runs `n` coroutines over `threads` io_contexts and waits for all of them.
template <typename MakeTask>
void RunParallel(size_t threads, size_t n, MakeTask make) {
  std::vector<std::unique_ptr<asio::io_context>> ctxs;
  ctxs.reserve(threads);
  for (size_t i = 0; i < threads; ++i) ctxs.push_back(std::make_unique<asio::io_context>(1));
  for (size_t i = 0; i < n; ++i) {
    auto& ctx = *ctxs[i % threads];
    asio::co_spawn(ctx, make(ctx.get_executor(), i), asio::detached);
  }
  std::vector<std::thread> runners;
  runners.reserve(ctxs.size());
  for (auto& c : ctxs) runners.emplace_back([&c] { c->run(); });
  for (auto& r : runners) r.join();
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"deal-rush: lightning-deal spike through the gateway"};
  std::string gateway = "127.0.0.1:8080";
  std::string deal = "dl-headphones";
  std::string admin_token = "dev-admin";
  int users = 5000;
  int quantity = 500;
  size_t connections = 256;
  size_t threads = 4;
  int ws_sample = 50;
  int64_t start_in_ms = 4000;
  std::string pg_url;
  int retries = 4;
  app.add_option("--gateway", gateway, "host:port of the gateway");
  app.add_option("--deal", deal);
  app.add_option("--users", users, "distinct buyers, one claim each");
  app.add_option("--quantity", quantity, "units for the fresh generation");
  app.add_option("--connections", connections, "concurrent keep-alive connections");
  app.add_option("--threads", threads, "client io threads");
  app.add_option("--ws-sample", ws_sample, "buyers that also listen on a WebSocket");
  app.add_option("--admin-token", admin_token);
  app.add_option("--start-in-ms", start_in_ms, "delay before the deal opens");
  app.add_option("--retries", retries, "client retries on 5xx/429 (full-jitter backoff)");
  app.add_option("--pg-url", pg_url,
                 "read committed orders straight from Postgres for the timeline "
                 "(the API's `sold` is cached for up to catalog.ttl_ms)");
  CLI11_PARSE(app, argc, argv);

  const auto colon = gateway.rfind(':');
  const Target target{gateway.substr(0, colon), gateway.substr(colon + 1)};
  connections = std::min<size_t>(connections, static_cast<size_t>(users));
  ws_sample = std::min(ws_sample, users);

  // 1. Fresh generation.
  int64_t starts_at_ms = 0;
  int64_t generation = 0;
  RunParallel(1, 1, [&](asio::any_io_executor ex, size_t) -> asio::awaitable<void> {
    Conn c(ex, target);
    const Json body{
        {"quantity", quantity}, {"starts_in_ms", start_in_ms}, {"duration_ms", 10 * 60'000}};
    auto r = co_await c.Send(http::verb::post, "/api/v1/admin/deals/" + deal + "/reset",
                             body.dump(), "", admin_token);
    if (r.status != 200) {
      fmt::print(stderr, "reset failed: {} {}\n", r.status, r.body);
      co_return;
    }
    const auto j = Json::parse(r.body);
    starts_at_ms = j.at("starts_at_ms").get<int64_t>();
    generation = j.at("generation").get<int64_t>();
  });

  if (starts_at_ms == 0) return 1;  // reset failed

  // 2. Logins.
  std::vector<std::string> tokens(static_cast<size_t>(users));
  std::atomic<int> next{0};
  const auto login_start = Clock::now();
  RunParallel(threads, std::min<size_t>(connections, 64),
              [&](asio::any_io_executor ex, size_t) -> asio::awaitable<void> {
                Conn c(ex, target);
                for (int i; (i = next.fetch_add(1)) < users;) {
                  const Json body{{"user", fmt::format("rush{}", i)}};
                  auto r = co_await c.Send(http::verb::post, "/api/v1/login", body.dump(), "");
                  if (r.status == 200) {
                    tokens[static_cast<size_t>(i)] = Json::parse(r.body).at("token");
                  }
                }
              });
  fmt::print("logged in {} users in {}\n", users, FormatNanos(NanosSince(login_start)));

  // Server clock offset, so the rush starts at the server's start instant.
  int64_t offset_ms = 0;
  RunParallel(1, 1, [&](asio::any_io_executor ex, size_t) -> asio::awaitable<void> {
    Conn c(ex, target);
    const auto before = std::chrono::system_clock::now();
    auto r = co_await c.Send(http::verb::get, "/api/v1/deals", "", "");
    const auto local_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(before.time_since_epoch()).count();
    offset_ms = Json::parse(r.body).at("server_time_ms").get<int64_t>() - local_ms;
  });

  Stats stats;
  std::atomic<bool> done{false};
  const int64_t start_local_ms = starts_at_ms - offset_ms;

  // 3 + 4. WebSocket listeners and the rush, on the same io threads.
  std::thread ws_thread;
  std::unique_ptr<asio::io_context> ws_ctx = std::make_unique<asio::io_context>(1);
  for (int i = 0; i < ws_sample; ++i) {
    const std::string& token = tokens[static_cast<size_t>(i)];
    asio::co_spawn(
        *ws_ctx,
        [&, token]() -> asio::awaitable<void> {
          auto ex = co_await asio::this_coro::executor;
          tcp::resolver resolver(ex);
          auto eps = co_await resolver.async_resolve(target.host, target.port, asio::use_awaitable);
          websocket::stream<beast::tcp_stream> ws(ex);
          co_await beast::get_lowest_layer(ws).async_connect(eps, asio::use_awaitable);
          beast::get_lowest_layer(ws).expires_never();
          ws.set_option(websocket::stream_base::decorator([token](websocket::request_type& req) {
            req.set(http::field::sec_websocket_protocol, "cartwheel.v1, " + token);
          }));
          co_await ws.async_handshake(target.host, "/api/v1/ws", asio::use_awaitable);
          beast::flat_buffer buf;
          while (!done.load()) {
            auto [ec, n] = co_await ws.async_read(buf, asio::as_tuple(asio::use_awaitable));
            if (ec) break;
            const auto msg = Json::parse(beast::buffers_to_string(buf.data()), nullptr, false);
            buf.consume(n);
            if (msg.is_discarded() || msg.value("type", "") != "order") continue;
            const std::string id = msg["order"].value("order_id", "");
            std::lock_guard lock(stats.mu);
            // Redriven orders can be notified twice; count the first push.
            if (auto it = stats.claim_sent.find(id); it != stats.claim_sent.end()) {
              stats.push_latency.Record(NanosSince(it->second));
              stats.pushes.fetch_add(1);
              stats.claim_sent.erase(it);
            }
          }
        },
        asio::detached);
  }
  ws_thread = std::thread([&] {
    auto guard = asio::make_work_guard(*ws_ctx);
    ws_ctx->run();
  });

  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  if (start_local_ms > now_ms) {
    fmt::print("deal opens in {} ms; {} connections waiting\n", start_local_ms - now_ms,
               connections);
    std::fflush(stdout);  // scripts wait for this line to time fault injection
    std::this_thread::sleep_for(std::chrono::milliseconds(start_local_ms - now_ms + 5));
  }

  const auto rush_start = Clock::now();

  // Every 100 ms: units taken at the gate vs orders committed in Postgres.
  struct Sample {
    int64_t t_ms;
    int taken;
    int sold;
  };
  std::mutex samples_mu;
  std::vector<Sample> samples;
  std::atomic<bool> sampling{true};
  std::unique_ptr<pqxx::connection> pg;
  if (!pg_url.empty()) pg = std::make_unique<pqxx::connection>(pg_url);
  std::thread sampler([&] {
    asio::io_context ctx(1);
    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
          Conn c(co_await asio::this_coro::executor, target);
          asio::steady_timer timer(co_await asio::this_coro::executor);
          auto tick = Clock::now();
          while (sampling.load()) {
            auto r = co_await c.Send(http::verb::get, "/api/v1/deals", "", "");
            const auto j = Json::parse(r.body, nullptr, false);
            if (!j.is_discarded() && j.contains("deals")) {
              for (const auto& d : j.at("deals")) {
                if (d.value("deal_id", "") != deal) continue;
                int committed = d.value("sold", 0);
                if (pg) {
                  pqxx::read_transaction tx(*pg);
                  committed = tx.exec(
                                    "SELECT count(*) FROM orders WHERE deal_id = $1 AND "
                                    "generation = $2 AND status = 'CONFIRMED'",
                                    pqxx::params{tx, deal, generation})
                                  .one_row()[0]
                                  .as<int>();
                }
                std::lock_guard lock(samples_mu);
                samples.push_back({NanosSince(rush_start) / 1'000'000,
                                   d.value("quantity", 0) - d.value("remaining", 0), committed});
              }
            }
            tick += std::chrono::milliseconds(100);
            timer.expires_at(tick);
            co_await timer.async_wait(asio::use_awaitable);
          }
        },
        asio::detached);
    ctx.run();
  });
  next = 0;
  RunParallel(threads, connections, [&](asio::any_io_executor ex, size_t) -> asio::awaitable<void> {
    Conn c(ex, target);
    asio::steady_timer pause(ex);
    std::mt19937_64 rng(std::random_device{}());
    for (int i; (i = next.fetch_add(1)) < users;) {
      const auto sent = Clock::now();
      int64_t expected = 0;
      stats.first_claim_ns.compare_exchange_strong(expected, NanosSince(rush_start));
      HttpResult r;
      // A well-behaved client: retry 5xx and 429 with full-jitter backoff.
      for (int attempt = 0;; ++attempt) {
        const auto attempt_start = Clock::now();
        r = co_await c.Send(http::verb::post, "/api/v1/deals/" + deal + "/claim", "",
                            tokens[static_cast<size_t>(i)]);
        stats.claim_latency.Record(NanosSince(attempt_start));
        const bool retryable = r.status == 0 || r.status == 429 || r.status >= 500;
        if (!retryable || attempt >= retries) break;
        stats.retries.fetch_add(1);
        const int64_t cap_ms = std::min<int64_t>(2000, int64_t{200} << attempt);
        pause.expires_after(
            std::chrono::milliseconds(std::uniform_int_distribution<int64_t>(0, cap_ms)(rng)));
        co_await pause.async_wait(asio::use_awaitable);
      }
      std::string key = fmt::format("http {}", r.status);
      if (r.status == 202 || r.status == 200 || r.status == 409) {
        const auto j = Json::parse(r.body, nullptr, false);
        if (!j.is_discarded()) key = j.value("outcome", key);
        const std::string order_id = j.is_discarded() ? "" : j.value("order_id", "");
        if (!order_id.empty()) {
          std::lock_guard lock(stats.mu);
          stats.acknowledged.push_back(order_id);
        }
        if (r.status == 202) {
          stats.reserved.fetch_add(1);
          stats.last_reserved_ns.store(NanosSince(rush_start));
        }
        if (!order_id.empty() && i < ws_sample) {
          std::lock_guard lock(stats.mu);
          stats.claim_sent.emplace(order_id, sent);
        }
      }
      std::lock_guard lock(stats.mu);
      ++stats.outcomes[key];
    }
  });
  const int64_t rush_ns = NanosSince(rush_start);

  // 5. Wait for the database to catch up with the gate: every acknowledged
  //    reservation committed, and nothing changing for a second.
  int sold = 0;
  int taken = 0;
  int64_t committed_ns = 0;
  int stable_ticks = 0;
  const int acked = static_cast<int>(stats.acknowledged.size());
  for (int i = 0; i < 1200 && stable_ticks < 30; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::lock_guard lock(samples_mu);
    if (samples.empty()) continue;
    const Sample last = samples.back();
    if (last.sold == sold && last.taken == taken && sold >= acked) {
      ++stable_ticks;
    } else {
      stable_ticks = 0;
    }
    if (last.sold != sold) committed_ns = last.t_ms * 1'000'000;
    sold = last.sold;
    taken = last.taken;
  }
  sampling = false;
  sampler.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let pushes land
  done = true;
  ws_ctx->stop();
  ws_thread.join();

  const auto lat = stats.claim_latency.Snapshot();
  const auto push = stats.push_latency.Snapshot();
  fmt::print("\n{} claims over {} connections in {} = {:.0f} claims/s\n", users, connections,
             FormatNanos(rush_ns), users / (static_cast<double>(rush_ns) / 1e9));
  fmt::print("claim latency  p50 {}  p90 {}  p99 {}  p99.9 {}  max {}\n",
             FormatNanos(lat.Percentile(50)), FormatNanos(lat.Percentile(90)),
             FormatNanos(lat.Percentile(99)), FormatNanos(lat.Percentile(99.9)),
             FormatNanos(lat.max()));
  fmt::print("outcomes:");
  for (const auto& [k, v] : stats.outcomes) fmt::print("  {}={}", k, v);
  fmt::print("\nsold out after {} (last unit reserved); all orders committed after {}\n",
             FormatNanos(stats.last_reserved_ns.load()), FormatNanos(committed_ns));
  if (push.count() > 0) {
    fmt::print("claim -> CONFIRMED push ({} of {} sampled buyers)  p50 {}  p99 {}  max {}\n",
               push.count(), ws_sample, FormatNanos(push.Percentile(50)),
               FormatNanos(push.Percentile(99)), FormatNanos(push.max()));
  }
  fmt::print("\ntimeline (ms since the deal opened: taken at the gate / committed in Postgres)\n");
  {
    int64_t last_printed = -1000;
    int last_sold = -1;
    for (const auto& smp : samples) {
      const bool early = smp.t_ms < 2000;
      if ((early && smp.t_ms - last_printed >= 100) || smp.t_ms - last_printed >= 1000 ||
          (smp.sold != last_sold && smp.t_ms - last_printed >= 500)) {
        fmt::print("  {:>6} ms  taken {:>5}  committed {:>5}\n", smp.t_ms, smp.taken, smp.sold);
        last_printed = smp.t_ms;
        last_sold = smp.sold;
      }
    }
  }
  // Correctness: never more committed than offered, and every order id a
  // buyer was given (202 RESERVED or 200 ALREADY_CLAIMED) is committed. Units
  // taken at the gate but never committed would be stranded holds.
  int acked_committed = acked;
  if (!pg_url.empty()) {
    pqxx::connection c(pg_url);
    pqxx::read_transaction tx(c);
    acked_committed = tx.exec(
                            "SELECT count(*) FROM orders WHERE order_id = ANY($1) AND "
                            "status = 'CONFIRMED'",
                            pqxx::params{tx, stats.acknowledged})
                          .one_row()[0]
                          .as<int>();
  }
  const bool no_oversell = sold <= quantity;
  const bool nothing_lost = acked_committed == acked && sold >= acked;
  fmt::print("retries: {}\n", stats.retries.load());
  fmt::print(
      "invariant: quantity={} taken={} committed={} order ids given={} of them committed={} "
      "stranded={} -> {}\n",
      quantity, taken, sold, acked, acked_committed, taken - sold,
      no_oversell && nothing_lost ? "OK (no oversell, no acknowledged order lost)" : "VIOLATED");
  return no_oversell && nothing_lost ? 0 : 1;
}
