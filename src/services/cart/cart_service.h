#pragma once

#include <prometheus/counter.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include "cartwheel/cart/v1/cart.grpc.pb.h"
#include "common/thread_pool.h"
#include "services/cart/cart_store.h"

namespace cartwheel::cart {

// gRPC front of CartStore.
//
// CartStore makes blocking KV calls, and blocking inside gRPC's callback
// threads starves the whole server. Each RPC is therefore handed to a bounded
// worker pool and finished from there. When the pool's queue is full the RPC
// fails immediately with RESOURCE_EXHAUSTED (load shedding), so the server
// never builds a backlog that callers would time out on.
class CartServiceImpl final : public v1::CartService::CallbackService {
 public:
  CartServiceImpl(CartStore* store, ThreadPool* workers, prometheus::Registry& registry);

  grpc::ServerUnaryReactor* GetCart(grpc::CallbackServerContext* ctx, const v1::GetCartRequest* req,
                                    v1::Cart* resp) override;
  grpc::ServerUnaryReactor* AddItem(grpc::CallbackServerContext* ctx, const v1::AddItemRequest* req,
                                    v1::Cart* resp) override;
  grpc::ServerUnaryReactor* RemoveItem(grpc::CallbackServerContext* ctx,
                                       const v1::RemoveItemRequest* req, v1::Cart* resp) override;
  grpc::ServerUnaryReactor* ClearCart(grpc::CallbackServerContext* ctx,
                                      const v1::ClearCartRequest* req, v1::Cart* resp) override;

 private:
  template <typename Fn>
  grpc::ServerUnaryReactor* Dispatch(grpc::CallbackServerContext* ctx, const char* op,
                                     v1::Cart* resp, Fn&& fn);

  CartStore* store_;
  ThreadPool* workers_;
  prometheus::Family<prometheus::Counter>& results_;
  prometheus::Family<prometheus::Histogram>& latency_;
};

}  // namespace cartwheel::cart
