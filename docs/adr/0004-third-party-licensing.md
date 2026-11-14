# ADR-0004 — Third-party licensing policy

- **Status:** Accepted
- **Date:** 2026-11-14
- **Deciders:** project owner, engineering

## Context

FlowGet is MIT-licensed and will be distributed as binaries (Windows installer, APK). Media tooling
in this space is a licensing minefield: yt-dlp is Unlicense, FFmpeg has LGPL and GPL builds, and the
convenient Android yt-dlp wrapper (`youtubedl-android`) is GPL-3.0.

## Decision

1. **No GPL/AGPL code may be linked into the application.** CI fails if a dependency manifest
   declares one.
2. FFmpeg ships as an **LGPL-2.1-or-later** build, linked **dynamically**, with license text, a
   written offer of source, and no GPL-only components (no `--enable-gpl`, no x264).
3. yt-dlp is invoked as a **separate process**, never embedded or linked. It keeps its own license
   and its own update cadence.
4. On Android, yt-dlp is not shipped at all in the default build (see ADR-0006).
5. `THIRD_PARTY_LICENSES.md` is generated in CI and reviewed before a release; every new dependency
   requires an ADR amendment or a note in the review.

## Consequences

- The MIT license stays truthful; users can embed FlowGet in other products.
- We lose some convenience: no `youtubedl-android`, no GPL encoders, a separate yt-dlp process to
  supervise and update.
- A "GPL variant" Android build is possible later, but only as a separate artifact with its own
  repository, notices, and source offer — never as a flag in the main build.

## Verification

- `scripts/check_dependencies.py` (M1) inspects `vcpkg.json`, `pubspec.yaml`, and `package.json`
  against a denylist and runs on every PR.
- Release checklist includes `ffmpeg -L` output and an `ldd`/`dumpbin` scan for unexpected GPL libs.
