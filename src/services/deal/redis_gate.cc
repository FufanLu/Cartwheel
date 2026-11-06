#include "services/deal/redis_gate.h"

#include <charconv>
#include <iterator>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <sw/redis++/redis++.h>

#include "common/logging.h"

namespace cartwheel::deal {
namespace {

namespace redis = sw::redis;

// One day: old generations disappear on their own.
constexpr int64_t kKeyTtlSeconds = int64_t{24} * 3600;

// Marks the holds hash as armed so it exists (and carries the TTL) before the
// first reservation. ':' never appears in a user id.
constexpr const char* kArmedField = ":armed";

// KEYS[1] stock, KEYS[2] holds; ARGV[1] user, ARGV[2] new order id.
// Returns {kind, order_id, remaining}; strings only, so the reply parses into
// one vector<string>.
constexpr const char* kReserveLua = R"lua(
local held = redis.call('HGET', KEYS[2], ARGV[1])
if held then
  return {'held', held, tostring(redis.call('GET', KEYS[1]) or '0')}
end
local stock = redis.call('GET', KEYS[1])
if not stock then
  return {'unarmed', '', '0'}
end
if tonumber(stock) <= 0 then
  return {'soldout', '', '0'}
end
local left = redis.call('DECR', KEYS[1])
redis.call('HSET', KEYS[2], ARGV[1], ARGV[2])
return {'reserved', ARGV[2], tostring(left)}
)lua";

// ARGV[1] quantity, ARGV[2] '1' = keep an existing stock key, ARGV[3] ttl s.
constexpr const char* kArmLua = R"lua(
if ARGV[2] == '1' and redis.call('EXISTS', KEYS[1]) == 1 then
  return {'kept'}
end
redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[3])
redis.call('DEL', KEYS[2])
redis.call('HSET', KEYS[2], ':armed', '1')
redis.call('EXPIRE', KEYS[2], ARGV[3])
return {'armed'}
)lua";

Status FromRedis(const redis::Error& e, std::string_view what) {
  if (dynamic_cast<const redis::TimeoutError*>(&e) != nullptr) {
    return Status::Unavailable(fmt::format("redis {}: timeout", what));
  }
  if (dynamic_cast<const redis::ReplyError*>(&e) != nullptr) {
    return Status::Internal(fmt::format("redis {}: {}", what, e.what()));
  }
  return Status::Unavailable(fmt::format("redis {}: {}", what, e.what()));
}

int64_t ParseInt(std::string_view s) {
  int64_t v = 0;
  std::from_chars(s.data(), s.data() + s.size(), v);
  return v;
}

}  // namespace

Result<RedisOptions> ParseRedisUrl(const std::string& url) {
  RedisOptions o;
  std::string_view rest = url;
  if (rest.starts_with("redis://")) rest.remove_prefix(8);
  if (auto at = rest.rfind('@'); at != std::string_view::npos) {
    std::string_view cred = rest.substr(0, at);
    if (auto colon = cred.find(':'); colon != std::string_view::npos) cred.remove_prefix(colon + 1);
    o.password = std::string(cred);
    rest.remove_prefix(at + 1);
  }
  if (auto slash = rest.find('/'); slash != std::string_view::npos) rest = rest.substr(0, slash);
  if (auto colon = rest.rfind(':'); colon != std::string_view::npos) {
    o.port = static_cast<int>(ParseInt(rest.substr(colon + 1)));
    rest = rest.substr(0, colon);
  }
  if (rest.empty() || o.port <= 0) return Status::InvalidArgument("bad redis url: " + url);
  o.host = std::string(rest);
  return o;
}

std::shared_ptr<redis::Redis> ConnectRedis(const RedisOptions& options) {
  redis::ConnectionOptions c;
  c.host = options.host;
  c.port = options.port;
  c.password = options.password;
  c.connect_timeout = std::chrono::milliseconds(options.connect_timeout_ms);
  c.socket_timeout = std::chrono::milliseconds(options.socket_timeout_ms);
  c.keep_alive = true;
  redis::ConnectionPoolOptions p;
  p.size = options.pool_size;
  // A bounded wait: when every connection is busy, fail fast instead of
  // queueing behind a slow Redis.
  p.wait_timeout = std::chrono::milliseconds(options.pool_wait_ms);
  return std::make_shared<redis::Redis>(c, p);
}

RedisInventoryGate::RedisInventoryGate(std::shared_ptr<redis::Redis> redis)
    : redis_(std::move(redis)) {}

std::string RedisInventoryGate::StockKey(const std::string& deal_id, int64_t generation) {
  return fmt::format("deal:{{{}}}:{}:stock", deal_id, generation);
}

std::string RedisInventoryGate::HoldsKey(const std::string& deal_id, int64_t generation) {
  return fmt::format("deal:{{{}}}:{}:holds", deal_id, generation);
}

const char* RedisInventoryGate::ReserveScript() {
  return kReserveLua;
}

