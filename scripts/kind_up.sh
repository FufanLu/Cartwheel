#!/usr/bin/env bash
# Cartwheel on a local Kubernetes cluster (kind) with the Helm chart.
#
#   scripts/kind_up.sh          build images, create the cluster, install
#   scripts/kind_up.sh down     delete the cluster
#
# Needs docker, kind, helm and kubectl. The first image build compiles every
# vcpkg dependency for Linux inside Docker (long; give Docker Desktop >= 8 GB
# of memory). Later builds reuse the builder image.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CLUSTER=${CARTWHEEL_KIND_CLUSTER:-cartwheel}

if [ "${1:-}" = "down" ]; then
  kind delete cluster --name "$CLUSTER"
  exit 0
fi

cd "$ROOT"
docker image inspect cartwheel-builder:dev >/dev/null 2>&1 ||
  docker build -f deploy/docker/builder.Dockerfile -t cartwheel-builder:dev .
docker build -f deploy/docker/services.Dockerfile -t cartwheel:dev .
docker build -f deploy/docker/web.Dockerfile -t cartwheel-web:dev .

if ! kind get clusters | grep -qx "$CLUSTER"; then
  cat <<EOF | kind create cluster --name "$CLUSTER" --config=-
kind: Cluster
apiVersion: kind.x-k8s.io/v1alpha4
nodes:
  - role: control-plane
    extraPortMappings:
      - { containerPort: 30080, hostPort: 8088 }   # web (NodePort)
EOF
fi
kind load docker-image cartwheel:dev cartwheel-web:dev --name "$CLUSTER"

helm upgrade --install cartwheel deploy/helm/cartwheel \
  --set kv.faultInjection=true --set gateway.adminToken=dev-admin --wait --timeout 10m
kubectl rollout status statefulset/wheel-node --timeout=5m
kubectl exec wheel-node-0 -- wheelctl -t 127.0.0.1:7100 ring
echo "storefront: http://localhost:8088   ops console: http://localhost:8088/ops"
