// deal-svc: lightning deals.
//
//   backend=infra (default): Redis gate, Redpanda/Kafka log, Postgres store;
//                            order-worker runs as its own process.
//   backend=memory:          everything in-process, including the worker
//                            loop; for demos and frontend work without infra.
//
//   CARTWHEEL_DEAL_BACKEND=infra CARTWHEEL_POSTGRES_URL=postgresql://... \
//   CARTWHEEL_REDIS_URL=redis://redis:6379 CARTWHEEL_KAFKA_BROKERS=redpanda:9092 deal-svc

#include <atomic>
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
#include "common/thread_pool.h"
#include "services/deal/deal_catalog.h"
#include "services/deal/deal_engine.h"
#include "services/deal/deal_service.h"
#include "services/deal/kafka_log.h"
#include "services/deal/memory_ports.h"
#include "services/deal/order_processor.h"
#include "services/deal/pg_store.h"
#include "services/deal/redis_gate.h"

namespace {

using namespace cartwheel;
using namespace cartwheel::deal;

// Demo catalog: discounted units of storefront products.
std::vector<DealRecord> DemoDeals(int64_t now_ms, int64_t first_start_in_ms) {
  constexpr int64_t kMinute = 60'000;
  return {
      {.deal_id = "dl-headphones",
       .sku = "CW-1005",
       .title = "Wired headphones",
       .price_cents = 2900,
       .list_price_cents = 4200,
       .quantity = 100,
       .starts_at_ms = now_ms + first_start_in_ms,
       .ends_at_ms = now_ms + first_start_in_ms + 30 * kMinute},
      {.deal_id = "dl-lamp",
       .sku = "CW-1004",
       .title = "Articulated desk lamp",
       .price_cents = 3900,
       .list_price_cents = 5800,
       .quantity = 50,
       .starts_at_ms = now_ms + first_start_in_ms + 2 * kMinute,
       .ends_at_ms = now_ms + first_start_in_ms + 32 * kMinute},
      {.deal_id = "dl-kettle",
       .sku = "CW-1001",
       .title = "Stovetop kettle",
       .price_cents = 2200,
       .list_price_cents = 3400,
       .quantity = 200,
       .starts_at_ms = now_ms + first_start_in_ms + 5 * kMinute,
       .ends_at_ms = now_ms + first_start_in_ms + 35 * kMinute},
  };
}

// Inserts the demo deals; restarts any that have ended so the demo always
// has something live.
Status SeedDeals(OrderStore* store, DealEngine* engine, const Clock* clock, int64_t start_in_ms) {
  const int64_t now = clock->WallMillis();
  for (const auto& d : DemoDeals(now, start_in_ms)) CW_RETURN_IF_ERROR(store->UpsertDeal(d));
  CW_ASSIGN_OR_RETURN(auto deals, store->LoadDeals());
  for (const auto& d : deals) {
    if (d.ends_at_ms > now) continue;
    v1::ResetDealRequest req;
    req.set_deal_id(d.deal_id);
    req.set_quantity(d.quantity);
    req.set_starts_in_ms(start_in_ms);
    req.set_duration_ms(d.ends_at_ms - d.starts_at_ms);
    CW_RETURN_IF_ERROR(engine->Reset(req).status());
  }
  return Status::Ok();
}

}  // namespace

