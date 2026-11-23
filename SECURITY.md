# Security model

Blackbox is a local developer tool. It is designed to be safe on a workstation
or build machine, not to be exposed to a network.

## Trust boundaries

- The recorder accepts data from the process it is embedded in. Records are
  validated before entering the ring; malformed internal input is rejected.
- The HTTP server is reachable only on `127.0.0.1` and only through an ephemeral
  port. It never binds `0.0.0.0`, IPv6 any-address or a Unix socket.
- Every data endpoint requires a 128-bit random token generated from the OS at
  server construction. Tokens are compared in constant time.
- Dump files are local files chosen by the user. The UI only opens, downloads or
  compares `.bbx` names returned by the configured directory listing; request
  strings are not used as arbitrary filesystem paths.

## Authentication

The token can be supplied as:

- a `token` query parameter (used by the launcher URL),
- a `bb_token` cookie set by the root page,
- `Authorization: Bearer <token>`,
- `X-Blackbox-Token: <token>`.

The cookie is intentionally not `HttpOnly`: the workbench JavaScript needs the
token to set the header on mutation requests, which prevents a cross-site form
from triggering a dump. It is `SameSite=Strict` and scoped to `/`. The server
sets `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`,
`X-Content-Type-Options: nosniff` and a restrictive CSP in the HTML.

The token is printed in the launcher URL. Avoid pasting that URL into shared
channels or screen recordings; anyone who obtains it while the server runs can
read the recorded history.

## Untrusted input

The following parsers are bounded and malformed-input tested/fuzzed:

- dump headers, frames, string tables and records,
- the query language,
- HTTP request lines, headers, parameters, URLs and bodies,
- JSON string escaping and UTF-8 validation,
- attribute keys/text and fixed record fields.

Readers limit file size, frame size, record count, string count, token count,
nesting depth and HTTP header/param/body size before or while parsing. Returned
data is validated; failures are explicit.

## What Blackbox records

The recorder stores whatever the embedder supplies: request metadata, SQL
statements, error text, attribute values, correlation IDs and state deltas. It
does not scrub secrets. Dumps are written with mode 0600, but they are not
encrypted. Treat them as sensitive production data, especially if attribute
values contain credentials or personal data.

Crash dumps are written into the same directory. If that directory is on a
shared volume, permissions and backups are the operator's responsibility.

## Denial of service

The loopback server has a fixed worker pool (12), a bounded queue (32), four SSE
streams, bounded request headers (16 KiB), at most 32 query parameters, a 4 KiB
body limit and an 8 MiB JSON output budget. Exceeding a limit returns 400/413/
429/503 rather than growing threads or memory without bound.

The ring has a hard capacity and one record has a hard size. A dump reader
rejects files above 256 MiB. Query parsing stops at 4096 bytes / 1024 tokens /
512 nodes / depth 32.

A local user who can connect to loopback can still authenticate only with the
token. A local user who can read the process memory or filesystem can read the
data regardless; that is outside the local-tool threat model.

## Crash handler

The handler does not allocate or lock and writes only to the precomputed crash
path. It re-raises the original signal. It cannot defend against a malicious
signal handler already installed by another component; disable installation if
your process owns its own crash reporting.

## Reporting

This is a personal open-source project. Open an issue in the repository with a
minimal reproduction and note whether data exposure, authentication bypass,
memory safety or unbounded resource use is involved. Do not include real dumps
or tokens in a public report.
