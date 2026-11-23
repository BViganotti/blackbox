# Performance notes

Blackbox prioritises bounded memory, deterministic behaviour and data-race
freedom over throughput. The benchmark binary reports measurements only; it does
not assert thresholds, because laptops, filesystems and thermal state vary.

## Reference run

Host: Apple M1 Max, macOS 26.7.3, arm64, release build (`-O3`), APFS temporary
directory. Full suite (`./build/blackbox_bench`):

```text
ring: reserved memory                       16.00 MiB
ring: capacity                            8192.00 records
ring: single-producer push                 237.08 ns/event
ring: single-producer rate             4217920.11 events/s
ring: four-producer push                   562.43 ns/event
ring: four-producer rate               1778012.38 events/s
ring: snapshot read                        148.54 ns/record
ring: snapshot rate                    6732090.17 records/s
dump: write                                 14.83 MiB/s
dump: write                               4623.76 ns/record
dump: read + validate                      101.26 MiB/s
dump: read + validate                      677.26 ns/record
dump: file size                              0.28 MiB
query: evaluate                              9.30 ns/record
query: matched                           71400.00 matches
causal: build+layout+explain              1438.85 us/trace
causal: edges per trace                   4999.00 edges
json: serialise                            180.76 MiB/s
```

The dump "write" number includes `fsync` and atomic rename for a 4096-record
file. It is intentionally an end-to-end durability number, not a raw write
speed. Query evaluation used a two-predicate expression with an attribute and
name search over 5000 records for 100 passes. Causal analysis used 5000 records
with a full cause chain.

## What is measured

- **Ring push**: validation, timestamping, slot seqlock write and sequence
  assignment for pre-built records. Four-producer mode adds contention on the
  ticket lock.
- **Ring read**: strict seqlock copies of 8192 records, repeated 100 times.
- **Dump write**: full string-table construction, record encoding, frame CRCs,
  fsync and rename.
- **Dump read**: file load, header/frame/footer CRC checks, record decode and
  full validation.
- **Query**: parse once, then evaluate every record per pass.
- **Causal**: copy/sort/validate, edge construction/dedup, adjacency build,
  layout and failure explanation per trace.
- **JSON**: record objects into a bounded writer.

## Interpretation

- A record is large (up to 2048 bytes) so ring memory is predictable but a
  16 MiB default retains 4096 records, not hundreds of thousands.
- Ring push is dominated by validation and copying ~2 KiB per record. It is
  still millions of events per second on this host.
- Dump throughput is dominated by `fsync`; batching records into one flush is
  strongly preferred over flushing per event.
- Snapshot response size, not CPU, is usually the UI bottleneck; the server caps
  responses at 8 MiB and the UI requests 1200 records.
- ASan/TSan builds are much slower and are not used for these numbers.
- The fuzz harness has fixed iteration costs; runtime grows linearly with
  `--iterations`.

Reproduce with:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/blackbox_bench
```
