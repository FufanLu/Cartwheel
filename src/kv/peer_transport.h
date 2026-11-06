#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <grpcpp/channel.h>

#include "cartwheel/cluster/v1/cluster.grpc.pb.h"
#include "cluster/runtime.h"

namespace cartwheel::kv {

// Production cluster::Transport over gRPC: every node-to-node message is one
// PeerService.Call carrying the message type and its serialized payload.
// Channels are created lazily per peer address and reused (HTTP/2
// multiplexing), with fast reconnect so a restarted peer is reachable again
// within ~100 ms.
class GrpcPeerTransport final : public cluster::Transport {
 public:
  using Resolver = std::function<std::string(const cluster::NodeId&)>;
  GrpcPeerTransport(cluster::NodeId self, Resolver resolve);

  void Call(const cluster::NodeId& to, cluster::MessageType type, std::string payload,
            int64_t timeout_ns, Done done) override;

 private:
  cluster::v1::PeerService::Stub* StubFor(const std::string& address);

  const cluster::NodeId self_;
  const Resolver resolve_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<cluster::v1::PeerService::Stub>> stubs_;
};

// Receiving side: hands each PeerService.Call to the node's RequestHandler,
// which may answer asynchronously (forwarded requests wait for a quorum).
class PeerServiceImpl final : public cluster::v1::PeerService::CallbackService {
 public:
  explicit PeerServiceImpl(cluster::RequestHandler* handler) : handler_(handler) {}
  grpc::ServerUnaryReactor* Call(grpc::CallbackServerContext* ctx,
                                 const cluster::v1::PeerRequest* req,
                                 cluster::v1::PeerResponse* resp) override;

 private:
  cluster::RequestHandler* handler_;
};

}  // namespace cartwheel::kv
