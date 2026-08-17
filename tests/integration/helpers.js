import { spawn, execFileSync } from 'child_process';
import { createConnection, createServer, Socket } from 'net';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'fs';
import { tmpdir } from 'os';
import WebSocket from 'ws';
import fetch from 'node-fetch';
import { resolve, dirname, join } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));

// CMake builds into an app bundle on macOS and leaves the executable in place
// everywhere else, so there is no single path that works on both. Probing is
// what removes the "works on Linux, mysteriously fails on a Mac" gap — the
// alternative was a hand-made build/anoa symlink that survived exactly until
// the next clean build.
const BINARY_CANDIDATES = [
  resolve(__dirname, '../../build/anoa.app/Contents/MacOS/anoa'),
  resolve(__dirname, '../../build/anoa'),
  resolve(__dirname, '../../build/anoa.exe'),
];

export const BINARY = process.env.ANOA_BINARY
  ?? BINARY_CANDIDATES.find((p) => existsSync(p))
  ?? BINARY_CANDIDATES[1];

export const HTTP_PORT = parseInt(process.env.ANOA_PORT ?? '9222', 10);
export const WS_PORT   = HTTP_PORT + 2;
export const BASE_URL  = `http://localhost:${HTTP_PORT}`;
export const WS_BASE   = `ws://localhost:${WS_PORT}`;

/** Wait until TCP port is accepting connections or timeout (ms). */
export function waitForPort(port, timeout = 10000) {
  return new Promise((resolve, reject) => {
    const deadline = Date.now() + timeout;
    function attempt() {
      const sock = createConnection(port, '127.0.0.1');
      sock.once('connect', () => { sock.destroy(); resolve(); });
      sock.once('error', () => {
        sock.destroy();
        if (Date.now() >= deadline) return reject(new Error(`Port ${port} not ready after ${timeout}ms`));
        setTimeout(attempt, 100);
      });
    }
    attempt();
  });
}

/**
 * Ask the kernel for an unused loopback port and hand it back.
 *
 * The suites that spawn anoa use the fixed ANOA_PORT triplet, because
 * the binary's three ports are derived from one another. A test that stands up
 * its *own* server (the fake CDP endpoint in terminal_cdp.test.js) has no such
 * constraint and should not squat on the triplet, so it takes an ephemeral one
 * from here instead. There is a race between the close() and the next listen(),
 * which is unavoidable without keeping the socket — with fileParallelism off
 * and nothing else on the box binding ephemeral ports, it does not bite.
 */
export function freePort() {
  return new Promise((resolve, reject) => {
    const srv = createServer();
    srv.once('error', reject);
    srv.listen(0, '127.0.0.1', () => {
      const { port } = srv.address();
      srv.close(() => resolve(port));
    });
  });
}

/**
 * Start the anoa binary and wait for HTTP port to be ready.
 * Returns the child process.
 */
/**
 * Wait until nothing is listening on a port.
 *
 * waitForPort() answers as soon as the port accepts, which is the wrong question
 * when a browser is still on its way out: the previous instance is still
 * listening, the new one loses the bind and dies, and every assertion that
 * follows is measured against the wrong process. That failure has shown up as
 * "expected 200 to be 401" — the old browser, which had no auth token — and as
 * a whole file's worth of cases silently marked skipped, because vitest reports
 * a beforeAll that threw as a skip rather than a failure.
 */
export function waitForPortFree(port, timeout = 10000) {
  const deadline = Date.now() + timeout;
  return new Promise((resolve, reject) => {
    function attempt() {
      const sock = new Socket();
      sock.setTimeout(500);
      const retry = () => {
        sock.destroy();
        if (Date.now() >= deadline)
          return reject(new Error(`Port ${port} still bound after ${timeout}ms`));
        setTimeout(attempt, 100);
      };
      sock.once('connect', retry);            // still listening
      sock.once('timeout', retry);
      sock.once('error', () => { sock.destroy(); resolve(); }); // refused = free
      sock.connect(port, '127.0.0.1');
    }
    attempt();
  });
}

