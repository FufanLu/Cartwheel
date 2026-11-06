#include "kv/peer_transport.h"

#include <chrono>

#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/channel_arguments.h>

#include "kv/grpc_status.h"

namespace cartwheel::kv {

GrpcPeerTransport::GrpcPeerTransport(cluster::NodeId self, Resolver resolve)
    : self_(std::move(self)), resolve_(std::move(resolve)) {}

cluster::v1::PeerService::Stub* GrpcPeerTransport::StubFor(const std::string& address) {
  std::lock_guard lock(mu_);
  auto& stub = stubs_[address];
  if (!stub) {
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_ENABLE_RETRIES, 0);
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 10'000);
    stub = cluster::v1::PeerService::NewStub(
        grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), args));
  }
  return stub.get();
}

void GrpcPeerTransport::Call(const cluster::NodeId& to, cluster::MessageType type,
                             std::string payload, int64_t timeout_ns, Done done) {
  const std::string address = resolve_(to);
  if (address.empty()) {
    done(Status::Unavailable("no address for " + to));
    return;
  }
  struct InFlight {
    grpc::ClientContext ctx;
    cluster::v1::PeerRequest req;
    cluster::v1::PeerResponse resp;
    Done done;
  };
  auto call = std::make_shared<InFlight>();
  call->ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::duration_cast<std::chrono::system_clock::duration>(
                             std::chrono::nanoseconds(timeout_ns)));
  call->req.set_from(self_);
  call->req.set_type(type);
  call->req.set_payload(std::move(payload));
  call->done = std::move(done);
  StubFor(address)->async()->Call(
      &call->ctx, &call->req, &call->resp, [call](const grpc::Status& s) {
        if (!s.ok()) {
          call->done(FromGrpc(s));
          return;
        }
        if (call->resp.code() != 0) {
          call->done(Status(static_cast<StatusCode>(call->resp.code()), call->resp.message()));
          return;
        }
        call->done(std::move(*call->resp.mutable_payload()));
      });
}

grpc::ServerUnaryReactor* PeerServiceImpl::Call(grpc::CallbackServerContext* ctx,
                                                const cluster::v1::PeerRequest* req,
                                                cluster::v1::PeerResponse* resp) {
  auto* reactor = ctx->DefaultReactor();
  handler_->HandleRequest(req->from(), req->type(), req->payload(),
                          [reactor, resp](Result<std::string> r) {
                            if (r.ok()) {
                              resp->set_payload(std::move(r).value());
                            } else {
                              resp->set_code(static_cast<uint32_t>(r.status().code()));
                              resp->set_message(r.status().message());
                            }
                            reactor->Finish(grpc::Status::OK);
                          });
  return reactor;
}

}  // namespace cartwheel::kv
