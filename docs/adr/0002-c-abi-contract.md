# ADR-0002 — The C ABI is the only integration boundary

- **Status:** Accepted
- **Date:** 2026-11-14
- **Deciders:** project owner, engineering

## Context

The engine is consumed by a Dart/Flutter UI, by a CLI, and (on Android) potentially by a foreground
service in a different process. The integration mechanism must survive UI restarts, must not copy
per-chunk data, and must not leak C++ types into Dart.

## Decision

All clients talk to the engine through the C ABI in `core/include/flowget/flowget.h`.

Rules:

1. `extern "C"`, no exceptions across the boundary, no C++ types in signatures.
2. Opaque handles (`flowget_engine*`), owned by the caller, destroyed by a matching `_destroy`.
3. Structs that may grow carry `struct_size` as their first field; readers reject unknown sizes.
4. Every fallible call returns `flowget_status`; human-readable detail is retrieved with
   `flowget_last_error(handle)`.
5. Progress is pushed: one callback registered at engine creation, invoked from the engine thread.
   Dart uses `NativeCallable.listener`, so no UI isolate is blocked and no polling timer exists.
6. Calls are thread-safe; callbacks are serialized. A callback must not call back into the engine
   (documented deadlock).
7. `flowget_api_version()` is checked by every client at startup.

## Consequences

- The same command/event vocabulary can later be carried over Binder on Android without redesigning
  the UI layer.
- Adding a field to a snapshot struct is source-compatible; removing or reordering is not.
- Debugging requires keeping the header and the Dart bindings in sync; `ffigen` generates the Dart
  side from the header, and a CI test asserts `flowget_api_version()` matches the Dart constant.

## Rejected alternatives

| Option | Why rejected |
|---|---|
| MethodChannel per event | Copies every progress update through the platform channel; measurable at 20 MB/s |
| Local HTTP/WebSocket bridge | Extra port, token handling, battery cost on Android, process supervision |
| Direct C++ <-> Dart bindings (`dart:ffi` with C++ classes) | Fragile ABI, no stable name mangling |
