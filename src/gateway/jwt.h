#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/status.h"

namespace cartwheel::gateway {

[[nodiscard]] std::string Base64UrlEncode(std::string_view in);
[[nodiscard]] Result<std::string> Base64UrlDecode(std::string_view in);

struct JwtClaims {
  std::string subject;  // user id
  std::string issuer;
  int64_t issued_at = 0;   // unix seconds
  int64_t expires_at = 0;  // unix seconds
};

// HS256 JSON Web Tokens (RFC 7519) with OpenSSL's HMAC-SHA256.
//
// Verify is strict where JWT libraries have historically been weak:
//   - the header must say alg "HS256"; "none" and every other algorithm are
//     rejected (no algorithm confusion);
//   - the signature is compared in constant time (CRYPTO_memcmp);
//   - exp is required, and iss must match; a small leeway absorbs clock skew.
class JwtSigner {
 public:
  explicit JwtSigner(std::string secret, std::string issuer = "cartwheel", int64_t leeway_s = 30);

  [[nodiscard]] std::string Sign(const std::string& subject, int64_t now_s, int64_t ttl_s) const;
  [[nodiscard]] Result<JwtClaims> Verify(std::string_view token, int64_t now_s) const;

 private:
  [[nodiscard]] std::string Mac(std::string_view signing_input) const;

  std::string secret_;
  std::string issuer_;
  int64_t leeway_s_;
};

}  // namespace cartwheel::gateway
