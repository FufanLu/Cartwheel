#include "services/deal/deal_service.h"

#include <chrono>
#include <memory>

#include "common/metrics.h"
#include "kv/grpc_status.h"

namespace cartwheel::deal {
namespace {

Status ValidUser(const std::string& user_id) {
  if (user_id.empty() || user_id.size() > 64) {
    return Status::InvalidArgument("user_id must be 1-64 bytes");
  }
  return Status::Ok();
}

}  // namespace

DealServiceImpl::DealServiceImpl(DealEngine* engine, ThreadPool* workers,
                                 prometheus::Registry& registry)
    : engine_(engine),
      workers_(workers),
      results_(prometheus::BuildCounter()
                   .Name("cartwheel_deal_requests_total")
                   .Help("Deal RPCs by operation and result code")
                   .Register(registry)),
      latency_(prometheus::BuildHistogram()
                   .Name("cartwheel_deal_request_duration_seconds")
                   .Help("Deal RPC latency including queueing in the worker pool")
                   .Register(registry)) {}

std::function<void(const Status&)> DealServiceImpl::Finisher(grpc::ServerUnaryReactor* reactor,
                                                             const char* op) {
  const auto start = std::chrono::steady_clock::now();
  return [this, reactor, op, start](const Status& s) {
    latency_.Add({{"op", op}}, LatencyBucketsSeconds())
        .Observe(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    results_.Add({{"op", op}, {"code", std::string(StatusCodeName(s.code()))}}).Increment();
    reactor->Finish(kv::ToGrpc(s));
  };
}

template <typename Resp, typename Fn>
grpc::ServerUnaryReactor* DealServiceImpl::Dispatch(grpc::CallbackServerContext* ctx,
                                                    const char* op, Resp* resp, Fn&& fn) {
  auto* reactor = ctx->DefaultReactor();
  auto finish = Finisher(reactor, op);
  const bool queued = workers_->TrySubmit([ctx, resp, finish, fn = std::forward<Fn>(fn)]() mutable {
    if (ctx->IsCancelled()) {
      finish(Status::Cancelled("deadline exceeded before execution"));
      return;
    }
    auto result = fn();
    const Status status = result.status();
    if (result.ok()) *resp = std::move(result).value();
    finish(status);
  });
  if (!queued) finish(Status::ResourceExhausted("deal-svc overloaded; retry with backoff"));
  return reactor;
}

grpc::ServerUnaryReactor* DealServiceImpl::ListDeals(grpc::CallbackServerContext* ctx,
                                                     const v1::ListDealsRequest* /*req*/,
                                                     v1::ListDealsResponse* resp) {
  return Dispatch(ctx, "list_deals", resp, [this] { return engine_->ListDeals(); });
}

grpc::ServerUnaryReactor* DealServiceImpl::GetDeal(grpc::CallbackServerContext* ctx,
                                                   const v1::GetDealRequest* req, v1::Deal* resp) {
  return Dispatch(ctx, "get_deal", resp, [this, req] { return engine_->GetDeal(req->deal_id()); });
}

grpc::ServerUnaryReactor* DealServiceImpl::Claim(grpc::CallbackServerContext* ctx,
                                                 const v1::ClaimRequest* req,
                                                 v1::ClaimResponse* resp) {
  auto* reactor = ctx->DefaultReactor();
  auto finish = Finisher(reactor, "claim");
  if (auto s = ValidUser(req->user_id()); !s.ok()) {
    finish(s);
    return reactor;
  }
  const bool queued = workers_->TrySubmit([this, ctx, req, resp, finish] {
    if (ctx->IsCancelled()) {
      finish(Status::Cancelled("deadline exceeded before execution"));
      return;
    }
    engine_->Claim(req->deal_id(), req->user_id(),
                   [resp, finish](Result<v1::ClaimResponse> result) {
                     const Status status = result.status();
                     if (result.ok()) *resp = std::move(result).value();
                     finish(status);
                   });
  });
  if (!queued) finish(Status::ResourceExhausted("deal-svc overloaded; retry with backoff"));
  return reactor;
}

grpc::ServerUnaryReactor* DealServiceImpl::GetOrder(grpc::CallbackServerContext* ctx,
                                                    const v1::GetOrderRequest* req,
                                                    v1::Order* resp) {
  return Dispatch(ctx, "get_order", resp, [this, req]() -> Result<v1::Order> {
    CW_RETURN_IF_ERROR(ValidUser(req->user_id()));
    return engine_->GetOrder(req->user_id(), req->order_id());
  });
}

grpc::ServerUnaryReactor* DealServiceImpl::ListOrders(grpc::CallbackServerContext* ctx,
                                                      const v1::ListOrdersRequest* req,
                                                      v1::ListOrdersResponse* resp) {
  return Dispatch(ctx, "list_orders", resp, [this, req]() -> Result<v1::ListOrdersResponse> {
    CW_RETURN_IF_ERROR(ValidUser(req->user_id()));
    return engine_->ListOrders(req->user_id());
  });
}

grpc::ServerUnaryReactor* DealServiceImpl::ResetDeal(grpc::CallbackServerContext* ctx,
                                                     const v1::ResetDealRequest* req,
                                                     v1::Deal* resp) {
  return Dispatch(ctx, "reset", resp, [this, req] { return engine_->Reset(*req); });
}

}  // namespace cartwheel::deal
