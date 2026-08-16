// `anoa terminal` against a fake /render/* endpoint — the default
// backend, with no --cdp anywhere.
//
// Why this suite exists: the whole argument for shipping both transports was
// that the HTTP path is a known-good control to diff the CDP path against, and
// until now nothing tested the control. terminal_cdp.test.js covers only
// --cdp, so no suite ever started the viewer on its default backend at all —
// which means the port of the frame loop from a blocking select() to a
// QSocketNotifier plus a QTimer had no end-to-end coverage either.
//
// It is deliberately the sibling of terminal_cdp.test.js: the same pty harness
// out of helpers.js, the same terminal geometry, the same fixed page size, and
// the same three input assertions (click coordinates, wheel sign, batched
// typing) so the two wires can be read side by side. Where the CDP suite
// asserts deltaY = -120, this one asserts dy = +120 on the same keystroke, and
// between them the inversion is a fact rather than a comment.
//
// The pty requirement, the `script(1)` harness and the macOS skip are all
// explained in helpers.js.

import { describe, it, expect, afterEach } from 'vitest';
import { createServer } from 'http';

import {
  HAVE_UTIL_LINUX_SCRIPT,
  TERM_COLS as COLS,
  TERM_ROWS as ROWS,
  VIEWER_ERR_PREFIX as ERR_PREFIX,
  freePort,
  launchViewer,
  sgrPress,
  viewerErrors,
  waitForPort,
  waitUntil,
} from './helpers.js';

// ── The page as the fake endpoint reports it ────────────────────────────────
//
// The viewport is what X-Anoa-Viewport-Width/Height advertise, and it is
// deliberately *not* the size of the image: /render/screenshot.ppm scales the
// page down to fit the terminal, and the viewer has to map clicks through the
// advertised viewport rather than through the pixels it received. Picking two
// different numbers is what makes that testable.
const VIEWPORT_W = 200;
const VIEWPORT_H = 100;

// What TerminalUi asks for, derived from the geometry rather than guessed:
// tick() requests COLS x (ROWS - 1) * 2 pixels, one per column and two per
// row, leaving the last terminal row for the status bar.
const FRAME_W = COLS;
const FRAME_H = (ROWS - 1) * 2;

// This endpoint answers at exactly the requested size (a legal response — the
// UI maps through the size it gets back, never the size it asked for), so the
// display map follows from the request: dispCols = width, dispRows =
// (height + 1) / 2.
const DISP_COLS = FRAME_W;
const DISP_ROWS = Math.trunc((FRAME_H + 1) / 2);

// mapCellToPage(), terminal_ui.cpp.
function cellToPage(cellX, cellY) {
  return {
    x: Math.trunc(((cellX + 0.5) * VIEWPORT_W) / DISP_COLS),
    y: Math.trunc(((cellY + 0.5) * VIEWPORT_H) / DISP_ROWS),
  };
}

/** A binary PPM of `w` x `h` mid-grey pixels. */
function makePpm(w, h) {
  return Buffer.concat([
    Buffer.from(`P6\n${w} ${h}\n255\n`, 'ascii'),
    Buffer.alloc(w * h * 3, 0x40),
  ]);
}

// ── The fake /render/* endpoint ─────────────────────────────────────────────

/**
 * An HTTP server answering just enough of /render/* for the viewer.
 *
 * Every request is recorded with its path, query and headers, so a test
 * asserts on `endpoint.requests` or the `screenshots` / `of(path)` views.
 *
 * `stallAfter` is the knob for the interesting failure: after that many
 * screenshot requests the socket is accepted and then nothing is ever written
 * to it, which is what a peer that hangs rather than refuses looks like.
 * `ppmBody` replaces the image with an arbitrary byte string.
 */
