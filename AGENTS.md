# AGENTS.md — anoa

> This file is human-curated project knowledge for AI agents.
> Agents may propose updates, but humans approve them.
> Research shows human-written AGENTS.md improves agent success ~4%.

---

## Project Overview

- **Name**: anoa (binary: `anoa`)
- **Type**: native desktop/CLI binary — a browser automation host, not a Node service
- **Stack**: C++17 + Qt 6 (Widgets, WebEngineWidgets, WebEngineCore, Network, WebSockets), built with CMake ≥ 3.16. `find_package(Qt6 6.4 REQUIRED ...)` is the floor; CI pins 6.7.3. Node is used **only** for the test suites under `tests/`.
- **Description**: A Qt WebEngine browser that exposes the live page over an HTTP `/render/*` + screenshot API and an authenticated CDP WebSocket proxy, and can render that page into a terminal via `anoa terminal`.

---

## Conventions

### File Structure
```
src/
├── main.cpp        # entry point + the raw-argv pre-scan that selects the mode
├── browser/        # AnoaBrowser — the QWebEngineView host, profiles, extensions
├── cdp/            # CdpProxy (authenticated WebSocket proxy) + CDP extension methods
├── config/         # Config struct, parseArgs(), loadConfigFile() — shared by both modes
├── http/           # HttpServer — /json/*, /render/*, screenshot and input endpoints
├── pdf/            # PdfHandler — print-to-PDF
└── terminal/       # `anoa terminal`: viewer UI, HTTP transport, CDP transport
resources/          # .desktop file, SVG icon, anoa.sh bundle launcher
tests/
├── unit/           # QTest + CTest (test_config.cpp, its own CMakeLists.txt)
├── integration/    # vitest (*.test.js) + two bash suites (*.test.sh)
├── e2e/            # Playwright (TS) and Puppeteer (JS) against a running binary
└── regression/     # smoke.sh — fast post-commit check of the 5 critical paths
.github/
├── workflows/      # ci.yml, release.yml, update-homebrew-tap.yml
├── homebrew/       # anoa.rb.tpl (cask), anoa-linux.rb.tpl (formula)
└── entitlements/   # anoa.entitlements for macOS codesigning
```

### Naming Conventions
- Source files: `snake_case.cpp` / `snake_case.h`, one directory per subsystem
- Classes / structs: `PascalCase` — Qt classes keep their `Q` prefix
- Methods and locals: `camelCase`
- Member variables: `m_camelCase`
- Constants: `kCamelCase` for file-local `constexpr` values, `UPPER_SNAKE_CASE` for macros
- CLI flags: `kebab-case`; HTTP endpoints: `kebab-case` under `/render/*`, `/json/*`
- **String literals in `src/` must be ASCII-only** (this also builds on MSVC). Write
  UTF-8 bytes as escapes (`"\xE2\x80\x94"`). Non-ASCII in comments is fine.

### Code Patterns
- Every subsystem is a `QObject` owning its own Qt resources; parent-child ownership,
  not manual `delete`.
- Signals/slots across a seam, never a blocking call: `FrameBackend` requests a frame
  and is answered by `frameReady` / `frameFailed`, because a WebSocket transport
  cannot answer synchronously without a nested event loop.
- Config flows one way: `parseArgs()` → `Config` struct → constructors. Nothing
  re-reads argv.
- Validation failures in `parseArgs()` print one line to stderr and `::exit(1)`.
  Runtime failures inside terminal mode instead ask `exec()` to return non-zero so
  the termios/alt-screen restore still runs.
- `-Wall -Wextra -Wpedantic` is on for the main target and the tree is warning-clean;
  keep it that way.
- **Any POSIX call whose name collides with a `QObject` member must be `::`-qualified
  inside a `QObject` subclass** (`::connect`, `::bind`, `::listen`, `::accept`) —
  unqualified lookup finds `QObject::connect` and stops.

### Testing Conventions
- **Unit (QTest + CTest)**: `make test` — it re-configures with `-DBUILD_TESTS=ON`,
  builds and runs `ctest --output-on-failure`. A plain `make build` leaves
  `BUILD_TESTS` OFF, so the test target does not exist. Sources live in `tests/unit/`.
  `parseArgs()` cannot be tested in-process (it reads argv off the live
  `QCoreApplication` and exits on error), so those cases re-invoke the test binary
  through `QProcess` with `ANOA_TEST_HARNESS=parse_args` and parse a JSON line
  off stdout.
