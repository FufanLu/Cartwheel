// wheelctl: operator CLI for CartwheelKV.
//
//   wheelctl --target localhost:7100 put carts alice '{"v":1}'
//   wheelctl get carts alice            # prints every sibling and the context clock
//   wheelctl stats | health

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <string>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <grpcpp/grpcpp.h>

#include "cartwheel/kv/v1/kv.grpc.pb.h"
#include "cluster/vector_clock.h"
#include "kv/grpc_status.h"

namespace {

using cartwheel::cluster::VectorClock;
namespace v1 = cartwheel::kv::v1;

std::string ClockString(const std::string& encoded) {
  auto c = VectorClock::Decode(encoded);
  return c.ok() ? c->ToString() : "<malformed>";
}

std::string HexOf(const std::string& bytes) {
  std::string out;
  for (const unsigned char c : bytes) out += fmt::format("{:02x}", c);
  return out;
}

// Text values print as-is; binary values (cart-svc stores protobuf) as a hex dump.
void PrintValue(const std::string& value) {
  const bool printable = std::all_of(value.begin(), value.end(), [](char c) {
    return std::isprint(static_cast<unsigned char>(c)) || c == '\n' || c == '\t';
  });
  if (printable) {
    fmt::print("  {}\n", value);
    return;
  }
  for (size_t off = 0; off < value.size(); off += 16) {
    std::string hex;
    std::string ascii;
    for (size_t i = off; i < std::min(off + 16, value.size()); ++i) {
      const auto c = static_cast<unsigned char>(value[i]);
      hex += fmt::format("{:02x} ", c);
      ascii += std::isprint(c) ? static_cast<char>(c) : '.';
    }
    fmt::print("  {:04x}  {:<48} |{}|\n", off, hex, ascii);
  }
}

template <typename Resp>
void PrintVersions(const Resp& resp) {
  if (resp.versions_size() == 0) fmt::print("(not found)\n");
  for (int i = 0; i < resp.versions_size(); ++i) {
    const auto& v = resp.versions(i);
    fmt::print("sibling {}  clock={}  written_at_ms={}  ({} bytes)\n", i + 1,
               ClockString(v.clock()), v.written_at_ms(), v.value().size());
    PrintValue(v.value());
  }
  fmt::print("context  {}  (hex {})\n", ClockString(resp.context()), HexOf(resp.context()));
}

grpc::ClientContext* WithDeadline(grpc::ClientContext* ctx, int64_t ms) {
  ctx->set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(ms));
  return ctx;
}

