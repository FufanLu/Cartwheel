#pragma once

#include <string>
#include <string_view>

#include "common/clock.h"
#include "gateway/http.h"
#include "gateway/jwt.h"

namespace cartwheel::gateway {

// User ids: 1-32 of [a-z0-9_-]. Small and URL-safe, and ':' never appears,
// which the deal gate relies on for its marker field.
[[nodiscard]] bool ValidUserId(std::string_view user);

// Bearer-token authentication for the REST and WebSocket edges.
class Authenticator {
 public:
  Authenticator(JwtSigner signer, int64_t ttl_s, const Clock* clock = SystemClock::Default());

  // A token for `user` and its expiry (unix seconds).
  [[nodiscard]] std::string Issue(const std::string& user, int64_t* expires_at) const;

  // From "Authorization: Bearer <token>".
  [[nodiscard]] Result<std::string> UserFromRequest(const Request& req) const;
  [[nodiscard]] Result<std::string> UserFromToken(std::string_view token) const;

  // Cart routes name the user in the path. Without a token that is allowed
  // only if anonymous carts are enabled; with one, the subject must match.
  [[nodiscard]] Status AuthorizeUser(const Request& req, const std::string& path_user,
                                     bool allow_anonymous) const;

 private:
  JwtSigner signer_;
  int64_t ttl_s_;
  const Clock* clock_;
};

}  // namespace cartwheel::gateway
