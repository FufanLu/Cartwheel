# Runbooks

One page per alert in `deploy/observability/alerts.yml`. Each page says what the alert means for a
customer, what to look at first, and the actions that are safe to take. Commands assume the local
stack (`scripts/local_cluster.sh`, gateway on :8090); in Kubernetes, use `kubectl exec` into a
wheel-node pod and the service DNS names instead.

| alert | page |
|---|---|
| CartwheelNodeDown, CartwheelMemberNotAlive | [node-down](node-down.md) |
| CartwheelHintBacklog | [hint-backlog](hint-backlog.md) |
| CartwheelQuorumFailures | [quorum-failures](quorum-failures.md) |
| CartwheelGatewayP99High, CartwheelGatewayShedding | [latency](latency.md) |
| CartwheelDealRedriveStuck, CartwheelOrderCommitFailing | [deal-pipeline](deal-pipeline.md) |