async function startFakeRender({ port, stallAfter = Infinity, ppmBody = null } = {}) {
  const requests = [];
  const stalled = [];

  const server = createServer((req, res) => {
    const [path, query] = req.url.split('?');
    requests.push({
      method: req.method,
      path,
      query: new URLSearchParams(query ?? ''),
      headers: req.headers,
    });

    if (path === '/render/screenshot.ppm') {
      const shots = requests.filter((r) => r.path === '/render/screenshot.ppm').length;
      if (shots > stallAfter) {
        // Hold the socket open and answer nothing, forever. Keeping the
        // reference is what stops Node from garbage-collecting the response
        // and closing the connection out from under the test.
        stalled.push(res);
        req.socket.setKeepAlive(true);
        return;
      }
      const q = new URLSearchParams(query ?? '');
      const body =
        ppmBody ?? makePpm(parseInt(q.get('w') ?? '0', 10), parseInt(q.get('h') ?? '0', 10));
      res.writeHead(200, {
        'Content-Type': 'image/x-portable-pixmap',
        'Content-Length': body.length,
        'X-Anoa-Viewport-Width': String(VIEWPORT_W),
        'X-Anoa-Viewport-Height': String(VIEWPORT_H),
      });
      res.end(body);
      return;
    }

    if (path.startsWith('/render/')) {
      res.writeHead(200, { 'Content-Length': 2 }).end('ok');
      return;
    }
    res.writeHead(404).end();
  });

  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, '127.0.0.1', resolve);
  });
  await waitForPort(port, 5000);

  return {
    port,
    requests,
    get screenshots() {
      return requests.filter((r) => r.path === '/render/screenshot.ppm');
    },
    /** Every request to one /render/* path, most recent last. */
    of(path) {
      return requests.filter((r) => r.path === path);
    },
    async close() {
      for (const res of stalled) res.destroy();
      await new Promise((resolve) => server.close(resolve));
    },
  };
}

const launchTerminal = (port, extraArgs = []) =>
  launchViewer(['--term-port', String(port), ...extraArgs]);

/**
 * Wait until the viewer has painted at least `count` frames.
 *
 * The first screenshot is runTerminal()'s 8x8 pre-flight probe, which happens
 * before the terminal is touched and fills no display map; the count below is
 * of real frames, so the probe is excluded.
 */
async function waitForFrames(endpoint, term, count = 2) {
  await waitUntil(
    () => !term.exit && endpoint.screenshots.length >= count + 1,
    `${count} frames (viewer stderr: ${term.exit?.stderr ?? 'still running'})`,
  );
  // The paint happens inside the viewer's event loop; give the frame it
  // triggers time to land before anything depends on the display map.
  await new Promise((resolve) => setTimeout(resolve, 200));
}

// ── Tests ───────────────────────────────────────────────────────────────────

