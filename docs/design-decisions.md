# Design decisions

Short records of the choices that shape the code, and what was traded away.

## 1. One fixed `Record`, not an event class hierarchy

A single trivially copyable struct with inline arrays covers logs, spans,
metrics, state changes and errors. It costs memory per record (up to 2048 bytes)
but gives the ring, dump writer, crash writer and reader one representation with
no ownership, allocation or lifetime rules. A variant with `std::string` would
have made the crash path and lock-free snapshot impossible without a second
representation.

Consequence: eight attributes, 96-byte text and 127-byte messages are hard
limits; oversized input is rejected.

## 2. Ring payloads are atomic word arrays

A classic seqlock over non-atomic bytes has a data race under the C++ memory
model even if the value is discarded after a stamp check. Blackbox stores each
payload word as `std::atomic<uint64_t>` and copies records through those words.
This keeps the read lock-free, removes the UB race and makes TSan clean.

Producers still need mutual exclusion for sequence assignment and slot writes.
A small yielded spin lock is used; readers never take it. The trade-off is
contended producers are serialised, while readers stay independent.

## 3. Overwrite, never block

A flight recorder must not stall the service because a reader is slow or absent.
When full, the ring overwrites the oldest slot. Consumers get a `lost` count and
the ring reports overwrites. Bounded blocking queues with backpressure were
rejected because a stuck UI would become a production outage.

## 4. Per-thread causal chains

Every record gets the previous record's sequence on the same thread as
`cause_seq`. This creates useful chains automatically without a global lock or
explicit IDs on every call. Cross-thread work is joined by correlation IDs and
span parent links. The limitation is that an unrelated log after an error is
still causally linked; the explainer handles that with heuristics and the raw
records remain visible.

## 5. Dumps are atomic and strict, with an explicit recovery mode

A dump is written to a temp file, fsynced and renamed. Readers validate a
version, header size, flags, every frame CRC, footer count/CRC and record
ordering. Recovery mode exists because the most valuable dump is often the one
from a crash; it returns only fully validated records and reports the damage.

Allocation is bounded before use; large lengths from corrupt headers cannot
force huge allocations.

## 6. Crash dumps use inline strings

The normal writer builds a bounded string table for size. The signal handler
cannot allocate, so crash dumps set the inline-string flag and encode each
record independently. This duplicates a small amount of path selection in
`crash.cpp` but keeps the handler simple and avoids a preallocated hash table.
The reader handles both encodings with the same record codec.

## 7. Forward-only causal edges

Cause, parent, span-life and correlation edges are only emitted when the target
has a larger sequence. This guarantees an acyclic graph without a cycle check.
If a parent is missing or points forward, it is counted as unresolved rather
than fabricated. Correlation edges link consecutive records with the same id,
so the graph can be dense but remains a DAG.

## 8. Explain-failure is a heuristic, not a proof

The walk prioritises cause, then span life, parent and correlation edges, and
picks the earliest timeout/error/failed span/abnormal state change as the root
cause. This matches the demo and common retry/fallback patterns. It cannot know
application intent; the full chain is always shown so the developer can judge.

## 9. Query parsing has hard, low limits

The UI query bar is not a database. Source, tokens, nodes, depth and string
length are bounded, evaluation is allocation-free and type-aware. There is no
regex, arithmetic, aggregation or join. Explicit parse errors are preferred to
guessing or truncating.

## 10. The UI is compiled in and token-gated

HTML, CSS and JS are embedded in the binary; there are no runtime files, CDNs or
external fonts. The server binds only `127.0.0.1` with an ephemeral port and a
random 128-bit token. Mutation endpoints require the token header, and dump
access is restricted to names returned by the directory listing. Plain HTTP is
acceptable for loopback; TLS is deliberately out of scope.

The cost is that editing the UI requires a rebuild, and the C++ binary carries
the assets (excluded from the LOC budget by the requirements).

## 11. No third-party dependencies and no CI

The requirement is a self-contained local product. CMake, Ninja, the standard
library and POSIX/macOS APIs are the entire toolchain. Assets are embedded by a
CMake script, tests use a tiny in-repo harness, and benchmarks/fuzzing are
executables. CI files are intentionally absent; the verification commands and
results are recorded in docs and `FINAL_REPORT.md`.

## 12. Deterministic demo

The demo cycles through a fixed sequence instead of random failures: two DB
timeouts, breaker open, fallback, open-breaker requests, recovery, success. That
makes the causal story reproducible for screenshots, tests and explanation
checks. Randomness is limited to durations and generated timestamps.
