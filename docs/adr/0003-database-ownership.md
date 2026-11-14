# ADR-0003 — SQLite is owned exclusively by the C++ core

- **Status:** Accepted
- **Date:** 2026-11-14
- **Deciders:** project owner, engineering

## Context

Both the Flutter side (list, settings) and the engine (segment bitmaps, checkpoints) need persistent
state. Two SQLite writers with two schemas is a classic source of `SQLITE_BUSY`, divergent
migrations, and resume bugs that only appear after a crash.

## Decision

The core owns the database file. Dart never opens it.

- Schema lives in `core/db/` with forward-only migrations tracked by `PRAGMA user_version`.
- WAL journal, `busy_timeout = 5000`, one writer (the engine thread).
- The engine checkpoints segment state every 250 ms and on pause/cancel/shutdown, with `fsync`
  before reporting success.
- The UI receives snapshots and events through the C ABI; it does not query SQL.
- Small UI-only preferences (theme, language, window geometry) use the platform preference store,
  not the download database.

## Consequences

- Crash recovery loses at most 250 ms of progress and can never leave a half-written bitmap.
- Query changes never require a Dart migration; the ABI is the contract.
- The UI cannot run ad-hoc queries. If a new view is needed, it becomes a snapshot field — a
  deliberate cost that keeps one source of truth.

## Rejected alternatives

| Option | Why rejected |
|---|---|
| Dart owns the DB (drift), engine asks via FFI | Chatty during checkpoints; engine cannot recover alone |
| Both write, different tables | Two migrations, two writers, no clear owner of invariants |
| Flat files per download | No queue queries, no atomic multi-field updates |
