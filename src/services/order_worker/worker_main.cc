// order-worker: consumes reservations from Redpanda and commits orders to
// Postgres in batches, then publishes each outcome on Redis for the gateway's
// WebSocket push. Run as many as there are partitions; the consumer group
// spreads partitions over them.
//
//   CARTWHEEL_POSTGRES_URL=postgresql://... CARTWHEEL_REDIS_URL=redis://redis:6379 \
//   CARTWHEEL_KAFKA_BROKERS=redpanda:9092 order-worker

#include <atomic>
#include <memory>
#include <thread>

#include <CLI/CLI.hpp>

#include "common/build_info.h"
#include "common/config.h"
#include "common/logging.h"
#include "common/metrics.h"
#include "common/signals.h"
#include "services/deal/kafka_log.h"
#include "services/deal/order_processor.h"
#include "services/deal/pg_store.h"
#include "services/deal/redis_gate.h"

int main(int argc, char** argv) {
  using namespace cartwheel;
  using namespace cartwheel::deal;
  BlockTerminationSignals();

  CLI::App app{"order-worker: reservations -> committed orders"};
  std::string config_path;
  app.add_option("-c,--config", config_path, "TOML config file (env vars override it)");
  CLI11_PARSE(app, argc, argv);

  auto cfg_result = config_path.empty() ? Result<Config>(Config()) : Config::FromFile(config_path);
  if (!cfg_result.ok()) {
    fmt::print(stderr, "order-worker: {}\n", cfg_result.status().ToString());
    return 2;
  }
  const Config& cfg = *cfg_result;
  InitLogging({.service = "order-worker",
               .level = cfg.GetString("log.level", "info"),
               .json = cfg.GetBool("log.json", false)});

  auto registry = GlobalMetricsRegistry();
  RegisterBuildInfo(*registry, "order-worker");
  auto metrics = MetricsServer::Start(cfg.GetString("metrics.listen", "0.0.0.0:9400"), registry);
  if (!metrics.ok()) {
    CW_LOG_ERROR("{}", metrics.status().ToString());
    return 1;
  }

  PgOrderStore store(std::make_shared<PgPool>(
      cfg.GetString("postgres.url", "postgresql://cartwheel:cartwheel@localhost:5432/cartwheel"),
      static_cast<size_t>(cfg.GetInt("postgres.pool", 2)),
      cfg.GetInt("postgres.acquire_timeout_ms", 1000)));
  if (auto s = store.Migrate(); !s.ok()) {
    CW_LOG_ERROR("postgres: {}", s.ToString());
    return 1;
  }

  auto redis_opts = ParseRedisUrl(cfg.GetString("redis.url", "redis://127.0.0.1:6379"));
  if (!redis_opts.ok()) {
    CW_LOG_ERROR("{}", redis_opts.status().ToString());
    return 2;
  }
  RedisNotifier notifier(ConnectRedis(*redis_opts));

  const KafkaOptions kopts{.brokers = cfg.GetString("kafka.brokers", "127.0.0.1:9092"),
                           .topic = cfg.GetString("kafka.topic", "deal-orders"),
                           .partitions = static_cast<int>(cfg.GetInt("kafka.partitions", 12)),
                           .replication = static_cast<int>(cfg.GetInt("kafka.replication", 1))};
  if (auto s = EnsureTopic(kopts); !s.ok()) {
    CW_LOG_ERROR("kafka: {}", s.ToString());
    return 1;
  }
  auto source = KafkaOrderSource::Create(kopts, cfg.GetString("kafka.group", "order-worker"));
  if (!source.ok()) {
    CW_LOG_ERROR("{}", source.status().ToString());
    return 1;
  }

  OrderProcessor processor(&store, &notifier, *registry);
  std::atomic<bool> stop{false};
  WorkerLoopOptions loop{.max_batch = static_cast<size_t>(cfg.GetInt("worker.max_batch", 500)),
                         .poll_wait_ms = cfg.GetInt("worker.poll_wait_ms", 20)};
  std::thread consumer([&] { RunWorkerLoop(source->get(), &processor, &stop, loop); });
  CW_LOG_INFO("order-worker consuming {} from {} (batch {}, version {} {})", kopts.topic,
              kopts.brokers, loop.max_batch, kVersion, kGitSha);

  const int sig = WaitForTerminationSignal();
  CW_LOG_INFO("signal {}: finishing the current batch", sig);
  stop.store(true);
  consumer.join();
  source->reset();  // close(): leave the group so partitions move at once
  return 0;
}