- **Integration (vitest)**: `cd tests/integration && npm install && npx vitest run`.
  `vitest.config.js` sets `fileParallelism: false` on purpose — each file spawns its
  own `anoa` on the same port triplet, so concurrent files would bind-clash
  or silently talk to the wrong instance. Binary and port come from `ANOA_BINARY` /
  `ANOA_PORT`.
- **Integration (bash)**: `bash tests/integration/port_layout.test.sh` and
  `bash tests/integration/extensions.test.sh`, both taking
  `ANOA_BINARY=./build/anoa`.
- **E2E**: `cd tests/e2e && npm install`, then `npx playwright test` (needs
  `npx playwright install --with-deps chromium`) and `node --test puppeteer.test.js`.
  Both attach to an already-running `anoa`; the test does not start it.
- **Regression**: `bash tests/regression/smoke.sh` — same `ANOA_BINARY`/`ANOA_PORT`
  contract.
- Tests run against a **real browser process**, never a mock. Each vitest and bash
  file starts and kills its own instance; the e2e suites are the exception and attach
  to one started outside them.
- Terminal-mode tests need a **pty**: `terminal_app.cpp` refuses to start unless both
  stdin and stdout are a terminal, so a pipe exercises nothing. `terminal_cdp.test.js`
  uses `script -q -e -c` from util-linux (skipped on macOS, whose `script` has no
  `-e`) and kills the process *group*, not just `script`.

---

## Known Gotchas

- **A frame is device pixels; every input endpoint is logical pixels.**
  `QWidget::grab()` returns a pixmap at the display's devicePixelRatio, so on a
  HiDPI screen `/render/screenshot.png` and `/render/stream.mjpeg` are twice the
  width of the coordinate space `/render/click` and friends speak. A client that
  maps a click through the image it was given lands at double the intended
  point, on a page that looks like it just ignored the click. `GET
  /render/viewport` exists to answer this and the `X-Anoa-Viewport-*` headers
  carry the same numbers — use one of them, never the image's own dimensions.

- **Grabbing the container is not grabbing a tab.** `AnoaBrowser` is a container
  whose active view is raised, so `browser->grab()` returns whatever is on top,
  regardless of the `?tab=` that was resolved for the request. The MJPEG stream
  did exactly this and so accepted `?tab=` and silently ignored it. Grab the
  view `resolveRenderTab()` returned.

- **Never tag a release without running the Release workflow on
  `workflow_dispatch` first.** It builds and smoke-tests every package and
  publishes nothing, so a broken package is caught before it reaches anyone.
  Skip it and the first person to find out is a user.

  This is not hypothetical. **v0.4.1 shipped a macOS app that deleted itself.**
  The bundle carried an empty
  `QtWebEngineCore.framework/Versions/Resources`; a framework may only hold
  version directories under `Versions/`, so `codesign --verify --deep --strict`
  reported *"embedded framework contains modified or invalid version"* and
  Gatekeeper on current macOS SIGKILLed the process **and removed the app** —
  which then surfaced to the user as brew's *"It seems the App source
  '/Applications/anoa-browser.app' is not there"*.

  Every check in CI passed. `spctl --assess` answered *"accepted, source=
  Notarized Developer ID"*. The directory reappeared **after** that guard and
  before `tar`, so every assurance was about a bundle that was no longer the one
  being shipped. The Package step now purges again, verifies the signature, and
  then extracts the finished archive and verifies *that*. The lesson generalises:
  **verify the artifact, not the working tree** — green CI is not evidence that
  what you published works. See `docs/BUILDING.md` → Releasing.

- **Terminal options are CLI-only.** `--term-host`, `--term-port`, `--term-token`,
  `--fps`, `--gfx` and `--cdp` are read by `parseArgs()` only; `loadConfigFile()` is
  deliberately *not* extended with them, so nothing in `--config` can set them.
