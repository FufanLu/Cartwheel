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
#include "common/tracing.h"
#include "kv/cluster_metrics.h"
#include "kv/cluster_node.h"
#include "kv/kv_service.h"
#include "kv/local_store.h"
#include "kv/peer_transport.h"
#include "kv/timer_scheduler.h"
#include "storage/engine.h"
#include "storage/lsm/db.h"

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
  // tracing.otlp_endpoint = "host:4317" turns on OpenTelemetry export.
  tracing::Init("wheel-node", cfg.GetString("tracing.otlp_endpoint", ""));

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

  // storage.engine = "lsm" (default, durable) | "mem" (tests, demos)
  std::unique_ptr<storage::Engine> engine;
  const std::string engine_name = cfg.GetString("storage.engine", "lsm");
  if (engine_name == "mem") {
    engine = storage::NewMemEngine();
  } else if (engine_name == "lsm") {
    storage::lsm::Options lsm;
    lsm.write_buffer_size = static_cast<size_t>(cfg.GetInt("storage.write_buffer_bytes", 4 << 20));
    lsm.block_cache_bytes = static_cast<size_t>(cfg.GetInt("storage.block_cache_bytes", 64 << 20));
    const std::string dir = cfg.GetString("storage.dir", "./data/" + node_id);
    auto opened = storage::lsm::NewLsmEngine(lsm, dir);
    if (!opened.ok()) {
      CW_LOG_ERROR("cannot open storage at {}: {}", dir, opened.status().ToString());
      return 1;
    }
    engine = std::move(opened).value();
    CW_LOG_INFO("storage: lsm at {}", dir);
  } else {
    CW_LOG_ERROR("unknown storage.engine '{}' (expected lsm or mem)", engine_name);
    return 2;
  }
  kv::LocalStore store(node_id, std::move(engine));

  // Cluster mode (cluster.enabled = true): a Dynamo-style node that gossips
  // with its seeds and coordinates quorum reads and writes.
  //   cluster.seeds = ["wheel-2=wheel-2:7100", ...]   (id=address)
  //   node.advertise = "wheel-1:7100"                 (how peers reach us)
  const bool clustered = cfg.GetBool("cluster.enabled", false);
  std::unique_ptr<kv::TimerScheduler> scheduler;
  std::unique_ptr<kv::GrpcPeerTransport> transport;
  std::unique_ptr<kv::ClusterNode> cluster_node;
  std::unique_ptr<kv::PeerServiceImpl> peer_service;
  // debug.fault_injection: lets the ops console partition this node.
  std::unique_ptr<kv::FaultSwitch> fault;
  std::unique_ptr<kv::IsolatingTransport> isolating_transport;
  std::unique_ptr<kv::IsolatingHandler> isolating_handler;
  if (cfg.GetBool("debug.fault_injection", false)) {
    fault = std::make_unique<kv::FaultSwitch>(SystemClock::Default());
    CW_LOG_WARN("fault injection enabled: the admin API can isolate this node");
  }
  if (clustered) {
    kv::ClusterOptions copts;
    copts.n = static_cast<size_t>(cfg.GetInt("cluster.n", 3));
    copts.r = static_cast<size_t>(cfg.GetInt("cluster.r", 2));
    copts.w = static_cast<size_t>(cfg.GetInt("cluster.w", 2));
    copts.vnodes = static_cast<int>(cfg.GetInt("cluster.vnodes", 64));
    scheduler = std::make_unique<kv::TimerScheduler>();
    // Resolve peer addresses through the node's membership table. Captures
    // `cluster_node` (main's scope), which outlives the transport.
    transport =
        std::make_unique<kv::GrpcPeerTransport>(node_id, [&cluster_node](const std::string& n) {
          return cluster_node ? cluster_node->membership().AddressOf(n) : std::string();
        });
    // Wall-clock millis: strictly larger after every restart, so peers
    // accept our reset heartbeat counter.
    const auto incarnation = static_cast<uint64_t>(SystemClock::Default()->WallMillis());
    cluster::Transport* peer_transport = transport.get();
    if (fault) {
      isolating_transport =
          std::make_unique<kv::IsolatingTransport>(transport.get(), scheduler.get(), fault.get());
      peer_transport = isolating_transport.get();
    }
    cluster::Runtime rt{peer_transport, scheduler.get(), SystemClock::Default(),
                        static_cast<uint64_t>(SystemClock::Default()->NowNanos())};
    cluster_node = std::make_unique<kv::ClusterNode>(
        node_id, cfg.GetString("node.advertise", listen), incarnation, &store, rt, copts);
    for (const auto& seed : cfg.GetStringList("cluster.seeds")) {
      const auto eq = seed.find('=');
      if (eq == std::string::npos) {
        CW_LOG_ERROR("bad cluster seed '{}' (expected id=host:port)", seed);
        return 2;
      }
      cluster_node->AddSeed(seed.substr(0, eq), seed.substr(eq + 1));
    }
    cluster::RequestHandler* handler = cluster_node.get();
    if (fault) {
      isolating_handler = std::make_unique<kv::IsolatingHandler>(cluster_node.get(), fault.get());
      handler = isolating_handler.get();
    }
    peer_service = std::make_unique<kv::PeerServiceImpl>(handler);
  }

  std::unique_ptr<kv::ClusterMetrics> cluster_metrics;
  if (cluster_node) {
    cluster_metrics = std::make_unique<kv::ClusterMetrics>(*registry, cluster_node.get());
  }
  kv::KvServiceImpl kv_service(&store, *registry, cluster_node.get());
  kv::AdminServiceImpl admin_service(&store, cluster_node.get(), fault.get());

  grpc::EnableDefaultHealthCheckService(true);
#if defined(CARTWHEEL_HAVE_GRPC_REFLECTION)
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
#endif
  grpc::ServerBuilder builder;
  int bound_port = 0;
  // gRPC sets SO_REUSEPORT by default: a second instance would silently share
  // the port and split traffic. Fail fast instead.
  builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials(), &bound_port);
  builder.RegisterService(&kv_service);
  builder.RegisterService(&admin_service);
  if (peer_service) builder.RegisterService(peer_service.get());
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
  if (cluster_node) cluster_node->Start();
  if (cluster_metrics) cluster_metrics->Start();
  CW_LOG_INFO("wheel-node {} serving on {} (engine={}, {}, metrics :{}, version {} {})", node_id,
              listen, store.engine().Name(),
              clustered ? fmt::format("cluster N={} R={} W={}", cluster_node->options().n,
                                      cluster_node->options().r, cluster_node->options().w)
                        : std::string("single node"),
              (*metrics)->port(), kVersion, kGitSha);

  const int sig = WaitForTerminationSignal();
  CW_LOG_INFO("signal {}: draining for {} ms", sig, drain_grace.count());
  admin_service.SetDraining();
  server->GetHealthCheckService()->SetServingStatus(false);
  std::this_thread::sleep_for(drain_grace);
  if (cluster_metrics) cluster_metrics->Stop();
  if (cluster_node) cluster_node->Stop();
  server->Shutdown(std::chrono::system_clock::now() + shutdown_deadline);
  if (scheduler) scheduler->Stop();
  CW_LOG_INFO("wheel-node {} stopped", node_id);
  tracing::Shutdown();
  return 0;
}
