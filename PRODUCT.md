# Product

<!-- impeccable:product-schema 1 -->

> Inferred from the original brief and the repository (2026-11-06). The user asked to proceed without an
> interview, so facts marked *(inferred)* have not been confirmed.

## Platform

web

## Stack

React + TypeScript + Vite (chosen by the user). Styling and libraries are delegated. The plan names
Tailwind, TanStack Query and ECharts for later milestones.

## Users

- **Interviewers and engineers evaluating the portfolio** *(inferred)*. They have a few minutes, usually on a
  laptop and sometimes on a phone, and are judging whether the author understands high-concurrency
  distributed systems. They want to *see* the mechanisms work: concurrent cart edits merging, a flash sale
  that never oversells, a node dying while traffic keeps flowing.
- **The author, demoing live** *(inferred)*. They drive the storefront and the ops console side by side while
  explaining the design, including injecting faults during a load test.

## Product Purpose

Cartwheel is a Dynamo-style distributed key-value store written in C++20 (CartwheelKV), with a commerce stack
on top: a shopping cart that is always writable, and a lightning-deal flash sale that never oversells. The web
frontend makes those backend guarantees visible and demonstrable. Success means a visitor can watch the
guarantees hold under real concurrency and failures, not just read claims about them.

## Positioning

Most portfolio shops are CRUD demos. Cartwheel's frontend is a window onto real distributed-systems
mechanisms: DVV siblings that were merged, quorum acknowledgements, hinted handoff, and per-node p99. Every
number it shows comes from the running system.

## Operating Context

- The storefront is a demo shop: about 6 demo products, a cart, and (from M4) lightning deals with countdowns.
- The ops console (M5) shows the hash ring with vnodes, node health, live QPS and p99 per node over WebSocket,
  the hint queue, anti-entropy progress, fault injection (kill or partition a node), a data explorer that shows
  siblings and vector clocks, and per-table throughput limits.
- Both surfaces are served by Nginx in docker-compose. In development, Vite proxies `/api` to the C++ gateway.

## Capabilities and Constraints

- Cart API (M1): `GET /api/v1/carts/{user}`, `POST /api/v1/carts/{user}/items` (with an `Idempotency-Key`
  header), `DELETE /api/v1/carts/{user}/items/{sku}`, and `DELETE /api/v1/carts/{user}`. Responses include
  `merged_siblings`; a value above 1 means concurrent versions were merged by the OR-Set CRDT.
- Errors return `{error: {code, message}, request_id}`. A 429/503 response carries `Retry-After`.
- There are no real payments and no real accounts. The user identity is a demo user id. *(inferred)*
- The product catalog is demo data until deal-svc arrives in M4.

## Brand Commitments

- The name is **Cartwheel**: Cart, from the Dynamo paper's shopping cart, plus Wheel, from the consistent-hashing ring.
- **Must not imitate Amazon** in name, logo, colors, typography, layout signatures, or "Prime" terminology. The
  project targets Amazon interviews, but it is an independent work.

## Evidence on Hand

- Real benchmark and trace data in `docs/illustrated/00-overload-toolbox.md`.
- No customers, testimonials, or production deployments exist. Never fabricate them.

## Product Principles

1. **Show the mechanism.** When the backend does something interesting (merging siblings, shedding load,
   retrying with jitter), the UI makes it visible and honest.
2. **Real numbers only.** Every metric, count, or latency shown comes from the live system or from recorded
   runs, never from placeholders that look real.
3. **Always responsive under failure.** The UI degrades the same way the backend does. It shows retry and
   overload states plainly and never freezes.
4. **One system, two surfaces.** The storefront and the ops console share one visual language, so a visitor
   reads them as the same product.

## Accessibility & Inclusion

Target WCAG 2.2 AA *(inferred)*: keyboard operable, visible focus, sufficient contrast, and reduced-motion
support for countdowns and live charts.
