# FlowGet

A fast, open-source download manager for **Windows** and **Android**.

FlowGet uses multiple parallel HTTP range requests, dynamically rebalances segments, and resumes
downloads across restarts. A browser extension sends links and media streams straight to the app,
and the desktop build can hand supported sites to [yt-dlp](https://github.com/yt-dlp/yt-dlp).

> **Status: M1 in progress.** Dependencies are pinned in `vcpkg.json`, and
> `flowget-cli get <url> -o <file>` downloads over a single connection. Segmented downloads, resume
> and rate limits are next. See [`tasks/todo.md`](tasks/todo.md) for the live milestone plan.

## What it is / what it is not

| Planned | Explicitly out of scope |
|---|---|
| Segmented HTTP/HTTPS downloads with resume | DRM circumvention (Widevine, FairPlay, PlayReady) |
| Queueing, speed limits, scheduling | Torrents / magnet links |
| HLS and DASH via yt-dlp and native segment downloaders | Live stream recording |
| FFmpeg-based conversion (desktop) | Bypassing paywalls or access controls |
| Browser integration (Chrome, Edge, Firefox) | Cloud sync, accounts, telemetry |

FlowGet does **not** claim to download "every video". Media support is bounded by what yt-dlp
supports, and by the formats we can fetch without breaking encryption.

## Repository layout

```
core/          C++20 download engine, platform independent, exposes a C ABI
cli/           flowget-cli — used by tests and as the M1 reference client
bindings/      dart:ffi bindings (M3)
app/           Flutter application for Windows and Android (M3+)
extension/     Manifest V3 browser extension (M4)
native_host/   Native Messaging host + installer (M4)
tests/         unit and integration tests, local HTTP/media fixtures
docs/          architecture notes and ADRs
tasks/         roadmap, lessons learned
```

## Build (Windows)

Prerequisites:

- Visual Studio 2022/2026 with the **Desktop development with C++** workload
- CMake ≥ 3.25 and Ninja on `PATH` (both ship with Visual Studio)
- A *Developer PowerShell* (so the MSVC environment is initialized)
- [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` pointing at it. Dependencies are
  declared in `vcpkg.json` and installed automatically on the first configure
  (`x64-windows-static-md`, so no DLLs have to be deployed next to the binaries):

  ```powershell
  git clone https://github.com/microsoft/vcpkg "$env:USERPROFILE\vcpkg"
  & "$env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat" -disableMetrics
  setx VCPKG_ROOT "$env:USERPROFILE\vcpkg"   # once, then reopen the shell
  ```

  <details>
  <summary>Visual Studio's bundled CMake rewrites <code>VCPKG_ROOT</code></summary>

  The `cmake.exe` shipped with Visual Studio sets `VCPKG_ROOT` to VS's own vcpkg copy in the
  environment its configure step sees (`cmake -E environment` shows it, even when the shell reports
  your path). Set `FLOWGET_VCPKG_ROOT` instead — the build prefers it over `VCPKG_ROOT`:

  ```powershell
  setx FLOWGET_VCPKG_ROOT "$env:USERPROFILE\vcpkg"
  ```

  </details>

  A checkout below a non-ASCII path also works: vcpkg writes absolute paths into its generated
  `*Config.cmake` files, which Windows parses in the ANSI code page, so CMake relocates the vcpkg
  install tree to `%LOCALAPPDATA%\FlowGet\vcpkg-installed` in that case.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
ctest --preset windows-x64
./build/windows-x64/cli/flowget-cli.exe --version
```

Formatting and hygiene checks use the pinned versions in `.pre-commit-config.yaml`:

```powershell
pipx install pre-commit   # once
pre-commit run --all-files
```

## Build (Linux)

`VCPKG_ROOT` must be set here too (same manifest, `x64-linux` triplet):

```bash
git clone https://github.com/microsoft/vcpkg "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
export VCPKG_ROOT="$HOME/vcpkg"
```

```bash
cmake --preset linux-x64
cmake --build --preset linux-x64
ctest --preset linux-x64
```

## Documentation

- [Architecture review and technology decisions](docs/architecture-review.md)
- [Architecture Decision Records](docs/adr/)
- [Roadmap](tasks/todo.md)

## License

FlowGet is released under the [MIT License](LICENSE).

Bundled third-party components keep their own licenses (FFmpeg is LGPL-2.1-or-later and is linked
dynamically; yt-dlp is invoked as a separate process). See `THIRD_PARTY_LICENSES.md` (added in M7).

## Legal notice

Downloading copyrighted material without permission may be illegal in your jurisdiction. FlowGet is
a general-purpose download tool; you are responsible for how you use it and for complying with the
terms of service of the sites you visit. The maintainers do not support or enable DRM circumvention.