Result<std::vector<std::string>> RedisInventoryGate::RunScript(
    const std::string& source, std::string* sha, std::initializer_list<std::string> keys,
    std::initializer_list<std::string> args) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    std::string digest;
    {
      std::lock_guard lock(sha_mu_);
      digest = *sha;
    }
    try {
      if (digest.empty()) {
        digest = redis_->script_load(source);
        std::lock_guard lock(sha_mu_);
        *sha = digest;
      }
      std::vector<std::string> out;
      redis_->evalsha(digest, keys.begin(), keys.end(), args.begin(), args.end(),
                      std::back_inserter(out));
      return out;
    } catch (const redis::ReplyError& e) {
      if (std::string_view(e.what()).starts_with("NOSCRIPT") && attempt == 0) {
        // Redis restarted and lost its script cache: load and retry once.
        std::lock_guard lock(sha_mu_);
        sha->clear();
        continue;
      }
      return FromRedis(e, "script");
    } catch (const redis::Error& e) {
      return FromRedis(e, "script");
    }
  }
  return Status::Internal("redis script: NOSCRIPT after reload");
}

Result<Reservation> RedisInventoryGate::Reserve(const std::string& deal_id, int64_t generation,
                                                const std::string& user_id,
                                                const std::string& order_id) {
  CW_ASSIGN_OR_RETURN(auto reply,
                      RunScript(kReserveLua, &reserve_sha_,
                                {StockKey(deal_id, generation), HoldsKey(deal_id, generation)},
                                {user_id, order_id}));
  if (reply.size() != 3) return Status::Internal("reserve: unexpected reply");
  Reservation r;
  r.order_id = reply[1];
  r.remaining = ParseInt(reply[2]);
  if (reply[0] == "reserved") {
    r.kind = Reservation::Kind::kReserved;
  } else if (reply[0] == "held") {
    r.kind = Reservation::Kind::kAlreadyClaimed;
  } else if (reply[0] == "unarmed") {
    r.kind = Reservation::Kind::kNotArmed;
  } else {
    r.kind = Reservation::Kind::kSoldOut;
  }
  return r;
}

Result<std::vector<std::pair<std::string, std::string>>> RedisInventoryGate::Holds(
    const std::string& deal_id, int64_t generation) {
  try {
    // One hash per deal of at most `quantity` entries; HSCAN would page
    // through bigger ones.
    std::vector<std::pair<std::string, std::string>> out;
    redis_->hgetall(HoldsKey(deal_id, generation), std::back_inserter(out));
    std::erase_if(out, [](const auto& kv) { return kv.first == kArmedField; });
    return out;
  } catch (const redis::Error& e) {
    return FromRedis(e, "hgetall");
  }
}

Status RedisInventoryGate::Arm(const std::string& deal_id, int64_t generation, int64_t quantity,
                               bool only_if_absent) {
  auto reply = RunScript(
      kArmLua, &arm_sha_, {StockKey(deal_id, generation), HoldsKey(deal_id, generation)},
      {std::to_string(quantity), only_if_absent ? "1" : "0", std::to_string(kKeyTtlSeconds)});
  return reply.status();
}

Result<int64_t> RedisInventoryGate::Remaining(const std::string& deal_id, int64_t generation) {
  try {
    auto v = redis_->get(StockKey(deal_id, generation));
    if (!v) return Status::NotFound("not armed");
    return ParseInt(*v);
  } catch (const redis::Error& e) {
    return FromRedis(e, "get");
  }
}

Result<std::optional<std::string>> RedisInventoryGate::HeldOrder(const std::string& deal_id,
                                                                 int64_t generation,
                                                                 const std::string& user_id) {
  try {
    auto v = redis_->hget(HoldsKey(deal_id, generation), user_id);
    if (!v || user_id == kArmedField) return std::optional<std::string>();
    return std::optional<std::string>(*v);
  } catch (const redis::Error& e) {
    return FromRedis(e, "hget");
  }
}

RedisNotifier::RedisNotifier(std::shared_ptr<redis::Redis> redis) : redis_(std::move(redis)) {}

void RedisNotifier::OrderUpdated(const v1::Order& order) {
  try {
    redis_->publish(NotificationChannel(order.user_id()), OrderJson(order));
  } catch (const redis::Error& e) {
    // Best effort by design: the order is in Postgres and ListOrders shows it.
    CW_LOG_WARN("notify {}: {}", order.order_id(), e.what());
  }
}

std::string OrderJson(const v1::Order& order) {
  const char* status = "PENDING";
  if (order.status() == v1::Order::STATUS_CONFIRMED) status = "CONFIRMED";
  if (order.status() == v1::Order::STATUS_REJECTED) status = "REJECTED";
  nlohmann::json j{{"order_id", order.order_id()},
                   {"deal_id", order.deal_id()},
                   {"user_id", order.user_id()},
                   {"price_cents", order.price_cents()},
                   {"status", status},
                   {"reserved_at_ms", order.reserved_at_ms()},
                   {"generation", order.generation()},
                   {"reason", order.reason()}};
  return j.dump();
}

}  // namespace cartwheel::deal
