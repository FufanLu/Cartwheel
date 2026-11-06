#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "services/deal/deal_ports.h"

namespace sw::redis {
class Redis;
}

namespace cartwheel::deal {

struct RedisOptions {
  std::string host = "127.0.0.1";
  int port = 6379;
  std::string password;
  size_t pool_size = 16;
  int64_t connect_timeout_ms = 200;
  int64_t socket_timeout_ms = 200;
  int64_t pool_wait_ms = 100;
};

// Parses "redis://[:password@]host[:port]".
Result<RedisOptions> ParseRedisUrl(const std::string& url);

std::shared_ptr<sw::redis::Redis> ConnectRedis(const RedisOptions& options);

// The inventory gate as two keys per (deal, generation), sharing a hash tag
// so they land in one Redis Cluster slot (a Lua script may only touch keys of
// one slot):
//
//   deal:{<id>}:<gen>:stock   integer, units left
//   deal:{<id>}:<gen>:holds   hash, user -> order id
//
// Reserve is a single Lua script, so check-and-decrement is atomic without
// locks or WATCH retries: Redis runs scripts one at a time. Keys expire a day
// after the last arm, so old generations clean themselves up.
class RedisInventoryGate final : public InventoryGate {
 public:
  explicit RedisInventoryGate(std::shared_ptr<sw::redis::Redis> redis);

  Result<Reservation> Reserve(const std::string& deal_id, int64_t generation,
                              const std::string& user_id, const std::string& order_id) override;
  Result<std::vector<std::pair<std::string, std::string>>> Holds(const std::string& deal_id,
                                                                 int64_t generation) override;
  Status Arm(const std::string& deal_id, int64_t generation, int64_t quantity,
             bool only_if_absent) override;
  Result<int64_t> Remaining(const std::string& deal_id, int64_t generation) override;
  Result<std::optional<std::string>> HeldOrder(const std::string& deal_id, int64_t generation,
                                               const std::string& user_id) override;

  static std::string StockKey(const std::string& deal_id, int64_t generation);
  static std::string HoldsKey(const std::string& deal_id, int64_t generation);
  static const char* ReserveScript();

 private:
  // EVALSHA, loading the script on NOSCRIPT (after a Redis restart).
  Result<std::vector<std::string>> RunScript(const std::string& source, std::string* sha,
                                             std::initializer_list<std::string> keys,
                                             std::initializer_list<std::string> args);

  std::shared_ptr<sw::redis::Redis> redis_;
  std::mutex sha_mu_;
  std::string reserve_sha_;
  std::string arm_sha_;
};

// Publishes order updates as JSON on "orders:<user>" for the gateway's
// WebSocket hub.
class RedisNotifier final : public OrderNotifier {
 public:
  explicit RedisNotifier(std::shared_ptr<sw::redis::Redis> redis);
  void OrderUpdated(const v1::Order& order) override;

 private:
  std::shared_ptr<sw::redis::Redis> redis_;
};

// JSON body shared by the notifier and the gateway.
std::string OrderJson(const v1::Order& order);

}  // namespace cartwheel::deal
