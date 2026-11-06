// cart-svc: shopping carts on CartwheelKV.
//
//   CARTWHEEL_KV_TARGET=wheel-1:7100 CARTWHEEL_SERVER_LISTEN=0.0.0.0:7200 cart-svc

#include <chrono>
#include <memory>
#include <thread>

#include <CLI/CLI.hpp>
#if defined(CARTWHEEL_HAVE_GRPC_REFLECTION)
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#endif
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/server_builder.h>

#include "client/kv_client.h"
#include "common/build_info.h"
#include "common/config.h"
#include "common/logging.h"
#include "common/metrics.h"
#include "common/signals.h"
#include "common/thread_pool.h"
#include "services/cart/cart_service.h"
#include "services/cart/cart_store.h"

int main(int argc, char** argv) {
  using namespace cartwheel;
  BlockTerminationSignals();

  CLI::App app{"cart-svc: shopping carts on CartwheelKV"};
  std::string config_path;
  app.add_option("-c,--config", config_path, "TOML config file (env vars override it)");
  CLI11_PARSE(app, argc, argv);

  auto cfg_result = config_path.empty() ? Result<Config>(Config()) : Config::FromFile(config_path);
  if (!cfg_result.ok()) {
    fmt::print(stderr, "cart-svc: {}\n", cfg_result.status().ToString());
    return 2;
  }
  const Config& cfg = *cfg_result;
  InitLogging({.service = "cart-svc",
               .level = cfg.GetString("log.level", "info"),
               .json = cfg.GetBool("log.json", false)});

  const std::string listen = cfg.GetString("server.listen", "0.0.0.0:7200");
  const std::string kv_target = cfg.GetString("kv.target", "localhost:7100");

  auto registry = GlobalMetricsRegistry();
  RegisterBuildInfo(*registry, "cart-svc");
  auto metrics = MetricsServer::Start(cfg.GetString("metrics.listen", "0.0.0.0:9200"), registry);
  if (!metrics.ok()) {
    CW_LOG_ERROR("{}", metrics.status().ToString());
    return 1;
  }

  client::KvClient kv({.target = kv_target, .timeout_ms = cfg.GetInt("kv.timeout_ms", 300)});
  cart::CartStore store(&kv);
  ThreadPool workers({.threads = static_cast<size_t>(cfg.GetInt("workers.threads", 16)),
                      .max_queue = static_cast<size_t>(cfg.GetInt("workers.max_queue", 512)),
                      .name = "cart"});
  cart::CartServiceImpl service(&store, &workers, *registry);

  grpc::EnableDefaultHealthCheckService(true);
#if defined(CARTWHEEL_HAVE_GRPC_REFLECTION)
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
#endif
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  if (!server || port == 0) {
    CW_LOG_ERROR("failed to listen on {}", listen);
    return 1;
  }
  CW_LOG_INFO("cart-svc serving on {} -> kv {} (version {} {})", listen, kv_target, kVersion,
              kGitSha);

  const int sig = WaitForTerminationSignal();
  CW_LOG_INFO("signal {}: shutting down", sig);
  server->GetHealthCheckService()->SetServingStatus(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(cfg.GetInt("server.drain_grace_ms", 1000)));
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));
  workers.Shutdown();
  return 0;
}
