#pragma once

#include <atomic>
#include <memory>

#include <prometheus/counter.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include "cartwheel/kv/v1/kv.grpc.pb.h"
#include "common/status.h"
#include "kv/local_store.h"

namespace cartwheel::kv {

// Request limits, borrowed from DynamoDB's item limits.
inline constexpr size_t kMaxTableName = 64;
inline constexpr size_t kMaxKeyBytes = 2048;
inline constexpr size_t kMaxValueBytes = size_t{400} * 1024;

Status ValidateTable(std::string_view table);
Status ValidateKey(std::string_view key);
Status ValidateValue(std::string_view value);

// Client-facing KvService on gRPC's callback API. In M1 the node serves
// requests from its LocalStore. M3 puts the quorum coordinator in between.
class KvServiceImpl final : public v1::KvService::CallbackService {
 public:
  KvServiceImpl(LocalStore* store, prometheus::Registry& registry);

  grpc::ServerUnaryReactor* Get(grpc::CallbackServerContext* ctx, const v1::GetRequest* req,
                                v1::GetResponse* resp) override;
  grpc::ServerUnaryReactor* Put(grpc::CallbackServerContext* ctx, const v1::PutRequest* req,
                                v1::PutResponse* resp) override;
  grpc::ServerUnaryReactor* Delete(grpc::CallbackServerContext* ctx, const v1::DeleteRequest* req,
                                   v1::DeleteResponse* resp) override;

  // Pure request handlers, unit-testable without a server.
  Status HandleGet(const v1::GetRequest& req, v1::GetResponse* resp);
  Status HandlePut(const v1::PutRequest& req, v1::PutResponse* resp);
  Status HandleDelete(const v1::DeleteRequest& req, v1::DeleteResponse* resp);

 private:
  struct OpMetrics {
    prometheus::Histogram* latency = nullptr;
    prometheus::Family<prometheus::Counter>* results = nullptr;
  };
  template <typename Fn>
  grpc::ServerUnaryReactor* Serve(grpc::CallbackServerContext* ctx, const OpMetrics& m,
                                  std::string_view op, Fn&& handler);

  LocalStore* store_;
  prometheus::Family<prometheus::Counter>& results_family_;
  OpMetrics get_metrics_;
  OpMetrics put_metrics_;
  OpMetrics delete_metrics_;
};

// Health, stats, and (debug builds) fault injection.
class AdminServiceImpl final : public v1::AdminService::CallbackService {
 public:
  explicit AdminServiceImpl(const LocalStore* store);

  grpc::ServerUnaryReactor* Health(grpc::CallbackServerContext* ctx, const v1::HealthRequest* req,
                                   v1::HealthResponse* resp) override;
  grpc::ServerUnaryReactor* NodeStats(grpc::CallbackServerContext* ctx,
                                      const v1::NodeStatsRequest* req,
                                      v1::NodeStatsResponse* resp) override;

  // Flipped by the shutdown path so load balancers drain this node first.
  void SetDraining() noexcept { draining_.store(true, std::memory_order_relaxed); }

 private:
  const LocalStore* store_;
  const int64_t started_ms_;
  std::atomic<bool> draining_{false};
};

}  // namespace cartwheel::kv
