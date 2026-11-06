// lsm-tour: runs the real LSM components on fixed inputs and prints what they
// produced, byte for byte. Output feeds docs/illustrated/02-inside-the-lsm-engine.md.

#include <cctype>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/coding.h"
#include "common/units.h"
#include "storage/crc32c.h"
#include "storage/lsm/block.h"
#include "storage/lsm/db.h"
#include "storage/lsm/memtable.h"
#include "storage/lsm/table.h"
#include "storage/lsm/write_batch.h"
#include "storage/wal.h"

namespace {

using namespace cartwheel;
using namespace cartwheel::storage;
using namespace cartwheel::storage::lsm;

void Hex(std::string_view bytes, size_t limit = 96) {
  for (size_t off = 0; off < std::min(bytes.size(), limit); off += 16) {
    std::string hex;
    std::string ascii;
    for (size_t i = off; i < std::min(off + 16, bytes.size()); ++i) {
      const auto c = static_cast<unsigned char>(bytes[i]);
      hex += fmt::format("{:02x} ", c);
      ascii += std::isprint(c) ? static_cast<char>(c) : '.';
    }
    fmt::print("  {:04x}  {:<48} |{}|\n", off, hex, ascii);
  }
}

std::string TempDir(const std::string& name) {
  const auto p = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(p);
  std::filesystem::create_directories(p);
  return p.string();
}

void TourWal() {
  fmt::print("## 1. A write batch in the WAL\n\n");
  WriteBatch b;
  b.Put("cart:ada", "v1");
  b.Delete("cart:bob");
  b.SetSequence(41);
  const std::string dir = TempDir("lsm-tour-wal");
  {
    wal::Writer w(Env::Default()->NewWritableFile(dir + "/000001.log").value());
    static_cast<void>(w.AddRecord(b.Contents()));
    static_cast<void>(w.Close());
  }
  const std::string bytes = ReadFileToString(Env::Default(), dir + "/000001.log").value();
  fmt::print(
      "WriteBatch{{Put(cart:ada, v1), Delete(cart:bob)}} at sequence 41 -> {} bytes on disk\n```\n",
      bytes.size());
  Hex(bytes);
  fmt::print("```\n");
  const uint32_t stored = DecodeFixed32(bytes.data());
  fmt::print(
      "header: masked crc {:08x} (unmasked {:08x}, recomputed {:08x}), length {}, type {} "
      "(FULL)\n\n",
      stored, crc32c::Unmask(stored), crc32c::Value(bytes.data() + 6, bytes.size() - 6),
      static_cast<unsigned>(static_cast<unsigned char>(bytes[4])) |
          (static_cast<unsigned>(static_cast<unsigned char>(bytes[5])) << 8),
      static_cast<int>(bytes[6]));
}

void TourSkipList() {
  fmt::print("## 2. The memtable's skiplist after 100,000 inserts\n\n");
  MemTable mem;
  for (uint64_t i = 0; i < 100'000; ++i) {
    mem.Add(i + 1, ValueType::kValue, fmt::format("key{:08d}", (i * 7919) % 100'000), "v");
  }
  const auto counts = mem.SkipListLevelCounts();
  fmt::print("| level | nodes | ratio to level below | expected (1/4) |\n|---:|---:|---:|---:|\n");
  for (size_t l = 0; l < counts.size(); ++l) {
    fmt::print("| {} | {} | {} | {} |\n", l, counts[l],
               l == 0 ? std::string("-")
                      : fmt::format("{:.3f}", static_cast<double>(counts[l]) /
                                                  static_cast<double>(counts[l - 1])),
               l == 0 ? std::string("-") : std::string("0.250"));
  }
  fmt::print("\narena memory: {:.1f} KiB for {} entries\n\n",
             static_cast<double>(mem.ApproximateMemoryUsage()) / 1024.0, mem.entries());
}

void TourTable() {
  fmt::print("## 3. Anatomy of an SSTable (2,000 keys, 100-byte values)\n\n");
  const std::string dir = TempDir("lsm-tour-sst");
  const std::string path = dir + "/000007.sst";
  {
    auto f = Env::Default()->NewWritableFile(path).value();
    TableBuilder tb(TableOptions{}, f.get());
    for (int i = 0; i < 2000; ++i) {
      tb.Add(MakeInternalKey(fmt::format("user:{:06d}", i), 1000 + static_cast<uint64_t>(i),
                             ValueType::kValue),
             std::string(100, static_cast<char>('a' + i % 26)));
    }
    static_cast<void>(tb.Finish());
    static_cast<void>(f->Close());
  }
  const std::string file = ReadFileToString(Env::Default(), path).value();
  Footer footer;
  static_cast<void>(
      footer.DecodeFrom(std::string_view(file).substr(file.size() - Footer::kEncodedLength)));
  fmt::print("file size {} bytes; footer (last 48 bytes):\n```\n", file.size());
  Hex(std::string_view(file).substr(file.size() - Footer::kEncodedLength));
  fmt::print("```\nindex block @{} ({} bytes), bloom filter @{} ({} bytes)\n\n",
             footer.index.offset, footer.index.size, footer.filter.offset, footer.filter.size);

  auto ra = Env::Default()->NewRandomAccessFile(path).value();
  auto index = std::make_shared<const Block>(ReadBlock(*ra, footer.index).value());
  auto it = Block::NewIterator(index);
  std::vector<std::pair<std::string, BlockHandle>> entries;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    Decoder d(it->value());
    BlockHandle h;
    static_cast<void>(h.DecodeFrom(&d));
    ParsedInternalKey p;
    static_cast<void>(ParseInternalKey(it->key(), &p));
    entries.emplace_back(fmt::format("{} @seq {}", p.user_key, p.sequence), h);
  }
  fmt::print("index: {} entries, one per data block (last key -> handle)\n\n", entries.size());
  fmt::print("| block | last key in block | offset | size |\n|---:|---|---:|---:|\n");
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i < 3 || i + 1 == entries.size()) {
      fmt::print("| {} | `{}` | {} | {} |\n", i, entries[i].first, entries[i].second.offset,
                 entries[i].second.size);
    } else if (i == 3) {
      fmt::print("| … | | | |\n");
    }
  }
  const auto first = std::make_shared<const Block>(ReadBlock(*ra, entries[0].second).value());
  uint64_t data_bytes = 0;
  for (const auto& e : entries) data_bytes += e.second.size + kBlockTrailerSize;
  const std::string filter = ReadBlock(*ra, footer.filter).value();
  fmt::print("\nblock 0: {} restart points (one per 16 keys); first data block starts:\n```\n",
             first->num_restarts());
  Hex(file, 64);
  fmt::print("```\n");
  fmt::print(
      "space: data {} B ({:.1f}%), bloom {} B = {:.1f} bits/key with k={}, index {} B, footer 48 "
      "B\n",
      data_bytes, 100.0 * static_cast<double>(data_bytes) / static_cast<double>(file.size()),
      filter.size(), 8.0 * static_cast<double>(filter.size() - 1) / 2000.0,
      static_cast<int>(filter.back()), footer.index.size + kBlockTrailerSize);
  fmt::print(
      "raw key+value bytes: {} -> on disk {} ({:+.1f}% overhead incl. internal-key trailers)\n\n",
      2000 * (11 + 100), file.size(),
      100.0 * (static_cast<double>(file.size()) / (2000.0 * 111.0) - 1.0));
}

