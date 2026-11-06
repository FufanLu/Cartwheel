#pragma once

#include <cassert>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace cartwheel {

// Error codes shared by every layer. They map 1:1 onto gRPC status codes at the
// RPC boundary (see kv/grpc_status.h), plus kThrottled and kCorruption, which
// carry extra meaning for retry policy and storage.
enum class StatusCode : uint8_t {
  kOk = 0,
  kNotFound,
  kInvalidArgument,
  kAlreadyExists,
  kFailedPrecondition,
  kAborted,
  kUnavailable,        // transient: retry with backoff
  kDeadlineExceeded,   // transient: retry only if idempotent
  kThrottled,          // caller exceeded its provisioned rate: back off
  kResourceExhausted,  // server is shedding load: back off
  kCorruption,         // checksum or format mismatch: never retry
  kIOError,
  kCancelled,
  kUnimplemented,
  kInternal,
  kUnauthenticated,   // no or invalid credentials
  kPermissionDenied,  // authenticated, but not allowed
};

std::string_view StatusCodeName(StatusCode code) noexcept;

// True for codes where retrying the same request may succeed.
constexpr bool IsRetryable(StatusCode code) noexcept {
  return code == StatusCode::kUnavailable || code == StatusCode::kDeadlineExceeded ||
         code == StatusCode::kThrottled || code == StatusCode::kResourceExhausted ||
         code == StatusCode::kAborted;
}

class [[nodiscard]] Status {
 public:
  Status() noexcept = default;
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

  static Status Ok() noexcept { return {}; }
  static Status NotFound(std::string msg = {}) { return {StatusCode::kNotFound, std::move(msg)}; }
  static Status InvalidArgument(std::string msg) {
    return {StatusCode::kInvalidArgument, std::move(msg)};
  }
  static Status AlreadyExists(std::string msg) {
    return {StatusCode::kAlreadyExists, std::move(msg)};
  }
  static Status FailedPrecondition(std::string msg) {
    return {StatusCode::kFailedPrecondition, std::move(msg)};
  }
  static Status Aborted(std::string msg) { return {StatusCode::kAborted, std::move(msg)}; }
  static Status Unavailable(std::string msg) { return {StatusCode::kUnavailable, std::move(msg)}; }
  static Status DeadlineExceeded(std::string msg) {
    return {StatusCode::kDeadlineExceeded, std::move(msg)};
  }
  static Status Throttled(std::string msg) { return {StatusCode::kThrottled, std::move(msg)}; }
  static Status ResourceExhausted(std::string msg) {
    return {StatusCode::kResourceExhausted, std::move(msg)};
  }
  static Status Corruption(std::string msg) { return {StatusCode::kCorruption, std::move(msg)}; }
  static Status IOError(std::string msg) { return {StatusCode::kIOError, std::move(msg)}; }
  static Status Cancelled(std::string msg) { return {StatusCode::kCancelled, std::move(msg)}; }
  static Status Unimplemented(std::string msg) {
    return {StatusCode::kUnimplemented, std::move(msg)};
  }
  static Status Internal(std::string msg) { return {StatusCode::kInternal, std::move(msg)}; }
  static Status Unauthenticated(std::string msg) {
    return {StatusCode::kUnauthenticated, std::move(msg)};
  }
  static Status PermissionDenied(std::string msg) {
    return {StatusCode::kPermissionDenied, std::move(msg)};
  }

  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::kOk; }
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] bool retryable() const noexcept { return IsRetryable(code_); }

  [[nodiscard]] bool IsNotFound() const noexcept { return code_ == StatusCode::kNotFound; }
  [[nodiscard]] bool IsCorruption() const noexcept { return code_ == StatusCode::kCorruption; }

  // "NotFound: key 'x'" or "OK".
  [[nodiscard]] std::string ToString() const;

  // Prefixes context while keeping the code: s.WithContext("open wal").
  [[nodiscard]] Status WithContext(std::string_view context) const;

  friend bool operator==(const Status& a, const Status& b) noexcept {
    return a.code_ == b.code_ && a.message_ == b.message_;
  }

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

std::ostream& operator<<(std::ostream& os, const Status& s);

// Result<T> holds either a value or a non-OK Status (a small std::expected).
template <typename T>
class [[nodiscard]] Result {
  static_assert(!std::is_same_v<std::remove_cvref_t<T>, Status>, "Result<Status> is meaningless");

 public:
  // Implicit on purpose so `return value;` and `return Status::NotFound();` both work.
  Result(T value) : storage_(std::in_place_index<1>, std::move(value)) {}        // NOLINT
  Result(Status status) : storage_(std::in_place_index<0>, std::move(status)) {  // NOLINT
    assert(!std::get<0>(storage_).ok() && "Result constructed from OK status without a value");
  }

  [[nodiscard]] bool ok() const noexcept { return storage_.index() == 1; }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Status& status() const& noexcept {
    static const Status kOk;
    return ok() ? kOk : std::get<0>(storage_);
  }
  [[nodiscard]] Status status() && {
    return ok() ? Status::Ok() : std::move(std::get<0>(storage_));
  }

  [[nodiscard]] T& value() & {
    assert(ok());
    return std::get<1>(storage_);
  }
  [[nodiscard]] const T& value() const& {
    assert(ok());
    return std::get<1>(storage_);
  }
  [[nodiscard]] T&& value() && {
    assert(ok());
    return std::get<1>(std::move(storage_));
  }

  template <typename U>
  [[nodiscard]] T value_or(U&& fallback) const& {
    return ok() ? std::get<1>(storage_) : static_cast<T>(std::forward<U>(fallback));
  }

  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }

 private:
  std::variant<Status, T> storage_;
};

}  // namespace cartwheel

#define CW_CONCAT_INNER_(a, b) a##b
#define CW_CONCAT_(a, b) CW_CONCAT_INNER_(a, b)

// Propagates a non-OK Status to the caller.
#define CW_RETURN_IF_ERROR(expr)             \
  do {                                       \
    ::cartwheel::Status cw_status_ = (expr); \
    if (!cw_status_.ok()) return cw_status_; \
  } while (false)

// CW_ASSIGN_OR_RETURN(auto file, OpenFile(path));
#define CW_ASSIGN_OR_RETURN(lhs, expr) \
  CW_ASSIGN_OR_RETURN_IMPL_(CW_CONCAT_(cw_result_, __LINE__), lhs, expr)
// NOLINTBEGIN(bugprone-macro-parentheses): `lhs` is a declaration, it cannot be parenthesized.
#define CW_ASSIGN_OR_RETURN_IMPL_(tmp, lhs, expr) \
  auto tmp = (expr);                              \
  if (!tmp.ok()) return std::move(tmp).status();  \
  lhs = std::move(tmp).value()
// NOLINTEND(bugprone-macro-parentheses)
