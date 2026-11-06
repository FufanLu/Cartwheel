// Client for the lightning-deal and ops endpoints. Like cart.ts, every call
// reports what the backend did (status, request id, latency), so the sheet can
// show the mechanism.

export interface Deal {
  deal_id: string;
  sku: string;
  title: string;
  price_cents: number;
  list_price_cents: number;
  quantity: number;
  /** Units the Redis gate can still reserve (live). */
  remaining: number;
  /** Orders committed in Postgres (cached up to 1 s). */
  sold: number;
  starts_at_ms: number;
  ends_at_ms: number;
  generation: number;
}

export type OrderStatus = 'PENDING' | 'CONFIRMED' | 'REJECTED';

export interface Order {
  order_id: string;
  deal_id: string;
  user_id: string;
  price_cents: number;
  status: OrderStatus;
  reserved_at_ms: number;
  generation: number;
  reason: string;
}

export type ClaimOutcome = 'RESERVED' | 'ALREADY_CLAIMED' | 'SOLD_OUT' | 'NOT_STARTED' | 'ENDED';

export interface ClaimResult {
  outcome: ClaimOutcome;
  order_id?: string;
  remaining: number;
}

export class HttpError extends Error {
  constructor(
    readonly status: number,
    readonly code: string,
    message: string,
    readonly retryAfterS: number | null,
    readonly requestId: string,
  ) {
    super(message);
  }
}

export interface Answer<T> {
  body: T;
  status: number;
  requestId: string;
  latencyMs: number;
}

async function request<T>(
  method: string,
  path: string,
  opts: { token?: string; admin?: string; body?: unknown; okStatuses?: number[] } = {},
): Promise<Answer<T>> {
  const headers: Record<string, string> = { Accept: 'application/json' };
  if (opts.body !== undefined) headers['Content-Type'] = 'application/json';
  if (opts.token) headers.Authorization = `Bearer ${opts.token}`;
  if (opts.admin) headers['X-Admin-Token'] = opts.admin;
  const started = performance.now();
  let res: Response;
  try {
    res = await fetch(path, {
      method,
      headers,
      body: opts.body === undefined ? undefined : JSON.stringify(opts.body),
    });
  } catch {
    throw new HttpError(0, 'Unreachable', 'The gateway did not answer.', null, '-');
  }
  const latencyMs = performance.now() - started;
  const requestId = res.headers.get('X-Request-Id') ?? '-';
  const text = await res.text();
  let json: unknown = null;
  try {
    json = text ? JSON.parse(text) : null;
  } catch {
    /* non-JSON error page */
  }
  const ok = res.ok || (opts.okStatuses ?? []).includes(res.status);
  if (!ok) {
    const err = (json as { error?: { code?: string; message?: string } } | null)?.error;
    const ra = res.headers.get('Retry-After');
    throw new HttpError(
      res.status,
      err?.code ?? `HTTP ${res.status}`,
      err?.message ?? (text || res.statusText),
      ra ? Number(ra) : null,
      requestId,
    );
  }
  return { body: json as T, status: res.status, requestId, latencyMs };
}

export const dealsApi = {
  login: (user: string) =>
    request<{ token: string; user: string; expires_at: number }>('POST', '/api/v1/login', {
      body: { user },
    }),
  list: () => request<{ deals: Deal[]; server_time_ms: number }>('GET', '/api/v1/deals'),
  // 409 carries a business outcome (sold out, not started, ended), not a fault.
  claim: (dealId: string, token: string) =>
    request<ClaimResult>('POST', `/api/v1/deals/${encodeURIComponent(dealId)}/claim`, {
      token,
      okStatuses: [409],
    }),
  orders: (token: string) => request<{ orders: Order[] }>('GET', '/api/v1/orders', { token }),
  reset: (dealId: string, admin: string, quantity: number, startsInMs: number) =>
    request<Deal>('POST', `/api/v1/admin/deals/${encodeURIComponent(dealId)}/reset`, {
      admin,
      body: { quantity, starts_in_ms: startsInMs, duration_ms: 10 * 60_000 },
    }),
};

// ---- ops ---------------------------------------------------------------

export interface OpsNode {
  id: string;
  address: string;
  reachable: boolean;
  error?: string;
  state?: 'ALIVE' | 'SUSPECT' | 'DEAD' | 'LEFT';
  ownership?: number;
  incarnation?: number;
  heartbeat?: number;
  counters?: {
    puts: number;
    gets: number;
    forwarded: number;
    quorum_failures: number;
    throttled: number;
    read_repairs: number;
    hints_stored: number;
    hints_delivered: number;
    hints_pending: number;
    anti_entropy_rounds: number;
    anti_entropy_keys_repaired: number;
  };
  stats?: {
    keys: number;
    bytes: number;
    gets_total: number;
    puts_total: number;
    window_get_p99_us: number;
    window_put_p99_us: number;
    window_ms: number;
    isolated_ms: number;
  };
}

export interface ClusterView {
  sampled_at_ms: number;
  observer: string;
  n?: number;
  r?: number;
  w?: number;
  tokens?: [number, string][];
  nodes: OpsNode[];
}

export const opsApi = {
  cluster: () => request<ClusterView>('GET', '/api/v1/ops/cluster'),
  isolate: (node: string, admin: string, durationMs: number) =>
    request<{ node_id: string; isolated_ms: number }>(
      'POST',
      `/api/v1/admin/nodes/${encodeURIComponent(node)}/isolate`,
      { admin, body: { duration_ms: durationMs } },
    ),
};
