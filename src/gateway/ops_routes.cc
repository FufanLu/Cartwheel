#include "gateway/ops_routes.h"

#include <atomic>
#include <cstdint>
#include <limits>

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <nlohmann/json.hpp>

#include "common/clock.h"
#include "gateway/grpc_await.h"
#include "kv/grpc_status.h"

namespace cartwheel::gateway {
namespace {

namespace kv1 = kv::v1;

// One node's answers to both RPCs.
struct Probe {
  grpc::ClientContext info_ctx;
  grpc::ClientContext stats_ctx;
  kv1::ClusterInfoRequest info_req;
  kv1::ClusterInfoResponse info;
  grpc::Status info_status;
  kv1::NodeStatsRequest stats_req;
  kv1::NodeStatsResponse stats;
  grpc::Status stats_status;
};

// Starts `n` callback RPCs and resumes the coroutine once all of them have
// completed, on the coroutine's own executor.
template <typename StartAll>
boost::asio::awaitable<void> AwaitAll(int n, StartAll start_all) {
  return boost::asio::async_initiate<const boost::asio::use_awaitable_t<>, void()>(
      [n, start_all = std::move(start_all)](auto handler) mutable {
        auto shared = std::make_shared<decltype(handler)>(std::move(handler));
        auto remaining = std::make_shared<std::atomic<int>>(n);
        auto one_done = [shared, remaining] {
          if (remaining->fetch_sub(1) != 1) return;
          auto ex = boost::asio::get_associated_executor(*shared);
          boost::asio::post(ex, [shared]() mutable { std::move (*shared)(); });
        };
        start_all(one_done);
      },
      boost::asio::use_awaitable);
}

}  // namespace

OpsRoutes::OpsRoutes(std::vector<OpsNode> nodes, std::string admin_token,
                     std::chrono::milliseconds timeout)
    : nodes_(std::move(nodes)), admin_token_(std::move(admin_token)), timeout_(timeout) {}

std::vector<OpsNode> OpsRoutes::FromSpecs(const std::vector<std::string>& specs) {
  std::vector<OpsNode> out;
  for (const auto& spec : specs) {
    const auto eq = spec.find('=');
    OpsNode n;
    n.id = eq == std::string::npos ? spec : spec.substr(0, eq);
    n.address = eq == std::string::npos ? spec : spec.substr(eq + 1);
    grpc::ChannelArguments args;
    // Reconnect quickly after a node restarts; the console polls every second.
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 200);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);
    n.stub = kv1::AdminService::NewStub(
        grpc::CreateCustomChannel(n.address, grpc::InsecureChannelCredentials(), args));
    out.push_back(std::move(n));
  }
  return out;
}

void OpsRoutes::Register(Router* router) {
  router->Add(http::verb::get, "/api/v1/ops/cluster",
              [this](RequestContext& c) { return Cluster(c); });
  router->Add(http::verb::post, "/api/v1/admin/nodes/{node}/isolate",
              [this](RequestContext& c) { return Isolate(c); });
}

