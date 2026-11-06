#include "client/kv_client.h"

#include <chrono>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "cartwheel/kv/v1/kv.grpc.pb.h"
#include "kv/grpc_status.h"

namespace cartwheel::client {

struct KvClient::Impl {
  KvClientOptions options;
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<kv::v1::KvService::Stub> stub;

  // Runs `rpc(ctx)` with a fresh deadline per attempt and jittered retries.
  template <typename Rpc>
  Status Call(Rpc&& rpc) {
    Backoff backoff(options.retry);
    for (;;) {
      grpc::ClientContext ctx;
      ctx.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::milliseconds(options.timeout_ms));
      // While the node restarts, wait (within the deadline) for the channel to
      // reconnect instead of failing fast with UNAVAILABLE.
      ctx.set_wait_for_ready(true);
      Status s = kv::FromGrpc(rpc(&ctx));
      if (s.ok() || !s.retryable()) return s;
      const auto delay = backoff.NextDelayMicros();
      if (!delay) return s.WithContext("retries exhausted");
      std::this_thread::sleep_for(std::chrono::microseconds(*delay));
    }
  }
};

namespace {

template <typename Resp>
KvRecord ToRecord(const Resp& resp, uint32_t replicas) {
  KvRecord rec;
  rec.values.reserve(static_cast<size_t>(resp.versions_size()));
  for (const auto& v : resp.versions()) rec.values.push_back(v.value());
  rec.context = resp.context();
  rec.replicas = replicas;
  return rec;
}

}  // namespace

KvClient::KvClient(KvClientOptions options) : impl_(std::make_unique<Impl>()) {
  impl_->options = std::move(options);
  grpc::ChannelArguments args;
  args.SetLoadBalancingPolicyName("round_robin");
  args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 30'000);
  args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10'000);
  // Retries are ours (with jitter). Disable the transparent gRPC layer so
  // attempts are not multiplied.
  args.SetInt(GRPC_ARG_ENABLE_RETRIES, 0);
  // gRPC's default reconnect backoff starts at 1 s, so a node restart made
  // every call fail for about a second. Reconnect quickly, cap at 2 s.
  args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);
  args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 100);
  args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 2000);
  impl_->channel =
      grpc::CreateCustomChannel(impl_->options.target, grpc::InsecureChannelCredentials(), args);
  impl_->stub = kv::v1::KvService::NewStub(impl_->channel);
}

KvClient::~KvClient() = default;

bool KvClient::WaitForReady(int64_t timeout_ms) {
  return impl_->channel->WaitForConnected(std::chrono::system_clock::now() +
                                          std::chrono::milliseconds(timeout_ms));
}

Result<KvRecord> KvClient::Get(std::string_view table, std::string_view key) {
  kv::v1::GetRequest req;
  req.set_table(std::string(table));
  req.set_key(std::string(key));
  kv::v1::GetResponse resp;
  CW_RETURN_IF_ERROR(impl_->Call([&](grpc::ClientContext* ctx) {
    resp.Clear();
    return impl_->stub->Get(ctx, req, &resp);
  }));
  return ToRecord(resp, resp.replicas_responded());
}

Result<KvRecord> KvClient::Put(std::string_view table, std::string_view key, std::string_view value,
                               std::string_view context) {
  kv::v1::PutRequest req;
  req.set_table(std::string(table));
  req.set_key(std::string(key));
  req.set_value(std::string(value));
  req.set_context(std::string(context));
  kv::v1::PutResponse resp;
  CW_RETURN_IF_ERROR(impl_->Call([&](grpc::ClientContext* ctx) {
    resp.Clear();
    return impl_->stub->Put(ctx, req, &resp);
  }));
  return ToRecord(resp, resp.replicas_acked());
}

Result<KvRecord> KvClient::Delete(std::string_view table, std::string_view key,
                                  std::string_view context) {
  kv::v1::DeleteRequest req;
  req.set_table(std::string(table));
  req.set_key(std::string(key));
  req.set_context(std::string(context));
  kv::v1::DeleteResponse resp;
  CW_RETURN_IF_ERROR(impl_->Call([&](grpc::ClientContext* ctx) {
    resp.Clear();
    return impl_->stub->Delete(ctx, req, &resp);
  }));
  return ToRecord(resp, resp.replicas_acked());
}

}  // namespace cartwheel::client