describe.skipIf(!HAVE_UTIL_LINUX_SCRIPT)('terminal against a fake /render/* endpoint', () => {
  let endpoint;
  let term;

  afterEach(async () => {
    if (term) await term.cleanup();
    if (endpoint) await endpoint.close();
    term = undefined;
    endpoint = undefined;
  });

  // THTTP-01 — the frame loop end to end. Two frames, not one: the second
  // proves the QTimer is re-arming rather than the viewer having painted once
  // and stopped, which is the failure the select() -> event loop port could
  // plausibly have introduced.
  it('paints repeatedly from /render/screenshot.ppm', async () => {
    const port = await freePort();
    endpoint = await startFakeRender({ port });

    term = launchTerminal(port);
    await waitForFrames(endpoint, term, 2);

    expect(term.exit).toBeNull();
    expect(term.tail).toContain(`http 127.0.0.1:${port} ${VIEWPORT_W}x${VIEWPORT_H}`);
    expect(term.tail).toContain('[halfblock]');
  }, 30000);

  // THTTP-02 — the halfblock geometry contract, and the pre-flight probe in
  // front of it. The probe is 8x8 by design: it exercises the endpoint and the
  // token rather than the renderer.
  it('probes at 8x8, then requests one pixel per column and two per row', async () => {
    const port = await freePort();
    endpoint = await startFakeRender({ port });

    term = launchTerminal(port);
    await waitForFrames(endpoint, term, 2);

    const shots = endpoint.screenshots;
    expect(shots[0].query.get('w')).toBe('8');
    expect(shots[0].query.get('h')).toBe('8');
    for (const shot of shots.slice(1)) {
      expect(shot.query.get('w')).toBe(String(FRAME_W));
      expect(shot.query.get('h')).toBe(String(FRAME_H));
    }
  }, 30000);

  // THTTP-03 — the control that TCDP-05's CSS-pixel assertion is diffed
  // against. Same cell, same arithmetic; the only difference is which
  // viewport the two backends divide by.
  it('a click at a known cell arrives at the mapped page coordinates', async () => {
    const port = await freePort();
    endpoint = await startFakeRender({ port });

    term = launchTerminal(port);
    await waitForFrames(endpoint, term);

    const CELL_X = 10;
    const CELL_Y = 5;
    const page = cellToPage(CELL_X, CELL_Y);

    term.send(sgrPress(0, CELL_X, CELL_Y));
    const clicks = await waitUntil(() => {
      const found = endpoint.of('/render/click');
      return found.length ? found : null;
    }, 'a /render/click');

    expect(clicks).toHaveLength(1);
    expect(clicks[0].method).toBe('POST');
    expect(clicks[0].query.get('x')).toBe(String(page.x));
    expect(clicks[0].query.get('y')).toBe(String(page.y));
    expect(clicks[0].query.get('button')).toBe('left');
  }, 30000);

  // THTTP-04 — the other half of the wheel-sign story. /render/scroll takes a
  // Qt angleDelta, where +120 means the wheel turned up; TCDP-04 asserts the
  // same keystroke reaches CDP as deltaY = -120. Asserting both sides in two
  // suites is what makes the inversion a fact rather than a comment in
  // cdp_frame_backend.cpp.
  it('a wheel-up produces /render/scroll dy = +120, the Qt angleDelta convention', async () => {
    const port = await freePort();
    endpoint = await startFakeRender({ port });

    term = launchTerminal(port);
    await waitForFrames(endpoint, term);

    const page = cellToPage(10, 5);
    term.send(sgrPress(64, 10, 5)); // wheel up, over the page
    const up = await waitUntil(() => {
      const found = endpoint.of('/render/scroll');
      return found.length ? found : null;
    }, 'a /render/scroll');

    // TCDP-04 asserts deltaY = -120 for this same keystroke; +120 here is the
    // other half of the inversion.
    expect(up[0].query.get('dy')).toBe('120');
    expect(up[0].query.get('x')).toBe(String(page.x));
    expect(up[0].query.get('y')).toBe(String(page.y));

    // The opposite notch is the opposite sign, so the assertion above pins a
    // convention rather than a constant.
    term.send(sgrPress(65, 10, 5));
    const both = await waitUntil(() => {
      const found = endpoint.of('/render/scroll');
      return found.length >= 2 ? found : null;
    }, 'a second /render/scroll');
    expect(both[1].query.get('dy')).toBe('-120');
  }, 30000);

  // THTTP-05 — parity with TCDP-07 and with the unit-level TERM-INPUT-04: a
  // burst is one request, not one per character.
  it('typing produces one batched /render/type', async () => {
    const port = await freePort();
    endpoint = await startFakeRender({ port });

    term = launchTerminal(port);
    await waitForFrames(endpoint, term);

    term.send('hello');
    const typed = await waitUntil(() => {
      const found = endpoint.of('/render/type');
      return found.length ? found : null;
    }, 'a /render/type');

    expect(typed).toHaveLength(1);
    expect(typed[0].query.get('text')).toBe('hello');
    expect(endpoint.of('/render/key')).toHaveLength(0);
  }, 30000);

  // THTTP-06 — --term-token. Never verified anywhere before, and it is the
  // difference between the viewer working against an authenticated
  // anoa and failing with a 401 it cannot explain.
  it('--term-token authenticates every request, header and query alike', async () => {
    const port = await freePort();
    endpoint = await startFakeRender({ port });

    term = launchTerminal(port, ['--term-token', 'sekrit']);
    await waitForFrames(endpoint, term);

    term.send(sgrPress(0, 10, 5));
    await waitUntil(() => endpoint.of('/render/click').length, 'a /render/click');

    expect(endpoint.requests.length).toBeGreaterThan(2);
    for (const req of endpoint.requests) {
      expect(req.headers.authorization).toBe('Bearer sekrit');
      // http_server.cpp accepts either spelling; the client sends both so a
      // token also survives a redirect that drops headers.
      expect(req.query.get('token')).toBe('sekrit');
    }
  }, 30000);

  // THTTP-07 — nothing listening. Mirrors TCDP-08: one line, named endpoint,
  // non-zero exit, and — because the pre-flight probe runs before the terminal
  // is touched — no alt-screen flash on the way out.
  it('an unreachable --term-port exits non-zero with one line naming it', async () => {
    const port = await freePort(); // nothing is listening on it

    term = launchTerminal(port);
    const { code, stderr } = await term.exited;
    expect(code).not.toBe(0);

    const lines = viewerErrors(stderr);
    expect(lines).toHaveLength(1);
    expect(lines[0]).toBe(
      `${ERR_PREFIX}cannot fetch 127.0.0.1:${port}/render/screenshot.ppm (no response)`,
    );
    // The follow-up hint carries no prefix, so viewerErrors() drops it; it is
    // still the second thing the user reads.
    expect(stderr).toContain('Is anoa running? Does it need --term-token?');
  }, 30000);

  // THTTP-09 — bug-001 at the wire. "P6 4000 4000 255" with no body is the
  // exact response that used to walk `pos` past the end of the buffer and copy
  // 48 MB out of a sixteen-byte allocation; TERM-BYTES-03 pins the parser,
  // this pins what a peer sending it actually gets.
  it('a truncated PPM claiming 4000x4000 is refused, not read past the end', async () => {
    const port = await freePort();
    endpoint = await startFakeRender({ port, ppmBody: Buffer.from('P6 4000 4000 255', 'ascii') });

    term = launchTerminal(port);
    const { code, stderr } = await term.exited;

    // Refused at the pre-flight probe, so the viewer never starts — and the
    // process exited on its own rather than dying on a signal.
    expect(code).toBe(1);
    expect(viewerErrors(stderr)).toEqual([
      `${ERR_PREFIX}cannot fetch 127.0.0.1:${port}/render/screenshot.ppm (malformed PPM)`,
    ]);
  }, 30000);
});

