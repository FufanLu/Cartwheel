// gateway: the HTTP edge. REST/JSON and WebSocket in, gRPC out.
//
//   CARTWHEEL_CART_TARGET=cart-svc:7200 CARTWHEEL_DEAL_TARGET=deal-svc:7300 \
//   CARTWHEEL_AUTH_JWT_SECRET=... CARTWHEEL_NOTIFY_REDIS_URL=redis://redis:6379 gateway

#include <chrono>
#include <memory>
#include <thread>

#include <CLI/CLI.hpp>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/channel_arguments.h>
#include <nlohmann/json.hpp>

#include "common/build_info.h"
#include "common/config.h"
#include "common/logging.h"
#include "common/metrics.h"
#include "common/signals.h"
#include "gateway/auth.h"
#include "gateway/cart_routes.h"
#include "gateway/deal_routes.h"
#include "gateway/http_server.h"
#include "gateway/ops_routes.h"
#include "gateway/ws_hub.h"

namespace {

std::shared_ptr<grpc::Channel> MakeChannel(const std::string& target) {
  grpc::ChannelArguments args;
  args.SetLoadBalancingPolicyName("round_robin");
  args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 30'000);
  args.SetInt(GRPC_ARG_ENABLE_RETRIES, 0);  // retry policy lives in clients, with jitter
  return grpc::CreateCustomChannel(target, grpc::InsecureChannelCredentials(), args);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace cartwheel;
  using namespace cartwheel::gateway;
  BlockTerminationSignals();

  CLI::App app{"gateway: Cartwheel HTTP edge"};
  std::string config_path;
  app.add_option("-c,--config", config_path, "TOML config file (env vars override it)");
  CLI11_PARSE(app, argc, argv);

  auto cfg_result = config_path.empty() ? Result<Config>(Config()) : Config::FromFile(config_path);
  if (!cfg_result.ok()) {
    fmt::print(stderr, "gateway: {}\n", cfg_result.status().ToString());
    return 2;
  }
  const Config& cfg = *cfg_result;
  InitLogging({.service = "gateway",
               .level = cfg.GetString("log.level", "info"),
               .json = cfg.GetBool("log.json", false)});

  auto registry = GlobalMetricsRegistry();
  RegisterBuildInfo(*registry, "gateway");

  std::string secret = cfg.GetString("auth.jwt_secret", "");
  if (secret.empty()) {
    secret = "cartwheel-dev-secret-change-me";
    CW_LOG_WARN("auth.jwt_secret is not set; using the built-in development secret");
  }
  const Authenticator auth(JwtSigner(secret), cfg.GetInt("auth.token_ttl_s", 3600));

  const std::string cart_target = cfg.GetString("cart.target", "localhost:7200");
  CartRoutes cart_routes(cart::v1::CartService::NewStub(MakeChannel(cart_target)),
                         std::chrono::milliseconds(cfg.GetInt("cart.timeout_ms", 800)), &auth,
                         cfg.GetBool("auth.anonymous_carts", true));

  const std::string deal_target = cfg.GetString("deal.target", "localhost:7300");
  DealRoutes deal_routes(
      deal::v1::DealService::NewStub(MakeChannel(deal_target)), &auth,
      {.upstream_timeout = std::chrono::milliseconds(cfg.GetInt("deal.timeout_ms", 800)),
       .claim_rate_per_sec = cfg.GetDouble("limits.claim_rate", 1.0),
       .claim_burst = cfg.GetDouble("limits.claim_burst", 3.0),
       .login_rate_per_sec = cfg.GetDouble("limits.login_rate", 2.0),
       .login_burst = cfg.GetDouble("limits.login_burst", 5.0),
       .admin_token = cfg.GetString("auth.admin_token", "")},
      *registry);

  // ops.kv_nodes = ["wheel-1=127.0.0.1:7101", ...]: the ops console's view.
  OpsRoutes ops_routes(OpsRoutes::FromSpecs(cfg.GetStringList("ops.kv_nodes")),
                       cfg.GetString("auth.admin_token", ""));

  WsHub hub(*registry);
  std::unique_ptr<OrderFeed> feed;
  if (const std::string redis = cfg.GetString("notify.redis_url", ""); !redis.empty()) {
    feed = std::make_unique<OrderFeed>(redis, &hub);
    feed->Start();
  } else {
    CW_LOG_WARN("notify.redis_url is not set; WebSocket clients get no order pushes");
  }

  Router router;
  cart_routes.Register(&router);
  deal_routes.Register(&router);
  ops_routes.Register(&router);
  router.Add(
      http::verb::get, "/healthz", [](RequestContext& c) -> boost::asio::awaitable<Response> {
        co_return JsonResponse(http::status::ok,
                               {{"status", "ok"}, {"version", kVersion}, {"git_sha", kGitSha}}, c);
      });
  router.Add(http::verb::get, "/metrics",
             [registry](RequestContext& c) -> boost::asio::awaitable<Response> {
               Response res{http::status::ok, c.request.version()};
               res.set(http::field::content_type, "text/plain; version=0.0.4");
               res.body() = SerializeMetrics(*registry);
               res.prepare_payload();
               co_return res;
             });

  HttpServer server({.address = cfg.GetString("server.address", "0.0.0.0"),
                     .port = static_cast<uint16_t>(cfg.GetInt("server.port", 8080)),
                     .threads = static_cast<size_t>(cfg.GetInt(
                         "server.threads", std::max(2U, std::thread::hardware_concurrency() / 2))),
                     .max_inflight = cfg.GetInt("server.max_inflight", 2048)},
                    &router, *registry);
  server.SetUpgradeHandler("/api/v1/ws",
                           [&hub, &auth](boost::beast::tcp_stream stream, Request req) {
                             return hub.Upgrade(std::move(stream), std::move(req), &auth);
                           });
  if (auto s = server.Start(); !s.ok()) {
    CW_LOG_ERROR("{}", s.ToString());
    return 1;
  }
  CW_LOG_INFO("gateway listening on :{} -> cart-svc {}, deal-svc {} (version {} {})", server.port(),
              cart_target, deal_target, kVersion, kGitSha);

  const int sig = WaitForTerminationSignal();
  CW_LOG_INFO("signal {}: stopping", sig);
  if (feed) feed->Stop();
  server.Stop();
  return 0;
}