- **The CDP `Input.dispatchMouseEvent` `deltaY` sign is inverted relative to
  `/render/scroll`.** Qt's `angleDelta` says which way the wheel turned (+120 = up);
  the DOM/CDP `deltaY` says how far the content should move (+120 = down). The seam
  carries the Qt convention, so `cdp_frame_backend.cpp` sends `deltaY = -dy`. That
  conversion happens at that boundary and nowhere else.
- **CDP coordinates are CSS pixels, screenshots are device pixels.** Every
  `Input.*` x/y must be divided by the deviceScaleFactor. `Page.getLayoutMetrics`
  does *not* return that factor — it is the ratio of `layoutViewport` to
  `cssLayoutViewport`, and endpoints predating Chrome's 2020 `css*` fields report
  both halves in CSS pixels, so the ratio is 1 while the screenshot is still 2x.
  See `viewportForImage()` before touching any of this.
- **`tests/unit` builds `anoa-config-lib` from `src/config/config.cpp` alone,
  against `Qt6::Core` only.** Never add terminal (or browser, or http) sources to
  that target, and never make `config.cpp` depend on QtGui/QtNetwork/QtWebSockets —
  the unit test job builds no WebEngine.
- **`anoa` uses three ports, not one.** `--port 9222` means HTTP on 9222,
  Chromium's internal DevTools on 9223 and the `CdpProxy` on 9224; `HttpServer`
  rewrites `webSocketDebuggerUrl` from +1 to +2 so clients land on the authenticated
  proxy. A terminal session started with `--cdp http://host:9222` therefore ends up
  dialling `ws://host:9224/...` — the host:port changing mid-session is correct.
- **Check `\since` before using any Qt API newer than 6.4.** The floor is
  `find_package(Qt6 6.4)` and distro builds really do use 6.4; CI's 6.7.3 will not
  catch the regression. `QWebSocket::errorOccurred` (6.5+) already needed a
  `QT_VERSION_CHECK` fallback.
- `parseArgs()` prints `Warning: --auth-token is not set; ...` unconditionally, so
  `anoa terminal` prints it too even though terminal mode starts no CDP
  server. Known noise — tests filter stderr on the `anoa terminal: ` prefix
  rather than counting lines.
- **"Browser failed to start" from a bash suite usually means `nc` is missing, not
  that the browser is broken.** `wait_for_port()` is `while ! nc -z ...` in
  `port_layout.test.sh`, `extensions.test.sh` and `smoke.sh`; with no netcat the
  loop can never succeed and every browser-launching case fails identically. A
  headless run also prints `QRhiGles2: Failed to create context`, `QVulkanInstance:
  Failed to initialize Vulkan` and `Unable to detect GPU vendor` on stderr, which
  makes "this box has no GPU, WebEngine can't come up" look like the obvious
  explanation. It is not — WebEngine falls back to software and `/json/version`
  answers in about two seconds. Before blaming the environment, `curl` the port by
  hand; a wrong call here gets written down as "do not chase this" and hides the
  next real regression.

---

## Architecture Decisions

### One binary, two modes (the merged-binary convention)

- **There is exactly one executable, `anoa`.** The terminal viewer used to be
  a second binary (`anoa-term`); it was merged in and the separate target is gone.
  `add_executable` appears once in the top-level `CMakeLists.txt`.
- **`terminal` is a bare positional word, detected by a raw-argv pre-scan in
  `src/main.cpp` before any application object exists.** It cannot be a
  `QCommandLineParser` positional: the parser needs a live `QCoreApplication`, and
  which application class to construct is precisely what the word decides. The
  pre-scan also removes the word from argv (shift left, `argv[--argc] = nullptr`,
  then re-examine the current slot) so the parser only ever sees options and
  `--help` does not echo it back. `--headless` is scanned in the same loop for the
  same reason — `QT_QPA_PLATFORM` must be set before `QApplication`.
  Known limitation, accepted: the scan has no option-arity knowledge, so
  `--profile-name terminal` is read as the subcommand.
- **Terminal mode constructs a `QCoreApplication`, browser mode a `QApplication`.**
  The primary use case is SSH with no display, where `QApplication` aborts unless
  `QT_QPA_PLATFORM` is set. `QImage` decode/scale/convert under a plain
  `QCoreApplication` is verified by unit tests, not assumed.