// THTTP-08 — G9.
//
// RenderHttpClient once set no SO_RCVTIMEO and no connect timeout, and since
// the frame loop is a QTimer callback that blocking read() happened *on the Qt
// event loop*. A peer that accepted the connection and then went quiet wedged
// the whole viewer: the QSocketNotifier never fired again so Ctrl-C was never
// read; ISIG is off in raw mode so ^C was not a signal either; and SIGTERM only
// set a flag the never-reached frame tick polls. The process survived
// everything short of SIGKILL, on a terminal left in raw mode on the alt
// screen.
//
// This case was committed as `it.fails` — passing *because* the viewer wedged,
// and written to go red the day a timeout landed. bug-003 landed it, and it
// went red exactly as designed, which is why the annotation is gone. It is an
// ordinary test now: the viewer must still answer Ctrl-C with a dead endpoint.
describe.skipIf(!HAVE_UTIL_LINUX_SCRIPT)('terminal against a stalling /render/* endpoint', () => {
  it('stays interactive when the endpoint accepts and then sends nothing', async () => {
    const port = await freePort();
    // stallAfter 1: the 8x8 pre-flight probe is answered, so the viewer starts
    // and takes the terminal; every frame request after it hangs.
    const endpoint = await startFakeRender({ port, stallAfter: 1 });
    const term = launchTerminal(port);

    try {
      await waitUntil(
        () => !term.exit && endpoint.screenshots.length >= 2,
        'the first stalled frame request',
      );

      // quit() writes Ctrl-C and resolves to null if the viewer was still
      // alive five seconds later and had to be killed by process group.
      const result = await term.quit(5000);
      expect(result).not.toBeNull();
    } finally {
      await term.cleanup();
      await endpoint.close();
    }
  }, 30000);
});
