# ADR-0001 — HTTP stack: libcurl multi interface

- **Status:** Accepted
- **Date:** 2026-11-14
- **Deciders:** project owner, engineering

## Context

The engine needs, on both Windows and Android: HTTP/1.1 and HTTP/2, `Range` requests, resume,
proxies, cookies, custom headers, TLS with a system trust store, keep-alive, and up to 16 concurrent
connections per download. The two realistic options are libcurl's multi interface and a hand-built
Boost.Asio + OpenSSL (+ nghttp2) stack.

## Decision

Use **libcurl** with the multi interface, driven from a single engine thread.

`HttpClient` is an internal interface so the transport is replaceable; v1 ships exactly one
implementation.

## Rationale

- Range/resume/proxy/cookie/TLS are solved problems in libcurl. Re-implementing them is 3-6 weeks of
  work plus permanent maintenance, and it is not what makes FlowGet different.
- `curl_multi_poll` + `curl_multi_wakeup` gives an event loop with no polling latency and no
  thread-per-segment model; 16 connections are cheap.
- libcurl on Android via NDK is a well-trodden path; Boost + OpenSSL cross-compilation is not.
- License: curl (MIT-like), nghttp2 (MIT). Compatible with the MIT project.

## Consequences

- We must handle several non-obvious defaults, documented in `docs/architecture-review.md` §3.1:
  `CURLMOPT_MAX_HOST_CONNECTIONS` defaults to 6; HTTP/2 multiplexing would collapse all segments
  onto one TCP connection (segment requests therefore force HTTP/1.1); `Accept-Encoding` must be
  `identity` to keep byte offsets meaningful.
- HTTP/3, if ever needed, is a curl build option rather than a rewrite.
- Debugging is C-style: every handle's options are set explicitly and reviewed in one place.

## Alternatives considered

| Option | Why rejected |
|---|---|
| Boost.Asio + OpenSSL + nghttp2 | 3-6× more code, cross-compilation burden, no functional gain for v1 |
| Platform APIs (WinHTTP / Cronet) | Two implementations, weaker resume/range control, Android Cronet is an extra dependency |
| Rust `reqwest`/`hyper` core | Would split the codebase; not justified by the requirements |
