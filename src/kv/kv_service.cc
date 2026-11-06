#include "kv/kv_service.h"

#include "common/logging.h"

#include <algorithm>

#include <chrono>
#include <string>

#include "common/build_info.h"
#include "common/clock.h"
#include "common/metrics.h"
#include "kv/grpc_status.h"

namespace cartwheel::kv {
namespace {

void FillVersions(const cluster::SiblingSet& set,
                  google::protobuf::RepeatedPtrField<v1::Version>* out) {
  for (const auto& s : set.siblings()) {
    if (s.tombstone) continue;
    auto* v = out->Add();
    v->set_value(s.value);
    v->set_clock(s.Clock().Encode());
    v->set_written_at_ms(s.written_at_ms);
  }
}

Result<cluster::VectorClock> DecodeContext(const std::string& ctx) {
  auto clock = cluster::VectorClock::Decode(ctx);
  if (!clock.ok()) return Status::InvalidArgument("malformed context");
  return clock;
}

}  // namespace

Status ValidateTable(std::string_view table) {
  if (table.empty() || table.size() > kMaxTableName) {
    return Status::InvalidArgument("table name must be 1-64 characters");
  }
  for (const char c : table) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return Status::InvalidArgument("table name may only contain [a-z0-9_-]");
  }
  return Status::Ok();
}

Status ValidateKey(std::string_view key) {
  if (key.empty() || key.size() > kMaxKeyBytes) {
    return Status::InvalidArgument("key must be 1-2048 bytes");
  }
  return Status::Ok();
}

Status ValidateValue(std::string_view value) {
  if (value.size() > kMaxValueBytes) return Status::InvalidArgument("value exceeds 400 KiB");
  return Status::Ok();
}

KvServiceImpl::KvServiceImpl(LocalStore* store, prometheus::Registry& registry,
                             ClusterNode* cluster)
    : store_(store),
      cluster_(cluster),
      results_family_(prometheus::BuildCounter()
                          .Name("cartwheel_kv_requests_total")
                          .Help("KV requests by operation and result code")
                          .Register(registry)) {
  auto& latency = prometheus::BuildHistogram()
                      .Name("cartwheel_kv_request_duration_seconds")
                      .Help("Server-side KV request latency")
                      .Register(registry);
  const auto buckets = LatencyBucketsSeconds();
  get_metrics_ = {&latency.Add({{"op", "get"}}, buckets), &results_family_};
  put_metrics_ = {&latency.Add({{"op", "put"}}, buckets), &results_family_};
  delete_metrics_ = {&latency.Add({{"op", "delete"}}, buckets), &results_family_};
}

template <typename Fn>
grpc::ServerUnaryReactor* KvServiceImpl::Serve(grpc::CallbackServerContext* ctx, const OpMetrics& m,
                                               std::string_view op, Fn&& handler) {
  const auto start = std::chrono::steady_clock::now();
  auto* reactor = ctx->DefaultReactor();
  // The deadline may already have expired while the request sat in a queue.
  // Doing the work anyway only adds load for a caller that has given up.
  Status s = ctx->IsCancelled() ? Status::Cancelled("client cancelled") : handler();
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  m.latency->Observe(secs);
  m.results->Add({{"op", std::string(op)}, {"code", std::string(StatusCodeName(s.code()))}})
      .Increment();
  reactor->Finish(ToGrpc(s));
  return reactor;
}

Status KvServiceImpl::HandleGet(const v1::GetRequest& req, v1::GetResponse* resp) {
  CW_RETURN_IF_ERROR(ValidateTable(req.table()));
  CW_RETURN_IF_ERROR(ValidateKey(req.key()));
  CW_ASSIGN_OR_RETURN(auto read, store_->Read(req.table(), req.key()));
  FillVersions(read.siblings, resp->mutable_versions());
  // Context is returned even for deleted keys, so a writer can supersede the tombstone.
  if (!read.siblings.empty()) resp->set_context(read.siblings.Join().Encode());
  resp->set_replicas_responded(1);
  return Status::Ok();
}