- **Terminal sources are POSIX-only and conditionally compiled.** termios, `select()`
  on stdin and SIGWINCH have no MSVC equivalent, so `src/terminal/*` sits behind
  `if(NOT WIN32)` in `CMakeLists.txt` and `main.cpp` reports the unsupported platform
  at runtime under `#ifdef Q_OS_WIN`. Verify a Windows change negatively
  (`g++ -E -DQ_OS_WIN ... | grep -c runTerminal` must be 0) — a syntax check proves
  nothing.
- **No flag changes meaning by mode.** `--port` and `--auth-token` keep their browser
  meaning (the ports this process serves, the token it requires) in both modes.
  Terminal *connection* settings, which would otherwise collide with them, get their
  own `--term-host` / `--term-port` / `--term-token` names for the endpoint being
  viewed. Terminal-only options with nothing to collide with (`--fps`, `--gfx`,
  `--cdp`) keep plain names. Both modes share one `QCommandLineParser`, so every flag
  is listed in both `--help` outputs — that is the price of never overloading one.

### Tabs: one browser, many pages

- **Ids are anoa's, not Chromium's.** `t1`, `t2`, minted by the registry and
  never recycled. A Chromium target GUID changes when a page is recreated, so an
  agent holding one would find itself driving a different page than the one it
  named. Both ids travel together in `/json/list` and `Target.getTargets`.
- **`--tab` resolves at the CDP target-selection seam**, not per command. The
  agent CLI already picks one page target out of `/json/list`; `--tab` narrows
  that single pick, so no command threads a tab through its own arguments and
  nothing downstream learns tabs exist.
- **The tab strip lives outside the view**, as a sibling in `BrowserWindow`, for
  the same load-bearing reason the toolbar does: anything inside the container
  appears in every screenshot and shifts the coordinate space `/render/click`
  is measured in. Verified rather than assumed — the viewport headers and the
  screenshot are identical with the strip shown and hidden.
- **`CdpExtensions` answers asynchronously rather than blocking.**
  `Target.createTarget` cannot reply in the same turn, because a page exists
  before its DevTools target does. `processCommand` therefore has three
  outcomes, not two, and the third defers. Waiting for the id would mean a
  nested event loop, against the rule that a seam is crossed by signals.
- **Two tabs naming one profile share one `QWebEngineProfile`.** Two Qt profile
  objects over one on-disk path corrupt each other's storage; this is not an
  optimisation. Profiles are reference counted, and `closeTab` destroys the view
  before dropping the reference — a profile freed while a page still holds it is
  a use-after-free inside Chromium.
- **Background views are covered, never hidden.** `AnoaBrowser` uses no layout:
  every view is a child at the container's geometry and the active one is
  raised. `QStackedLayout` was the obvious choice and was wrong — it HIDES the
  views it is not showing, and a hidden `QWebEngineView` processes no input at
  all, neither Qt synthetic events nor CDP `Input.dispatchMouseEvent`, while
  both paths still answer "clicked". Reads worked throughout, which is what made
  it easy to miss. A view must also be `show()`n at creation: one that was never
  shown takes no input even once it is raised.

### Other decisions

- **Transports sit behind the `FrameBackend` seam**, so `--cdp` selects an external
  CDP endpoint while the default path talks to `/render/*` over HTTP, and the viewer
  UI knows about neither. The seam is asynchronous by necessity (see Code Patterns).
- **CDP clients connect through `CdpProxy`, not Chromium directly**, which is what
  makes bearer-token auth possible at all.
- Version comes from the git tag: CI passes `-DANOA_VERSION_OVERRIDE=$TAG`, so
  `CMakeLists.txt`'s `project(... VERSION)` is only the local default.

---

## Dependencies & Integrations

- **Qt 6.4+** (6.7.3 in CI) — WebEngineWidgets, WebEngineCore, Network, WebSockets,
  Widgets; `Test`, `Core`, `Gui` additionally for the unit test target. Point CMake at
  it with `QT_PREFIX=` / `-DCMAKE_PREFIX_PATH=`.
- **libcups2-dev** is required on Linux: `Qt6PrintSupport` is a WebEngineWidgets
  dependency, and without CUPS headers `find_package` fails with the misleading
  *"Failed to find WebEngineWidgets"*.
