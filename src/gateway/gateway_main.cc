// gateway: the HTTP edge. REST/JSON in, gRPC out.
//
//   CARTWHEEL_CART_TARGET=cart-svc:7200 CARTWHEEL_SERVER_PORT=8080 gateway

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
#include "gateway/cart_routes.h"
#include "gateway/http_server.h"

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

  const std::string cart_target = cfg.GetString("cart.target", "localhost:7200");
  CartRoutes cart_routes(cart::v1::CartService::NewStub(MakeChannel(cart_target)),
                         std::chrono::milliseconds(cfg.GetInt("cart.timeout_ms", 800)));

  Router router;
  cart_routes.Register(&router);
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
  if (auto s = server.Start(); !s.ok()) {
    CW_LOG_ERROR("{}", s.ToString());
    return 1;
  }
  CW_LOG_INFO("gateway listening on :{} -> cart-svc {} (version {} {})", server.port(), cart_target,
              kVersion, kGitSha);

  const int sig = WaitForTerminationSignal();
  CW_LOG_INFO("signal {}: stopping", sig);
  server.Stop();
  return 0;
}