Status KvServiceImpl::HandlePut(const v1::PutRequest& req, v1::PutResponse* resp) {
  CW_RETURN_IF_ERROR(ValidateTable(req.table()));
  CW_RETURN_IF_ERROR(ValidateKey(req.key()));
  CW_RETURN_IF_ERROR(ValidateValue(req.value()));
  CW_ASSIGN_OR_RETURN(auto ctx, DecodeContext(req.context()));
  CW_ASSIGN_OR_RETURN(auto written,
                      store_->CoordinateWrite(req.table(), req.key(), ctx, req.value(), false));
  FillVersions(written.siblings, resp->mutable_versions());
  resp->set_context(written.siblings.Join().Encode());
  resp->set_replicas_acked(1);
  return Status::Ok();
}

Status KvServiceImpl::HandleDelete(const v1::DeleteRequest& req, v1::DeleteResponse* resp) {
  CW_RETURN_IF_ERROR(ValidateTable(req.table()));
  CW_RETURN_IF_ERROR(ValidateKey(req.key()));
  CW_ASSIGN_OR_RETURN(auto ctx, DecodeContext(req.context()));
  CW_ASSIGN_OR_RETURN(auto written, store_->CoordinateWrite(req.table(), req.key(), ctx, "", true));
  FillVersions(written.siblings, resp->mutable_versions());
  resp->set_context(written.siblings.Join().Encode());
  resp->set_replicas_acked(1);
  return Status::Ok();
}

