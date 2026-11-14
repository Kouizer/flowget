# app/

Flutter application for Windows and Android.

Arrives in **M3** (Windows) and **M6** (Android):

- Classic download-manager layout: category tree on the left, transfer list on the right.
- Per-segment progress visualisation driven by engine events (100 ms coalescing).
- Turkish and English UI, light/dark theme, tray icon on Windows, notification actions on Android.
- On Android the engine lives in a `dataSync` foreground service; the UI attaches to it (ADR-0006).

Nothing here yet on purpose: the UI is built on a stable engine and a frozen C ABI, not in parallel
with them.
