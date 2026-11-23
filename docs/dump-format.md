# Blackbox dump format (`.bbx`)

A dump is a self-contained, portable bundle of records. It contains no pointers,
no absolute paths, no compression and no external references. All integers are
little-endian. Checksums are CRC-32C (Castagnoli).

## File layout

```text
┌──────────────── 64-byte header ────────────────┐
│ magic "BBXDMP01", version, flags, sizes, seqs  │
├──────────────── optional string frame ─────────┤
├──────────────── record frame ──────────────────┤
├──────────────── record frame ──────────────────┤
│ ...                                            │
├──────────────── 32-byte footer ────────────────┤
│ magic "BBXEND01", count, records CRC, footer CRC
└────────────────────────────────────────────────┘
```

### Header (64 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | magic `BBXDMP01` |
| 8 | 4 | format version, currently 1 |
| 12 | 4 | flags |
| 16 | 4 | header size, must be 64 |
| 20 | 8 | creation wall time, Unix nanoseconds |
| 28 | 8 | record count |
| 36 | 8 | first sequence |
| 44 | 8 | last sequence |
| 52 | 4 | string table count |
| 56 | 4 | reserved, must be 0 |
| 60 | 4 | CRC-32C of bytes 0..59 |

Flags:

| Bit | Name | Meaning |
|---:|---|---|
| 0 | inline strings | no string table frame; strings are inline |
| 1 | crash dump | written by the async-signal-safe handler |
| 2 | partial | reserved for partial captures |

Unknown flag bits are rejected.

### Frame header (9 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | frame type |
| 1 | 4 | payload length |
| 5 | 4 | CRC-32C of the payload |

Frame types: `1` string table, `2` record, `3` metadata (currently skipped).
Unknown types are rejected by the strict reader.

### String table frame

```text
varint count
repeat count times:
  varint byte_length
  byte_length bytes (UTF-8/raw, no NUL)
```

Strings are interned in id order; a record string field is a `varint` id when
the inline flag is clear. The writer rejects tables above 16,384 strings or
strings above 256 bytes. The crash path uses inline strings, so no table is
needed under a signal.

### Record frame payload

```text
u8  codec version (1)
u8  kind      (1 log, 2 span_begin, 3 span_end, 4 metric,
               5 state_change, 6 error, 7 mark, 8 note)
u8  level     (0 trace .. 5 fatal)
u8  status    (0 unset .. 5 rejected)
u8  attribute count (<= 8)
varint  seq
varint  wall_ns      (zigzag)
varint  mono_ns      (zigzag)
varint  duration_ns  (zigzag)
varint  int_value    (zigzag)
f64     double_value
u64     trace high
u64     trace low
varint  span, parent, correlation, cause_seq
string  name, message, state_key
value   value, state_old, state_new
repeat attribute count:
  string key
  value  value
```

`value` is `u8 type` followed by: nothing for null, `u8` for bool, `varint` for
int, `f64` for float, `string` for string. The order above is the on-disk order
implemented by `encode_record` / `decode_record`.

The reader validates the entire record, including enum ranges, NUL termination,
attribute key syntax, duplicate keys, finite floats and span invariants. It
rejects duplicate or decreasing sequences.

### Footer (32 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | magic `BBXEND01` |
| 8 | 8 | record count |
| 16 | 4 | CRC-32C chained over every record payload |
| 20 | 4 | reserved, must be 0 |
| 24 | 4 | CRC-32C of bytes 0..23 |
| 28 | 4 | end marker `0x42425845` |

## Writing

`write_dump_file` validates every record, requires strictly increasing non-zero
sequences and at most 1,000,000 records, then:

1. opens `PATH.tmp.<pid>.<random>` with mode 0600 and `O_EXCL`,
2. writes header, strings, frames and footer,
3. calls `fsync`,
4. closes and atomically `rename`s to the final path.

A failure removes the temporary file. A reader never sees a partially written
final path.

`Recorder::flush_dump` snapshots the retained ring, writes the file and prunes
oldest dumps beyond `RecorderConfig::max_dumps` (default 64).

## Reading

`read_dump_bytes` and `read_dump_file` have two modes:

- **strict** (`require_footer = true`, the default): header, every frame CRC,
  footer CRC, counts, ordering and the absence of trailing bytes must all be
  valid. Any damage fails the read.
- **recovery** (`require_footer = false`): the valid prefix is returned and
  `truncated`, `corrupted`, `complete` and `error` explain what happened. Every
  returned record is still fully validated.

The reader never allocates from an unvalidated length. File size (256 MiB),
record count (1,000,000), frame size (8 MiB) and string count are bounded before
allocation.

The UI uses recovery mode when opening a dump so a crash-truncated file still
shows useful history; the strict API remains available for tooling.

## Crash dumps

The crash handler writes the same format with `inline_strings | crash` flags.
It writes a zero-count header, frames, then seeks back to patch the count and
first/last sequence before writing the footer. Only records that are completely
published in the ring at the moment of the signal are included.

## Compatibility

The version and header size are explicit and checked. Changing record field
layout requires a new version; old readers must fail cleanly rather than guess.
There is no automatic migration yet.
