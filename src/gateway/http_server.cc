#include "gateway/http_server.h"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "common/metrics.h"

namespace cartwheel::gateway {

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

HttpServer::HttpServer(HttpServerOptions options, const Router* router,
                       prometheus::Registry& registry)
    : options_(std::move(options)),
      router_(router),
      pool_(options_.threads),
      requests_(prometheus::BuildCounter()
                    .Name("cartwheel_http_requests_total")
                    .Help("HTTP requests by route pattern, method and status")
                    .Register(registry)),
      latency_(prometheus::BuildHistogram()
                   .Name("cartwheel_http_request_duration_seconds")
                   .Help("HTTP request latency by route pattern")
                   .Register(registry)),
      inflight_gauge_(prometheus::BuildGauge()
                          .Name("cartwheel_http_inflight_requests")
                          .Help("Requests currently being handled")
                          .Register(registry)
                          .Add({})),
      shed_(prometheus::BuildCounter()
                .Name("cartwheel_http_shed_total")
                .Help("Requests rejected with 503 because max_inflight was reached")
                .Register(registry)
                .Add({})) {}

HttpServer::~HttpServer() {
  Stop();
}

void HttpServer::SetUpgradeHandler(std::string path, UpgradeHandler handler) {
  upgrade_path_ = std::move(path);
  upgrade_ = std::move(handler);
}

Status HttpServer::Start() {
  try {
    const tcp::endpoint ep{asio::ip::make_address(options_.address), options_.port};
    acceptor_ = std::make_unique<tcp::acceptor>(accept_context_);
    acceptor_->open(ep.protocol());
    acceptor_->set_option(asio::socket_base::reuse_address(true));
    acceptor_->bind(ep);
    acceptor_->listen(asio::socket_base::max_listen_connections);
    bound_port_ = acceptor_->local_endpoint().port();
  } catch (const std::exception& e) {
    return Status::Unavailable(
        fmt::format("bind {}:{}: {}", options_.address, options_.port, e.what()));
  }
  pool_.Start();
  asio::co_spawn(accept_context_, AcceptLoop(), asio::detached);
  accept_thread_ = std::thread([this] { accept_context_.run(); });
  return Status::Ok();
}

void HttpServer::Stop() {
  // Stop accepting first, then stop the connection loops. Destroying the
  // contexts destroys suspended session coroutines and closes their sockets.
  accept_context_.stop();
  if (accept_thread_.joinable()) accept_thread_.join();
  acceptor_.reset();
  pool_.Stop();
}

asio::awaitable<void> HttpServer::AcceptLoop() {
  for (;;) {
    auto& ctx = pool_.Next();
    auto [ec, socket] = co_await acceptor_->async_accept(ctx, asio::as_tuple(asio::use_awaitable));
    if (ec) {
      if (ec == asio::error::operation_aborted) co_return;
      CW_LOG_WARN("accept: {}", ec.message());
      continue;
    }
    socket.set_option(tcp::no_delay(true));
    asio::co_spawn(ctx, Session(std::move(socket)), asio::detached);
  }
}

asio::awaitable<void> HttpServer::Session(tcp::socket socket) {
  beast::tcp_stream stream(std::move(socket));
  beast::flat_buffer buffer;
  for (;;) {
    stream.expires_after(options_.idle_timeout);
    http::request_parser<http::string_body> parser;
    parser.body_limit(options_.max_body_bytes);
    auto [ec, n] =
        co_await http::async_read(stream, buffer, parser, asio::as_tuple(asio::use_awaitable));
    if (ec) break;  // closed, timed out, or malformed: drop the connection
    if (upgrade_ && boost::beast::websocket::is_upgrade(parser.get())) {
      std::string_view target = parser.get().target();
      target = target.substr(0, target.find('?'));
      if (target == upgrade_path_) {
        stream.expires_never();
        co_await upgrade_(std::move(stream), parser.release());
        co_return;
      }
    }
    const Request& req = parser.get();

    const auto start = std::chrono::steady_clock::now();
    // Propagate the caller's request id (bounded, to keep logs sane) or mint one.
    std::string request_id(req["X-Request-Id"].substr(0, 64));
    if (request_id.empty()) {
      request_id = fmt::format("gw-{:x}-{:x}", bound_port_,
                               request_seq_.fetch_add(1, std::memory_order_relaxed));
    }
    std::string route = "unmatched";

    Response res;
    const int64_t now_inflight = inflight_.fetch_add(1, std::memory_order_relaxed) + 1;
    inflight_gauge_.Increment();
    if (now_inflight > options_.max_inflight) {
      // Shedding costs almost nothing, so the server stays responsive while
      // overloaded instead of queueing work its callers will time out on.
      shed_.Increment();
      RequestContext ctx{req, {}, request_id, "shed"};
      res = ErrorResponse(Status::ResourceExhausted("gateway overloaded"), ctx);
      route = "shed";
    } else {
      // A caller's traceparent (a browser, k6, another service) continues
      // its trace; otherwise the gateway starts one.
      tracing::Span span =
          tracing::StartSpan("HTTP " + std::string(req.method_string()), tracing::Kind::kServer,
                             std::string_view(req["traceparent"]));
      res = co_await Dispatch(req, request_id, &route, &span);
      span.SetName(std::string(req.method_string()) + " " + route);
      span.SetAttribute("http.request.method", std::string_view(req.method_string()));
      span.SetAttribute("http.route", route);
      span.SetAttribute("http.response.status_code", static_cast<int64_t>(res.result_int()));
      span.SetAttribute("cartwheel.request_id", request_id);
      if (res.result_int() >= 500) span.SetError(res.reason());
      if (span) res.set("X-Trace-Id", span.TraceId());
    }
    inflight_.fetch_sub(1, std::memory_order_relaxed);
    inflight_gauge_.Decrement();

    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    latency_.Add({{"route", route}}, LatencyBucketsSeconds()).Observe(secs);
    requests_
        .Add({{"route", route},
              {"method", std::string(req.method_string())},
              {"status", std::to_string(res.result_int())}})
        .Increment();
    CW_LOG_DEBUG("{} {} {} {} {:.2f}ms rid={}", req.method_string(), req.target(), res.result_int(),
                 route, secs * 1e3, request_id);

    const bool keep_alive = res.keep_alive();
    auto [wec, wn] = co_await http::async_write(stream, res, asio::as_tuple(asio::use_awaitable));
    if (wec || !keep_alive) break;
  }
  boost::system::error_code ignored;
  // The error is reported through `ignored`; a peer that already left is not an error here.
  // NOLINTNEXTLINE(bugprone-unused-return-value)
  stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
}

asio::awaitable<Response> HttpServer::Dispatch(const Request& req, std::string request_id,
                                               std::string* route, const tracing::Span* span) {
  auto match = router_->Find(req.method(), req.target());
  RequestContext ctx{req, std::move(match.params), std::move(request_id), match.route, span};
  if (match.handler == nullptr) {
    *route = match.path_known ? "method_not_allowed" : "not_found";
    co_return JsonResponse(
        match.path_known ? http::status::method_not_allowed : http::status::not_found,
        {{"error",
          {{"code", match.path_known ? "MethodNotAllowed" : "NotFound"},
           {"message", std::string(req.target())}}}},
        ctx);
  }
  *route = match.route;
  try {
    co_return co_await (*match.handler)(ctx);
  } catch (const std::exception& e) {
    CW_LOG_ERROR("handler {} threw: {}", match.route, e.what());
    co_return ErrorResponse(Status::Internal("unexpected error"), ctx);
  }
}

}  // namespace cartwheel::gateway