export async function startBrowser(extraArgs = []) {
  // The port has to be ours before we ask for it, or we bind nothing and talk
  // to whatever is still there.
  await waitForPortFree(HTTP_PORT);

  const args = ['--headless', '--no-sandbox', `--port=${HTTP_PORT}`, ...extraArgs];
  const proc = spawn(BINARY, args, {
    env: { ...process.env, QPA_PLATFORM: 'offscreen' },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let stderr = '';
  proc.stdout.on('data', (d) => process.env.ANOA_VERBOSE && process.stdout.write(d));
  proc.stderr.on('data', (d) => {
    stderr += d;
    if (process.env.ANOA_VERBOSE) process.stderr.write(d);
  });

  // A browser that dies on startup would otherwise be waited on for the full
  // timeout and then reported as a port problem, with the reason it actually
  // gave thrown away.
  const died = new Promise((_, reject) => {
    proc.once('exit', (code) => {
      reject(new Error(`anoa exited with ${code} before binding ${HTTP_PORT}`
                       + (stderr ? `: ${stderr.trim().split('\n').slice(-3).join(' | ')}` : '')));
    });
  });
  died.catch(() => {}); // an exit after a successful start is not an error

  await Promise.race([waitForPort(HTTP_PORT, 15000), died]);
  return proc;
}

/** Send SIGTERM to the browser process and wait for it to exit. */
export function stopBrowser(proc) {
  return new Promise((resolve) => {
    if (!proc || proc.exitCode !== null) return resolve();
    proc.once('exit', resolve);
    proc.kill('SIGTERM');
  });
}

/**
 * Open a WebSocket to the proxy, optionally with an auth token in the URL.
 * Resolves once the connection is open.
 */
export function openWs(path = '', token = null) {
  const url = token
    ? `${WS_BASE}${path}?token=${token}`
    : `${WS_BASE}${path}`;
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    ws.once('open', () => resolve(ws));
    ws.once('error', reject);
  });
}

/**
 * Open a WS to the actual devtools page URL from /json/list.
 * Returns { ws, targetId }.
 */
export async function openDevtoolsWs(token = null) {
  // /json/list is legitimately empty for a moment after startup: a tab is built
  // before its Chromium target exists, and the list is assembled from tabs whose
  // target id has resolved. Binding the HTTP port — all startBrowser waits for —
  // happens first, so asking straight away is a race that a loaded machine
  // loses. It surfaced as a whole file reported "skipped" with zero failures,
  // because vitest calls a beforeAll that threw a skip.
  const deadline = Date.now() + 15000;
  let list = [];
  for (;;) {
    const resp = await fetch(`${BASE_URL}/json/list`);
    list = await resp.json();
    if (list.length) break;
    if (Date.now() >= deadline)
      throw new Error('/json/list stayed empty for 15s');
    await new Promise((r) => setTimeout(r, 150));
  }
  const target = list[0];
  // The URL returned in webSocketDebuggerUrl already points to our proxy port.
  const wsUrl = token
    ? `${target.webSocketDebuggerUrl}?token=${token}`
    : target.webSocketDebuggerUrl;
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(wsUrl);
    ws.once('open', () => resolve({ ws, targetId: target.id }));
    ws.once('error', reject);
  });
}

/**
 * Send a CDP command on an open WebSocket and wait for the matching response.
 */
export function sendCdp(ws, method, params = {}, id = 1) {
  return new Promise((resolve, reject) => {
    const handler = (data) => {
      try {
        const msg = JSON.parse(data.toString());
        if (msg.id === id) {
          ws.off('message', handler);
          resolve(msg);
        }
      } catch { /* ignore non-JSON */ }
    };
    ws.on('message', handler);
    ws.send(JSON.stringify({ id, method, params }));
    // Timeout safety
    setTimeout(() => {
      ws.off('message', handler);
      reject(new Error(`CDP response for id=${id} (${method}) timed out`));
    }, 10000);
  });
}

