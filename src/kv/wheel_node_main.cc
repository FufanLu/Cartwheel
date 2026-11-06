// wheel-node: one CartwheelKV storage node.
//
//   wheel-node --config deploy/config/wheel-node.toml
//   CARTWHEEL_NODE_ID=wheel-1 CARTWHEEL_SERVER_LISTEN=0.0.0.0:7100 wheel-node
//
// Shutdown follows the usual drain sequence. SIGTERM marks the node DRAINING,
// so health checks fail and load balancers stop routing to it. After a grace
// period it stops accepting RPCs, then lets in-flight RPCs finish until a
// deadline.

#include <chrono>
#include <memory>
#include <thread>

#include <CLI/CLI.hpp>
#if defined(CARTWHEEL_HAVE_GRPC_REFLECTION)
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#endif
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/server_builder.h>

#include "common/build_info.h"
#include "common/config.h"
#include "common/logging.h"
#include "common/metrics.h"
#include "common/signals.h"
#include "kv/kv_service.h"
#include "kv/local_store.h"
#include "storage/engine.h"

int main(int argc, char** argv) {
  using namespace cartwheel;
  BlockTerminationSignals();  // before any thread is spawned, so all inherit the mask

  CLI::App app{"wheel-node: CartwheelKV storage node"};
  std::string config_path;
  app.add_option("-c,--config", config_path, "TOML config file (env vars override it)");
  bool version = false;
  app.add_flag("--version", version, "print version and exit");
  CLI11_PARSE(app, argc, argv);
  if (version) {
    fmt::print("wheel-node {} ({}, {})\n", kVersion, kGitSha, kBuildType);
    return 0;
  }

  auto cfg_result = config_path.empty() ? Result<Config>(Config()) : Config::FromFile(config_path);
  if (!cfg_result.ok()) {
    fmt::print(stderr, "wheel-node: {}\n", cfg_result.status().ToString());
    return 2;
  }
  const Config& cfg = *cfg_result;

  InitLogging({.service = "wheel-node",
               .level = cfg.GetString("log.level", "info"),
               .json = cfg.GetBool("log.json", false)});

  const std::string node_id = cfg.GetString("node.id", "wheel-1");
  const std::string listen = cfg.GetString("server.listen", "0.0.0.0:7100");
  const std::string metrics_listen = cfg.GetString("metrics.listen", "0.0.0.0:9100");
  const auto drain_grace = std::chrono::milliseconds(cfg.GetInt("server.drain_grace_ms", 2000));
  const auto shutdown_deadline =
      std::chrono::milliseconds(cfg.GetInt("server.shutdown_deadline_ms", 5000));

  auto registry = GlobalMetricsRegistry();
  RegisterBuildInfo(*registry, "wheel-node");
  auto metrics = MetricsServer::Start(metrics_listen, registry);
  if (!metrics.ok()) {
    CW_LOG_ERROR("{}", metrics.status().ToString());
    return 1;
  }

  kv::LocalStore store(node_id, storage::NewMemEngine());
  kv::KvServiceImpl kv_service(&store, *registry);
  kv::AdminServiceImpl admin_service(&store);

  grpc::EnableDefaultHealthCheckService(true);
#if defined(CARTWHEEL_HAVE_GRPC_REFLECTION)
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
#endif
  grpc::ServerBuilder builder;
  int bound_port = 0;
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials(), &bound_port);
  builder.RegisterService(&kv_service);
  builder.RegisterService(&admin_service);
  // Bound concurrent streams per connection; excess requests queue in HTTP/2
  // flow control instead of piling up inside the server.
  builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS, 1024);
  builder.SetMaxReceiveMessageSize(static_cast<int>(kv::kMaxValueBytes + size_t{64} * 1024));
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server || bound_port == 0) {
    CW_LOG_ERROR("failed to listen on {}", listen);
    return 1;
  }
  server->GetHealthCheckService()->SetServingStatus(true);
  CW_LOG_INFO("wheel-node {} serving on {} (engine={}, metrics :{}, version {} {})", node_id,
              listen, store.engine().Name(), (*metrics)->port(), kVersion, kGitSha);

  const int sig = WaitForTerminationSignal();
  CW_LOG_INFO("signal {}: draining for {} ms", sig, drain_grace.count());
  admin_service.SetDraining();
  server->GetHealthCheckService()->SetServingStatus(false);
  std::this_thread::sleep_for(drain_grace);
  server->Shutdown(std::chrono::system_clock::now() + shutdown_deadline);
  CW_LOG_INFO("wheel-node {} stopped", node_id);
  return 0;
}