- **Chromium**, embedded via QtWebEngine — no external browser is downloaded.
- **Node ≥ 20** for the test suites only (vitest, ws, node-fetch, @playwright/test,
  puppeteer-core). Each `tests/*` directory has its own `package.json`.
- **`nc` (netcat-openbsd)** is required by the three bash suites that launch the
  browser — `port_layout.test.sh`, `extensions.test.sh` and `regression/smoke.sh`
  all wait on readiness with `while ! nc -z 127.0.0.1 <port>`. Without it every
  probe fails and the suites report *"Browser failed to start"*, which reads
  exactly like a product failure and is not one. See *Known Gotchas*.
- **Homebrew** is the distribution channel: `.github/homebrew/*.tpl` are rendered by
  `update-homebrew-tap.yml` on release. macOS builds are codesigned/notarized using
  `.github/entitlements/anoa.entitlements`.

---

## Development Setup

```bash
# Qt 6 + CMake must be installed first (Homebrew on macOS, aqtinstall or the
# distro packages on Linux). On Linux also: sudo apt-get install libcups2-dev

make build                       # Debug build -> build/anoa
make release                     # Release build -> build-release/
make release-static QT_PREFIX=/opt/Qt/6.7.3/gcc_64   # what the release job runs
make test                        # re-configures with -DBUILD_TESTS=ON, runs ctest
make coverage                    # gcov the unit libs, fail under COVERAGE_MIN (default 80)
make lint                        # clang-tidy over src/ (skipped if not installed)
make help                        # every target and the QT_PREFIX in effect

./build/anoa --headless --no-sandbox --port 9222   # run the browser
./build/anoa terminal --term-port 9222             # view it in a terminal
```

---

## Jonggrang Workflow

Jonggrang uses a **two-phase planning** flow so humans can review and edit a plan before AI decomposes it into tasks.

### Full workflow

```bash
# Phase 1 — generate a human-readable draft plan
jonggrang plan "add JWT authentication"
# → AI writes .jonggrang/.drafts/<session>/plan.md (high-level, no tasks yet)
# → Interactive options:
#     Approve           → run Phase 2 immediately
#     Edit with AI      → describe changes, AI revises plan, loop back
#     Edit in $EDITOR   → open editor, loop back
#     Save draft        → exit, run "jonggrang approve" later
#     Abort             → discard the draft plan.md

# Resume after accidental close:
jonggrang plan
# → no description → shows list of pending + archived plans
# → pick one → shows plan + interactive options again

# Phase 2 — approve plan → decompose into tasks
jonggrang approve
# → AI reads .jonggrang/.drafts/<session>/plan.md → runs `jonggrang task import` to create tasks
# → plan.md is archived to .jonggrang/.output/features/<featureId>/plan.md

# Execute tasks
jonggrang work
```

### Shorthand options

```bash
# Plan + auto-approve + tasks in one shot (skips human review)
jonggrang plan "add JWT auth" --yes

# Deep mode: 3-phase analysis (discovery + brainstorm + condense) → enriched plan
# Adds Affected Areas, Risks, and Alternatives Considered sections to plan.md
jonggrang plan "add JWT auth" --deep

# Deep mode + auto-approve in one shot
jonggrang plan "add JWT auth" --deep --yes

# Full pipeline: plan → approve → execute in one shot
jonggrang work "add JWT auth" --yes

# Execute existing tasks only (skip pending plan warning)
jonggrang work --ignore-plan
```

### Modifying an approved plan

| Situation | Command |
|-----------|---------|
| Add new scope on top of done work | `jonggrang plan "also add rate limiting"` |
| Change remaining pending work | `jonggrang plan "use Passport.js instead"` |
| Undo completed tasks | Not supported — create new tasks to override |

**Rule: completed tasks are immutable.** They reflect real code. Any correction must be a new task that fixes/replaces the previous implementation.

### Plan file format

