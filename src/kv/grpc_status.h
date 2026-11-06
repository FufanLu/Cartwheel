#pragma once

#include <grpcpp/support/status.h>

#include "common/status.h"

namespace cartwheel::kv {

// Status <-> grpc::Status at the RPC boundary. kThrottled and kCorruption
// have no gRPC equivalent, so they travel as RESOURCE_EXHAUSTED / DATA_LOSS
// with a "Throttled: " / "Corruption: " message prefix that FromGrpc restores.
[[nodiscard]] grpc::Status ToGrpc(const Status& s);
[[nodiscard]] Status FromGrpc(const grpc::Status& s);

}  // namespace cartwheel::kv
