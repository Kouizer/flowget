# ADR-0006 — Android media pipeline: Media3 + native HLS/DASH, no embedded yt-dlp

- **Status:** Accepted
- **Date:** 2026-11-14
- **Deciders:** project owner, engineering

## Context

Desktop can delegate site extraction to yt-dlp as a child process. Android needs an answer to three
separate questions: how to run yt-dlp, how to convert media, and how to reach the app from a browser.

Facts that constrain the design:

- `youtubedl-android` bundles CPython and yt-dlp **as a linked library** and is **GPL-3.0** → it would
  relicense the whole app.
- `ffmpeg-kit` was retired in January 2025 and its repository is archived; community rebuilds have
  unclear maintenance and licensing.
- Chrome for Android does not support extensions; Firefox for Android does, but WebExtensions there
  cannot use native messaging.
- Android 15 limits `dataSync` foreground services to 6 hours per 24 hours and forbids starting them
  from `BOOT_COMPLETED`.

## Decision

1. **No yt-dlp on Android in v1.** Direct media URLs plus native HLS/DASH segment downloaders cover
   the honest use cases. Considered again only as a separate GPL-3.0 artifact, post-v1.
2. **Conversion:** AndroidX Media3 `Transformer` (Apache-2.0) for remux/transcode of common cases.
   A self-built LGPL FFmpeg is the fallback if a required format is missing — decided by measurement,
   not by default.
3. **Browser integration:** `ACTION_SEND` (text/plain) share intent is the primary path; `ACTION_VIEW`
   for http(s) links. A Firefox Android extension may later talk to a token-protected loopback bridge
   hosted by the foreground service; it is not part of v1.
4. **Background execution:** downloads run inside a `dataSync` foreground service that owns the
   engine; the UI attaches to it. `onTimeout` checkpoints and stops cleanly.

## Consequences

- Smaller APK (~no Python), clean licensing, simpler support matrix.
- Users cannot paste a YouTube URL on Android and expect a download; the UI must say so explicitly
  instead of failing mysteriously.
- The service/UI split must exist from M3 in the ABI design, even though Windows runs in-process.

## Rejected alternatives

| Option | Why rejected |
|---|---|
| `youtubedl-android` (GPL-3.0) | Relicenses the app; violates ADR-0004 |
| Termux + yt-dlp integration | Unacceptable UX, unsupported environment |
| Bundled GPL FFmpeg | Same licensing problem, plus APK size |
| Cronet/ExoPlayer downloader only | No resume control at segment granularity, weaker queueing |
