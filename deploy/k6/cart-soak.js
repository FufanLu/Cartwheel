// Cart soak with a node partition in the middle.
//
//   k6 run -e BASE=http://127.0.0.1:8090 -e ADMIN=dev-admin deploy/k6/cart-soak.js
//   (-e NODE=none runs the same load without the partition, as a control)
//
// 50 shoppers read, add and remove items on their own carts for 90 s. At
// t=30 s a separate scenario isolates wheel-3 from its peers for 15 s (the
// ops console's fault injection). The thresholds encode the availability
// claim: carts stay writable through the partition.
import http from 'k6/http';
import { check, sleep } from 'k6';
import { Counter } from 'k6/metrics';

const BASE = __ENV.BASE || 'http://127.0.0.1:8090';
const ADMIN = __ENV.ADMIN || 'dev-admin';
const NODE = __ENV.NODE || 'wheel-3';
const writes = new Counter('cart_writes');

export const options = {
  scenarios: {
    shoppers: { executor: 'constant-vus', vus: 50, duration: '90s', exec: 'shop' },
    partition: { executor: 'shared-iterations', vus: 1, iterations: 1, startTime: '30s', exec: 'partition' },
  },
  thresholds: {
    'http_req_failed{scenario:shoppers}': ['rate<0.01'],
    'http_req_duration{scenario:shoppers}': ['p(99)<500'],
    checks: ['rate>0.99'],
  },
};

const SKUS = [
  ['CW-1001', 3400, 'Stovetop kettle'],
  ['CW-1002', 1400, 'Stoneware mug'],
  ['CW-1003', 950, 'Grid notebook'],
  ['CW-1006', 1900, 'Canvas tote'],
];

export function shop() {
  const user = `k6-soak-${__VU}`;
  const [sku, price, title] = SKUS[Math.floor(Math.random() * SKUS.length)];
  const headers = { 'Content-Type': 'application/json', 'Idempotency-Key': `${__VU}-${__ITER}` };
  const add = http.post(`${BASE}/api/v1/carts/${user}/items`,
    JSON.stringify({ sku, quantity: 1, unit_price_cents: price, title }), { headers, tags: { op: 'add' } });
  check(add, { 'add 200': (r) => r.status === 200 });
  writes.add(1);
  const get = http.get(`${BASE}/api/v1/carts/${user}`, { tags: { op: 'get' } });
  check(get, { 'get 200': (r) => r.status === 200 });
  if (Math.random() < 0.3) {
    const del = http.del(`${BASE}/api/v1/carts/${user}/items/${sku}`, null, { tags: { op: 'remove' } });
    check(del, { 'remove 200': (r) => r.status === 200 });
  }
  sleep(0.05);
}

export function partition() {
  if (NODE === 'none') return; // control run: no fault
  const r = http.post(`${BASE}/api/v1/admin/nodes/${NODE}/isolate`, JSON.stringify({ duration_ms: 15000 }),
    { headers: { 'X-Admin-Token': ADMIN, 'Content-Type': 'application/json' } });
  check(r, { 'isolate accepted': (x) => x.status === 200 });
}
