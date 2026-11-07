#pragma once

// W3C trace context over gRPC metadata (header-only; include where gRPC is
// already linked).

#include <string>

#include <grpcpp/client_context.h>
#include <grpcpp/server_context.h>

#include "common/tracing.h"

namespace cartwheel::tracing {

inline std::string IncomingParent(const grpc::CallbackServerContext* ctx) {
  const auto& md = ctx->client_metadata();
  auto it = md.find("traceparent");
  return it == md.end() ? std::string() : std::string(it->second.data(), it->second.size());
}

// Sends `span`'s context, or the thread's current parent when `span` is null.
inline void Inject(grpc::ClientContext* ctx, const Span& span) {
  std::string tp = span ? span.TraceParent() : CurrentParent();
  if (!tp.empty()) ctx->AddMetadata("traceparent", tp);
}

}  // namespace cartwheel::tracing
