# bindings/

Dart bindings for the engine's C ABI (`core/include/flowget/flowget.h`).

Arrives in **M3**:

- `ffigen.yaml` — generates the raw `dart:ffi` layer from the header.
- `lib/src/engine.dart` — hand-written, safe wrapper (handle lifetime via `Finalizer`, typed
  commands, `Stream<EngineEvent>` backed by `NativeCallable.listener`).
- `test/` — ABI conformance tests against the real shared library, including the
  `flowget_api_version()` match check required by ADR-0002.

Nothing here yet on purpose: the ABI grows with the engine, and bindings are generated, not authored.
