<p align="center">
  <img src="docs/anoa-logo.png" alt="Anoa Browser" width="420">
</p>

<p align="center">
  A browser you drive from anywhere — a script, a terminal, or a window.<br>
  One self-contained binary. No Node.js, no npm, no separate driver.
</p>

<p align="center">
  <a href="#install">Install</a> ·
  <a href="#usage">Usage</a> ·
  <a href="#tabs">Tabs</a> ·
  <a href="#terminal-viewer-anoa-terminal">Terminal viewer</a> ·
  <a href="docs/BUILDING.md">Building</a>
</p>

---

Full [Chrome DevTools Protocol](https://chromedevtools.github.io/devtools-protocol/) support, so Playwright, Puppeteer and anything else that speaks CDP connect to it as they would to Chrome.

```bash
anoa --headless --port 9222     # start it once, leave it running
anoa open example.com           # then talk to it
anoa snapshot -i                # see what is on the page, with refs
anoa click @e2                  # act on it
anoa terminal                   # or watch it happen, in your terminal
```

The browser is the session. Every command above is a separate process that
attaches, does one thing and exits — the page, the cookies and the scroll
position survive between them, so commands chain with `&&` for free.

## Features

- **A command per action, for agents** — `open`, `snapshot`, `click`, `fill`, `get`, `eval`, `wait`, `screenshot`. `snapshot` hands back refs (`@e1`, `@e2`) that later commands target, and clicks are hit-tested, so a button under a consent banner is reported rather than clicked through. `anoa help` groups them; `anoa skills get core` prints the workflow for an agent to read
- **Speaks CDP** — connect Playwright, Puppeteer or any Chrome-compatible client, with Chrome-compatible discovery endpoints (`/json`, `/json/version`, `/json/list`) and a WebSocket proxy with session multiplexing and optional bearer-token auth
- **Drive it over plain HTTP** — an interactive live view you can drop in an `<iframe>`, PNG screenshots, MJPEG stream per tab, and full mouse/keyboard injection through `/render/*`, no CDP client required
- **See it in your terminal** — `anoa terminal` renders the live page as ANSI or as real images in iTerm2/kitty, and forwards your clicks, scrolls and typing back to it. Point it at a running browser, at any external Chrome endpoint with `--cdp`, or at nothing at all and it hosts its own
- **A window when you want one** — an address bar with back / forward / reload, and an auto-hiding toolbar that returns when the pointer reaches the top edge
- **Works behind tunnels and proxies** — no Origin rejections, with `--auth-token` for access control
- **Print to PDF, profiles, extensions** — `Page.printToPDF`, isolated cookie jars and localStorage per named profile, and unpacked Chromium extensions (manifest v2)

---

## Install

### macOS (Homebrew) — Intel & Apple Silicon

One universal (x86_64 + arm64) build serves both architectures:

```bash
brew tap porcupine-md/tap
brew trust porcupine-md/tap        # tap ships a cask + formula
brew install --cask anoa
```

Installs a single `anoa` shim into your `PATH`, pointing at `anoa.app/Contents/MacOS/anoa`. That one executable is the whole product — the terminal viewer is the `anoa terminal` subcommand of it, not a separate file in the bundle. The app is Developer ID signed and notarized by Apple, so it opens with no Gatekeeper warnings. (The cask still clears the quarantine flag on install — a harmless no-op safety net.)

**Upgrade** — `brew update` first, so the tap picks up the newest release:

```bash
brew update
brew upgrade --cask anoa
```

### Linux (AppImage) — recommended

One file. Download, make it executable, run it — no unpacking, no install, no
root:

```bash
chmod +x anoa-x86_64.AppImage          # or anoa-aarch64.AppImage
./anoa-x86_64.AppImage --headless --port 9222
./anoa-x86_64.AppImage open example.com
```

Built for **x86_64** and **aarch64**, both on Ubuntu 22.04 against the same Qt,
so both run on anything with **glibc 2.35 or newer** — Ubuntu 22.04, Debian 12
and later, which is what an ARM board usually runs.

Verified on a stock Ubuntu 24.04: headless, a real window with GLX, the terminal
viewer, and the agent commands.

It carries its own FUSE, so it does **not** need the `libfuse2` package that
Ubuntu dropped in 22.04 — the runtime AppImage's own tooling embeds by default
fails there with `dlopen(): error loading libfuse.so.2` before anything runs.

**On a host with no FUSE at all** — most containers — run it without mounting:

```bash
APPIMAGE_EXTRACT_AND_RUN=1 ./anoa-x86_64.AppImage --version
```

That unpacks the whole payload on every invocation: about 6 seconds per command
against 1 for the tarball. Fine for a one-off, wrong for an agent loop — use the
tarball or the container image there instead.

**A stripped system needs Chromium's own runtime libraries.** Not an AppImage
limitation — the same set the tarball and the container image need, because it
is what running a Chromium engine costs. Any ordinary desktop already has all
of it. On something genuinely bare:

```bash
sudo apt install ca-certificates libnss3 libnspr4 libxcomposite1 libxdamage1 \
  libxrandr2 libxkbcommon0 libdrm2 libasound2 libcups2 libatk1.0-0 \
  libatk-bridge2.0-0 libatspi2.0-0 libxshmfence1 libglib2.0-0 libgl1 \
  libglx-mesa0 libegl1 libgbm1 libfontconfig1 libfreetype6 libdbus-1-3 \
  fonts-liberation
```

Measured on a bare `debian:12-slim` with nothing installed at all: `--version`,
the browser, `eval`, and the whole `anoa tab` registry work; only page rendering
needs the list above. With it, everything does — `open`, `get text`,
screenshots and a second tab included.

### Linux (Homebrew)

```bash
brew tap porcupine-md/tap
brew install anoa-linux
```

The formula creates exactly one symlink, `bin/anoa` → `libexec/anoa.sh`, and terminal mode is reached through it as `anoa terminal`. If you are upgrading from a release that installed a second command, read the breaking-change note at the end of [Terminal Viewer](#terminal-viewer-anoa-terminal).

**Upgrade:**

```bash
brew update
brew upgrade anoa-linux
```

### Linux (install script — no root)

Installs the portable bundle under your home directory and puts the launcher on your `PATH`:

```bash
curl -fsSL https://raw.githubusercontent.com/porcupine-md/anoa-browser/master/scripts/install-linux.sh | bash
```

```
~/.local/lib/anoa/     the unpacked bundle — self-contained, nothing system-wide
~/.local/bin/anoa  ->  ../lib/anoa/anoa.sh
```

The symlink points at the *launcher*, never at the raw binary — the launcher is what sets up the environment the bundled libraries need. It tells you if `~/.local/bin` is not on your `PATH`, and re-running it upgrades in place.

```bash
install-linux.sh --version v0.4.0     # pin a release instead of taking the latest
install-linux.sh --prefix /opt/anoa   # somewhere other than ~/.local
install-linux.sh --uninstall          # remove it again
```

### Linux (portable tarball)

The release tarball is self-contained: one executable, every shared library it needs under `lib/`, its resources and translations, plus a launcher script that wires them together. Terminal mode is a subcommand of that executable, so the tarball contains no second binary.

```bash
tar xzf anoa-linux-x86_64.tar.gz
./anoa/anoa.sh --headless --port 9222   # launcher, not the raw binary
./anoa/anoa.sh terminal                 # terminal viewer — same launcher
```

Always go through `anoa.sh`: the raw executable next to it has none of that environment set up, in terminal mode as much as in browser mode.

**Headed mode uses your machine's own libraries.** The OpenGL dispatch
libraries, libX11 and libstdc++ are taken from your system whenever it has
them, and the bundle's copies fill in only what is genuinely missing — your
graphics driver is loaded into the process and is built against your system's,
so a vendored copy in front of it is what produces `Could not initialize GLX`
on a desktop that is otherwise perfectly healthy. Nothing to install on a
normal desktop; on a bare server that wants a window:

```bash
sudo apt install libgl1 libglx-mesa0 libegl1 libopengl0   # Debian/Ubuntu
```

If your GL still cannot satisfy Chromium — `Could not initialize GLX` — fall back
to software rendering:

```bash
QTWEBENGINE_CHROMIUM_FLAGS="--disable-gpu" anoa
```

`--headless` and `anoa terminal` never open a window and are unaffected either way.

### Container (Docker / Podman)

```bash
docker run --rm -p 9222:9222 ghcr.io/porcupine-md/anoa-browser
```

Headless, no display server, no sandbox privileges needed. The CDP endpoint is
on 9222, so Playwright and Puppeteer connect to it exactly as they would to a
local browser. Drive it with the agent commands from inside the container:

```bash
docker run -d --name anoa -p 9222:9222 ghcr.io/porcupine-md/anoa-browser
docker exec anoa anoa open example.com
docker exec anoa anoa snapshot -i
docker exec anoa anoa click @e1
```

Tags: a version (`v0.6.0`) for each release, `latest` for the newest release,
`edge` for master. Build it yourself with `docker build -t anoa .` — the
Dockerfile installs a published release rather than compiling, and takes
`--build-arg ANOA_VERSION=v0.6.0` to pin one.

The browser runs as a non-root user and the endpoint is **unauthenticated by
default**. Pass `--auth-token` and do not publish the port to an untrusted
network without one:

```bash
docker run --rm -p 9222:9222 ghcr.io/porcupine-md/anoa-browser \
  --headless --no-sandbox --port 9222 --auth-token "$SECRET"
```

### Windows

Download `anoa-windows-x86_64.zip` from [Releases](https://github.com/porcupine-md/anoa-browser/releases) and run `anoa.exe`.

---

## Building from source

Prebuilt packages are above. To build it yourself — prerequisites, targets, architecture and the release process — see **[docs/BUILDING.md](docs/BUILDING.md)**.

---

## Usage

```
anoa [options]

Options:
  -p, --port <N>        CDP HTTP/WebSocket port (default: 9222)
  --headless            Run in offscreen/headless mode (no display required)
  --no-sandbox          Disable Chromium sandbox
  --profile <name>      Named browser profile (isolated cookies/storage)
  --profile-dir <dir>   Base directory for browser profiles
  --auth-token <secret> Require Bearer token for CDP WebSocket connections
  --extension <path>    Load unpacked Chromium extension directory (repeatable)
  --config <file>       Path to JSON or INI config file
  --width <px>          Browser viewport/window width (default: 1280)
  --height <px>         Browser viewport/window height (default: 720)
```

### Examples

```bash
# Headless on port 9222 (default)
./anoa --headless --port 9222

# Headed with a named profile
./anoa --port 9222 --profile myprofile

# With bearer token auth
./anoa --headless --port 9222 --auth-token mysecret

# Connect Playwright
node -e "
const { chromium } = require('playwright');
(async () => {
  const browser = await chromium.connectOverCDP('http://localhost:9222');
  const page = browser.contexts()[0].pages()[0];
  await page.goto('https://example.com');
  console.log(await page.title());
  await browser.close();
})();
"

# Connect Puppeteer
node -e "
const puppeteer = require('puppeteer-core');
(async () => {
  const browser = await puppeteer.connect({ browserURL: 'http://localhost:9222' });
  const page = await browser.newPage();
  await page.goto('https://example.com');
  console.log(await page.title());
  await browser.close();
})();
"
```

### Port layout

The binary uses 3 consecutive ports:

| Port | Purpose |
|---|---|
| `N` (e.g. 9222) | HTTP discovery + WebSocket CDP proxy |
| `N+1` (e.g. 9223) | Chromium's own DevTools endpoint, internal |
| `N+2` (e.g. 9224) | Internal WebSocket proxy upstream |

### Remote CDP access

Chromium 111+ rejects DevTools WebSocket connections whose `Origin` header is not allowlisted. anoa starts Chromium with `--remote-allow-origins=*` so remote CDP clients (tunnels, reverse proxies, browser-based frontends) can connect from arbitrary origins. Access control is enforced by the proxy layer via `--auth-token` instead.

---

## Agent commands

Everything here attaches to a browser that is already running and leaves it
running. Start one first:

```bash
anoa --headless --port 9222 &
```

```bash
anoa open example.com                 # navigate (scheme optional)
anoa snapshot -i                      # interactive elements, with refs
anoa click @e2                        # act by ref, or by any CSS selector
anoa fill @e3 "user@example.com"
anoa get text                         # the page's visible text
anoa get attr @e1 href
anoa eval "document.title"
anoa wait --selector ".results"
anoa screenshot page.png
anoa status                           # what it is attached to right now
```

`snapshot` prints one line per element, refs first, because the ref is what the
next command needs:

```
  @e1   link       Documentation
  @e3   textbox    Search  [required]
  @e7   button     Sign in
```

Refs are written onto the DOM nodes, which is what lets a ref minted by one
process be used by the next. They stay valid until the page replaces those
nodes — **re-snapshot after anything that changes the page**. A ref that no
longer resolves says so rather than acting on the wrong element.

Clicks are hit-tested against the point they land on:

```
anoa: @e7 is covered by <div> Accept cookies — dismiss it, then re-snapshot
```

### Beyond the core loop

```bash
anoa find role button                 # locate by role, text or CSS — all return refs
anoa find text "Sign in" --nth 2

anoa cookies                          # cookies and storage
anoa cookies set sid abc123
anoa storage local set token xyz
anoa storage session clear

anoa set viewport 390 844 3           # emulation
anoa set device iphone-14             # …or a preset; no name lists them
anoa set media dark
anoa set offline on

anoa console                          # what the page logged
anoa errors                           # uncaught exceptions
anoa network                          # fetch/XHR it made: method, status, ms

anoa wait --text "Welcome"            # richer waits
anoa wait --url "/dashboard"
anoa wait --fn "window.app?.ready"
anoa wait "#spinner" --state hidden
```

`console`, `errors` and `network` are recorded **inside the page**, which is
what lets them report what happened *before* the command ran — a one-shot
process could never have subscribed to the events in time. The buffer resets on
every page load and holds the last 500 entries; only `fetch` and `XMLHttpRequest`
are seen, not document or subresource loads.

Add `--json` to any command for structured output. Exit codes are meaningful:
`0` success, `1` the command failed, `2` bad usage, `3` no browser is listening.

`anoa help` lists every command grouped by what it does; `anoa help state`
prints one group. For agents, **`anoa skills get commands`** is the full
reference and `anoa skills get core` is the workflow — both printed straight
from the binary, so instructions can never drift from the CLI you have.

**Uploads, downloads, dialogs and popups** are handled rather than ignored.
`alert`/`confirm`/`prompt` are answered without blocking, `window.open` opens a
background tab, downloads are saved (`--download-dir`), and a file input is
filled with `anoa upload <target> <file>` — clicking one only asks for a dialog
nobody can answer.

Not implemented, so you know not to reach for them: React introspection, Web
Vitals, accessibility audits, a credential vault, an MCP server, plugins, and
request interception — `anoa network` observes, it cannot block or rewrite.

---

## Tabs

One browser holds many pages. Each has a short id — `t1`, `t2` — that stays
valid between commands and between processes, so an agent can capture one and
come back to it.

```bash
anoa tab new example.com          # opens a tab, prints its id
anoa tab list                     # every tab; * marks the active one
anoa tab select t2                # make it the active one
anoa tab close t2                 # the last tab cannot be closed
```

Or name it, and stop keeping track of ids:

```bash
anoa tab new example.com --name search
anoa --tab search get text
```

A name is an alias, not a rename: the id keeps working and `--tab` takes either.
Names must be unique, and cannot be shaped like an id (`t2`) — that is what
keeps `--tab t2` from ever meaning two different tabs.

Every other command takes `--tab <id>` and acts on the active tab without it:

```bash
TAB=$(anoa tab new example.com)
anoa --tab "$TAB" get text        # reads that tab, whatever else is on screen
```

The `/render/*` endpoints take the same choice as `?tab=<id>`, and answer
`404 {"error":"no tab t9"}` for one that is not there rather than quietly using
the active tab. `/json/list` reports one entry per tab, each carrying
`anoaTabId` and `anoaActive` alongside the fields every CDP client already
reads.

**Logins survive between runs.** Cookies and local storage go to a profile named
`default` under your platform's application data directory, so a browser you log
into stays logged in the next time you start one. `--profile <name>` picks a
different one, `--profile-dir <dir>` moves them, and `--ephemeral` keeps nothing
at all.

**Cookies are shared between tabs unless you say otherwise.** A login in one tab
is a login in all of them, which is usually what you want. Two flags change
that, and they mean different things:

```bash
anoa tab new example.com --profile work   # its own cookies, kept on disk
anoa tab new example.com --isolated       # its own cookies, gone with the tab
```

That is how two tabs hold two different logins to one site at the same time.

In the window, a tab strip appears once there is more than one tab. In the
terminal viewer, **Ctrl-N** cycles tabs and the status row shows which one you
are looking at.

### What tabs do not do

- No tab groups, pinning, reordering, or session restore across restarts.
- One window per process. Opening a tab never opens a window.
- `anoa terminal` with no target hosts its own single-tab browser; multi-tab
  applies to a browser you connect to.
- No per-tab viewport or device emulation — the size is process-wide.
- Extensions load once, against the shared profile. A tab on its own profile
  gets none of them.

---

## Web Render Endpoints

The HTTP server exposes a `/render/*` family for inspecting the live browser view from any web browser or CLI tool — no CDP client required.

All endpoints share the same `--auth-token` auth as the CDP endpoints: pass the secret as a `Bearer` header or `?token=` query parameter.

### Endpoints

| Method | Path | Response | Description |
|---|---|---|---|
| `GET` | `/render?tab=<id>&embed=1` | `text/html` | Live view — MJPEG stream you can click, type and drag in, with a tab bar and URL bar. `embed=1` drops the chrome for embedding; `tab=` picks the tab to drive |
| `GET` | `/render/viewport?tab=<id>` | `application/json` | `{"width","height","dpr"}` — the logical size the input endpoints below speak. Not the frame's pixel size: frames are device pixels, so on a HiDPI display the image is `dpr`× wider than the coordinates that drive it |
| `GET` | `/render/screenshot.png` | `image/png` | Current frame as a PNG snapshot; `X-Anoa-Viewport-Width/Height` headers carry the logical viewport size |
| `GET` | `/render/screenshot.ppm?w=<px>&h=<px>` | `image/x-portable-pixmap` | Current frame as binary PPM (P6), scaled server-side (aspect ratio kept); `X-Anoa-Viewport-Width/Height` headers carry the logical viewport size for coordinate mapping |
| `GET` | `/render/html` | `text/html` | Rendered DOM source (`page()->toHtml()`) |
| `POST` | `/render/navigate?url=<url>` | `text/plain` | Load a URL into the embedded browser |
| `POST` | `/render/click?x=<px>&y=<px>&button=left\|right\|middle` | `text/plain` | Synthesize a mouse click at viewport coordinates (button defaults to `left`) |
| `POST` | `/render/scroll?dy=<delta>&x=<px>&y=<px>` | `text/plain` | Synthesize a mouse wheel event; `dy` in angle-delta units (±120 per notch, **negative scrolls down**), `x`/`y` default to the viewport center. Roughly 60 px of page per notch |
| `POST` | `/render/type?text=<text>` | `text/plain` | Type text into the focused element (URL-encoded query param, or raw request body) |
| `POST` | `/render/key?key=<name>&mods=<list>` | `text/plain` | Press a named key: `enter`, `tab`, `backspace`, `delete`, `escape`, `space`, `up`, `down`, `left`, `right`, `home`, `end`, `pageup`, `pagedown`, `insert`, `f1`–`f12`, or a single character. `mods` is a comma-separated list of `ctrl`, `shift`, `alt`, `meta` — `key=a&mods=ctrl` is Select All |
| `POST` | `/render/move?x=<px>&y=<px>&buttons=<list>&mods=<list>` | `text/plain` | Move the pointer. `buttons` names what is still held, which is what makes a move a drag rather than a hover |
| `POST` | `/render/mousedown?x=<px>&y=<px>&button=<b>&mods=<list>` | `text/plain` | Press and hold |
| `POST` | `/render/mouseup?x=<px>&y=<px>&button=<b>&mods=<list>` | `text/plain` | Release. A press and a release at one point is still a click |
| `POST` | `/render/tab/new?url=<url>&name=<name>` | `application/json` | `{"id":"t3"}` |
| `POST` | `/render/tab/close?tab=<id>` | `application/json` | Closes it; `409` for the last remaining tab |
| `GET` | `/render/stream.mjpeg?tab=<id>` | `multipart/x-mixed-replace` | MJPEG live stream (~10 fps) of the named tab, or the active one |

### Embedding the live view

`/render` is a page you can put in an `<iframe>`. It streams the tab over MJPEG
and forwards mouse and keyboard back, so the person looking at your app can use
the browser — hover a menu, drag a selection, type into a form, press Ctrl+A —
without leaving it.

```html
<iframe src="http://localhost:9222/render?embed=1&tab=t2&token=mysecret"
        width="1280" height="720" style="border:0"></iframe>
```

- `embed=1` drops the tab bar and URL bar, leaving just the view. Omit it to get
  the full viewer.
- `tab=<id>` picks which tab this frame drives. Omit it to follow whichever tab
  is active. Two frames on two tab ids drive two tabs at once — one can be the
  agent's, one the user's.
- `token=` is required when `--auth-token` is set. It stays in the frame's own
  URL; the page is served verbatim and never has the server's token written
  into it.
- Click the view once before typing. Keyboard events only reach a focused
  frame, and an iframe starts unfocused — the view says so until it has focus.

**Who is allowed to frame it.** The view is not a picture of a browser, it is a
handle on one. A page that can frame it can watch a logged-in session and act
inside it. So the default is same-origin only, sent as
`Content-Security-Policy: frame-ancestors 'self'`, and embedding elsewhere is
opt-in:

```bash
./anoa --headless --port 9222 --embed-origin https://app.example.com
```

`--embed-origin` is repeatable. `--embed-origin '*'` removes the restriction
altogether — the frame will load anywhere, which is worth doing only on a
machine where that is already true of everything else.

A named origin may also **read** the JSON endpoints — `Access-Control-Allow-Origin`
echoes it back — so the host page can call `/json/list` and draw its own tab bar
around the frame instead of using the one the viewer ships. Nothing is opened up
that framing had not already opened: an origin trusted to drive the browser but
not to list its tabs can show the view and not build anything around it. With no
`--embed-origin` set, no CORS headers are sent at all.

A worked example — host page, its own tab bar, new-tab button — is in
[`examples/embed/`](examples/embed/).

Note that the `/render/*` control endpoints have never carried an origin check
of their own, and `--auth-token` is off by default. On a shared or untrusted
machine, set a token.

### Usage example

```bash
# 1. Start anoa with a token
./anoa --headless --port 9222 --auth-token mysecret

# 2. Navigate the browser to a page
curl -X POST "http://localhost:9222/render/navigate?url=https%3A%2F%2Fexample.com&token=mysecret"

# 3. Open the live viewer in any browser
open "http://localhost:9222/render?token=mysecret"

# 4. Fetch a PNG screenshot with curl
curl -H "Authorization: Bearer mysecret" \
     http://localhost:9222/render/screenshot.png \
     -o screenshot.png

# 5. Navigate the browser to a new URL
curl -X POST "http://localhost:9222/render/navigate?url=https%3A%2F%2Fnews.ycombinator.com&token=mysecret"

# 6. Stream live MJPEG (e.g. in VLC or ffplay)
ffplay "http://localhost:9222/render/stream.mjpeg?token=mysecret"

# 7. Click at viewport coordinates (640, 360)
curl -X POST "http://localhost:9222/render/click?x=640&y=360&token=mysecret"

# 8. Scroll down one wheel notch
curl -X POST "http://localhost:9222/render/scroll?dy=-120&token=mysecret"
```

---

## Terminal Viewer (`anoa terminal`)

`anoa terminal` renders a live browser view directly in your terminal and forwards terminal mouse and keyboard input back to the page — click a link in your terminal and the browser clicks it. It is a **mode of the `anoa` binary**, not a separate program: the word `terminal` before any options selects it, and that mode never starts a browser window, an HTTP server, or a CDP proxy of its own.

**Given no target, it hosts its own browser.** Plain `anoa terminal` — no `--term-host`, no `--term-port`, no `--cdp` — starts a browser inside the viewer process and renders it directly, with no port opened and nothing to start first:

```bash
anoa terminal          # that is the whole setup
```

Naming any target switches it back to being a client: with `--term-host`/`--term-port` it views a running `anoa` over the [`/render/*` endpoints](#web-render-endpoints), and with [`--cdp`](#attaching-to-an-external-cdp-endpoint---cdp) it attaches to any external Chrome/Chromium/Playwright endpoint. Those two remain thin clients that host nothing, which is what makes them usable over SSH on a machine with no browser at all. The embedded mode necessarily carries the WebEngine stack, and its status bar says `embedded` where the others name a host and port.

**POSIX only.** The terminal sources are not compiled into the Windows build at all; there, `anoa terminal` prints `Error: terminal mode is not supported on Windows` and exits non-zero.

Two rendering backends, auto-detected:

| Backend | Quality | Terminals |
|---|---|---|
| `iterm` / `kitty` | Full-resolution PNG (crisp) | iTerm2, WezTerm (`iterm`); kitty, Ghostty (`kitty`) |
| `halfblock` | ANSI truecolor ▀ cells (1 cell = 1×2 px, pixelated) | Everything else with truecolor support |

### Invocation

```
anoa terminal [options]

Options:
  (none)                 Host a browser in-process and view that
  --term-host <host>     Host of the anoa to view (default: 127.0.0.1)
  --term-port <N>        HTTP port of the anoa to view (1-65535, default: 9222)
  --term-token <secret>  Bearer token, if the viewed endpoint requires one
  --fps <N>              Refresh rate, 1-120 (default: 30)
  --gfx <mode>           auto | halfblock | iterm | kitty (default: auto)
  --cdp <url>            Attach to an external CDP endpoint instead of /render/*
```

The connection flags are spelled `--term-*` deliberately. `--port` and `--auth-token` keep their browser meaning on the same shared parser (the port *this* process listens on, the token *it* demands), so no flag changes meaning between modes.

**Terminal options are CLI-only.** `--config` reads the browser options from a JSON or INI file, but nothing in that file is consulted for terminal mode — `--term-host`, `--term-port`, `--term-token`, `--fps`, `--gfx` and `--cdp` must be passed on the command line.

`--gfx auto` picks the image protocol from `TERM`/`TERM_PROGRAM`; pass `--gfx iterm` or `--gfx kitty` explicitly if detection misses (e.g. inside tmux, which hides the outer terminal — image protocols need tmux ≥ 3.4 with `allow-passthrough`, otherwise use `--gfx halfblock`).

### Controls

Mouse reporting uses the SGR extended protocol (`ESC [ < btn ; col ; row M`), which every modern terminal emits; cells are mapped back to page coordinates using the viewport size the endpoint reports, so clicks land where you see them even on a HiDPI page.

| Input | Action |
|---|---|
| Left/right/middle mouse click | Click at that position in the page |
| Mouse wheel | Scroll the page under the pointer |
| Typing (any text, incl. paste) | Typed into the focused element — a whole paste burst is forwarded as one event |
| `Enter` / `Backspace` / `Tab` | Forwarded as key events (`Backspace` accepts both DEL and BS) |
| Arrow keys | Forwarded to the page — they move the caret in a focused field, otherwise they scroll |
| `Ctrl-L` | Open the address prompt on the status row |
| `Ctrl-R` | Reload |
| `Alt-Left` / `Alt-Right` | Back / forward through history |
| `Ctrl-N` | Next tab; the status row shows which one is on screen (`t2/3`) |
| `Ctrl-B` | Show or hide the status bar |
| `Ctrl-C` / `Ctrl-Q` | Quit and restore the terminal |

**The status bar starts hidden**, so the page gets every row of the terminal; `Ctrl-B` brings it back. The address prompt ignores the setting and takes the row whenever it is open, so `Ctrl-L` is never typing into something invisible.

The image backends (`iterm`, `kitty`) fit the frame into the cell grid with its aspect ratio kept, so a page shaped differently from the terminal is letterboxed — visible as unused rows below the page. In embedded mode the viewer removes the cause rather than the symptom: it asks its own browser for a viewport with the terminal's proportions, keeping the width from `--width` and deriving only the height, so the frame fills the grid. Over `--term-port` and `--cdp` the page belongs to somebody else and is left alone, so the letterbox stays. Halfblock never letterboxes at all — it asks for exactly the grid it has.

While the address prompt is open it owns the keyboard — nothing reaches the page — and `Enter` navigates, `Ctrl-C` (or `Ctrl-G`) abandons the line without leaving the viewer, `Ctrl-U` clears it. A bare host is fine: `example.com` becomes `https://example.com`, while anything that already names a scheme (`http:`, `file:`, `about:`) is used as typed, and `localhost:8080` is read as a host and port rather than a scheme.

`Esc` is deliberately *not* a cancel key. Telling a lone `Esc` from the start of an arrow or mouse report needs a timeout, and guessing is what made split escape sequences type their letters into the page (bug-002); the prompt swallows whole sequences instead.

There is no single-letter quit: `q` is an ordinary printable character and is typed into the page, so you can fill in a search box without the viewer exiting. `Ctrl-C` is delivered as a keystroke rather than a signal (the terminal runs with `ISIG` off), so it quits immediately.

The status bar shows the last event forwarded to the browser (`click 640,360`, `typed "hello"`, `key backspace`, …) alongside the backend, endpoint and viewport size. If it doesn't change when you click, your terminal isn't delivering mouse reports — check its mouse-reporting setting, or in tmux enable `set -g mouse on`.

```bash
# 1. Start the browser (any machine, headless or headed)
./anoa --headless --port 9222 --auth-token mysecret

# 2. Point it somewhere
curl -X POST "http://localhost:9222/render/navigate?url=https%3A%2F%2Fnews.ycombinator.com&token=mysecret"

# 3. Watch and control it from your terminal (works over SSH too)
./anoa terminal --term-host localhost --term-port 9222 --term-token mysecret
```

Requires a terminal with SGR mouse support; the halfblock fallback additionally needs truecolor (iTerm2, kitty, Alacritty, WezTerm, GNOME Terminal, tmux ≥ 3.2, …). Both stdin and stdout must be a terminal — piping either one is refused, since there is nothing to drive and nothing to paint.

### Attaching to an external CDP endpoint (`--cdp`)

`--cdp <url>` replaces the `/render/*` transport with a CDP WebSocket client (`Page.captureScreenshot` for frames, `Input.dispatchMouseEvent` / `dispatchKeyEvent` / `insertText` for input), so the viewer can drive any Chrome, Chromium, Edge or Playwright/Puppeteer-launched browser — not just anoa.

Two URL forms are accepted:

| Form | Behaviour |
|---|---|
| `http://host:port` or `https://host:port` | Fetches `/json/list` and attaches to the **first `type: "page"` target**, dialling that target's `webSocketDebuggerUrl`. A URL with a path keeps it verbatim (`http://host/proxy/devtools` fetches exactly that), which is what makes reverse-proxied endpoints work; an empty path or a bare `/` becomes `/json/list`. |
| `ws://host:port/devtools/page/<id>` | Dialled directly — no discovery request, and the target you name is the target you get. |

```bash
# Attach to a Chrome started with --remote-debugging-port=9222
anoa terminal --cdp http://127.0.0.1:9222

# Attach to one specific page target, skipping discovery
anoa terminal --cdp ws://127.0.0.1:9222/devtools/page/ABC123 --gfx kitty
```

**Auth token.** `--term-token` is *not* ignored under `--cdp` — it becomes the bearer token for the CDP endpoint, sent both as an `Authorization: Bearer <secret>` header and as a `?token=<secret>` query parameter, on the `/json/list` request and on the WebSocket dial. That is what anoa's own `--auth-token` proxy expects, and endpoints that ignore an unexpected header or query parameter (plain Chrome) are unaffected. `--term-host` and `--term-port` *are* ignored under `--cdp`, and the viewer says so on stderr before it takes over the screen.

**`wss://` is not supported.** TLS CDP endpoints are rejected at argument-parsing time, because the shipped build has no TLS backend. Use `ws://`, or `http://` and let discovery hand you the right `ws://` URL. Tunnel it (SSH port-forward, stunnel) if the endpoint is only reachable over TLS.

Two things worth knowing when the endpoint is anoa itself: it uses [three consecutive ports](#port-layout), so `--cdp http://127.0.0.1:9222` discovers on 9222 and then dials `ws://127.0.0.1:9224/…` — the port shown in the status bar changing mid-session is correct, not a fault. And a dropped connection is retried with an exponential backoff (250 ms doubling to 8 s) that the status bar reports as `connecting` / `reconnecting (attempt N)`; before the *first* successful connect the retries are capped, so a wrong URL fails with a message instead of spinning forever.

### Breaking change: `anoa-term` is gone

The standalone `anoa-term` binary no longer exists and ships **no compatibility shim, symlink, or wrapper** — a shim would either be the second binary this merge removed, or a symlink whose `argv[0]` sniffing outlives its usefulness. Type `anoa terminal` instead; every flag it used has a `--term-*` equivalent listed above (`--host` → `--term-host`, `--port` → `--term-port`, `--token` → `--term-token`; `--fps` and `--gfx` are unchanged, though `--fps` now defaults to 30 and accepts up to 120).

Upgrading in place can leave a stale `anoa-term` on your `PATH` that no longer resolves. Two hazards:

- **Linux (Homebrew):** `brew upgrade` unlinks the old keg before linking the new one, which normally takes `bin/anoa-term` with it — but it does not do so reliably if the link was force-linked, hand-created, or left behind by a partially failed unlink. The symptom is a *dangling* symlink into the new keg's `libexec/anoa-term`, so you get "No such file or directory" rather than "command not found".
- **macOS (cask):** the cask's `binary` shim for `anoa-term` is removed on **reinstall**, not on upgrade, so the old shim can survive a `brew upgrade --cask`.

If `anoa-term` still appears on your `PATH` after upgrading, uninstall and reinstall:

```bash
brew uninstall anoa-linux && brew install anoa-linux   # Linux
brew uninstall --zap --cask anoa && brew install --cask anoa   # macOS
```

---

## CDP Protocol Support

### Supported / Passing

| Command | Status |
|---|---|
| `Browser.getVersion` | Pass — Chromium passthrough |
| `Target.getTargets` | Pass — returns active pages |
| `Page.navigate` | Pass |
| `Page.printToPDF` | Pass — intercepted and handled natively |
| `Profiler.enable` | Pass — stub `{}` |
| `HeapProfiler.enable` | Pass — stub `{}` |
| `Security.enable` | Pass — stub `{}` |
| `Security.setIgnoreCertificateErrors` | Pass — stub `{}` |
| `Target.createBrowserContext` | Stubbed → synthetic context ID |
| `Target.disposeBrowserContext` | Stubbed → no-op |
| `Browser.setDownloadBehavior` | Stubbed → `{}` |
| `Browser.getWindowForTarget` | Stubbed → `{}` |

### Tabs over CDP

`Target.getTargets`, `createTarget`, `activateTarget`, `closeTarget` and
`getTargetInfo` are answered from anoa's own tab registry, so `browser.newPage()`
works and each tab appears as its own target:

```js
const page = await browser.newPage();   // opens a real second tab
```

`Target.createTarget` takes two extra parameters anoa understands and other
clients ignore: `anoaProfile` for a persistent named cookie jar and
`anoaIsolated` for a throwaway one.

`Target.createBrowserContext` still returns a synthetic id — Playwright's
`newContext()` expects more than a tab-scoped profile provides.

---

## Running on CI

`--headless` needs no display server on any platform, and `--version` and
`--help` work on a machine with none either.

```bash
anoa --headless --no-sandbox --port 9222
```

On a runner with no GPU, add `--no-sandbox` as above; the extra flags some
environments want are in [docs/BUILDING.md](docs/BUILDING.md#headless-machines).

---

## License

MIT