```markdown
---
feature: jwt-auth
branch: feat/jwt-auth
work_type: MEDIUM
description: JWT authentication with login, register, refresh
created_at: 2026-04-16T10:30:00Z
---

# Plan: JWT Authentication

## Approach
...

## Phases
1. DB schema — users + refresh_tokens
2. Auth service — register/login/refresh
3. JWT middleware
...

## Key Decisions
- Token storage: httpOnly cookie

## Out of Scope
- OAuth, 2FA, email verification
```

---

## Bug Reporting

When you discover a defect **outside the scope of your current task**, report it immediately:

```bash
# Report a bug and create a BUGFIX task in one shot
jonggrang bug "description of what is broken" --feature <feature_id>
# When asked "Create a task now?" → y

# Or save for later (batch convert)
jonggrang bug "description" --feature <feature_id>
# When asked "Create a task now?" → n
jonggrang bug convert --feature <feature_id>   # converts all open bugs to tasks later
```

Get the `feature_id` by running: `jonggrang task show <id>` — look for the `feature_id` field in the output.

**Rules:**
- Do NOT fix out-of-scope bugs inline — stay focused on your current task
- Report real defects only (crashes, wrong return values, broken edge cases)
- Do NOT report style issues, TODOs, or future features — those go in the plan

Bug reports are saved to `.jonggrang/.output/features/<feature_id>/bugs.md` and can be viewed with:
```bash
jonggrang bug list
```

---

## Task Management CLI

Use the `jonggrang task` CLI to manage tasks instead of editing `.jonggrang/jonggrang-tasks.json` directly.

### Commands

```bash
# List & inspect
jonggrang task list                         # list all tasks (JSON output)
jonggrang task list pending                 # filter by status
jonggrang task show task-001                # show task detail
jonggrang task next                         # show next eligible task

# Create & modify
jonggrang task add --title "Add login page" --priority 1
jonggrang task add --title "Write tests" --blocked-by task-001
jonggrang task update task-001 --status in_progress
jonggrang task update task-001 --files src/login.ts,src/login.test.ts

# Complete & block
jonggrang task done task-001                # mark completed + passes=true
jonggrang task block task-002 --reason "Waiting for API spec"

# Remove (cleans up blocked_by refs)
jonggrang task remove task-003
```

### Output

- Default output is **JSON** (machine-readable for agents)
- Add `--pretty` for human-readable table format
- Add `--json` to force JSON when in a TTY

### Available flags for add/update

| Flag | Description |
|------|-------------|
| `--title` | Task title |
| `--desc` | Task description |
| `--priority` | Priority (1 = highest) |
| `--status` | pending, in_progress, completed, blocked, waiting, skipped |
| `--skill` | Skill name |
| `--blocked-by` | Comma-separated dependency task IDs |
| `--files` | Comma-separated file paths |
| `--reason` | Reason (used with `block`) |

---

## Jonggrang Notes

This section is updated by Jonggrang during work sessions.
Human should review and curate periodically.

### Patterns Discovered

- A fourth Qt6::Core-only unit target, `anoa-tab-ids-lib`, holds `tab_ids.cpp`.
  The rule that a WebEngine source is never folded into a Core-only target still
  stands: each of the four is its own library so that adding a WebEngine
  dependency fails loudly instead of quietly pulling WebEngine into the unit job.
  New targets must be added in THREE places — `tests/unit/CMakeLists.txt`, the
  `TARGETS` list in `tests/coverage.sh`, and that script's
  `cmake --build --target` line. Missing the last one passes `make test` and
  fails `make coverage` with "Not Run".
<!-- Agent appends here, human curates -->

- **Three unit targets now, not one** (phase 14). `anoa-config-lib` is unchanged;
  `anoa-terminal-bytes-lib` (`src/terminal/frame_bytes.cpp`) and
  `anoa-terminal-ui-lib` (`terminal_ui.cpp`) are new, both Qt6::Core-only and both
  inside `if(NOT WIN32)`. The rule in *Known Gotchas* still holds — never fold a
  terminal source into `anoa-config-lib`; add a target instead.
  `cmake -DANOA_TEST_SANITIZE=ON` builds them with ASan+UBSan; CI runs it on Linux.
