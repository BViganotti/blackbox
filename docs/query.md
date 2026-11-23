# Query language

Blackbox queries are predicates over `bb::Record`. They are parsed by a
recursive-descent parser in `src/core/query.cpp` and evaluated per record with no
allocation. The same language powers the UI query bar and the library
`bb::Query` API.

An empty query matches every record.

## Grammar

```text
query      := or EOF
or         := and ("OR" and)*
and        := unary ("AND" unary)*
unary      := "NOT" unary | "(" query ")" | predicate
predicate  := field operator value
```

Keywords are case-insensitive. `AND` binds tighter than `OR`; `NOT` binds
tightest. Parentheses override precedence.

Values are double-quoted or single-quoted strings with `\\`, `\"`, `\'`, `\n`,
`\t` and `\r` escapes, bare identifiers, integers, decimals, `true`, `false`, or
durations.

## Fields

| Field | Value type | Operators | Meaning |
|---|---|---|---|
| `kind` | name | `= != < <= > >=` | log, span_begin, span_end, metric, state_change, error, mark, note |
| `level` | name | comparison | trace, debug, info, warn, error, fatal |
| `status` | name | comparison | unset, ok, error, cancelled, timeout, rejected |
| `name` | string | `= != ~ !~` | record name (span, metric, mark) |
| `message` | string | `= != ~ !~` | log, error or note message |
| `text`, `any` | string | `= != ~ !~` | searches kind/level/status/name/message/state key and attribute keys and string values |
| `trace` | hex | `= != ~ !~` | exact 32-hex trace id, or a hex substring |
| `span` | integer | numeric | span id |
| `parent` | integer | numeric | parent span id |
| `correlation`, `corr` | integer | numeric | correlation id |
| `seq`, `sequence` | integer | numeric | ring sequence |
| `cause`, `cause_seq` | integer | numeric | causal predecessor sequence |
| `duration`, `elapsed` | integer/ns | numeric | span duration |
| `state`, `state_key` | string | `= != ~ !~` | state key |
| `state.old` | string | `= != ~ !~` | previous state rendered as text |
| `state.new` | string | `= != ~ !~` | next state rendered as text |
| `value` | typed | numeric or string | metric/typed value |
| `attr.KEY`, `attr["KEY"]` | typed | numeric or string | attribute lookup |

Attribute keys must satisfy the recorder rules: non-empty, at most 27 bytes,
ASCII letters/digits and `.`, `_`, `-`, `:`, `/`, starting and ending with an
alphanumeric or `_`.

## Semantics

- `=` and `!=` compare type-aware. A numeric attribute compared with a numeric
  literal is numeric; a string attribute with a string literal is lexical.
- `~` is an ASCII case-insensitive substring test. `!~` negates it.
- `<`, `<=`, `>`, `>=` are numeric for numbers, lexical for strings and ordinal
  for kind/level/status.
- Missing attributes never match, including with `!=`. This is deliberate: a
  missing value is unknown, not unequal.
- A type mismatch (for example an integer attribute against a quoted string)
  never matches.
- `duration` accepts a bare integer in nanoseconds or a unit: `1ns`, `500us`,
  `250ms`, `2s`. Decimals are allowed; the value is floored to nanoseconds.
- Trace exact matching requires 32 hexadecimal digits and a non-zero trace.
  `trace ~` accepts any non-empty hex substring.
- State values are compared through their human-readable text
  (`null`, `true`, `12`, `1.5`, or the string itself).

## Limits and failure

| Limit | Value |
|---|---:|
| Source bytes | 4096 |
| Tokens | 1024 |
| Syntax nodes | 512 |
| Nesting depth | 32 |
| String literal | 512 bytes |

Limit violations and syntax errors return `std::nullopt` from
`bb::Query::parse` with a message containing the byte offset. Nothing is
truncated or ignored. The UI marks the query field invalid and shows the error.

## Examples

```text
kind = error
level >= warn AND duration > 100ms
message ~ timeout
attr.http.status = 500
attr["db.system"] = postgres
state.new = open
correlation = 51966
trace = 0123456789abcdeffedcba9876543210
trace ~ 89abcdef
NOT (kind = metric OR kind = log)
```

## C++ API

```cpp
#include <blackbox/query.hpp>

std::string error;
auto query = bb::Query::parse("level >= warn AND message ~ timeout", &error);
if (!query) {
  // error explains the problem
}

for (const bb::Record& record : records) {
  if (query->matches(record)) {
    // ...
  }
}

auto matching = bb::filter_records(records, *query, 1000);
```

`Query` is immutable, copyable and thread-safe; the parsed tree is shared.
