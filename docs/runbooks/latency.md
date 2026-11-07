# Edge latency is high, or the gateway is shedding

**What customers feel:** slow pages, or 503 with `Retry-After` when the gateway sheds.

**Look first**

1. Grafana *p99 latency by route*: one route or all of them?
2. A trace of a slow request. Every response carries `X-Trace-Id`; open it in Jaeger and find
   the span that holds the time (illustrated/05 §1). Typical culprits are a KV quorum waiting
   on a slow replica, Kafka acks on claims, or queueing in a service's worker pool.
3. `cartwheel_http_inflight_requests` against `server.max_inflight`. Shedding means the gateway
   is protecting itself; the cause is downstream or the load is real.
4. `cartwheel_gateway_rate_limited_total`: 429s are per-user limits doing their job, not an
   outage.

**Actions**

- Real load: scale gateways and services horizontally; they are stateless.
- One slow node: see [node-down](node-down.md). Isolating it from traffic is better than letting
  it drag every quorum.
- Do not raise `max_inflight` to stop the 503s. That turns fast rejections into slow timeouts
  for everyone (illustrated/01 §5).
