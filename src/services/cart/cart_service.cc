#include "services/cart/cart_service.h"

#include <chrono>
#include <memory>
#include <string>

#include "common/metrics.h"
#include "kv/grpc_status.h"

namespace cartwheel::cart {

CartServiceImpl::CartServiceImpl(CartStore* store, ThreadPool* workers,
                                 prometheus::Registry& registry)
    : store_(store),
      workers_(workers),
      results_(prometheus::BuildCounter()
                   .Name("cartwheel_cart_requests_total")
                   .Help("Cart RPCs by operation and result code")
                   .Register(registry)),
      latency_(prometheus::BuildHistogram()
                   .Name("cartwheel_cart_request_duration_seconds")
                   .Help("Cart RPC latency including queueing in the worker pool")
                   .Register(registry)) {}

template <typename Fn>
grpc::ServerUnaryReactor* CartServiceImpl::Dispatch(grpc::CallbackServerContext* ctx,
                                                    const char* op, v1::Cart* resp, Fn&& fn) {
  auto* reactor = ctx->DefaultReactor();
  const auto start = std::chrono::steady_clock::now();
  auto finish = [this, reactor, op, start](const Status& s) {
    latency_.Add({{"op", op}}, LatencyBucketsSeconds())
        .Observe(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    results_.Add({{"op", op}, {"code", std::string(StatusCodeName(s.code()))}}).Increment();
    reactor->Finish(kv::ToGrpc(s));
  };
  const bool queued = workers_->TrySubmit([ctx, resp, finish, fn = std::forward<Fn>(fn)]() mutable {
    if (ctx->IsCancelled()) {  // deadline passed while queued: skip the work
      finish(Status::Cancelled("deadline exceeded before execution"));
      return;
    }
    auto result = fn();
    const Status status = result.status();
    if (result.ok()) *resp = std::move(result).value();
    finish(status);
  });
  if (!queued) finish(Status::ResourceExhausted("cart-svc overloaded; retry with backoff"));
  return reactor;
}

grpc::ServerUnaryReactor* CartServiceImpl::GetCart(grpc::CallbackServerContext* ctx,
                                                   const v1::GetCartRequest* req, v1::Cart* resp) {
  return Dispatch(ctx, "get", resp, [this, req] { return store_->Get(req->user_id()); });
}

grpc::ServerUnaryReactor* CartServiceImpl::AddItem(grpc::CallbackServerContext* ctx,
                                                   const v1::AddItemRequest* req, v1::Cart* resp) {
  return Dispatch(ctx, "add", resp, [this, req] {
    return store_->AddItem(req->user_id(), req->item(), req->idempotency_key());
  });
}

grpc::ServerUnaryReactor* CartServiceImpl::RemoveItem(grpc::CallbackServerContext* ctx,
                                                      const v1::RemoveItemRequest* req,
                                                      v1::Cart* resp) {
  return Dispatch(ctx, "remove", resp,
                  [this, req] { return store_->RemoveItem(req->user_id(), req->sku()); });
}

grpc::ServerUnaryReactor* CartServiceImpl::ClearCart(grpc::CallbackServerContext* ctx,
                                                     const v1::ClearCartRequest* req,
                                                     v1::Cart* resp) {
  return Dispatch(ctx, "clear", resp, [this, req] { return store_->Clear(req->user_id()); });
}

}  // namespace cartwheel::cart