void TourCompaction() {
  fmt::print("## 4. Flushes and compactions, as they happen\n\n");
  fmt::print(
      "Tiny limits so the tree grows deep quickly: memtable 64 KiB, L1 target 256 KiB, files 64 "
      "KiB.\n");
  fmt::print(
      "Workload: 60,000 puts over 20,000 keys (each key overwritten ~3x), then 5,000 "
      "deletes.\n\n```\n");
  Options o;
  o.write_buffer_size = 64_KiB;
  o.max_file_size = 64_KiB;
  o.levels.l1_target_bytes = 256_KiB;
  int total = 0;
  int shown_first = 0;
  int shown_drops = 0;
  o.event_listener = [&](const std::string& e) {
    ++total;
    // The first few events show the tree forming; later, show compactions
    // that actually discarded overwritten versions or tombstones.
    const bool drops =
        e.find("dropped 0 shadowed versions and 0 tombstones") == std::string::npos &&
        e.starts_with("compaction");
    if (shown_first < 6) {
      fmt::print("{}\n", e);
      ++shown_first;
    } else if (drops && shown_drops < 5 && total % 3 == 0) {
      if (shown_drops == 0) fmt::print("...\n");
      fmt::print("{}\n", e);
      ++shown_drops;
    }
  };
  const std::string dir = TempDir("lsm-tour-db");
  auto db = DB::Open(o, dir).value();
  for (uint64_t i = 0; i < 60'000; ++i) {
    static_cast<void>(
        db->Put({}, fmt::format("key{:06d}", (i * 7919) % 20'000), std::string(40, 'x')));
  }
  for (uint64_t i = 0; i < 5'000; ++i)
    static_cast<void>(db->Delete({}, fmt::format("key{:06d}", i)));
  static_cast<void>(db->Flush());
  static_cast<void>(db->WaitForCompactions());
  fmt::print("... ({} events in total)\n```\n\n", total);
  const auto s = db->Stats();
  fmt::print("final shape: {}\n", s.levels);
  fmt::print("| level | files | bytes |\n|---:|---:|---:|\n");
  for (size_t l = 0; l < s.level_files.size(); ++l) {
    if (s.level_files[l] > 0)
      fmt::print("| L{} | {} | {} |\n", l, s.level_files[l], s.level_bytes[l]);
  }
  fmt::print("\nuser bytes {} -> flushed {} + compacted {}  => write amplification {:.2f}\n",
             s.bytes_written_user, s.bytes_written_flush, s.bytes_written_compaction,
             s.WriteAmplification());
  fmt::print("entries in tables: {} for 15,000 live keys\n", s.table_entries);
  fmt::print("flushes {}, compactions {}, trivial moves {}, write groups {} for {} writes\n\n",
             s.flushes, s.compactions, s.trivial_moves, s.write_groups, s.writes);
}

void TourGroupCommit() {
  fmt::print("## 5. Group commit: fsync'ed writes from 1 vs 8 threads\n\n");
  fmt::print("| threads | writes | WAL records (groups) | writes per fsync | writes/s |\n");
  fmt::print("|---:|---:|---:|---:|---:|\n");
  for (const int threads : {1, 8}) {
    const std::string dir = TempDir(fmt::format("lsm-tour-gc{}", threads));
    auto db = DB::Open(Options{}, dir).value();
    constexpr int kPerThread = 300;
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads));
    for (int t = 0; t < threads; ++t) {
      pool.emplace_back([&db, t] {
        for (int i = 0; i < kPerThread; ++i) {
          static_cast<void>(db->Put({.sync = true}, fmt::format("t{}-{}", t, i), "value"));
        }
      });
    }
    for (auto& th : pool) th.join();
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const auto s = db->Stats();
    fmt::print("| {} | {} | {} | {:.1f} | {:.0f} |\n", threads, s.writes, s.write_groups,
               static_cast<double>(s.writes) / static_cast<double>(s.write_groups),
               static_cast<double>(s.writes) / secs);
  }
  fmt::print("\n");
}

}  // namespace

int main() {
  TourWal();
  TourSkipList();
  TourTable();
  TourCompaction();
  TourGroupCommit();
  return 0;
}
