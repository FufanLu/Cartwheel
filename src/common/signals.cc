#include "common/signals.h"

#include <csignal>
#include <pthread.h>

namespace cartwheel {
namespace {

sigset_t TerminationSignals() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  return set;
}

}  // namespace

void BlockTerminationSignals() {
  const sigset_t set = TerminationSignals();
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

int WaitForTerminationSignal() {
  const sigset_t set = TerminationSignals();
  int sig = 0;
  sigwait(&set, &sig);
  return sig;
}

}  // namespace cartwheel
