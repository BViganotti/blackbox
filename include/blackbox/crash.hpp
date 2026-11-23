#pragma once

#include <blackbox/ring.hpp>

#include <string>
#include <string_view>

namespace bb {

/// Installs async-signal-safe handlers for SIGSEGV, SIGABRT, SIGBUS, SIGILL and
/// SIGFPE. On a fatal signal the handler writes a self-contained crash dump
/// (inline strings, crash flag, footer and checksums) into `directory`, fsyncs,
/// atomically renames it into place and re-raises the signal so the process
/// still terminates normally.
///
/// The handler uses only preallocated storage and async-signal-safe system
/// calls; it does not allocate, lock, use stdio or throw. An alternate signal
/// stack is installed so stack-overflow crashes can still be recorded.
///
/// Only one handler can be installed per process. Returns false and fills
/// `error` when `directory` is empty or cannot be created, when a handler is
/// already installed, or when the OS rejects the installation.
bool install_crash_handler(Ring& ring, std::string_view directory,
                           std::string* error = nullptr);

/// Removes the handler and restores the previous dispositions. When
/// `expected_ring` is non-null, the handler is only removed if it belongs to
/// that ring, so one Recorder cannot remove another's handler. Returns true if
/// a handler was removed.
bool uninstall_crash_handler(Ring* expected_ring = nullptr);

/// True while a crash handler is installed.
bool crash_handler_installed() noexcept;

}  // namespace bb