void KvServiceImpl::Record(const OpMetrics& m, std::string_view op, const Status& s,
                           std::chrono::steady_clock::time_point start) {
  m.latency->Observe(
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
  m.results->Add({{"op", std::string(op)}, {"code", std::string(StatusCodeName(s.code()))}})
      .Increment();
}

template <typename Resp>
void KvServiceImpl::CoordinatedWrite(grpc::CallbackServerContext* /*ctx*/, const OpMetrics& m,
                                     std::string_view op, const std::string& table,
                                     const std::string& key, const std::string& value,
                                     const std::string& context, bool tombstone, uint32_t w,
                                     Resp* resp, grpc::ServerUnaryReactor* reactor) {
  const auto start = std::chrono::steady_clock::now();
  Status s = ValidateTable(table);
  if (s.ok()) s = ValidateKey(key);
  if (s.ok()) s = ValidateValue(value);
  auto clock = DecodeContext(context);
  if (s.ok() && !clock.ok()) s = clock.status();
  if (!s.ok()) {
    Record(m, op, s, start);
    reactor->Finish(ToGrpc(s));
    return;
  }
  cluster_->Put(table, key, value, std::move(clock).value(), tombstone, w,
                [this, &m, op, resp, reactor, start](Result<CoordinatorResult> r) {
                  if (r.ok()) {
                    FillVersions(r->siblings, resp->mutable_versions());
                    resp->set_context(r->siblings.Join().Encode());
                    resp->set_replicas_acked(r->replicas);
                  }
                  Record(m, op, r.status(), start);
                  reactor->Finish(ToGrpc(r.status()));
                });
}

grpc::ServerUnaryReactor* KvServiceImpl::Get(grpc::CallbackServerContext* ctx,
                                             const v1::GetRequest* req, v1::GetResponse* resp) {
  if (cluster_ == nullptr) {
    return Serve(ctx, get_metrics_, "get", [&] { return HandleGet(*req, resp); });
  }
  auto* reactor = ctx->DefaultReactor();
  const auto start = std::chrono::steady_clock::now();
  Status s = ValidateTable(req->table());
  if (s.ok()) s = ValidateKey(req->key());
  if (!s.ok()) {
    Record(get_metrics_, "get", s, start);
    reactor->Finish(ToGrpc(s));
    return reactor;
  }
  cluster_->Get(req->table(), req->key(), req->r(),
                [this, resp, reactor, start](Result<CoordinatorResult> r) {
                  if (r.ok()) {
                    FillVersions(r->siblings, resp->mutable_versions());
                    if (!r->siblings.empty()) resp->set_context(r->siblings.Join().Encode());
                    resp->set_replicas_responded(r->replicas);
                  }
                  Record(get_metrics_, "get", r.status(), start);
                  reactor->Finish(ToGrpc(r.status()));
                });
  return reactor;
}

grpc::ServerUnaryReactor* KvServiceImpl::Put(grpc::CallbackServerContext* ctx,
                                             const v1::PutRequest* req, v1::PutResponse* resp) {
  if (cluster_ == nullptr) {
    return Serve(ctx, put_metrics_, "put", [&] { return HandlePut(*req, resp); });
  }
  auto* reactor = ctx->DefaultReactor();
  CoordinatedWrite(ctx, put_metrics_, "put", req->table(), req->key(), req->value(), req->context(),
                   false, req->w(), resp, reactor);
  return reactor;
}

grpc::ServerUnaryReactor* KvServiceImpl::Delete(grpc::CallbackServerContext* ctx,
                                                const v1::DeleteRequest* req,
                                                v1::DeleteResponse* resp) {
  if (cluster_ == nullptr) {
    return Serve(ctx, delete_metrics_, "delete", [&] { return HandleDelete(*req, resp); });
  }
  auto* reactor = ctx->DefaultReactor();
  CoordinatedWrite(ctx, delete_metrics_, "delete", req->table(), req->key(), std::string(),
                   req->context(), true, req->w(), resp, reactor);
  return reactor;
}

AdminServiceImpl::AdminServiceImpl(const LocalStore* store, ClusterNode* cluster,
                                   FaultSwitch* fault)
    : store_(store),
      cluster_(cluster),
      fault_(fault),
      started_ms_(SystemClock::Default()->WallMillis()),
      window_reads_(store->ReadLatency()),
      window_writes_(store->WriteLatency()) {}

grpc::ServerUnaryReactor* AdminServiceImpl::Isolate(grpc::CallbackServerContext* ctx,
                                                    const v1::IsolateRequest* req,
                                                    v1::IsolateResponse* resp) {
  auto* reactor = ctx->DefaultReactor();
  if (fault_ == nullptr) {
    reactor->Finish(ToGrpc(Status::FailedPrecondition(
        "fault injection is disabled (set debug.fault_injection = true)")));
    return reactor;
  }
  const int64_t ms = std::clamp<int64_t>(req->duration_ms(), 0, 120'000);
  fault_->IsolateFor(ms);
  CW_LOG_WARN("fault injection: isolated from peers for {} ms", ms);
  resp->set_node_id(store_->node_id());
  resp->set_isolated_ms(ms);
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

grpc::ServerUnaryReactor* AdminServiceImpl::ClusterInfo(grpc::CallbackServerContext* ctx,
                                                        const v1::ClusterInfoRequest* /*req*/,
                                                        v1::ClusterInfoResponse* resp) {
  auto* reactor = ctx->DefaultReactor();
  resp->set_node_id(store_->node_id());
  if (cluster_ == nullptr) {
    resp->set_n(1);
    resp->set_r(1);
    resp->set_w(1);
    auto* m = resp->add_members();
    m->set_node_id(store_->node_id());
    m->set_state("ALIVE");
    m->set_ownership(1.0);
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
  const auto& o = cluster_->options();
  resp->set_n(static_cast<uint32_t>(o.n));
  resp->set_r(static_cast<uint32_t>(o.r));
  resp->set_w(static_cast<uint32_t>(o.w));
  const auto ring = cluster_->RingSnapshot();
  const auto share = ring.Ownership();
  for (const auto& member : cluster_->membership().Snapshot()) {
    auto* m = resp->add_members();
    m->set_node_id(member.node_id());
    m->set_address(member.address());
    std::string state = cluster::v1::Member::State_Name(member.state());
    if (state.starts_with("STATE_")) state = state.substr(6);
    m->set_state(state);
    m->set_incarnation(member.incarnation());
    m->set_heartbeat(member.heartbeat());
    if (auto it = share.find(member.node_id()); it != share.end()) m->set_ownership(it->second);
  }
  const auto s = cluster_->Stats();
  resp->set_puts(s.puts);
  resp->set_gets(s.gets);
  resp->set_forwarded(s.forwarded);
  resp->set_quorum_failures(s.quorum_failures);
  resp->set_throttled(s.throttled);
  resp->set_read_repairs(s.read_repairs);
  resp->set_hints_stored(s.hints_stored);
  resp->set_hints_delivered(s.hints_delivered);
  resp->set_hints_pending(s.hints_pending);
  resp->set_anti_entropy_rounds(s.anti_entropy_rounds);
  resp->set_anti_entropy_keys_repaired(s.anti_entropy_keys_repaired);
  for (const auto& t : ring.tokens()) {
    auto* tok = resp->add_tokens();
    tok->set_position(t.position);
    tok->set_node_id(t.node);
  }
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

grpc::ServerUnaryReactor* AdminServiceImpl::PreferenceList(grpc::CallbackServerContext* ctx,
                                                           const v1::PreferenceListRequest* req,
                                                           v1::PreferenceListResponse* resp) {
  auto* reactor = ctx->DefaultReactor();
  const std::string skey = LocalStore::StorageKey(req->table(), req->key());
  resp->set_position(cluster::RingHash(skey));
  if (cluster_ != nullptr) {
    const auto ring = cluster_->RingSnapshot();
    for (const auto& n : ring.PreferenceList(skey, cluster_->options().n)) resp->add_home(n);
    for (const auto& n :
         ring.PreferenceList(skey, cluster_->options().n, cluster_->membership().DeadNodes())) {
      resp->add_live(n);
    }
  } else {
    resp->add_home(store_->node_id());
    resp->add_live(store_->node_id());
  }
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

grpc::ServerUnaryReactor* AdminServiceImpl::SetTableLimits(grpc::CallbackServerContext* ctx,
                                                           const v1::TableLimitsRequest* req,
                                                           v1::TableLimitsResponse* /*resp*/) {
  auto* reactor = ctx->DefaultReactor();
  if (cluster_ == nullptr) {
    reactor->Finish(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "not in cluster mode"));
    return reactor;
  }
  cluster_->SetTableLimits(req->table(), req->reads_per_sec(), req->writes_per_sec());
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

grpc::ServerUnaryReactor* AdminServiceImpl::Health(grpc::CallbackServerContext* ctx,
                                                   const v1::HealthRequest* /*req*/,
                                                   v1::HealthResponse* resp) {
  resp->set_state(draining_.load(std::memory_order_relaxed) ? v1::HealthResponse::STATE_DRAINING
                                                            : v1::HealthResponse::STATE_SERVING);
  resp->set_node_id(store_->node_id());
  resp->set_version(std::string(kVersion));
  resp->set_git_sha(std::string(kGitSha));
  resp->set_uptime_ms(SystemClock::Default()->WallMillis() - started_ms_);
  auto* reactor = ctx->DefaultReactor();
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

grpc::ServerUnaryReactor* AdminServiceImpl::NodeStats(grpc::CallbackServerContext* ctx,
                                                      const v1::NodeStatsRequest* /*req*/,
                                                      v1::NodeStatsResponse* resp) {
  const auto stats = store_->engine().Stats();
  const auto reads = store_->ReadLatency();
  const auto writes = store_->WriteLatency();
  resp->set_node_id(store_->node_id());
  resp->set_keys(stats.keys);
  resp->set_bytes(stats.bytes);
  resp->set_gets_total(store_->reads());
  resp->set_puts_total(store_->writes());
  resp->set_get_p50_us(static_cast<double>(reads.Percentile(50)) / 1e3);
  resp->set_get_p99_us(static_cast<double>(reads.Percentile(99)) / 1e3);
  resp->set_put_p50_us(static_cast<double>(writes.Percentile(50)) / 1e3);
  resp->set_put_p99_us(static_cast<double>(writes.Percentile(99)) / 1e3);
  {
    // Close a window once at least a second has passed; between closes,
    // every caller sees the last closed window.
    std::lock_guard lock(window_mu_);
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - window_start_).count();
    if (elapsed >= 1000) {
      const auto r = reads.Minus(window_reads_);
      const auto w = writes.Minus(window_writes_);
      last_window_get_p99_us_ = static_cast<double>(r.Percentile(99)) / 1e3;
      last_window_put_p99_us_ = static_cast<double>(w.Percentile(99)) / 1e3;
      last_window_ms_ = elapsed;
      window_reads_ = reads;
      window_writes_ = writes;
      window_start_ = now;
    }
    resp->set_window_get_p99_us(last_window_get_p99_us_);
    resp->set_window_put_p99_us(last_window_put_p99_us_);
    resp->set_window_ms(last_window_ms_);
  }
  resp->set_isolated_ms(fault_ != nullptr ? fault_->RemainingMs() : 0);
  auto* reactor = ctx->DefaultReactor();
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

}  // namespace cartwheel::kv
