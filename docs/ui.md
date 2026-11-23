# Workbench guide

The UI is original HTML, CSS and JavaScript compiled into the binary. It talks
to the local server over same-origin `fetch` and `EventSource`; there are no
external fonts, images, scripts or network requests.

## Launch and authentication

`blackbox_launcher` prints a URL of the form:

```text
http://127.0.0.1:50343/?token=<32 hex digits>
```

Opening that URL sets an `bb_token` cookie (same-site, path `/`) and loads the
workbench. Every API request also accepts the token as a query parameter,
`Authorization: Bearer`, or `X-Blackbox-Token`. The health endpoint is public;
all data endpoints require a token. Mutation endpoints (dump, demo trigger)
require the header form.

If the cookie is missing and the URL has no token, the workbench shows an error
instead of data. The server never binds a non-loopback interface.

## Top bar

- **Live indicator** — green when the SSE stream is open, yellow while
  connecting/reconnecting, grey for an offline dump.
- **Query field** — enter a predicate and press Enter or **Run**. Invalid
  queries mark the field and open the Query tab with the parser message.
- **Time window** — 1, 5, 15 or 60 minutes, or all retained records. Changing it
  refetches and restarts the stream with the same window.
- **Refresh** (`R`) — fetch a snapshot and refresh the dump list.
- **Dump** (`D`) — write every retained record to the configured directory and
  refresh the list.
- **Demo** — POST a trigger to the demo service (when attached).
- **Live** — shown while viewing a dump; returns to the live stream.

The status strip shows returned events, spans, errors, state changes, cumulative
dropped records, ring memory, sequence window and update time.

## Causal graph

Each node is a record. Edges are colour-coded:

| Edge | Colour | Meaning |
|---|---|---|
| cause | orange | `cause_seq` |
| parent span | blue | child span began inside the parent |
| span life | purple | span begin → matching span end |
| correlation | grey | consecutive records with the same correlation id |

The graph opens in **Scope: failure**, showing only the latest failure chain.
Press the scope button to show every node; selecting a record outside the chain
also widens the scope. Drag to pan, wheel to zoom, double-click or **Fit** to
fit the canvas, click a node to inspect it. The root cause has a red glow.

Dense windows are still dense in the full scope; the failure scope is the
intended starting point.

## Timeline and events

The timeline has four lanes: spans, errors, state changes, and logs/metrics.
Hover a marker for its label and time; click to select. The event list below is
newest-first, limited to 1200 records per snapshot. Error and warn rows are
tinted. The quick-filter row applies common predicates (`Errors`, `Warn+`,
`Spans`, `State`, `Metrics`).

## Request waterfall

One row per `span_begin`, sorted by monotonic start time. Indentation reflects
nesting depth; bar width is relative to the visible span range. Error and
timeout spans are red. The duration appears next to the name and in the tooltip.
Click a row to select the span begin record.

## Inspector tabs

### Failure

The latest error/timeout/failed span/abnormal state change is the starting
point. Blackbox walks backwards through cause, span-life, parent and correlation
edges, then marks the earliest anomaly as the root cause. The panel shows the
summary, clickable chain, findings and root cause. A chain is a heuristic, not a
formal proof.

### Event

All fields of the selected record, attributes, typed value, state old/new, a
**Copy JSON** button, cause-chain navigation and a parent-span link.

### State

State changes grouped by key, in first-seen order, with old → new values and
links back to the records.

### Dumps

Saved `.bbx` files with validity, record count, size and creation time. Actions:

- **Open** — load the bundle in recovery mode into the static dump view.
- **Download** — download the original bytes.
- **Dump now** — write the current retained window.
- **Compare** — pick two valid dumps; the Compare tab reports the first event
  shape or state divergence, ignoring timestamps and ids.

### Compare

Side-by-side dump metadata, event divergence (with the first differing index),
and state divergence (key, transition index, left/right values).

### Query

Current query, result count, parse error, recent queries (stored locally in
`localStorage`, maximum twelve), example queries and grammar help.

## Offline dumps

```sh
blackbox_launcher --no-browser --no-demo --open-dump /path/to/trace.bbx
```

The launcher points the dump browser at the file's directory and passes
`?dump=<name>` to the UI, which opens the bundle without starting the event
stream. **Back to live** starts the live view (empty without a demo, unless the
launcher is also recording through the library).

## Keyboard

| Key | Action |
|---|---|
| `/` | focus the query field |
| `Enter` in query | run |
| `R` | refresh snapshot and dumps |
| `D` | write a dump |
| `Esc` | clear selection / blur the field |
| `Enter` on a row | select it |

## Browser support and accessibility

The UI uses standard canvas, CSS grid, `fetch`, `EventSource`,
`ResizeObserver` and `localStorage`. Recent Chrome, Safari and Firefox are
targeted. All interactive controls are real buttons/inputs with focus-visible
styles; labels are associated; colour is supplemented by text statuses.

CSP is restricted to `'self'` scripts/styles/connections and data images. The
only local storage is optional query history.
