# Integrating with a real service

Blackbox is manual instrumentation, not an auto-instrumenting agent. It does not
patch HTTP frameworks, database drivers or job schedulers. The intended pattern
is a small adapter at the request/job boundary plus spans around the calls you
already care about.

## Request boundary

Use one `RequestScope` per request, on the thread that handles it:

```cpp
void handle_request(const HttpRequest& request) {
  bb::RequestScope scope(recorder);          // new correlation, clean context
  bb::Span trace_span = recorder.span(
      "http.request",
      {{"http.route", bb::Value::string(request.path())},
       {"http.method", bb::Value::string(request.method())}});

  // ... call the handler ...
  trace_span.set_status(ok ? bb::Status::Ok : bb::Status::Error);
}
```

If you use a thread pool, call `reset_thread_context()` or `RequestScope` at the
start of every task. Do not carry one task's span handle into another task.

## HTTP middleware

Wrap the framework's existing middleware entry point. Common attributes:
`http.route`, `http.method`, `http.status`, `http.bytes`. On a downstream error,
emit `recorder.error(...)` with `Status::Timeout`/`Error` and set the span status.

If the framework retries, give each attempt its own child span and keep the
correlation id from the original request.

## Database calls

```cpp
{
  bb::Span query = recorder.span("db.query",
      {{"db.system", bb::Value::string("postgres")},
       {"db.statement", bb::Value::string(stable_statement)}});
  // execute
  if (timed_out) {
    recorder.error("database timeout", bb::Status::Timeout,
                   {{"db.timeout_ms", bb::Value::integer(timeout_ms)}});
    query.set_status(bb::Status::Timeout);
  }
}
```

Attribute values are bounded; pass a normalized statement or operation name, not
an unbounded SQL string.

## Retry, breaker and fallback

The demo shows the intended causal shape:

```text
request -> db.query (timeout) -> retry (timeout)
        -> state breaker closed -> open
        -> fallback.cache -> request complete
```

Emit a `state_change` for every breaker transition and give the state a short,
stable key (`breaker.state`, not per-customer names). The explainer looks for
timeouts, failed spans and abnormal state values.

## Background jobs and threads

- Set a fresh correlation for each job.
- Use one span per job attempt; do not share `Span` objects across threads.
- Emit metrics with stable names and bounded attribute sets.
- Avoid high-cardinality attributes (user IDs, raw URLs, request bodies); they
  consume attribute space and make the UI noisy.

## Shutdown

Stop producers before destroying the `Recorder`:

```cpp
pool.stop();                 // join request workers
demo.stop();                 // any helper threads
auto dump = recorder.flush_dump();  // optional final bundle
```

`Span` destructors are safe no-ops if the recorder has already been destroyed,
but other recorder calls and the thread-local context still require a live
recorder. `UiServer` must be stopped before the recorder is destroyed.

## Failure handling

- Recording APIs return `false` for invalid or oversized input; inspect
  `Recorder::stats()` if you care.
- Dump failures (`disk full`, directory removed) are returned in
  `DumpResult::error`; recording continues.
- The crash handler is best-effort and one per process. Disable it if the host
  already owns fatal-signal reporting.
- The UI is an observer; it is not on the service's request path.

## What not to instrument

- Secrets, tokens, full request/response bodies or unbounded user text.
- Per-request high-cardinality metric attributes.
- Tight loops at every iteration; aggregate counters instead.
- Every internal function. Start with the request, outbound calls, retries,
  breaker transitions and fallback paths.

## Verification

Add a test that runs one request through your adapter and asserts a causal chain
from the request span to the intentional error and fallback. The repository's
`test_demo.cpp` and `test_causal.cpp` are small examples.
