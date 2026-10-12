# Building anoa

Everything in this file is for people building from source or cutting releases.
If you only want to *use* anoa, the [README](../README.md) has prebuilt
packages for macOS, Linux and Windows and you never need to read this.

---

## Prerequisites

| Dependency | Version | Notes |
|---|---|---|
| Qt6 | ≥ 6.4 | Modules: WebEngineWidgets, WebEngineCore, Network, WebSockets, Widgets |
| CMake | ≥ 3.16 | Build system |
| C++ compiler | C++17 | GCC ≥ 10, Clang ≥ 12, MSVC 2022 |

There are no third-party dependencies beyond Qt6 — every subsystem is built on
Qt's own classes.

**Ubuntu**
```bash
apt install qt6-webengine-dev qt6-websockets-dev libqt6network6-dev
```

**macOS**
```bash
brew install qt
```

**Windows**
Use the [Qt Online Installer](https://www.qt.io/download). Select Qt 6.4+ with
WebEngine and the MSVC 2022 components.

---

## Build

```bash
git clone git@github.com:porcupine-md/anoa-browser.git
cd anoa

make                        # Debug build (dynamic linking)
make release                # Release build
make release-static         # Release build, statically linked
make install                # Install release to INSTALL_PREFIX (default: dist/)
```

All targets:

```
make                        # Debug build (dynamic)
make static                 # Debug build (static)
make release                # Release build (dynamic)
make release-static         # Release build (static)
make install                # Install release to INSTALL_PREFIX (default: dist/)
make install-static         # Install static release to INSTALL_PREFIX
make test                   # Build and run tests
make coverage               # Coverage report, gated at 80%
make clean-all              # Remove all build dirs and dist/
make help                   # Show all targets
```

Override variables:

```bash
make release-static QT_PREFIX=/path/to/qt JOBS=8 INSTALL_PREFIX=/opt/anoa
```

### Terminal mode is POSIX-only

`src/terminal/` is built on termios, `SIGWINCH` and `QSocketNotifier` over
stdin, none of which have an MSVC equivalent. On Windows those sources are left
out of the target entirely (`if(NOT WIN32)` in `CMakeLists.txt`) and
`anoa terminal` reports the unsupported platform at runtime. A syntax
check would not prove this, so `tests/integration/build_shape.test.sh` asserts
it negatively — every `src/terminal/` reference must sit inside the guard.

### Headless machines

On a CI runner with no GPU:

```bash
export QTWEBENGINE_CHROMIUM_FLAGS="--disable-gpu --no-sandbox"
./anoa --headless --port 9222
```

`--headless` selects the offscreen platform itself, so no display server is
needed on Linux and `DISPLAY` is never required on macOS. `--version` and
`--help` do the same, so they work on a machine with no display at all.

---

## Architecture

```
anoa
├── main.cpp                  # CLI parsing, application bootstrap
├── config/                   # Config struct from CLI flags + env vars
├── browser/
│   ├── anoa_browser          # QWebEngineView subclass, profiles, extensions
│   └── browser_window        # headed-mode chrome; wraps the view, never nests in it
├── common/url_input          # what a human typed -> something QUrl accepts
├── http/                     # QTcpServer — /json, /json/version, /json/list, /render/*
├── cdp/
│   ├── cdp_proxy             # QWebSocketServer bridge, session multiplexing, auth
│   └── cdp_extensions        # Profiler / HeapProfiler / Security domain stubs
├── terminal/                 # `anoa terminal` — POSIX only
│   ├── terminal_app          # event loop — QSocketNotifier(stdin) + frame QTimer
│   ├── terminal_ui           # termios raw mode, SIGWINCH, iTerm2/kitty image protocols or
│   │                         # ANSI half-block fallback, status bar, SGR mouse/key parsing
│   ├── frame_backend         # transport seam — frames out, input in
│   ├── render_http_client    # /render/* backend
│   ├── inprocess_frame_backend # embedded backend — grabs a browser in this process
│   ├── cdp_client            # --cdp transport — QWebSocket, id/response correlation
│   └── cdp_frame_backend     # --cdp backend — Page.captureScreenshot / Input.*
└── pdf/                      # Page.printToPDF interceptor
```

### The transport seam

`terminal_ui.cpp` is written against `FrameBackend` alone and never opens a
connection itself, which is what makes the three transports genuinely
swappable. The seam is asynchronous by necessity — a WebSocket transport cannot
answer a frame request synchronously without a nested event loop — so it is
`frameReady` / `frameFailed` signals rather than a blocking call.

### The chrome wraps the view

`BrowserWindow` is a parent of `AnoaBrowser`, never a container inside it.
`HttpServer` captures frames with `m_browser->grab()` and reports
`m_browser->width()/height()` as the viewport that `/render/click` coordinates
are measured in, so a toolbar living inside the view would appear in every
screenshot and shift every click by its own height.

---

## Releasing (maintainers)

1. Bump `project(anoa VERSION ...)` in `CMakeLists.txt` and merge it.
2. **Run the Release workflow on `workflow_dispatch` and wait for it to pass.**
   It builds and smoke-tests every package and publishes nothing — no release,
   no tap commit. This step is not optional; see the warning below.
3. Tag `vX.Y.Z` and push it — via GitHub (**Releases → Draft a new release →
   Choose a tag → Create new tag**) or `git tag vX.Y.Z && git push origin vX.Y.Z`.
4. CI builds Linux / macOS / Windows, signs and notarizes the macOS app,
   publishes the GitHub Release, and updates the Homebrew tap.

> **Why step 2 exists.** v0.4.1 shipped a macOS app that deleted itself on first
> run. The bundle contained an empty
> `QtWebEngineCore.framework/Versions/Resources` — an invalid framework layout —
> so Gatekeeper SIGKILLed the process and removed the app, and the user saw
> brew's *"It seems the App source is not there"* on their next upgrade.
>
> Every check passed. `spctl --assess` answered *"accepted, source=Notarized
> Developer ID"*. The stray directory came back **after** that guard and before
> `tar`, so the thing that was verified was not the thing that was published.
> The Package step now purges again, verifies the signature, and extracts the
> finished archive to verify that too — but the general rule is the one worth
> remembering: **verify the artifact, not the working tree.**

The tag name is what the built binary reports: CI passes it through
`ANOA_VERSION_OVERRIDE`. Keep the `CMakeLists.txt` version equal to it — a
mismatch shows up as a binary that disagrees with the release it came from.

Tag pushes **require** the macOS signing secrets (`MACOS_CERT_P12_BASE64`,
`MACOS_CERT_PASSWORD`, `APPLE_ID`, `APPLE_PASSWORD`, `APPLE_TEAM_ID`) — CI fails
fast if any are missing, so an unsigned build can never be released. Without
`HOMEBREW_TAP_TOKEN` the tap update is skipped with a warning rather than
failing the release.

To exercise the pipeline without publishing, run the Release workflow manually
(`workflow_dispatch`): it builds and smoke-tests every package, and creates no
release and no tap commit.

If the tap update was skipped or failed, resync it without re-running the
release: **Actions → Update Homebrew tap → Run workflow**, or
`gh workflow run update-homebrew-tap.yml -f tag=vX.Y.Z`. Leave `tag` empty to
sync the latest release; the run is idempotent and pushes nothing when the tap
already matches.

## Distro packages (OBS)

The recipes in `packaging/obs/` build anoa against the **distribution's own
Qt**, not a bundled copy — the opposite of every other channel here. The
openSUSE Build Service compiles and hosts the result for openSUSE, Fedora,
Debian and Ubuntu from that one set of files.

Publish a release to it the same way the tap is resynced: **Actions → Update
OBS → Run workflow**, or `gh workflow run update-obs.yml -f tag=vX.Y.Z`. Leave
`tag` empty for the latest release. The run is idempotent and commits nothing
when OBS already matches. It needs `OBS_USERNAME` and `OBS_PASSWORD` secrets
and an `OBS_PROJECT` variable; without them it warns and exits 0 rather than
failing, like the tap.

Two things a packager has to get right, both of which cost a late and
confusing failure when they are wrong:

- **`PORTABLE_RPATH=OFF`.** Left on, the installed binary carries an RPATH of
  `$ORIGIN/lib` naming a directory no package ships, and it does not start.
- **`CMAKE_BUILD_TYPE=RelWithDebInfo`, not `Release`.** `CMakeLists.txt` adds a
  link-time `-s` under `Release`, which strips the binary before rpm can
  extract debug information from it. The build then fails at packaging time
  with *"Empty %files file debugsourcefiles.list"* — a message that says
  nothing about the strip flag that caused it.

`scripts/validate_packaging.sh` checks both, plus that every path the spec
claims is one CMake actually installs and that the Qt floor in the recipes
matches `find_package(Qt6 ...)`. It runs in CI on every push and again on the
rendered files before they are pushed to OBS — verify the artifact, not the
working tree.

**Ubuntu 22.04 cannot be targeted.** It ships Qt 6.2, below the 6.4 floor the
build declares. Debian 12 ships exactly 6.4.2 and works; Ubuntu 24.04 works.
22.04 users want the AppImage, which exists precisely because its glibc 2.35
floor reaches further back than a distro package can.

### Verifying a release actually shipped

Green CI is not the same as a working install. Worth checking by hand:

```bash
# the tap's checksum must match the published asset, or brew install fails for everyone
curl -fsSL https://github.com/porcupine-md/anoa-browser/releases/download/vX.Y.Z/anoa-macos-universal.tar.gz \
  | shasum -a 256

# and what brew now offers
brew update && brew info --cask porcupine-md/tap/anoa
```

---

## Tests

```bash
make test        # unit suites (QTest + CTest)
make coverage    # same, with a coverage gate at 80%
```

The container suite needs an image rather than a build:

```bash
docker build -t anoa:test .
tests/e2e/container_e2e.sh anoa:test
```

It picks whichever of docker or podman can actually see that tag, because on a
machine with both they keep separate image stores and "docker exists" is no
reason to believe docker has the image podman just built.

Integration and regression suites live in `tests/`:

| Suite | Runner | Notes |
|---|---|---|
| `tests/unit/` | QTest + CTest | config parsing, PPM decoding, the terminal UI |
| `tests/integration/*.test.js` | vitest | HTTP, CDP proxy, profiles, PDF, extensions |
| `tests/integration/*.test.sh` | bash | port layout, build shape, Qt floor |
| `tests/e2e/` | Playwright + Puppeteer | connect over CDP to a running binary |
| `tests/regression/smoke.sh` | bash | end-to-end sanity |
| `tests/e2e/container_e2e.sh` | bash | the container image, against a real release |

The bash suites need `nc` (netcat) to wait for readiness — without it every
browser-launching case fails identically and the errors look like GPU problems.

**On macOS two suites cannot run.** `terminal_http.test.js` and
`terminal_cdp.test.js` drive a pty through GNU `script -q -c`; macOS ships BSD
`script`, which has no `-c`, so both skip themselves. `port_layout.test.sh`
fails several cases on bash 3.2 (macOS's `/bin/bash`) because an empty array
expansion under `nounset` is an error there. Neither is a behaviour difference —
check both on Linux or in CI before believing a regression.


Four static libraries carry the logic the unit suite can reach, and every one of
them is Qt6::Core only because the unit CI job builds no WebEngine at all:

| Target | Source |
|---|---|
| `anoa-config-lib` | `src/config/config.cpp` |
| `anoa-terminal-bytes-lib` | `src/terminal/frame_bytes.cpp` |
| `anoa-terminal-ui-lib` | `src/terminal/terminal_ui.cpp` |
| `anoa-tab-ids-lib` | `src/browser/tab_ids.cpp` |

A source never joins an existing one of these to save a target: they are separate
so that adding a WebEngine dependency to any of them fails loudly rather than
quietly pulling WebEngine into the unit job. `make coverage` measures all four,
and the `COVERAGE_MIN` gate applies to each.
