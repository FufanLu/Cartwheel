// A lightning-deal spike at the HTTP edge: new buyers arrive at up to 1,500
// per second, each logs in once and claims once.
//
//   k6 run -e BASE=http://127.0.0.1:8090 -e ADMIN=dev-admin -e QUANTITY=2000 deploy/k6/deal-spike.js
//
// setup() starts a fresh generation of the deal. The checks encode the
// contract: every claim gets a definite business answer (202 reserved, 409
// sold out) or a retryable 429/503, never an error page; teardown() reads the
// deal back and fails the run if more units were taken than offered.
import http from 'k6/http';
import { check, fail } from 'k6';
import { Counter } from 'k6/metrics';

const BASE = __ENV.BASE || 'http://127.0.0.1:8090';
const ADMIN = __ENV.ADMIN || 'dev-admin';
const DEAL = __ENV.DEAL || 'dl-kettle';
const QUANTITY = Number(__ENV.QUANTITY || 2000);
const reserved = new Counter('deal_reserved');
const soldOut = new Counter('deal_sold_out');
const throttled = new Counter('deal_retryable');

export const options = {
  scenarios: {
    rush: {
      executor: 'ramping-arrival-rate',
      startRate: 50,
      timeUnit: '1s',
      preAllocatedVUs: 400,
      maxVUs: 1500,
      stages: [
        { target: 1500, duration: '5s' },
        { target: 1500, duration: '20s' },
        { target: 0, duration: '5s' },
      ],
    },
  },
  thresholds: {
    'checks{check:definite answer}': ['rate>0.999'],
    'http_req_duration{name:claim}': ['p(99)<300'],
  },
  setupTimeout: '30s',
};

export function setup() {
  const r = http.post(`${BASE}/api/v1/admin/deals/${DEAL}/reset`,
    JSON.stringify({ quantity: QUANTITY, starts_in_ms: 0, duration_ms: 600000 }),
    { headers: { 'X-Admin-Token': ADMIN, 'Content-Type': 'application/json' } });
  if (r.status !== 200) fail(`reset failed: ${r.status} ${r.body}`);
  return { generation: r.json('generation') };
}

export default function () {
  const user = `k6-${__VU}-${__ITER}`;
  const login = http.post(`${BASE}/api/v1/login`, JSON.stringify({ user }), { tags: { name: 'login' } });
  if (login.status !== 200) return;
  const token = login.json('token');
  const r = http.post(`${BASE}/api/v1/deals/${DEAL}/claim`, null,
    { headers: { Authorization: `Bearer ${token}` }, tags: { name: 'claim' } });
  check(r, { 'definite answer': (x) => [202, 200, 409, 429, 503].includes(x.status) });
  if (r.status === 202) reserved.add(1);
  else if (r.status === 409) soldOut.add(1);
  else if (r.status === 429 || r.status === 503) throttled.add(1);
}

export function teardown() {
  const d = http.get(`${BASE}/api/v1/deals`).json('deals').find((x) => x.deal_id === DEAL);
  const taken = d.quantity - d.remaining;
  console.log(`deal ${DEAL}: quantity ${d.quantity}, taken at the gate ${taken}, committed ${d.sold}`);
  if (taken > d.quantity || d.sold > d.quantity) fail('oversold');
}
