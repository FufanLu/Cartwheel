#include "gateway/deal_routes.h"

#include <cmath>

#include <grpcpp/client_context.h>
#include <nlohmann/json.hpp>

#include "gateway/grpc_await.h"
#include "kv/grpc_status.h"

namespace cartwheel::gateway {
namespace {

namespace dv1 = deal::v1;

void PrepareContext(grpc::ClientContext* g, const RequestContext& ctx,
                    std::chrono::milliseconds timeout) {
  g->set_deadline(std::chrono::system_clock::now() + timeout);
  g->AddMetadata("x-request-id", ctx.request_id);
}

const char* OrderStatusName(dv1::Order::Status s) {
  switch (s) {
    case dv1::Order::STATUS_PENDING: return "PENDING";
    case dv1::Order::STATUS_CONFIRMED: return "CONFIRMED";
    case dv1::Order::STATUS_REJECTED: return "REJECTED";
    default: return "UNKNOWN";
  }
}

}  // namespace

DealRoutes::DealRoutes(std::shared_ptr<dv1::DealService::Stub> stub, const Authenticator* auth,
                       DealRoutesOptions options, prometheus::Registry& registry)
    : stub_(std::move(stub)),
      auth_(auth),
      options_(std::move(options)),
      claim_limiter_({.rate_per_sec = options_.claim_rate_per_sec, .burst = options_.claim_burst}),
      login_limiter_({.rate_per_sec = options_.login_rate_per_sec, .burst = options_.login_burst}),
      limited_(prometheus::BuildCounter()
                   .Name("cartwheel_gateway_rate_limited_total")
                   .Help("Requests rejected by a per-user rate limit, by route")
                   .Register(registry)) {}

void DealRoutes::Register(Router* router) {
  router->Add(http::verb::post, "/api/v1/login", [this](RequestContext& c) { return Login(c); });
  router->Add(http::verb::get, "/api/v1/deals", [this](RequestContext& c) { return ListDeals(c); });
  router->Add(http::verb::post, "/api/v1/deals/{deal}/claim",
              [this](RequestContext& c) { return Claim(c); });
  router->Add(http::verb::get, "/api/v1/orders",
              [this](RequestContext& c) { return ListOrders(c); });
  router->Add(http::verb::get, "/api/v1/orders/{order}",
              [this](RequestContext& c) { return GetOrder(c); });
  router->Add(http::verb::post, "/api/v1/admin/deals/{deal}/reset",
              [this](RequestContext& c) { return Reset(c); });
}

nlohmann::json DealRoutes::DealToJson(const dv1::Deal& d) {
  return {{"deal_id", d.deal_id()},
          {"sku", d.sku()},
          {"title", d.title()},
          {"price_cents", d.price_cents()},
          {"list_price_cents", d.list_price_cents()},
          {"quantity", d.quantity()},
          {"remaining", d.remaining()},
          {"sold", d.sold()},
          {"starts_at_ms", d.starts_at_ms()},
          {"ends_at_ms", d.ends_at_ms()},
          {"generation", d.generation()}};
}

nlohmann::json DealRoutes::OrderToJson(const dv1::Order& o) {
  return {{"order_id", o.order_id()},
          {"deal_id", o.deal_id()},
          {"user_id", o.user_id()},
          {"price_cents", o.price_cents()},
          {"status", OrderStatusName(o.status())},
          {"reserved_at_ms", o.reserved_at_ms()},
          {"generation", o.generation()},
          {"reason", o.reason()}};
}

Response DealRoutes::Throttled(int64_t retry_in_ns, const RequestContext& ctx) const {
  Response res = ErrorResponse(Status::Throttled("slow down"), ctx);
  const auto secs = static_cast<int64_t>(std::ceil(static_cast<double>(retry_in_ns) / 1e9));
  res.set(http::field::retry_after, std::to_string(std::max<int64_t>(secs, 1)));
  return res;
}

boost::asio::awaitable<Response> DealRoutes::Login(RequestContext& ctx) {
  std::string user;
  try {
    user = nlohmann::json::parse(ctx.request.body()).at("user").get<std::string>();
  } catch (const nlohmann::json::exception&) {
    co_return ErrorResponse(Status::InvalidArgument("body must be {\"user\": \"...\"}"), ctx);
  }
  if (!ValidUserId(user)) {
    co_return ErrorResponse(Status::InvalidArgument("user: 1-32 of a-z 0-9 _ -"), ctx);
  }
  if (const int64_t wait = login_limiter_.Admit(user); wait > 0) {
    limited_.Add({{"route", "login"}}).Increment();
    co_return Throttled(wait, ctx);
  }
  // Demo identity: no password. The token is what the rest of the stack
  // trusts, so swapping in a real identity provider only changes this route.
  int64_t expires_at = 0;
  const std::string token = auth_->Issue(user, &expires_at);
  co_return JsonResponse(http::status::ok,
                         {{"token", token}, {"user", user}, {"expires_at", expires_at}}, ctx);
}

boost::asio::awaitable<Response> DealRoutes::ListDeals(RequestContext& ctx) {
  grpc::ClientContext g;
  PrepareContext(&g, ctx, options_.upstream_timeout);
  dv1::ListDealsRequest req;
  dv1::ListDealsResponse resp;
  const grpc::Status s = co_await AwaitGrpc(
      [&](auto done) { stub_->async()->ListDeals(&g, &req, &resp, std::move(done)); });
  if (!s.ok()) co_return ErrorResponse(kv::FromGrpc(s), ctx);
  nlohmann::json deals = nlohmann::json::array();
  for (const auto& d : resp.deals()) deals.push_back(DealToJson(d));
  co_return JsonResponse(http::status::ok,
                         {{"deals", std::move(deals)}, {"server_time_ms", resp.server_time_ms()}},
                         ctx);
}

boost::asio::awaitable<Response> DealRoutes::Claim(RequestContext& ctx) {
  auto user = auth_->UserFromRequest(ctx.request);
  if (!user.ok()) co_return ErrorResponse(user.status(), ctx);
  if (const int64_t wait = claim_limiter_.Admit(*user); wait > 0) {
    limited_.Add({{"route", "claim"}}).Increment();
    co_return Throttled(wait, ctx);
  }
  grpc::ClientContext g;
  PrepareContext(&g, ctx, options_.upstream_timeout);
  dv1::ClaimRequest req;
  req.set_deal_id(ctx.params.at("deal"));
  req.set_user_id(*user);
  dv1::ClaimResponse resp;
  const grpc::Status s = co_await AwaitGrpc(
      [&](auto done) { stub_->async()->Claim(&g, &req, &resp, std::move(done)); });
  if (!s.ok()) co_return ErrorResponse(kv::FromGrpc(s), ctx);

  const char* outcome = "UNKNOWN";
  http::status code = http::status::conflict;
  switch (resp.outcome()) {
    case dv1::ClaimResponse::OUTCOME_RESERVED:
      outcome = "RESERVED";
      code = http::status::accepted;  // the order is pending in the log
      break;
    case dv1::ClaimResponse::OUTCOME_ALREADY_CLAIMED:
      outcome = "ALREADY_CLAIMED";
      code = http::status::ok;
      break;
    case dv1::ClaimResponse::OUTCOME_SOLD_OUT: outcome = "SOLD_OUT"; break;
    case dv1::ClaimResponse::OUTCOME_NOT_STARTED: outcome = "NOT_STARTED"; break;
    case dv1::ClaimResponse::OUTCOME_ENDED: outcome = "ENDED"; break;
    default: code = http::status::internal_server_error;
  }
  nlohmann::json body{{"outcome", outcome}, {"remaining", resp.remaining()}};
  if (!resp.order_id().empty()) {
    body["order_id"] = resp.order_id();
    body["status"] = "PENDING";
  }
  co_return JsonResponse(code, body, ctx);
}

boost::asio::awaitable<Response> DealRoutes::ListOrders(RequestContext& ctx) {
  auto user = auth_->UserFromRequest(ctx.request);
  if (!user.ok()) co_return ErrorResponse(user.status(), ctx);
  grpc::ClientContext g;
  PrepareContext(&g, ctx, options_.upstream_timeout);
  dv1::ListOrdersRequest req;
  req.set_user_id(*user);
  dv1::ListOrdersResponse resp;
  const grpc::Status s = co_await AwaitGrpc(
      [&](auto done) { stub_->async()->ListOrders(&g, &req, &resp, std::move(done)); });
  if (!s.ok()) co_return ErrorResponse(kv::FromGrpc(s), ctx);
  nlohmann::json orders = nlohmann::json::array();
  for (const auto& o : resp.orders()) orders.push_back(OrderToJson(o));
  co_return JsonResponse(http::status::ok, {{"orders", std::move(orders)}}, ctx);
}

boost::asio::awaitable<Response> DealRoutes::GetOrder(RequestContext& ctx) {
  auto user = auth_->UserFromRequest(ctx.request);
  if (!user.ok()) co_return ErrorResponse(user.status(), ctx);
  grpc::ClientContext g;
  PrepareContext(&g, ctx, options_.upstream_timeout);
  dv1::GetOrderRequest req;
  req.set_user_id(*user);
  req.set_order_id(ctx.params.at("order"));
  dv1::Order resp;
  const grpc::Status s = co_await AwaitGrpc(
      [&](auto done) { stub_->async()->GetOrder(&g, &req, &resp, std::move(done)); });
  if (!s.ok()) co_return ErrorResponse(kv::FromGrpc(s), ctx);
  co_return JsonResponse(http::status::ok, OrderToJson(resp), ctx);
}

boost::asio::awaitable<Response> DealRoutes::Reset(RequestContext& ctx) {
  if (options_.admin_token.empty() ||
      ctx.request["X-Admin-Token"] != std::string_view(options_.admin_token)) {
    co_return ErrorResponse(Status::PermissionDenied("admin token required"), ctx);
  }
  dv1::ResetDealRequest req;
  req.set_deal_id(ctx.params.at("deal"));
  try {
    const auto body = ctx.request.body().empty() ? nlohmann::json::object()
                                                 : nlohmann::json::parse(ctx.request.body());
    req.set_quantity(body.value("quantity", 100));
    req.set_starts_in_ms(body.value("starts_in_ms", int64_t{10'000}));
    req.set_duration_ms(body.value("duration_ms", int64_t{10} * 60'000));
  } catch (const nlohmann::json::exception& e) {
    co_return ErrorResponse(Status::InvalidArgument(std::string("bad JSON body: ") + e.what()),
                            ctx);
  }
  grpc::ClientContext g;
  PrepareContext(&g, ctx, options_.upstream_timeout);
  dv1::Deal resp;
  const grpc::Status s = co_await AwaitGrpc(
      [&](auto done) { stub_->async()->ResetDeal(&g, &req, &resp, std::move(done)); });
  if (!s.ok()) co_return ErrorResponse(kv::FromGrpc(s), ctx);
  co_return JsonResponse(http::status::ok, DealToJson(resp), ctx);
}

}  // namespace cartwheel::gateway
