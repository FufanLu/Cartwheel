#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "services/deal/deal_ports.h"

namespace RdKafka {
class Producer;
class KafkaConsumer;
class DeliveryReportCb;
}  // namespace RdKafka

namespace cartwheel::deal {

struct KafkaOptions {
  std::string brokers = "127.0.0.1:9092";
  std::string topic = "deal-orders";
  int partitions = 12;
  int replication = 1;  // 3 in production; 1 for the single-broker dev stack
  // A reservation the broker has not acknowledged by then fails: definitely
  // (never sent) or possibly (sent, ack lost; see PublishOutcome::kUnknown).
  int message_timeout_ms = 3000;
  int linger_ms = 2;  // small batching window: bounded latency, fewer requests
};

// Creates the topic if it does not exist yet.
Status EnsureTopic(const KafkaOptions& options);

// Producer side. Idempotent producer (enable.idempotence: no duplicates or
// reordering from internal retries) with acks=all. Delivery reports run on a
// dedicated poll thread, which completes each Publish callback.
class KafkaOrderLog final : public OrderLog {
 public:
  static Result<std::unique_ptr<KafkaOrderLog>> Create(const KafkaOptions& options);
  ~KafkaOrderLog() override;
  KafkaOrderLog(const KafkaOrderLog&) = delete;
  KafkaOrderLog& operator=(const KafkaOrderLog&) = delete;

  void Publish(const v1::OrderPlaced& event, Done done) override;

  // Waits up to timeout_ms for in-flight records (shutdown).
  void Flush(int timeout_ms);

 private:
  explicit KafkaOrderLog(KafkaOptions options);

  KafkaOptions options_;
  std::unique_ptr<RdKafka::DeliveryReportCb> dr_cb_;
  std::unique_ptr<RdKafka::Producer> producer_;
  std::atomic<bool> running_{true};
  std::thread poller_;
};

// Consumer side for order-worker: consumer group, manual offset commits
// (enable.auto.commit=false) so offsets move only after Postgres commits.
class KafkaOrderSource final : public OrderSource {
 public:
  static Result<std::unique_ptr<KafkaOrderSource>> Create(const KafkaOptions& options,
                                                          const std::string& group_id);
  ~KafkaOrderSource() override;
  KafkaOrderSource(const KafkaOrderSource&) = delete;
  KafkaOrderSource& operator=(const KafkaOrderSource&) = delete;

  std::vector<v1::OrderPlaced> Poll(size_t max, int64_t wait_ms) override;
  Status Commit() override;

 private:
  KafkaOrderSource() = default;
  Status Connect();
  // True while the consumer holds at least one partition.
  bool HasAssignment();

  KafkaOptions options_;
  std::string group_id_;
  std::unique_ptr<RdKafka::KafkaConsumer> consumer_;
  // Watchdog: a consumer that has held no partition this long is recreated.
  // Observed after a laptop sleep: the group evicted both workers on heartbeat
  // expiry and the librdkafka consumers kept polling without ever rejoining.
  std::chrono::steady_clock::time_point last_assigned_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point last_check_ = std::chrono::steady_clock::now();
};

}  // namespace cartwheel::deal
