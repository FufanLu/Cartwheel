[English](README.md) | **中文**

# Cartwheel

**一个用 C++20 编写的 Dynamo 风格分布式 KV 存储，以及一套建在它上面的高并发电商系统。**

2007 年的 Dynamo 论文描述过这样一个购物车：即使服务器故障、网络分区，也必须能写入。Cartwheel 把这个系统完整做了出来。**CartwheelKV** 是一个无主（leaderless）的 KV 存储，用 quorum 做复制，存储引擎是自研的 LSM。在它之上有购物车服务、绝不超卖的秒杀（Lightning Deal）链路、C++ 边缘网关，以及 React 写的商城前端和运维控制台。

购物车和秒杀库存分别站在 CAP 取舍的两端，这两种一致性模型在同一个系统里同时存在：

| 数据 | 要求 | 实现方式 |
|---|---|---|
| 购物车 | 永远可写，分区期间也一样（AP） | CartwheelKV：N/R/W quorum、sloppy quorum + hinted handoff、vector clock、OR-Set CRDT 合并 |
| 秒杀库存 | 绝不超卖（CP） | Redis Lua 原子闸门 → Redpanda（Kafka）削峰 → 幂等写入 Postgres |

![商城页面：两个并发写入合并后的购物车](docs/images/storefront-desktop.webp)

*另一个窗口改过购物车之后的商城页面：带括号标记的那一行来自另一个窗口，没有任何写入被覆盖。截图来自真实运行的服务。*

## 架构

```mermaid
flowchart LR
  web[React 商城 + 运维控制台] --> nginx[Nginx]
  nginx --> gw["gateway<br/>Beast + Asio 协程<br/>JWT · GCRA 限流 · 降载"]
  gw -->|gRPC| cart[cart-svc]
  gw -->|gRPC| deal[deal-svc]
  cart -->|CartwheelKV SDK| kv
  deal --> pg[(Postgres)]
  deal --> redis[(Redis Lua 闸门)]
  deal --> rp[[Redpanda]]
  rp --> worker[order-worker] --> pg
  subgraph kv[CartwheelKV 集群]
    n1((wheel-node)) --- n2((wheel-node)) --- n3((wheel-node)) --- n4((wheel-node)) --- n5((wheel-node)) --- n1
  end
```

## 进度

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 | 工具链、构建系统、过载保护原语（GCRA 限流、jitter 退避、直方图、有界线程池）、CI | ✅ |
| M1 | 端到端骨架：单节点（DVV siblings）、cart-svc（OR-Set CRDT）、Beast + 协程网关、商城前端、docker-compose | ✅ |
| M2 | LSM 存储引擎（WAL、skiplist memtable、SSTable、布隆过滤器、compaction） | |
| M3 | 分布式：一致性哈希、gossip、quorum、vector clock、hinted handoff、Merkle 反熵、确定性仿真 | |
| M4 | 秒杀链路：Redis 闸门、Redpanda、order-worker、Postgres；网关鉴权与限流 | |
| M5 | 商城前端与运维控制台（哈希环、实时 QPS/p99、故障注入） | |
| M6 | 可观测性（Prometheus、Grafana、OpenTelemetry）、k6 压测、混沌实验 | |
| M7 | Helm/kind 部署、PRFAQ、runbook | |

## 构建

依赖：CMake ≥ 3.25、Ninja、支持 C++20 的编译器（Apple Clang 17 / Clang 18 / GCC 13），以及 [vcpkg](https://github.com/microsoft/vcpkg)（克隆到 `~/vcpkg`，或者用 `$VCPKG_ROOT` 指定位置）。

```bash
cmake --preset dev          # 第一次运行会通过 vcpkg 编译依赖，gRPC 比较慢
cmake --build --preset dev
ctest --preset dev

cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan   # ThreadSanitizer
```

Preset：`dev`（Debug）、`release`（RelWithDebInfo）、`asan`（Address + UB）、`tsan`（Thread）。

## 运行

```bash
# 本地二进制（先按上面的步骤构建）
build/dev/src/kv/wheel-node &
CARTWHEEL_KV_TARGET=127.0.0.1:7100 build/dev/src/services/cart/cart-svc &
CARTWHEEL_CART_TARGET=127.0.0.1:7200 build/dev/src/gateway/gateway &
(cd web && pnpm install && pnpm dev)          # http://localhost:5173
scripts/e2e_smoke.sh http://127.0.0.1:8080    # 验证幂等、20 个并发加购、删除的商品不会复活

# 或者用容器跑整套服务
docker compose -f deploy/compose/docker-compose.yml up -d --build   # http://localhost:8088
```

在商城页面里点 **Race two writers**，会同时发出两个加购请求。存储里会留下两个 DVV sibling，购物车显示 "Merged 2 concurrent versions"，两件商品都在。

## 文档

- [图解](docs/illustrated/README.md)：每次跟着一个请求走完整个系统，所有数据都来自真实运行（英文）。
- [架构决策记录 ADR](docs/adr/)（英文）
