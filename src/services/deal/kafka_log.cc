#include "services/deal/kafka_log.h"

#include <chrono>

#include <fmt/format.h>
#include <librdkafka/rdkafka.h>
#include <librdkafka/rdkafkacpp.h>

#include "common/logging.h"

namespace cartwheel::deal {
namespace {

// The Done callback travels through librdkafka as the message's opaque
// pointer and is owned by whoever completes it.
struct Pending {
  OrderLog::Done done;
};

class DeliveryReport final : public RdKafka::DeliveryReportCb {
 public:
  void dr_cb(RdKafka::Message& message) override {
    std::unique_ptr<Pending> pending(static_cast<Pending*>(message.msg_opaque()));
    if (!pending) return;
    if (message.err() == RdKafka::ERR_NO_ERROR) {
      pending->done(PublishOutcome::kDelivered, "");
      return;
    }
    // librdkafka knows whether the record may have reached the broker: a
    // timeout after transmission is "possibly persisted". DealEngine keeps
    // the hold and re-publishes in both cases; the distinction is reported in
    // metrics.
    const auto status = message.status();
    const PublishOutcome outcome = status == RdKafka::Message::MSG_STATUS_NOT_PERSISTED
                                       ? PublishOutcome::kFailed
                                       : PublishOutcome::kUnknown;
    pending->done(outcome, message.errstr());
  }
};

Status SetAll(RdKafka::Conf* conf,
              std::initializer_list<std::pair<const char*, std::string>> values) {
  std::string err;
  for (const auto& [k, v] : values) {
    if (conf->set(k, v, err) != RdKafka::Conf::CONF_OK) {
      return Status::InvalidArgument(fmt::format("kafka config {}={}: {}", k, v, err));
    }
  }
  return Status::Ok();
}

}  // namespace

Status EnsureTopic(const KafkaOptions& options) {
  char errstr[512];
  rd_kafka_conf_t* conf = rd_kafka_conf_new();
  if (rd_kafka_conf_set(conf, "bootstrap.servers", options.brokers.c_str(), errstr,
                        sizeof(errstr)) != RD_KAFKA_CONF_OK) {
    rd_kafka_conf_destroy(conf);
    return Status::InvalidArgument(errstr);
  }
  rd_kafka_t* rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
  if (rk == nullptr) return Status::Unavailable(fmt::format("kafka admin: {}", errstr));

  rd_kafka_NewTopic_t* topic = rd_kafka_NewTopic_new(options.topic.c_str(), options.partitions,
                                                     options.replication, errstr, sizeof(errstr));
  rd_kafka_queue_t* queue = rd_kafka_queue_new(rk);
  rd_kafka_CreateTopics(rk, &topic, 1, nullptr, queue);
  rd_kafka_event_t* ev = rd_kafka_queue_poll(queue, 10'000);
  Status result = Status::Ok();
  if (ev == nullptr) {
    result = Status::Unavailable("kafka admin: CreateTopics timed out");
  } else if (rd_kafka_event_error(ev) != RD_KAFKA_RESP_ERR_NO_ERROR) {
    result = Status::Unavailable(fmt::format("kafka admin: {}", rd_kafka_event_error_string(ev)));
  } else {
    size_t n = 0;
    const rd_kafka_topic_result_t** topics =
        rd_kafka_CreateTopics_result_topics(rd_kafka_event_CreateTopics_result(ev), &n);
    for (size_t i = 0; i < n; ++i) {
      const auto err = rd_kafka_topic_result_error(topics[i]);
      if (err == RD_KAFKA_RESP_ERR_NO_ERROR) {
        CW_LOG_INFO("created topic {} ({} partitions, rf {})", options.topic, options.partitions,
                    options.replication);
      } else if (err != RD_KAFKA_RESP_ERR_TOPIC_ALREADY_EXISTS) {
        result = Status::Unavailable(
            fmt::format("create topic {}: {}", options.topic, rd_kafka_err2str(err)));
      }
    }
  }
  if (ev != nullptr) rd_kafka_event_destroy(ev);
  rd_kafka_queue_destroy(queue);
  rd_kafka_NewTopic_destroy(topic);
  rd_kafka_destroy(rk);
  return result;
}

KafkaOrderLog::KafkaOrderLog(KafkaOptions options) : options_(std::move(options)) {}

Result<std::unique_ptr<KafkaOrderLog>> KafkaOrderLog::Create(const KafkaOptions& options) {
  std::unique_ptr<KafkaOrderLog> log(new KafkaOrderLog(options));
  log->dr_cb_ = std::make_unique<DeliveryReport>();
  std::unique_ptr<RdKafka::Conf> conf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
  CW_RETURN_IF_ERROR(
      SetAll(conf.get(), {{"bootstrap.servers", options.brokers},
                          {"client.id", "deal-svc"},
                          {"enable.idempotence", "true"},
                          {"acks", "all"},
                          {"linger.ms", std::to_string(options.linger_ms)},
                          {"message.timeout.ms", std::to_string(options.message_timeout_ms)},
                          // Bounded local queue: when the broker
                          // cannot keep up, produce fails fast
                          // (QUEUE_FULL) instead of buffering.
                          {"queue.buffering.max.messages", "100000"},
                          {"compression.type", "lz4"},
                          // librdkafka leaves Nagle on by default;
                          // with delayed ACKs that adds tens of ms
                          // to every small produce request.
                          {"socket.nagle.disable", "true"}}));
  std::string err;
  if (conf->set("dr_cb", log->dr_cb_.get(), err) != RdKafka::Conf::CONF_OK) {
    return Status::Internal("kafka dr_cb: " + err);
  }
  log->producer_.reset(RdKafka::Producer::create(conf.get(), err));
  if (!log->producer_) return Status::Unavailable("kafka producer: " + err);
  log->poller_ = std::thread([raw = log.get()] {
    while (raw->running_.load(std::memory_order_acquire)) raw->producer_->poll(50);
  });
  return log;
}

KafkaOrderLog::~KafkaOrderLog() {
  Flush(5000);
  running_.store(false, std::memory_order_release);
  if (poller_.joinable()) poller_.join();
  // Anything still queued gets its delivery report (with an error) here.
  producer_->poll(0);
}

void KafkaOrderLog::Flush(int timeout_ms) {
  if (producer_) producer_->flush(timeout_ms);
}

void KafkaOrderLog::Publish(const v1::OrderPlaced& event, Done done) {
  std::string payload = event.SerializeAsString();
  auto* pending = new Pending{std::move(done)};
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  // Keyed by user: spreads a hot deal over all partitions while keeping one
  // user's records in order.
  const RdKafka::ErrorCode err = producer_->produce(
      options_.topic, RdKafka::Topic::PARTITION_UA, RdKafka::Producer::RK_MSG_COPY, payload.data(),
      payload.size(), event.user_id().data(), event.user_id().size(), now_ms, pending);
  if (err != RdKafka::ERR_NO_ERROR) {
    // Never enqueued (local queue full, unknown topic...): definitely not in
    // the log, and no delivery report will come.
    std::unique_ptr<Pending> owned(pending);
    owned->done(PublishOutcome::kFailed, RdKafka::err2str(err));
  }
}

Result<std::unique_ptr<KafkaOrderSource>> KafkaOrderSource::Create(const KafkaOptions& options,
                                                                   const std::string& group_id) {
  std::unique_ptr<KafkaOrderSource> src(new KafkaOrderSource());
  std::unique_ptr<RdKafka::Conf> conf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
  CW_RETURN_IF_ERROR(SetAll(conf.get(), {{"bootstrap.servers", options.brokers},
                                         {"group.id", group_id},
                                         {"client.id", "order-worker"},
                                         {"enable.auto.commit", "false"},
                                         {"auto.offset.reset", "earliest"},
                                         {"partition.assignment.strategy", "cooperative-sticky"},
                                         // A crashed worker's partitions move after
                                         // 10 s instead of the 45 s default.
                                         {"session.timeout.ms", "10000"},
                                         {"heartbeat.interval.ms", "2000"},
                                         {"socket.nagle.disable", "true"},
                                         {"enable.partition.eof", "false"}}));
  std::string err;
  src->consumer_.reset(RdKafka::KafkaConsumer::create(conf.get(), err));
  if (!src->consumer_) return Status::Unavailable("kafka consumer: " + err);
  if (auto e = src->consumer_->subscribe({options.topic}); e != RdKafka::ERR_NO_ERROR) {
    return Status::Unavailable("kafka subscribe: " + RdKafka::err2str(e));
  }
  return src;
}

KafkaOrderSource::~KafkaOrderSource() {
  if (!consumer_) return;
  // Leaves the group cleanly, so the partitions move at once instead of
  // after session.timeout.ms.
  // unsubscribe() first: it revokes the assignment and sends LeaveGroup
  // (close() alone was observed to let the session expire instead).
  if (auto e = consumer_->unsubscribe(); e != RdKafka::ERR_NO_ERROR) {
    CW_LOG_WARN("kafka unsubscribe: {}", RdKafka::err2str(e));
  }
  const auto err = consumer_->close();
  CW_LOG_INFO("kafka consumer closed: {}", RdKafka::err2str(err));
}

std::vector<v1::OrderPlaced> KafkaOrderSource::Poll(size_t max, int64_t wait_ms) {
  std::vector<v1::OrderPlaced> out;
  int timeout = static_cast<int>(wait_ms);
  while (out.size() < max) {
    std::unique_ptr<RdKafka::Message> msg(consumer_->consume(timeout));
    timeout = 0;  // after the first record, only drain what is already fetched
    if (msg->err() == RdKafka::ERR__TIMED_OUT) break;
    if (msg->err() != RdKafka::ERR_NO_ERROR) {
      CW_LOG_WARN("kafka consume: {}", msg->errstr());
      break;
    }
    v1::OrderPlaced e;
    if (!e.ParseFromArray(msg->payload(), static_cast<int>(msg->len()))) {
      // A poison record must not wedge the partition; skip it loudly.
      CW_LOG_ERROR("skipping undecodable record at {}[{}]@{}", msg->topic_name(), msg->partition(),
                   msg->offset());
      continue;
    }
    out.push_back(std::move(e));
  }
  return out;
}

Status KafkaOrderSource::Commit() {
  const auto err = consumer_->commitSync();
  if (err == RdKafka::ERR_NO_ERROR || err == RdKafka::ERR__NO_OFFSET) return Status::Ok();
  return Status::Unavailable("kafka commit: " + RdKafka::err2str(err));
}

}  // namespace cartwheel::deal
