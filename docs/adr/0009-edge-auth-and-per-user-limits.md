# ADR 0009: Stateless tokens and per-user rate limits at the gateway

- Status: accepted
- Date: 2026-11-06

## Context

The deal endpoints need to know *who* is calling: one unit per customer is enforced per user id,
and bots pressing the button must be slowed before they reach deal-svc. The gateway is a
stateless, horizontally scaled C++ process (ADR 0002). Browsers also open a WebSocket for order
pushes, and a browser cannot attach an `Authorization` header to a WebSocket.

## Decision

- **HS256 JWTs** issued by `POST /api/v1/login`, verified in the gateway on every request.
  No session store is needed, and any gateway can verify any token. The verifier checks the HMAC
  in constant time before parsing, accepts only `alg` HS256 (so `none` and algorithm confusion
  are rejected), and requires `exp` and the issuer, with 30 s of leeway for clock skew. The demo
  login has no password; only this route would change for a real identity provider.
- **The WebSocket token** travels in `Sec-WebSocket-Protocol: cartwheel.v1, <jwt>`. The server
  answers with `cartwheel.v1`, or with 401 before the upgrade.
- **Cart routes** stay usable anonymously for the M1 storefront (`auth.anonymous_carts`). A
  request that does carry a token may only touch that user's cart (403 otherwise).
- **A per-user GCRA limit** on claim (1/s, burst 3) and login (2/s, burst 5). The state is one
  64-bit theoretical arrival time per key in a 64-way sharded map, bounded by evicting refilled
  buckets first. Rejections are 429 with an exact `Retry-After`.
- **The admin reset endpoint** requires `X-Admin-Token`, and it is disabled when no token is
  configured.

## Consequences

- Verification costs one HMAC and no network call. Revocation before `exp` is not possible;
  tokens are short-lived (1 h) instead.
- The limits are per gateway instance, so N instances allow N× the rate. A global limit would
  move the same GCRA into Redis, with one script call per request.
- HS256 means every gateway holds the signing secret. With separate issuers and verifiers, RS256
  or EdDSA would let gateways hold only a public key.
