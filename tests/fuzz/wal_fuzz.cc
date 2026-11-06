// The WAL reader must never crash or loop on arbitrary bytes (a torn or
// bit-rotted log after a crash), only skip or stop.
#include <memory>
#include <string>
#include <vector>

#include "mem_files.h"
#include "storage/wal.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace cartwheel;
  std::string bytes(reinterpret_cast<const char*>(data), size);
  // Repeat the input so fragments cross the 32 KiB block boundary too.
  if (size > 0 && size < 512) {
    while (bytes.size() < storage::wal::kBlockSize + 600) bytes.append(bytes);
  }
  for (const bool paranoid : {false, true}) {
    storage::wal::Reader r(std::make_unique<fuzz::MemSequentialFile>(bytes));
    std::string rec;
    size_t n = 0;
    while (r.ReadRecord(&rec, paranoid) && n < 100000) ++n;
  }
  return 0;
}

std::vector<std::string> FuzzSeeds() {
  // A real log produced by the writer: two records, one spanning blocks.
  class Sink final : public cartwheel::storage::WritableFile {
   public:
    std::string data;
    cartwheel::Status Append(std::string_view d) override {
      data.append(d);
      return {};
    }
    cartwheel::Status Flush() override { return {}; }
    cartwheel::Status Sync() override { return {}; }
    cartwheel::Status Close() override { return {}; }
    uint64_t Size() const override { return data.size(); }
  };
  auto sink = std::make_unique<Sink>();
  Sink* raw = sink.get();
  cartwheel::storage::wal::Writer w(std::move(sink));
  static_cast<void>(w.AddRecord("hello"));
  static_cast<void>(w.AddRecord(std::string(40000, 'x')));
  return {raw->data, raw->data.substr(0, 100)};
}