int main(int argc, char** argv) {
  BlockTerminationSignals();

  CLI::App app{"deal-svc: lightning deals (Redis gate, Redpanda log, Postgres)"};
  std::string config_path;
  app.add_option("-c,--config", config_path, "TOML config file (env vars override it)");
  CLI11_PARSE(app, argc, argv);

  auto cfg_result = config_path.empty() ? Result<Config>(Config()) : Config::FromFile(config_path);
  if (!cfg_result.ok()) {
    fmt::print(stderr, "deal-svc: {}\n", cfg_result.status().ToString());
    return 2;
  }
  const Config& cfg = *cfg_result;
  InitLogging({.service = "deal-svc",
               .level = cfg.GetString("log.level", "info"),
               .json = cfg.GetBool("log.json", false)});

  auto registry = GlobalMetricsRegistry();
  RegisterBuildInfo(*registry, "deal-svc");
  auto metrics = MetricsServer::Start(cfg.GetString("metrics.listen", "0.0.0.0:9300"), registry);
  if (!metrics.ok()) {
    CW_LOG_ERROR("{}", metrics.status().ToString());
    return 1;
  }

  const std::string backend = cfg.GetString("deal.backend", "infra");
  const Clock* clock = SystemClock::Default();

  std::unique_ptr<InventoryGate> gate;
  std::unique_ptr<OrderLog> log;
  std::unique_ptr<OrderStore> store;
  // memory backend only: the in-process worker.
  MemoryOrderLog* memory_log = nullptr;

  if (backend == "memory") {
    gate = std::make_unique<MemoryInventoryGate>();
    auto mlog = std::make_unique<MemoryOrderLog>();
    memory_log = mlog.get();
    log = std::move(mlog);
    store = std::make_unique<MemoryOrderStore>();
  } else {
    auto pg = std::make_unique<PgOrderStore>(std::make_shared<PgPool>(
        cfg.GetString("postgres.url", "postgresql://cartwheel:cartwheel@localhost:5432/cartwheel"),
        static_cast<size_t>(cfg.GetInt("postgres.pool", 16)),
        cfg.GetInt("postgres.acquire_timeout_ms", 200)));
    if (auto s = pg->Migrate(); !s.ok()) {
      CW_LOG_ERROR("postgres: {}", s.ToString());
      return 1;
    }
    store = std::move(pg);

    auto redis_opts = ParseRedisUrl(cfg.GetString("redis.url", "redis://127.0.0.1:6379"));
    if (!redis_opts.ok()) {
      CW_LOG_ERROR("{}", redis_opts.status().ToString());
      return 2;
    }
    redis_opts->pool_size = static_cast<size_t>(cfg.GetInt("redis.pool", 32));
    gate = std::make_unique<RedisInventoryGate>(ConnectRedis(*redis_opts));

    KafkaOptions kopts{.brokers = cfg.GetString("kafka.brokers", "127.0.0.1:9092"),
                       .topic = cfg.GetString("kafka.topic", "deal-orders"),
                       .partitions = static_cast<int>(cfg.GetInt("kafka.partitions", 12)),
                       .replication = static_cast<int>(cfg.GetInt("kafka.replication", 1))};
    if (auto s = EnsureTopic(kopts); !s.ok()) {
      CW_LOG_ERROR("kafka: {}", s.ToString());
      return 1;
    }
    auto klog = KafkaOrderLog::Create(kopts);
    if (!klog.ok()) {
      CW_LOG_ERROR("{}", klog.status().ToString());
      return 1;
    }
    log = std::move(klog).value();
  }

  DealCatalog catalog(store.get(), clock, cfg.GetInt("catalog.ttl_ms", 1000));
  DealEngine engine(&catalog, gate.get(), log.get(), store.get(), clock, *registry);
  if (cfg.GetBool("deal.seed", true)) {
    if (auto s =
            SeedDeals(store.get(), &engine, clock, cfg.GetInt("deal.seed_start_in_ms", 60'000));
        !s.ok()) {
      CW_LOG_ERROR("seeding deals: {}", s.ToString());
      return 1;
    }
    catalog.Invalidate();
  }
  if (auto s = engine.ArmAll(); !s.ok()) {
    CW_LOG_ERROR("arming the gate: {}", s.ToString());
    return 1;
  }

  std::atomic<bool> stop_worker{false};
  std::thread worker;
  std::unique_ptr<OrderProcessor> processor;
  if (memory_log != nullptr) {
    // No push channel without Redis; clients see updates by polling orders.
    processor = std::make_unique<OrderProcessor>(store.get(), nullptr, *registry);
    worker = std::thread([&] { RunWorkerLoop(memory_log, processor.get(), &stop_worker); });
  }

  // Hold sweeper: any hold without a stored order after the grace period is
  // re-published (covers a crash between Reserve and Publish).
  std::thread sweeper([&] {
    const auto interval = std::chrono::milliseconds(cfg.GetInt("deal.sweep_interval_ms", 5000));
    const int64_t grace = cfg.GetInt("deal.sweep_grace_ms", 30'000);
    while (!stop_worker.load()) {
      for (auto waited = std::chrono::milliseconds(0); waited < interval && !stop_worker.load();
           waited += std::chrono::milliseconds(100)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (stop_worker.load()) break;
      if (auto r = engine.SweepOnce(grace); !r.ok()) {
        CW_LOG_WARN("hold sweeper: {}", r.status().ToString());
      }
    }
  });

  ThreadPool workers({.threads = static_cast<size_t>(cfg.GetInt("workers.threads", 16)),
                      .max_queue = static_cast<size_t>(cfg.GetInt("workers.max_queue", 1024)),
                      .name = "deal"});
  DealServiceImpl service(&engine, &workers, *registry);

  grpc::EnableDefaultHealthCheckService(true);
#if defined(CARTWHEEL_HAVE_GRPC_REFLECTION)
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
#endif
  const std::string listen = cfg.GetString("server.listen", "0.0.0.0:7300");
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  if (!server || port == 0) {
    CW_LOG_ERROR("failed to listen on {}", listen);
    return 1;
  }
  CW_LOG_INFO("deal-svc serving on {} (backend={}, version {} {})", listen, backend, kVersion,
              kGitSha);

  const int sig = WaitForTerminationSignal();
  CW_LOG_INFO("signal {}: shutting down", sig);
  server->GetHealthCheckService()->SetServingStatus(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(cfg.GetInt("server.drain_grace_ms", 1000)));
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));
  workers.Shutdown();
  if (auto* klog = dynamic_cast<KafkaOrderLog*>(log.get())) klog->Flush(5000);
  stop_worker.store(true);
  sweeper.join();
  if (memory_log != nullptr) memory_log->Close();
  if (worker.joinable()) worker.join();
  return 0;
}
