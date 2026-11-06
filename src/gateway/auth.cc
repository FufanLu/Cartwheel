#include "gateway/auth.h"

#include <algorithm>

namespace cartwheel::gateway {

bool ValidUserId(std::string_view user) {
  return !user.empty() && user.size() <= 32 && std::all_of(user.begin(), user.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}

Authenticator::Authenticator(JwtSigner signer, int64_t ttl_s, const Clock* clock)
    : signer_(std::move(signer)), ttl_s_(ttl_s), clock_(clock) {}

std::string Authenticator::Issue(const std::string& user, int64_t* expires_at) const {
  const int64_t now = clock_->WallNanos() / 1'000'000'000;
  if (expires_at != nullptr) *expires_at = now + ttl_s_;
  return signer_.Sign(user, now, ttl_s_);
}

Result<std::string> Authenticator::UserFromToken(std::string_view token) const {
  CW_ASSIGN_OR_RETURN(auto claims, signer_.Verify(token, clock_->WallNanos() / 1'000'000'000));
  if (!ValidUserId(claims.subject)) return Status::Unauthenticated("bad subject");
  return claims.subject;
}

Result<std::string> Authenticator::UserFromRequest(const Request& req) const {
  const auto header = req[http::field::authorization];
  constexpr std::string_view kBearer = "Bearer ";
  if (header.empty()) return Status::Unauthenticated("missing Authorization: Bearer token");
  if (!std::string_view(header).starts_with(kBearer)) {
    return Status::Unauthenticated("expected a Bearer token");
  }
  return UserFromToken(std::string_view(header).substr(kBearer.size()));
}

Status Authenticator::AuthorizeUser(const Request& req, const std::string& path_user,
                                    bool allow_anonymous) const {
  if (req[http::field::authorization].empty()) {
    return allow_anonymous ? Status::Ok()
                           : Status::Unauthenticated("missing Authorization: Bearer token");
  }
  CW_ASSIGN_OR_RETURN(auto user, UserFromRequest(req));
  if (user != path_user) return Status::PermissionDenied("token is for another user");
  return Status::Ok();
}

}  // namespace cartwheel::gateway
