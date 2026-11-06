#include "common/status.h"

namespace cartwheel {

std::string_view StatusCodeName(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::kOk: return "OK";
    case StatusCode::kNotFound: return "NotFound";
    case StatusCode::kInvalidArgument: return "InvalidArgument";
    case StatusCode::kAlreadyExists: return "AlreadyExists";
    case StatusCode::kFailedPrecondition: return "FailedPrecondition";
    case StatusCode::kAborted: return "Aborted";
    case StatusCode::kUnavailable: return "Unavailable";
    case StatusCode::kDeadlineExceeded: return "DeadlineExceeded";
    case StatusCode::kThrottled: return "Throttled";
    case StatusCode::kResourceExhausted: return "ResourceExhausted";
    case StatusCode::kCorruption: return "Corruption";
    case StatusCode::kIOError: return "IOError";
    case StatusCode::kCancelled: return "Cancelled";
    case StatusCode::kUnimplemented: return "Unimplemented";
    case StatusCode::kInternal: return "Internal";
    case StatusCode::kUnauthenticated: return "Unauthenticated";
    case StatusCode::kPermissionDenied: return "PermissionDenied";
  }
  return "Unknown";
}

std::string Status::ToString() const {
  if (ok()) return "OK";
  std::string out(StatusCodeName(code_));
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  return out;
}

Status Status::WithContext(std::string_view context) const {
  if (ok()) return *this;
  std::string msg(context);
  if (!message_.empty()) {
    msg += ": ";
    msg += message_;
  }
  return {code_, std::move(msg)};
}

std::ostream& operator<<(std::ostream& os, const Status& s) {
  return os << s.ToString();
}

}  // namespace cartwheel