// ── `anoa terminal` on a pty ────────────────────────────────────────
//
// Shared by terminal_cdp.test.js and terminal_http.test.js, which are the same
// suite pointed at two transports: one fake endpoint each, the same viewer,
// the same assertions about what reaches the wire. Everything in this section
// is transport-agnostic on purpose — the fake endpoints stay in their suites.
//
// ── A pty is required, and here is exactly why ──────────────────────────────
// runTerminal() refuses to start unless both stdin and stdout are a terminal,
// and TerminalUi takes the terminal size from TIOCGWINSZ and reads input in
// termios raw mode. Over a pipe the binary exits 1 with "stdin/stdout must be
// a terminal" before it has looked at its transport flags at all, so a pipe
// exercises *nothing* on this path — not even the failure cases, which would
// pass for the wrong reason.
//
// A committed general pty harness is out of scope for this feature, so the pty
// here is `script(1)` from util-linux: one process that allocates a pty, runs
// the viewer on it, forwards our stdin into it and returns the child's exit
// status (-e). Two details make it enough rather than a harness:
//   * `stty rows N cols M` inside the same shell sets the window size on the
//     pty slave, which is what makes the cell -> page arithmetic in the suites
//     predictable instead of dependent on the runner's terminal.
//   * `2><file>` inside the same shell keeps stderr off the pty, so the
//     one-line failure messages are readable without the alt-screen paint
//     mixed into them.
// BSD `script` (macOS) takes different arguments and has no -e; the pty-driven
// tests skip there rather than pretend. CI's integration job is ubuntu-22.04,
// where util-linux script is part of the base image.

export const HAVE_UTIL_LINUX_SCRIPT = (() => {
  try {
    return /util-linux/.test(execFileSync('script', ['--version'], { encoding: 'utf8' }));
  } catch {
    return false;
  }
})();

// The terminal geometry every pty-driven test runs at. Fixed on purpose: the
// coordinate assertions are arithmetic over these numbers, so they are
// constants of the harness rather than per-test knobs.
export const TERM_COLS = 100;
export const TERM_ROWS = 30;
export const TERM_FPS = 10; // slower than the 30 fps default: fewer frames, same behaviour

export const VIEWER_ERR_PREFIX = 'anoa terminal: ';

