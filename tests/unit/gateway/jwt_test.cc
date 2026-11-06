#include "gateway/jwt.h"

#include <gtest/gtest.h>

#include "gateway/auth.h"
#include "gateway/keyed_limiter.h"

namespace cartwheel::gateway {
namespace {

constexpr int64_t kNow = 1'800'000'000;  // unix seconds

TEST(Base64Url, RoundTripsEveryLengthWithoutPadding) {
  std::string data;
  for (int i = 0; i < 64; ++i) {
    data.push_back(static_cast<char>(i * 37 + 11));
    const std::string enc = Base64UrlEncode(data);
    EXPECT_EQ(enc.find_first_of("+/="), std::string::npos);
    auto dec = Base64UrlDecode(enc);
    ASSERT_TRUE(dec.ok());
    EXPECT_EQ(*dec, data);
  }
  EXPECT_FALSE(Base64UrlDecode("a").ok());
  EXPECT_FALSE(Base64UrlDecode("ab+c").ok());
}

TEST(Jwt, SignAndVerify) {
  JwtSigner signer("secret");
  const std::string token = signer.Sign("ada", kNow, 3600);
  auto claims = signer.Verify(token, kNow + 10);
  ASSERT_TRUE(claims.ok()) << claims.status();
  EXPECT_EQ(claims->subject, "ada");
  EXPECT_EQ(claims->issuer, "cartwheel");
  EXPECT_EQ(claims->expires_at, kNow + 3600);
}

TEST(Jwt, RejectsExpiredTokensAfterLeeway) {
  JwtSigner signer("secret", "cartwheel", /*leeway_s=*/30);
  const std::string token = signer.Sign("ada", kNow, 60);
  EXPECT_TRUE(signer.Verify(token, kNow + 60 + 30).ok());
  auto late = signer.Verify(token, kNow + 60 + 31);
  EXPECT_EQ(late.status().code(), StatusCode::kUnauthenticated);
}

TEST(Jwt, RejectsAnotherKeyAndTampering) {
  JwtSigner signer("secret");
  JwtSigner other("other-secret");
  const std::string token = signer.Sign("ada", kNow, 3600);
  EXPECT_FALSE(other.Verify(token, kNow).ok());

  // Swap the payload for one claiming to be someone else, keep the signature.
  const std::string forged_payload =
      Base64UrlEncode(R"({"sub":"admin","iss":"cartwheel","iat":0,"exp":9999999999})");
  const size_t dot1 = token.find('.');
  const size_t dot2 = token.find('.', dot1 + 1);
  const std::string forged = token.substr(0, dot1 + 1) + forged_payload + token.substr(dot2);
  EXPECT_FALSE(signer.Verify(forged, kNow).ok());
}

TEST(Jwt, RejectsAlgNone) {
  JwtSigner signer("secret");
  const std::string header = Base64UrlEncode(R"({"alg":"none","typ":"JWT"})");
  const std::string payload =
      Base64UrlEncode(R"({"sub":"ada","iss":"cartwheel","iat":0,"exp":9999999999})");
  EXPECT_FALSE(signer.Verify(header + "." + payload + ".", kNow).ok());
  EXPECT_FALSE(signer.Verify(header + "." + payload, kNow).ok());
}

TEST(Jwt, RejectsWrongIssuerAndMalformedTokens) {
  JwtSigner a("secret", "issuer-a");
  JwtSigner b("secret", "issuer-b");
  EXPECT_FALSE(b.Verify(a.Sign("ada", kNow, 60), kNow).ok());
  for (const char* bad : {"", ".", "..", "a.b", "a.b.c.d", "!!!.###.$$$"}) {
    EXPECT_FALSE(a.Verify(bad, kNow).ok()) << bad;
  }
}

TEST(Auth, UserIdsAreSmallAndUrlSafe) {
  EXPECT_TRUE(ValidUserId("ada"));
  EXPECT_TRUE(ValidUserId("user_42-x"));
  EXPECT_FALSE(ValidUserId(""));
  EXPECT_FALSE(ValidUserId("Ada"));
  EXPECT_FALSE(ValidUserId("a:b"));
  EXPECT_FALSE(ValidUserId(std::string(33, 'a')));
}

TEST(Auth, CartAccessFollowsTheToken) {
  ManualClock clock(kNow * 1'000'000'000);
  Authenticator auth(JwtSigner("secret"), 3600, &clock);
  int64_t exp = 0;
  const std::string token = auth.Issue("ada", &exp);
  EXPECT_EQ(exp, kNow + 3600);

  Request anon{http::verb::get, "/api/v1/carts/ada", 11};
  EXPECT_TRUE(auth.AuthorizeUser(anon, "ada", /*allow_anonymous=*/true).ok());
  EXPECT_EQ(auth.AuthorizeUser(anon, "ada", false).code(), StatusCode::kUnauthenticated);

  Request mine = anon;
  mine.set(http::field::authorization, "Bearer " + token);
  EXPECT_TRUE(auth.AuthorizeUser(mine, "ada", false).ok());
  EXPECT_EQ(auth.AuthorizeUser(mine, "grace", true).code(), StatusCode::kPermissionDenied);

  Request garbage = anon;
  garbage.set(http::field::authorization, "Bearer nope");
  EXPECT_EQ(auth.AuthorizeUser(garbage, "ada", true).code(), StatusCode::kUnauthenticated);
}

TEST(KeyedLimiter, EachKeyGetsItsOwnBucket) {
  ManualClock clock(1'000'000'000);
  KeyedRateLimiter limiter({.rate_per_sec = 1, .burst = 3}, &clock);
  for (int i = 0; i < 3; ++i) EXPECT_EQ(limiter.Admit("ada"), 0) << i;
  const int64_t wait = limiter.Admit("ada");
  EXPECT_GT(wait, 0);
  EXPECT_LE(wait, 1'000'000'000);
  // Another user is unaffected.
  EXPECT_EQ(limiter.Admit("grace"), 0);
  // After the advertised wait, ada gets one more.
  clock.Advance(wait);
  EXPECT_EQ(limiter.Admit("ada"), 0);
  EXPECT_GT(limiter.Admit("ada"), 0);
}

TEST(KeyedLimiter, MemoryStaysBoundedUnderAKeyFlood) {
  ManualClock clock(1'000'000'000);
  KeyedRateLimiter limiter({.rate_per_sec = 1, .burst = 1, .shards = 4, .max_keys_per_shard = 100},
                           &clock);
  for (int i = 0; i < 10'000; ++i) {
    (void)limiter.Admit("bot-" + std::to_string(i));
    clock.Advance(1'000);
  }
  EXPECT_LE(limiter.tracked_keys(), 4U * 100U);
}

}  // namespace
}  // namespace cartwheel::gateway
