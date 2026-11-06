#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "cartwheel/kv/v1/kv.grpc.pb.h"
#include "gateway/http.h"

namespace cartwheel::gateway {

struct OpsNode {
  std::string id;
  std::string address;
  std::shared_ptr<kv::v1::AdminService::Stub> stub;
};

// Read-only cluster view for the ops console, plus one fault-injection action:
//   GET  /api/v1/ops/cluster                 every node's membership view,
//                                            ring tokens, counters and stats
//   POST /api/v1/admin/nodes/{node}/isolate  X-Admin-Token; {duration_ms}
//
// The gateway asks every node in parallel with a short deadline, so a dead or
// isolated node costs one timeout, not one per node, and shows up as
// unreachable instead of stalling the page.
class OpsRoutes {
 public:
  OpsRoutes(std::vector<OpsNode> nodes, std::string admin_token,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(400));
  void Register(Router* router);

  // "wheel-1=127.0.0.1:7101" entries -> nodes with stubs.
  static std::vector<OpsNode> FromSpecs(const std::vector<std::string>& specs);

 private:
  boost::asio::awaitable<Response> Cluster(RequestContext& ctx);
  boost::asio::awaitable<Response> Isolate(RequestContext& ctx);

  std::vector<OpsNode> nodes_;
  std::string admin_token_;
  std::chrono::milliseconds timeout_;
};

}  // namespace cartwheel::gateway
