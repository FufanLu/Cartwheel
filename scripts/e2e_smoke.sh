#!/usr/bin/env bash
# End-to-end smoke test against a running stack (compose or local binaries).
#   scripts/e2e_smoke.sh [base-url]     default http://localhost:8088
set -euo pipefail
BASE=${1:-http://localhost:8088}
USER_ID="smoke-$RANDOM$RANDOM"
fail() { echo "FAIL: $*" >&2; exit 1; }
jqget() { python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }

echo "-> health"; curl -fsS "$BASE/healthz" >/dev/null || fail "healthz"

echo "-> add with idempotency key, three times"
for _ in 1 2 3; do
  curl -fsS -X POST "$BASE/api/v1/carts/$USER_ID/items" -H 'Content-Type: application/json' \
    -H 'Idempotency-Key: smoke-1' -d '{"sku":"CW-1001","quantity":1,"unit_price_cents":3400,"title":"Kettle"}' >/dev/null
done
q=$(curl -fsS "$BASE/api/v1/carts/$USER_ID" | jqget "d['items'][0]['quantity']")
[ "$q" = "1" ] || fail "idempotent add applied $q times"

echo "-> 20 concurrent adds"
for i in $(seq 1 20); do
  curl -fsS -o /dev/null -X POST "$BASE/api/v1/carts/$USER_ID/items" -H 'Content-Type: application/json' \
    -d '{"sku":"CW-1002","quantity":1,"unit_price_cents":1400,"title":"Mug"}' &
done
wait
read -r mugs siblings < <(curl -fsS "$BASE/api/v1/carts/$USER_ID" | \
  jqget "' '.join([str(sum(i['quantity'] for i in d['items'] if i['sku']=='CW-1002')), str(d['merged_siblings'])])")
[ "$mugs" = "20" ] || fail "expected 20 mugs after concurrent adds, got $mugs"
echo "   20/20 adds kept; siblings at read: $siblings"

echo "-> remove does not resurrect"
curl -fsS -X DELETE "$BASE/api/v1/carts/$USER_ID/items/CW-1002" >/dev/null
left=$(curl -fsS "$BASE/api/v1/carts/$USER_ID" | jqget "sum(i['quantity'] for i in d['items'] if i['sku']=='CW-1002')")
[ "$left" = "0" ] || fail "removed item came back: $left"

echo "-> error mapping"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/v1/carts/$USER_ID/items" -d '{bad')
[ "$code" = "400" ] || fail "bad JSON returned $code"

echo "PASS ($USER_ID)"
