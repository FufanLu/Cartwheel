#pragma once

#include <prometheus/counter.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include "cartwheel/deal/v1/deal.grpc.pb.h"
#include "common/thread_pool.h"
#include "services/deal/deal_engine.h"

namespace cartwheel::deal {

// gRPC front of DealEngine. Like cart-svc, blocking work (Redis, Postgres)
// runs on a bounded pool and a full queue answers RESOURCE_EXHAUSTED at once.
// Claim finishes from the order-log callback, not from the pool thread.
class DealServiceImpl final : public v1::DealService::CallbackService {
 public:
  DealServiceImpl(DealEngine* engine, ThreadPool* workers, prometheus::Registry& registry);

  grpc::ServerUnaryReactor* ListDeals(grpc::CallbackServerContext* ctx,
                                      const v1::ListDealsRequest* req,
                                      v1::ListDealsResponse* resp) override;
  grpc::ServerUnaryReactor* GetDeal(grpc::CallbackServerContext* ctx, const v1::GetDealRequest* req,
                                    v1::Deal* resp) override;
  grpc::ServerUnaryReactor* Claim(grpc::CallbackServerContext* ctx, const v1::ClaimRequest* req,
                                  v1::ClaimResponse* resp) override;
  grpc::ServerUnaryReactor* GetOrder(grpc::CallbackServerContext* ctx,
                                     const v1::GetOrderRequest* req, v1::Order* resp) override;
  grpc::ServerUnaryReactor* ListOrders(grpc::CallbackServerContext* ctx,
                                       const v1::ListOrdersRequest* req,
                                       v1::ListOrdersResponse* resp) override;
  grpc::ServerUnaryReactor* ResetDeal(grpc::CallbackServerContext* ctx,
                                      const v1::ResetDealRequest* req, v1::Deal* resp) override;

 private:
  // Runs fn() -> Result<Resp> on the pool and finishes the RPC with it.
  template <typename Resp, typename Fn>
  grpc::ServerUnaryReactor* Dispatch(grpc::CallbackServerContext* ctx, const char* op, Resp* resp,
                                     Fn&& fn);
  std::function<void(const Status&)> Finisher(grpc::ServerUnaryReactor* reactor, const char* op);

  DealEngine* engine_;
  ThreadPool* workers_;
  prometheus::Family<prometheus::Counter>& results_;
  prometheus::Family<prometheus::Histogram>& latency_;
};

}  // namespace cartwheel::deal