- **TerminalUi is testable without a terminal.** Construct it, never call `begin()`,
  drive the public `feedInput()` with a stub `FrameBackend`. Skipping `begin()`
  leaves `m_cols`/`m_rows` at 80x24, which is what makes the status-bar arithmetic a
  constant rather than a property of the runner. Redirect stdout while doing it —
  and replay the capture to stderr on failure, because QTest's logger writes to
  stdout too and a failing `QVERIFY` would otherwise vanish into the temp file.
- **The pty harness lives in `tests/integration/helpers.js`** (`launchViewer`,
  `waitUntil`, `viewerErrors`, `sgrPress`, `TERM_COLS`/`TERM_ROWS`/`TERM_FPS`).
  `terminal_cdp.test.js` and `terminal_http.test.js` share it; fake endpoints stay
  in their own suites.
- **Coverage is `make coverage`** (phase 15) — `tests/coverage.sh` behind
  `-DANOA_TEST_COVERAGE=ON`, gcov over the three Qt6::Core-only unit libraries,
  failing under `COVERAGE_MIN` (default 80). It is mutually exclusive with
  `ANOA_TEST_SANITIZE`; CMake errors out if both are on. Two traps it already
  handles, so do not "simplify" them away: stale `.gcda` **merge** into the next
  run, so they must be deleted first or the number depends on how often the
  suite has been run; and gcov's own `Lines executed:N% of M` summary inflates
  `M` with Qt template instantiations that are not lines of the source file
  (14 phantom lines for `config.cpp`), so the script counts the per-line `.gcov`
  listing instead.
- **Two shell suites need no binary at all**: `build_shape.test.sh` (Windows
  compile-out, `if(NOT WIN32)` coverage) and `qt_floor.test.sh` (syntax-only against
  the declared Qt floor — `QT_FLOOR_PREFIX=/opt/Qt/6.4.3/gcc_64`, installed with
  `aqt install-qt linux desktop 6.4.3 gcc_64 -m qtwebsockets`, ~20 s, no WebEngine).

### Gotchas Discovered
<!-- Agent appends here, human curates -->

- **A stalling `/render/*` peer wedges the viewer and needs SIGKILL** (bug-003, open).
  `RenderHttpClient::httpRequest()` sets no socket timeouts and now runs on the Qt
  event loop, so a peer that accepts and goes quiet stops the `QSocketNotifier` from
  ever firing again: Ctrl-C is never read, ISIG is off so byte 3 is not a signal, and
  SIGTERM only sets a flag the never-reached frame tick polls. SIGKILL skips
  `atexit(restoreTerminal)`, leaving the terminal in raw mode on the alt screen.
  THTTP-08 is committed as `it.fails()` and passes *because* of this; it turns red
  when the fix lands.
- **The status bar's link field is invisible at 100 columns.** The row is over budget
  before any link, so the link yields by design. Assert on the wire in the pty suites;
  the yielding rule itself is covered in the unit suite at 80 columns.
- **`--profile terminal` cannot work**, and that is accepted, not a bug: the raw-argv
  pre-scan has no option-arity knowledge, so the word is taken as the subcommand
  wherever it appears and the parser then reports a missing value. TERM-MODE-05 pins
  it so it is not rediscovered as a regression.
- **`TerminalUi`'s tty-only lines are covered by the pty suites, not by gcov.**
  `enterRawMode()`'s success path, `begin()` past raw mode, `end()`,
  `terminalSize()`'s `TIOCGWINSZ` branch and the three "repaint once started"
  lines are 29 of `terminal_ui.cpp`'s lines and they will never show as covered:
  the pty suites reach them through a *separate, uninstrumented* `anoa`
  process. Do not add a pty to the unit target to chase them — the invariant
  that this suite never calls `begin()` is what keeps its 80x24 geometry a
  constant rather than a property of the CI runner.
- **The quit flag has no reset**, so `testQuitSignalStopsTheFrameLoop` must stay
  the last slot in `test_terminal_ui.cpp`. Every `tick()` after it returns false.
  New cases go above it.
- **`anoa --help` aborts without a display.** Browser mode constructs a
  `QApplication` before `parseArgs()` runs, so `--help` needs `QT_QPA_PLATFORM=offscreen`
  (the real Qt variable — `port_layout.test.sh` uses a misspelt `QPA_PLATFORM`
  elsewhere and gets away with it only because those cases also pass `--headless`).