int Fail(const grpc::Status& s) {
  fmt::print(stderr, "error: {}\n", cartwheel::kv::FromGrpc(s).ToString());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"wheelctl: CartwheelKV operator CLI"};
  std::string target = "localhost:7100";
  int64_t timeout_ms = 2000;
  app.add_option("-t,--target", target, "wheel-node address");
  app.add_option("--timeout-ms", timeout_ms, "per-call deadline");
  app.require_subcommand(1);

  std::string table;
  std::string key;
  std::string value;
  std::string context_hex;
  auto* get = app.add_subcommand("get", "read every sibling of a key");
  get->add_option("table", table)->required();
  get->add_option("key", key)->required();
  auto* put = app.add_subcommand("put", "write a value (blind unless --context is given)");
  put->add_option("table", table)->required();
  put->add_option("key", key)->required();
  put->add_option("value", value)->required();
  put->add_option("--context", context_hex, "hex context from a previous get");
  auto* del = app.add_subcommand("delete", "write a tombstone");
  del->add_option("table", table)->required();
  del->add_option("key", key)->required();
  del->add_option("--context", context_hex, "hex context from a previous get");
  auto* ring = app.add_subcommand("ring", "cluster members, ring ownership and repair counters");
  auto* where = app.add_subcommand("where", "preference list of a key");
  where->add_option("table", table)->required();
  where->add_option("key", key)->required();
  double read_rate = 0;
  double write_rate = 0;
  auto* limits =
      app.add_subcommand("limits", "set per-table provisioned throughput (0 = unlimited)");
  limits->add_option("table", table)->required();
  limits->add_option("reads_per_sec", read_rate)->required();
  limits->add_option("writes_per_sec", write_rate)->required();
  auto* health = app.add_subcommand("health", "node health and build");
  auto* stats = app.add_subcommand("stats", "key count and latency percentiles");
  CLI11_PARSE(app, argc, argv);

  std::string context;
  for (size_t i = 0; i + 1 < context_hex.size(); i += 2) {
    context.push_back(static_cast<char>(std::stoi(context_hex.substr(i, 2), nullptr, 16)));
  }

  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  auto kv = v1::KvService::NewStub(channel);
  auto admin = v1::AdminService::NewStub(channel);
  grpc::ClientContext ctx;

  if (*get) {
    v1::GetRequest req;
    req.set_table(table);
    req.set_key(key);
    v1::GetResponse resp;
    if (auto s = kv->Get(WithDeadline(&ctx, timeout_ms), req, &resp); !s.ok()) return Fail(s);
    PrintVersions(resp);
  } else if (*put) {
    v1::PutRequest req;
    req.set_table(table);
    req.set_key(key);
    req.set_value(value);
    req.set_context(context);
    v1::PutResponse resp;
    if (auto s = kv->Put(WithDeadline(&ctx, timeout_ms), req, &resp); !s.ok()) return Fail(s);
    PrintVersions(resp);
  } else if (*del) {
    v1::DeleteRequest req;
    req.set_table(table);
    req.set_key(key);
    req.set_context(context);
    v1::DeleteResponse resp;
    if (auto s = kv->Delete(WithDeadline(&ctx, timeout_ms), req, &resp); !s.ok()) return Fail(s);
    PrintVersions(resp);
  } else if (*ring) {
    v1::ClusterInfoResponse r;
    if (auto s = admin->ClusterInfo(WithDeadline(&ctx, timeout_ms), {}, &r); !s.ok())
      return Fail(s);
    fmt::print("asked {}: N={} R={} W={}, {} tokens on the ring\n", r.node_id(), r.n(), r.r(),
               r.w(), r.tokens_size());
    fmt::print("{:<10} {:<22} {:<8} {:>9} {:>14}\n", "node", "address", "state", "ownership",
               "incarnation");
    for (const auto& m : r.members()) {
      fmt::print("{:<10} {:<22} {:<8} {:>8.1f}% {:>14}\n", m.node_id(), m.address(), m.state(),
                 m.ownership() * 100.0, m.incarnation());
    }
    fmt::print("coordinator: puts={} gets={} forwarded={} quorum_failures={} throttled={}\n",
               r.puts(), r.gets(), r.forwarded(), r.quorum_failures(), r.throttled());
    fmt::print(
        "repair: read_repairs={} hints stored={} delivered={} pending={} "
        "anti-entropy rounds={} keys repaired={}\n",
        r.read_repairs(), r.hints_stored(), r.hints_delivered(), r.hints_pending(),
        r.anti_entropy_rounds(), r.anti_entropy_keys_repaired());
  } else if (*where) {
    v1::PreferenceListRequest req;
    req.set_table(table);
    req.set_key(key);
    v1::PreferenceListResponse r;
    if (auto s = admin->PreferenceList(WithDeadline(&ctx, timeout_ms), req, &r); !s.ok()) {
      return Fail(s);
    }
    fmt::print("{}/{} hashes to {:#018x} ({:.4f} of the ring)\n", table, key, r.position(),
               static_cast<double>(r.position()) / 18446744073709551616.0);
    fmt::print("home replicas: {}\n", fmt::join(r.home(), " -> "));
    fmt::print("live targets:  {}\n", fmt::join(r.live(), " -> "));
  } else if (*limits) {
    v1::TableLimitsRequest req;
    req.set_table(table);
    req.set_reads_per_sec(read_rate);
    req.set_writes_per_sec(write_rate);
    v1::TableLimitsResponse r;
    if (auto s = admin->SetTableLimits(WithDeadline(&ctx, timeout_ms), req, &r); !s.ok()) {
      return Fail(s);
    }
    fmt::print("{}: reads {} /s, writes {} /s on this coordinator\n", table, read_rate, write_rate);
  } else if (*health) {
    v1::HealthResponse resp;
    if (auto s = admin->Health(WithDeadline(&ctx, timeout_ms), {}, &resp); !s.ok()) return Fail(s);
    fmt::print("{}  {}  version {} ({})  uptime {:.1f}s\n", resp.node_id(),
               v1::HealthResponse::State_Name(resp.state()), resp.version(), resp.git_sha(),
               static_cast<double>(resp.uptime_ms()) / 1000.0);
  } else if (*stats) {
    v1::NodeStatsResponse r;
    if (auto s = admin->NodeStats(WithDeadline(&ctx, timeout_ms), {}, &r); !s.ok()) return Fail(s);
    fmt::print("node {}  keys={}  bytes={}\n", r.node_id(), r.keys(), r.bytes());
    fmt::print("gets={}  p50={:.1f}us  p99={:.1f}us\n", r.gets_total(), r.get_p50_us(),
               r.get_p99_us());
    fmt::print("puts={}  p50={:.1f}us  p99={:.1f}us\n", r.puts_total(), r.put_p50_us(),
               r.put_p99_us());
  }
  return 0;
}