const shq = (s) => `'${String(s).replace(/'/g, `'\\''`)}'`;

/**
 * Run `anoa terminal <args>` on a pty at TERM_COLS x TERM_ROWS.
 *
 * Returns a handle whose `send()` writes raw bytes to the viewer's stdin (they
 * reach it through the pty exactly as a terminal would deliver them, i.e. as
 * the escape sequences a real keypress or SGR mouse report produces), and
 * whose `exited` resolves with { code, stderr } once it is gone.
 *
 * `--gfx halfblock` is forced rather than left at 'auto': the image protocols
 * would probe the pty for a cell size and sit through the timeout.
 */
export function launchViewer(extraArgs = []) {
  const dir = mkdtempSync(join(tmpdir(), 'anoa-term-'));
  const errPath = join(dir, 'stderr.txt');

  const argv = ['terminal', '--gfx', 'halfblock', '--fps', String(TERM_FPS), ...extraArgs];
  const inner = `stty rows ${TERM_ROWS} cols ${TERM_COLS}; exec ${shq(BINARY)} ${argv.map(shq).join(' ')} 2>${shq(errPath)}`;

  // detached: script leads its own process group, so a test that has to give
  // up can take the whole tree (script -> sh -> anoa) down at once.
  // Killing script alone would orphan the viewer, which would then sit in its
  // reconnect loop for the rest of the run.
  const proc = spawn('script', ['-q', '-e', '-c', inner, '/dev/null'], {
    stdio: ['pipe', 'pipe', 'pipe'],
    detached: true,
    env: { ...process.env, TERM: 'xterm-256color' },
  });

  // The viewer repaints the whole grid every frame, so only a tail is kept —
  // enough for the status bar, which is the last thing written each frame.
  let tail = '';
  proc.stdout.on('data', (d) => {
    tail = (tail + d.toString('latin1')).slice(-16384);
  });
  proc.stderr.on('data', () => {}); // script's own diagnostics; the viewer's go to errPath

  let exit = null;
  const exited = new Promise((resolve) => {
    proc.once('close', (code) => {
      let stderr = '';
      try {
        stderr = readFileSync(errPath, 'utf8');
      } catch { /* the shell never got far enough to create it */ }
      exit = { code, stderr };
      resolve(exit);
    });
  });

  return {
    proc,
    get exit() { return exit; },
    exited,
    get tail() { return tail; },
    send(bytes) {
      proc.stdin.write(Buffer.from(bytes, 'latin1'));
    },
    /**
     * Ctrl-C: ISIG is off in raw mode, so this is byte 3, not a signal.
     * Resolves to null if the viewer was still alive after `timeout` ms and
     * had to be killed — which is a result a test is allowed to assert on.
     */
    async quit(timeout = 5000) {
      if (exit) return exit;
      try {
        proc.stdin.write(Buffer.from([3]));
      } catch { /* already gone */ }
      const result = await Promise.race([
        exited,
        new Promise((resolve) => setTimeout(() => resolve(null), timeout)),
      ]);
      if (result) return result;
      try {
        process.kill(-proc.pid, 'SIGKILL'); // the group, not just script
      } catch { /* already reaped */ }
      await exited;
      return null;
    },
    async cleanup() {
      await this.quit();
      rmSync(dir, { recursive: true, force: true });
    },
  };
}

/** Poll until `fn()` is truthy, failing loudly (never silently) on timeout. */
export async function waitUntil(fn, what, timeout = 15000) {
  const deadline = Date.now() + timeout;
  for (;;) {
    const value = fn();
    if (value) return value;
    if (Date.now() >= deadline) throw new Error(`timed out after ${timeout}ms waiting for ${what}`);
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
}

/**
 * The distinct viewer error lines in a captured stderr.
 *
 * Two filters, both deliberate. Lines without the prefix are dropped because
 * parseArgs() emits an unrelated "--auth-token is not set" warning on every
 * invocation, terminal mode included — pre-existing, not this path's, and
 * asserting it away either way would encode it. Duplicates are collapsed
 * because a discovery failure is written twice by design: once while the alt
 * screen is still up (where a user would never see it) and once after the
 * terminal has been handed back. Redirecting stderr to a file is what makes
 * both copies visible here; what the user reads is one line.
 */
export function viewerErrors(stderr) {
  return [...new Set(stderr.split('\n').filter((line) => line.startsWith(VIEWER_ERR_PREFIX)))];
}

/**
 * SGR mouse press, exactly what a terminal writes: the button field is 0-2 for
 * left/middle/right and 64/65 for wheel up/down, and coordinates are 1-based.
 */
export const sgrPress = (btn, cellX, cellY) => `\x1b[<${btn};${cellX + 1};${cellY + 1}M`;

/** Get the webSocketDebuggerUrl from /json/version. */
export async function getWsDebuggerUrl(token = null) {
  const headers = token ? { Authorization: `Bearer ${token}` } : {};
  const resp = await fetch(`${BASE_URL}/json/version`, { headers });
  const json = await resp.json();
  return json.webSocketDebuggerUrl;
}

/**
 * Open a tab through the CDP proxy and return its anoa id.
 *
 * Goes through Target.createTarget rather than the CLI so an integration test
 * needs nothing but the browser it already started.
 */
export async function newTab(url = 'about:blank', params = {}) {
  const wsUrl = await getWsDebuggerUrl();
  const ws = new WebSocket(wsUrl);
  await new Promise((res, rej) => { ws.once('open', res); ws.once('error', rej); });
  try {
    const created = await sendCdp(ws, 'Target.createTarget', { url, ...params }, 8001);
    const targetId = created.result?.targetId;
    // createTarget answers with the engine's id; the anoa id is what a test
    // types next, so it is looked up rather than assumed.
    const listed = await sendCdp(ws, 'Target.getTargets', {}, 8002);
    const info = (listed.result?.targetInfos ?? [])
      .find((t) => t.targetId === targetId);
    return { tabId: info?.anoaTabId, targetId };
  } finally {
    ws.close();
  }
}

/** Every page entry /json/list reports, in the browser's own order. */
export async function listTabs() {
  const resp = await fetch(`${BASE_URL}/json/list`);
  const targets = await resp.json();
  return targets.filter((t) => t.type === 'page');
}
