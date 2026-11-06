#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <prometheus/counter.h>
#include <prometheus/registry.h>

#include "cartwheel/deal/v1/deal.grpc.pb.h"
#include "gateway/auth.h"
#include "gateway/http.h"
#include "gateway/keyed_limiter.h"

namespace cartwheel::gateway {

struct DealRoutesOptions {
  std::chrono::milliseconds upstream_timeout{800};
  // Per-user claim limit. A real buyer clicks once or twice; a bot hammering
  // the button is held to this rate (429 + Retry-After) before reaching
  // deal-svc.
  double claim_rate_per_sec = 1.0;
  double claim_burst = 3.0;
  double login_rate_per_sec = 2.0;
  double login_burst = 5.0;
  // Required in X-Admin-Token for the reset endpoint; empty disables it.
  std::string admin_token;
};

// REST facade for deal-svc:
//   POST /api/v1/login                       {user} -> {token, user, expires_at}
//   GET  /api/v1/deals                       catalog + live stock + server time
//   POST /api/v1/deals/{deal}/claim          auth; 202 reserved, 200 already yours,
//                                            409 sold out / not started / ended
//   GET  /api/v1/orders                      auth; the caller's orders
//   GET  /api/v1/orders/{order}              auth
//   POST /api/v1/admin/deals/{deal}/reset    X-Admin-Token; {quantity, starts_in_ms, duration_ms}
class DealRoutes {
 public:
  DealRoutes(std::shared_ptr<deal::v1::DealService::Stub> stub, const Authenticator* auth,
             DealRoutesOptions options, prometheus::Registry& registry);
  void Register(Router* router);

  [[nodiscard]] static nlohmann::json DealToJson(const deal::v1::Deal& d);
  [[nodiscard]] static nlohmann::json OrderToJson(const deal::v1::Order& o);

 private:
  boost::asio::awaitable<Response> Login(RequestContext& ctx);
  boost::asio::awaitable<Response> ListDeals(RequestContext& ctx);
  boost::asio::awaitable<Response> Claim(RequestContext& ctx);
  boost::asio::awaitable<Response> ListOrders(RequestContext& ctx);
  boost::asio::awaitable<Response> GetOrder(RequestContext& ctx);
  boost::asio::awaitable<Response> Reset(RequestContext& ctx);

  [[nodiscard]] Response Throttled(int64_t retry_in_ns, const RequestContext& ctx) const;

  std::shared_ptr<deal::v1::DealService::Stub> stub_;
  const Authenticator* auth_;
  DealRoutesOptions options_;
  KeyedRateLimiter claim_limiter_;
  KeyedRateLimiter login_limiter_;
  prometheus::Family<prometheus::Counter>& limited_;
};

}  // namespace cartwheel::gateway
