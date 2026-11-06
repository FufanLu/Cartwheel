#pragma once

namespace cartwheel {

// Graceful-shutdown plumbing shared by every server binary.
//
// BlockTerminationSignals() must run first in main(), before any thread
// exists. SIGINT and SIGTERM are then blocked in every thread, since threads
// inherit the mask. WaitForTerminationSignal() consumes them synchronously
// with sigwait(), so the shutdown sequence that follows can run arbitrary
// code, with none of the restrictions of an async signal handler.
void BlockTerminationSignals();
int WaitForTerminationSignal();

}  // namespace cartwheel
