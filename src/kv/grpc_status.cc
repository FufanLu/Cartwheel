#include "kv/grpc_status.h"

#include <string>
#include <string_view>

namespace cartwheel::kv {
namespace {

constexpr std::string_view kThrottledPrefix = "Throttled: ";

}  // namespace

grpc::Status ToGrpc(const Status& s) {
  using grpc::StatusCode;
  switch (s.code()) {
    case cartwheel::StatusCode::kOk: return grpc::Status::OK;
    case cartwheel::StatusCode::kNotFound: return {StatusCode::NOT_FOUND, s.message()};
    case cartwheel::StatusCode::kInvalidArgument:
      return {StatusCode::INVALID_ARGUMENT, s.message()};
    case cartwheel::StatusCode::kAlreadyExists: return {StatusCode::ALREADY_EXISTS, s.message()};
    case cartwheel::StatusCode::kFailedPrecondition:
      return {StatusCode::FAILED_PRECONDITION, s.message()};
    case cartwheel::StatusCode::kAborted: return {StatusCode::ABORTED, s.message()};
    case cartwheel::StatusCode::kUnavailable: return {StatusCode::UNAVAILABLE, s.message()};
    case cartwheel::StatusCode::kDeadlineExceeded:
      return {StatusCode::DEADLINE_EXCEEDED, s.message()};
    case cartwheel::StatusCode::kThrottled:
      return {StatusCode::RESOURCE_EXHAUSTED, std::string(kThrottledPrefix) + s.message()};
    case cartwheel::StatusCode::kResourceExhausted:
      return {StatusCode::RESOURCE_EXHAUSTED, s.message()};
    case cartwheel::StatusCode::kCorruption: return {StatusCode::DATA_LOSS, s.message()};
    case cartwheel::StatusCode::kIOError: return {StatusCode::INTERNAL, s.message()};
    case cartwheel::StatusCode::kCancelled: return {StatusCode::CANCELLED, s.message()};
    case cartwheel::StatusCode::kUnimplemented: return {StatusCode::UNIMPLEMENTED, s.message()};
    case cartwheel::StatusCode::kInternal: return {StatusCode::INTERNAL, s.message()};
    case cartwheel::StatusCode::kUnauthenticated: return {StatusCode::UNAUTHENTICATED, s.message()};
    case cartwheel::StatusCode::kPermissionDenied:
      return {StatusCode::PERMISSION_DENIED, s.message()};
  }
  return {StatusCode::UNKNOWN, s.message()};
}

Status FromGrpc(const grpc::Status& s) {
  const std::string& msg = s.error_message();
  switch (s.error_code()) {
    case grpc::StatusCode::OK: return Status::Ok();
    case grpc::StatusCode::NOT_FOUND: return Status::NotFound(msg);
    case grpc::StatusCode::INVALID_ARGUMENT: return Status::InvalidArgument(msg);
    case grpc::StatusCode::ALREADY_EXISTS: return Status::AlreadyExists(msg);
    case grpc::StatusCode::FAILED_PRECONDITION: return Status::FailedPrecondition(msg);
    case grpc::StatusCode::ABORTED: return Status::Aborted(msg);
    case grpc::StatusCode::UNAVAILABLE: return Status::Unavailable(msg);
    case grpc::StatusCode::DEADLINE_EXCEEDED: return Status::DeadlineExceeded(msg);
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      if (std::string_view(msg).starts_with(kThrottledPrefix)) {
        return Status::Throttled(msg.substr(kThrottledPrefix.size()));
      }
      return Status::ResourceExhausted(msg);
    case grpc::StatusCode::DATA_LOSS: return Status::Corruption(msg);
    case grpc::StatusCode::CANCELLED: return Status::Cancelled(msg);
    case grpc::StatusCode::UNIMPLEMENTED: return Status::Unimplemented(msg);
    case grpc::StatusCode::UNAUTHENTICATED: return Status::Unauthenticated(msg);
    case grpc::StatusCode::PERMISSION_DENIED: return Status::PermissionDenied(msg);
    default: return Status::Internal(msg);
  }
}

}  // namespace cartwheel::kv
