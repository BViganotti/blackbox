# Crash safety

Blackbox installs handlers for `SIGSEGV`, `SIGABRT`, `SIGBUS`, `SIGILL` and
`SIGFPE` when `RecorderConfig::dump_directory` is set and
`install_crash_handlers` is true (the default). The goal is to preserve the last
published ring records when a native service dies, without making the handler
itself a new failure mode.

## Preconditions

Installation is normal code and may allocate, create directories, generate a
random filename and call `sigaltstack`/`sigaction`. It happens before the
handler can run. The following are prepared in advance:

- a 256 KiB alternate signal stack,
- a static 8 KiB record encoder buffer,
- the final and temporary path strings (`crash-<UTC>-<pid>-<random>.bbx`),
- saved signal dispositions and the old alternate stack,
- the ring pointer.

Only one handler is installed per process. A second `Recorder` with a dump
directory receives `crash_error()` instead of replacing the first handler.

## Handler work

The handler performs:

1. an atomic "busy" guard so recursive crashes fall through to the default
   disposition,
2. a bounded, lock-free read of published ring slots,
3. inline-string record encoding into the static buffer,
4. raw `write` of frame headers and payloads,
5. `lseek` back to patch the header count and sequence range,
6. footer write, best-effort `fsync`, `close` and atomic `rename`,
7. reset to `SIG_DFL`, unblock the signal, `raise`, then `_exit`.

It does not call `malloc`, `new`, `std::string`, iostreams, `printf`, `mutex`,
`condition_variable`, the file-system library or any code that can allocate or
lock. The encode and validation helpers it calls only inspect fixed arrays.

The ring seqlock can observe a slot being written or overwritten by another
thread. Such slots are skipped; no torn record can enter the dump. Writers are
not blocked.

## Output

The crash dump uses the normal `.bbx` format with:

- `inline_strings` and `crash` flags,
- a string count of zero,
- records in ascending sequence order,
- a footer with the actual record count and chained record CRC,
- the header patched with record count, first and last sequence,
- creation time taken at installation (not in the handler).

If encoding or I/O fails, the temporary file is unlinked and the original
signal is re-raised. A partially written `.bbx` final name is never published.

## Testing

`tests/test_crash.cpp` forks a child that records four events, writes a pipe
marker, raises `SIGSEGV`, and is reaped by the parent. The parent asserts:

- the child died from the signal (not an exit code),
- a valid crash `.bbx` exists,
- the footer is complete and CRCs pass,
- the records include the deliberate error,
- no `.tmp` file remains.

The crash test is skipped under ASan and TSan, whose own signal interceptors
replace or observe the process dispositions. The rest of the suite, including
dump recovery, runs under both sanitizers.

Manual check:

```sh
./build/blackbox_launcher --no-browser --dump-dir /tmp/bb-crash
# kill -SEGV the launcher PID, then:
ls /tmp/bb-crash
# use the UI's Dumps tab, or write a small program using read_dump_file()
```

## Limitations

- `SIGKILL` and kernel panics cannot be intercepted.
- A crash while the disk is full or the dump directory is unwritable loses the
  dump; the process still terminates normally.
- The handler's `fsync` is best effort. Data may still be in the page cache if
  the machine loses power immediately afterwards.
- A stack overflow is handled via the alternate stack, but if the alternate stack
  is corrupted the handler cannot recover.
- The handler captures the ring at the instant of the signal; records still
  being written by other threads are skipped.
- Only one handler exists per process. Embedders with their own crash handler
  should set `install_crash_handlers = false`.
- The handler writes to the same directory as normal dumps and is subject to the
  same retention listing; crash files are not automatically pruned while the
  process is running because the process is dying.
