#include "gateway/jwt.h"

#include <array>

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace cartwheel::gateway {
namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int DecodeChar(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

}  // namespace

std::string Base64UrlEncode(std::string_view in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) |
                       (static_cast<uint8_t>(in[i + 1]) << 8) | static_cast<uint8_t>(in[i + 2]);
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
  }
  const size_t rest = in.size() - i;
  if (rest == 1) {
    const uint32_t v = static_cast<uint8_t>(in[i]) << 16;
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
  } else if (rest == 2) {
    const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8);
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
  }
  return out;  // unpadded, as JWT requires
}

Result<std::string> Base64UrlDecode(std::string_view in) {
  if (in.size() % 4 == 1) return Status::InvalidArgument("base64url: bad length");
  std::string out;
  out.reserve(in.size() * 3 / 4);
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    const int d = DecodeChar(c);
    if (d < 0) return Status::InvalidArgument("base64url: bad character");
    acc = (acc << 6) | static_cast<uint32_t>(d);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xff);
    }
  }
  return out;
}

JwtSigner::JwtSigner(std::string secret, std::string issuer, int64_t leeway_s)
    : secret_(std::move(secret)), issuer_(std::move(issuer)), leeway_s_(leeway_s) {}

std::string JwtSigner::Mac(std::string_view signing_input) const {
  std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
  size_t len = 0;
  EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, secret_.data(), secret_.size(),
            reinterpret_cast<const unsigned char*>(signing_input.data()), signing_input.size(),
            out.data(), out.size(), &len);
  return {reinterpret_cast<const char*>(out.data()), len};
}

std::string JwtSigner::Sign(const std::string& subject, int64_t now_s, int64_t ttl_s) const {
  static const std::string kHeader = Base64UrlEncode(R"({"alg":"HS256","typ":"JWT"})");
  const nlohmann::json claims{
      {"sub", subject}, {"iss", issuer_}, {"iat", now_s}, {"exp", now_s + ttl_s}};
  std::string input = kHeader + "." + Base64UrlEncode(claims.dump());
  const std::string sig = Base64UrlEncode(Mac(input));
  return input + "." + sig;
}

Result<JwtClaims> JwtSigner::Verify(std::string_view token, int64_t now_s) const {
  const size_t dot1 = token.find('.');
  const size_t dot2 = dot1 == std::string_view::npos ? dot1 : token.find('.', dot1 + 1);
  if (dot2 == std::string_view::npos || token.find('.', dot2 + 1) != std::string_view::npos) {
    return Status::Unauthenticated("malformed token");
  }
  const std::string_view signing_input = token.substr(0, dot2);
  auto sig = Base64UrlDecode(token.substr(dot2 + 1));
  if (!sig.ok()) return Status::Unauthenticated("malformed signature");
  const std::string expected = Mac(signing_input);
  if (sig->size() != expected.size() ||
      CRYPTO_memcmp(sig->data(), expected.data(), expected.size()) != 0) {
    return Status::Unauthenticated("bad signature");
  }
  // Only now look inside: nothing unauthenticated is parsed beyond base64.
  auto header = Base64UrlDecode(token.substr(0, dot1));
  auto payload = Base64UrlDecode(token.substr(dot1 + 1, dot2 - dot1 - 1));
  if (!header.ok() || !payload.ok()) return Status::Unauthenticated("malformed token");
  try {
    const auto h = nlohmann::json::parse(*header);
    if (h.value("alg", "") != "HS256") return Status::Unauthenticated("unsupported alg");
    const auto p = nlohmann::json::parse(*payload);
    JwtClaims c;
    c.subject = p.at("sub").get<std::string>();
    c.issuer = p.value("iss", "");
    c.issued_at = p.value("iat", int64_t{0});
    c.expires_at = p.at("exp").get<int64_t>();
    if (c.issuer != issuer_) return Status::Unauthenticated("wrong issuer");
    if (now_s > c.expires_at + leeway_s_) return Status::Unauthenticated("token expired");
    if (c.issued_at > now_s + leeway_s_) return Status::Unauthenticated("token from the future");
    if (c.subject.empty()) return Status::Unauthenticated("empty subject");
    return c;
  } catch (const nlohmann::json::exception&) {
    return Status::Unauthenticated("malformed claims");
  }
}

}  // namespace cartwheel::gateway
