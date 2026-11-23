# Documentation

Start with the repository [README](../README.md) for build and launch commands.

| Document | Contents |
|---|---|
| [architecture.md](architecture.md) | modules, data flow, ring, recorder, server, failure behaviour |
| [query.md](query.md) | query grammar, fields, operators, semantics, limits |
| [dump-format.md](dump-format.md) | `.bbx` header, frames, records, footer, recovery |
| [crash-safety.md](crash-safety.md) | signal-safe design, testing and limitations |
| [embedding.md](embedding.md) | C++ API, lifetimes and thread-safety rules |
| [ui.md](ui.md) | every workbench panel and control |
| [design-decisions.md](design-decisions.md) | trade-offs and rejected alternatives |
| [performance.md](performance.md) | benchmark methodology and reference results |
| [testing.md](testing.md) | test inventory, sanitizers, fuzzing, soak, benchmarks |
| [packaging.md](packaging.md) | install, CMake package, shared/static ABI scope |
| [integration.md](integration.md) | adapters for requests, DB calls, retries and shutdown |

Project-level records:

- [CHANGELOG](../CHANGELOG.md)
- [LIMITATIONS](../LIMITATIONS.md)
- [SECURITY](../SECURITY.md)
- [FINAL_REPORT](../FINAL_REPORT.md)
