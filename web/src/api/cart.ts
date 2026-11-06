// Typed client for the gateway's cart API. Every call reports what the
// backend actually did (request id, latency, siblings merged) so the UI can
// show the mechanism, not just the result.

export interface CartItem {
  sku: string;
  quantity: number;
  unit_price_cents: number;
  title: string;
}

export interface Cart {
  user_id: string;
  items: CartItem[];
  total_cents: number;
  /** Concurrent versions folded into this view; > 1 means a merge happened. */
  merged_siblings: number;
}

export interface CallTrace {
  method: string;
  path: string;
  status: number;
  requestId: string;
  latencyMs: number;
  idempotencyKey?: string;
  at: number;
}

export class ApiError extends Error {
  constructor(
    readonly status: number,
    readonly code: string,
    message: string,
    readonly retryAfterS: number | null,
    readonly trace: CallTrace,
  ) {
    super(message);
  }
}

async function call(
  method: string,
  path: string,
  body?: unknown,
  idempotencyKey?: string,
): Promise<{ cart: Cart; trace: CallTrace }> {
  const headers: Record<string, string> = { Accept: 'application/json' };
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  if (idempotencyKey) headers['Idempotency-Key'] = idempotencyKey;
  const started = performance.now();
  let res: Response;
  try {
    res = await fetch(path, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
    });
  } catch {
    const trace = { method, path, status: 0, requestId: '-', latencyMs: 0, idempotencyKey, at: Date.now() };
    throw new ApiError(0, 'Unavailable', 'The gateway did not answer.', 1, trace);
  }
  const trace: CallTrace = {
    method,
    path,
    status: res.status,
    requestId: res.headers.get('X-Request-Id') ?? '-',
    latencyMs: Math.round((performance.now() - started) * 10) / 10,
    idempotencyKey,
    at: Date.now(),
  };
  const json = await res.json().catch(() => null);
  if (!res.ok) {
    const retry = res.headers.get('Retry-After');
    throw new ApiError(
      res.status,
      json?.error?.code ?? 'Unknown',
      json?.error?.message ?? res.statusText,
      retry ? Number(retry) : null,
      trace,
    );
  }
  return { cart: json as Cart, trace };
}

const enc = encodeURIComponent;

export const cartApi = {
  get: (user: string) => call('GET', `/api/v1/carts/${enc(user)}`),
  add: (user: string, item: CartItem, idempotencyKey: string) =>
    call('POST', `/api/v1/carts/${enc(user)}/items`, item, idempotencyKey),
  remove: (user: string, sku: string) =>
    call('DELETE', `/api/v1/carts/${enc(user)}/items/${enc(sku)}`),
  clear: (user: string) => call('DELETE', `/api/v1/carts/${enc(user)}`),
};

export function newIdempotencyKey(): string {
  return crypto.randomUUID();
}

export function formatCents(cents: number): string {
  return `$${(cents / 100).toFixed(2)}`;
}
