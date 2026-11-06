// Process-crash durability: a child process writes to the LSM store and
// reports every acknowledged key over a pipe; the parent SIGKILLs it at a
// random moment, reopens the store, and checks every acknowledged key.
//
// Writes use sync=false. A process crash, unlike a power cut, keeps the OS
// page cache, so an acknowledged write (already handed to write(2)) must
// survive. Power-loss semantics are covered by db_test's
// SyncedWritesSurvivePowerLoss with the fault-injection env.

#include <chrono>
#include <cstdio>
#include <random>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <gtest/gtest.h>

#include "../unit/storage/storage_test_util.h"
#include "common/units.h"
#include "storage/lsm/db.h"

namespace cartwheel::storage::lsm {
namespace {

std::string ValueFor(uint64_t i) {
  return "value-" + std::to_string(i * 2654435761ULL);
}

[[noreturn]] void ChildWriter(const std::string& dir, uint64_t start, int ack_fd) {
  Options o;
  o.write_buffer_size = 256_KiB;  // flushes and compactions happen during the run
  o.max_file_size = 128_KiB;
  o.levels.l1_target_bytes = 512_KiB;
  auto db = DB::Open(o, dir);
  if (!db.ok()) _exit(2);
  for (uint64_t i = start;; ++i) {
    if (!(*db)->Put({}, "key-" + std::to_string(i), ValueFor(i)).ok()) _exit(3);
    if (::write(ack_fd, &i, sizeof(i)) != sizeof(i)) _exit(4);
  }
}

TEST(LsmCrashTest, AcknowledgedWritesSurviveSigkill) {
  testing::TempDir dir;
  std::mt19937_64 rng(2026);
  uint64_t next = 0;
  for (int round = 0; round < 6; ++round) {
    std::fflush(stdout);  // the child must not inherit (and later repeat) buffered output
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      ::close(fds[0]);
      ChildWriter(dir.path(), next, fds[1]);
    }
    ::close(fds[1]);
    // Let the child run 150-600 ms, reading acks as they arrive.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(150 + rng() % 450);
    uint64_t last_acked = next;
    bool any = false;
    while (std::chrono::steady_clock::now() < deadline) {
      uint64_t i = 0;
      if (::read(fds[0], &i, sizeof(i)) == sizeof(i)) {
        last_acked = i;
        any = true;
      }
    }
    ::kill(pid, SIGKILL);
    // Drain acks the child wrote before dying.
    for (uint64_t i = 0; ::read(fds[0], &i, sizeof(i)) == sizeof(i);) {
      last_acked = i;
      any = true;
    }
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFSIGNALED(status)) << "round " << round << ": child exited early";
    ASSERT_TRUE(any) << "round " << round << ": child acknowledged nothing";

    auto db = DB::Open(Options{}, dir.path());
    ASSERT_TRUE(db.ok()) << "round " << round << ": " << db.status();
    for (uint64_t i = 0; i <= last_acked; ++i) {
      auto v = (*db)->Get("key-" + std::to_string(i));
      ASSERT_TRUE(v.ok()) << "round " << round << ": lost acknowledged key " << i << " of "
                          << last_acked;
      ASSERT_EQ(*v, ValueFor(i));
    }
    std::printf("round %d: %llu keys acknowledged, all present after SIGKILL; %s\n", round,
                static_cast<unsigned long long>(last_acked + 1), (*db)->Stats().levels.c_str());
    next = last_acked + 1;
  }
}

}  // namespace
}  // namespace cartwheel::storage::lsm
