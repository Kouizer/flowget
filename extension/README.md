# extension/

Manifest V3 browser extension for Chrome, Edge, and Firefox.

Arrives in **M4**:

- Observes media requests (`webRequest` observation only — blocking is enterprise-only in MV3) and
  scans the DOM for `<video>`/`<source>` plus performance resource entries.
- Context menu and "Download with FlowGet" action.
- Sends `{url, referer, cookies, filename}` to the native host over stdio JSON framing; the engine
  does the actual transfer.

One source manifest is transformed into `manifest.chromium.json` and `manifest.firefox.json` at build
time. Firefox for Android cannot use native messaging, so mobile links travel through the OS share
sheet instead (ADR-0006).