boost::asio::awaitable<Response> OpsRoutes::Cluster(RequestContext& ctx) {
  std::vector<std::unique_ptr<Probe>> probes;
  probes.reserve(nodes_.size());
  for (size_t i = 0; i < nodes_.size(); ++i) probes.push_back(std::make_unique<Probe>());
  const auto deadline = std::chrono::system_clock::now() + timeout_;
  co_await AwaitAll(static_cast<int>(nodes_.size() * 2), [&](const auto& one_done) {
    for (size_t i = 0; i < nodes_.size(); ++i) {
      Probe* p = probes[i].get();
      p->info_ctx.set_deadline(deadline);
      p->stats_ctx.set_deadline(deadline);
      nodes_[i].stub->async()->ClusterInfo(&p->info_ctx, &p->info_req, &p->info,
                                           [p, one_done](grpc::Status s) {
                                             p->info_status = std::move(s);
                                             one_done();
                                           });
      nodes_[i].stub->async()->NodeStats(&p->stats_ctx, &p->stats_req, &p->stats,
                                         [p, one_done](grpc::Status s) {
                                           p->stats_status = std::move(s);
                                           one_done();
                                         });
    }
  });

  // The membership states come from one observer: the first reachable node
  // in configuration order. Each node's own counters come from itself.
  const Probe* observer = nullptr;
  std::string observer_id;
  for (size_t i = 0; i < nodes_.size(); ++i) {
    if (probes[i]->info_status.ok()) {
      observer = probes[i].get();
      observer_id = nodes_[i].id;
      break;
    }
  }
  nlohmann::json nodes = nlohmann::json::array();
  for (size_t i = 0; i < nodes_.size(); ++i) {
    const Probe& p = *probes[i];
    nlohmann::json n{{"id", nodes_[i].id}, {"address", nodes_[i].address}};
    n["reachable"] = p.info_status.ok() || p.stats_status.ok();
    if (!p.info_status.ok()) n["error"] = p.info_status.error_message();
    if (observer != nullptr) {
      for (const auto& m : observer->info.members()) {
        if (m.node_id() != nodes_[i].id) continue;
        n["state"] = m.state();
        n["ownership"] = m.ownership();
        n["incarnation"] = m.incarnation();
        n["heartbeat"] = m.heartbeat();
      }
    }
    if (p.info_status.ok()) {
      const auto& c = p.info;
      n["counters"] = {{"puts", c.puts()},
                       {"gets", c.gets()},
                       {"forwarded", c.forwarded()},
                       {"quorum_failures", c.quorum_failures()},
                       {"throttled", c.throttled()},
                       {"read_repairs", c.read_repairs()},
                       {"hints_stored", c.hints_stored()},
                       {"hints_delivered", c.hints_delivered()},
                       {"hints_pending", c.hints_pending()},
                       {"anti_entropy_rounds", c.anti_entropy_rounds()},
                       {"anti_entropy_keys_repaired", c.anti_entropy_keys_repaired()}};
    }
    if (p.stats_status.ok()) {
      const auto& s = p.stats;
      n["stats"] = {{"keys", s.keys()},
                    {"bytes", s.bytes()},
                    {"gets_total", s.gets_total()},
                    {"puts_total", s.puts_total()},
                    {"window_get_p99_us", s.window_get_p99_us()},
                    {"window_put_p99_us", s.window_put_p99_us()},
                    {"window_ms", s.window_ms()},
                    {"isolated_ms", s.isolated_ms()}};
    }
    nodes.push_back(std::move(n));
  }
  nlohmann::json body{{"sampled_at_ms", SystemClock::Default()->WallMillis()},
                      {"observer", observer_id},
                      {"nodes", std::move(nodes)}};
  if (observer != nullptr) {
    body["n"] = observer->info.n();
    body["r"] = observer->info.r();
    body["w"] = observer->info.w();
    nlohmann::json tokens = nlohmann::json::array();
    // Positions as fractions of the 2^64 ring: enough precision to draw.
    constexpr auto kRing = static_cast<double>(std::numeric_limits<uint64_t>::max());
    for (const auto& t : observer->info.tokens()) {
      tokens.push_back({static_cast<double>(t.position()) / kRing, t.node_id()});
    }
    body["tokens"] = std::move(tokens);
  }
  co_return JsonResponse(http::status::ok, body, ctx);
}

boost::asio::awaitable<Response> OpsRoutes::Isolate(RequestContext& ctx) {
  if (admin_token_.empty() || ctx.request["X-Admin-Token"] != std::string_view(admin_token_)) {
    co_return ErrorResponse(Status::PermissionDenied("admin token required"), ctx);
  }
  const std::string& id = ctx.params.at("node");
  const OpsNode* node = nullptr;
  for (const auto& n : nodes_) {
    if (n.id == id) node = &n;
  }
  if (node == nullptr) co_return ErrorResponse(Status::NotFound("no node " + id), ctx);
  kv1::IsolateRequest req;
  try {
    const auto body = ctx.request.body().empty() ? nlohmann::json::object()
                                                 : nlohmann::json::parse(ctx.request.body());
    req.set_duration_ms(body.value("duration_ms", int64_t{10'000}));
  } catch (const nlohmann::json::exception& e) {
    co_return ErrorResponse(Status::InvalidArgument(std::string("bad JSON body: ") + e.what()),
                            ctx);
  }
  grpc::ClientContext g;
  g.set_deadline(std::chrono::system_clock::now() + timeout_);
  kv1::IsolateResponse resp;
  const grpc::Status s = co_await AwaitGrpc(
      [&](auto done) { node->stub->async()->Isolate(&g, &req, &resp, std::move(done)); });
  if (!s.ok()) co_return ErrorResponse(kv::FromGrpc(s), ctx);
  co_return JsonResponse(http::status::ok,
                         {{"node_id", resp.node_id()}, {"isolated_ms", resp.isolated_ms()}}, ctx);
}

}  // namespace cartwheel::gateway
