import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import fetch from 'node-fetch';
import WebSocket from 'ws';
import net from 'node:net';
import {
  startBrowser, stopBrowser, BASE_URL, HTTP_PORT,
  newTab, listTabs, getWsDebuggerUrl, sendCdp,
} from './helpers.js';

/**
 * The live view: /render as something you can drive, not just watch.
 *
 * The endpoints under test are the ones a pointer needs and a click endpoint
 * cannot provide — a press that outlives one event, a move that carries what is
 * held, and a stream that shows the tab you asked for rather than the one that
 * happens to be raised.
 */

const post = (path) => fetch(`${BASE_URL}${path}`, { method: 'POST' });

/** Run an expression in a tab and hand back its value. */
async function evalIn(tabId, expression) {
  const targets = await listTabs();
  const target = targets.find((t) => t.anoaTabId === tabId);
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.once('open', res); ws.once('error', rej); });
  try {
    const r = await sendCdp(ws, 'Runtime.evaluate', { expression, returnByValue: true }, 7001);
    return r.result?.result?.value;
  } finally {
    ws.close();
  }
}

/**
 * Read exactly one JPEG out of the multipart stream and hand back its bytes.
 *
 * Raw sockets rather than fetch(): the stream never ends, so anything that
 * waits for a body waits forever.
 */
function firstFrame(query = '') {
  return new Promise((resolve, reject) => {
    const sock = net.createConnection(HTTP_PORT, '127.0.0.1');
    const timer = setTimeout(() => { sock.destroy(); reject(new Error('no frame in 15s')); }, 15000);
    let buf = Buffer.alloc(0);
    let need = -1;
    let body = null;

    sock.on('connect', () => {
      sock.write(`GET /render/stream.mjpeg${query} HTTP/1.1\r\nHost: localhost\r\n\r\n`);
    });
    sock.on('error', (e) => { clearTimeout(timer); reject(e); });
    sock.on('data', (chunk) => {
      buf = Buffer.concat([buf, chunk]);
      if (body === null) {
        // Response headers, then the first part's own headers.
        const end = buf.indexOf('\r\n\r\n');
        if (end < 0) return;
        const rest = buf.subarray(end + 4);
        const partEnd = rest.indexOf('\r\n\r\n');
        if (partEnd < 0) return;
        const partHdr = rest.subarray(0, partEnd).toString();
        const m = /content-length:\s*(\d+)/i.exec(partHdr);
        if (!m) { clearTimeout(timer); sock.destroy(); reject(new Error('no part length')); return; }
        need = parseInt(m[1], 10);
        body = rest.subarray(partEnd + 4);
      } else {
        body = Buffer.concat([body, chunk]);
      }
      if (body && body.length >= need) {
        clearTimeout(timer);
        sock.destroy();
        resolve(body.subarray(0, need));
      }
    });
  });
}

describe('Live view — pointer endpoints', () => {
  let proc;

  beforeAll(async () => { proc = await startBrowser(); }, 20000);
  afterAll(() => stopBrowser(proc));

  // LV-01
  it('GET /render/viewport reports the tab size the input endpoints speak', async () => {
    const resp = await fetch(`${BASE_URL}/render/viewport`);
    expect(resp.status).toBe(200);
    const j = await resp.json();
    expect(j.width).toBeGreaterThan(0);
    expect(j.height).toBeGreaterThan(0);
    // The frame is device pixels and the coordinates are not, so a client that
    // assumed they were the same would be wrong by exactly this factor.
    expect(j.dpr).toBeGreaterThan(0);
  });

  // LV-02
  it('move, mousedown and mouseup each accept a well-formed request', async () => {
    for (const path of ['/render/move?x=10&y=10',
                        '/render/mousedown?x=10&y=10',
                        '/render/mouseup?x=10&y=10']) {
      expect((await post(path)).status, path).toBe(200);
    }
  });

  // LV-03
  it('the pointer endpoints refuse coordinates and buttons they cannot honour', async () => {
    expect((await post('/render/move?x=-1&y=10')).status).toBe(400);
    expect((await post('/render/mousedown?x=10')).status).toBe(400);
    expect((await post('/render/mouseup?x=10&y=10&button=elbow')).status).toBe(400);
  });

  // LV-04 — the whole reason these exist. A click endpoint cannot express a
  // press that is still held while the pointer moves, which is what selecting
  // text and dragging an element both are.
  it('a press, a move and a release arrive as a drag, not as three clicks', async () => {
    const tabs = await listTabs();
    const tabId = tabs[0].anoaTabId;
    await evalIn(tabId, `window.__log = [];
      for (const t of ['mousemove', 'mousedown', 'mouseup', 'click'])
        document.addEventListener(t, e => window.__log.push(t + ':' + e.clientX + ',' + e.clientY + ':b' + e.buttons));
      'ready'`);

    await post('/render/mousedown?x=60&y=50');
    await post('/render/move?x=200&y=120&buttons=left');
    await post('/render/mouseup?x=200&y=120');
    await new Promise((r) => setTimeout(r, 400));

    const log = await evalIn(tabId, 'window.__log.join("|")');
    expect(log).toContain('mousedown:60,50:b1');
    // The move carries the held button through — without it Chromium reads a
    // drag as a hover and no selection ever starts.
    expect(log).toContain('mousemove:200,120:b1');
    expect(log).toContain('mouseup:200,120:b0');
    // down + up at one point is still a click, so nothing regressed for callers
    // that only ever wanted one.
    expect(log).toContain('click:200,120');
  });

  // LV-05
  it('modifiers reach the page', async () => {
    const tabs = await listTabs();
    const tabId = tabs[0].anoaTabId;
    await evalIn(tabId, `window.__mods = '';
      document.addEventListener('mousemove', e => {
        window.__mods = (e.ctrlKey ? 'C' : '') + (e.shiftKey ? 'S' : '') + (e.altKey ? 'A' : '');
      }); 'ready'`);
    await post('/render/move?x=90&y=70&mods=ctrl,shift');
    await new Promise((r) => setTimeout(r, 400));
    expect(await evalIn(tabId, 'window.__mods')).toBe('CS');
  });

  // LV-06 — Ctrl+A was unreachable before: the key table held fourteen names
  // and no modifier, so every shortcut a page defines was undriveable.
  it('a letter with a modifier is a shortcut, not typed text', async () => {
    const tabs = await listTabs();
    const tabId = tabs[0].anoaTabId;
    await evalIn(tabId, `document.body.innerHTML = '<input id=i>';
      document.getElementById('i').focus(); 'ready'`);

    await post('/render/type?text=halo%20dunia');
    await new Promise((r) => setTimeout(r, 300));
    expect(await evalIn(tabId, 'document.getElementById("i").value')).toBe('halo dunia');

    await post('/render/key?key=a&mods=ctrl');
    await new Promise((r) => setTimeout(r, 300));
    const selected = await evalIn(tabId,
      'const i = document.getElementById("i"); i.value.substring(i.selectionStart, i.selectionEnd)');
    expect(selected).toBe('halo dunia');
    // And the shortcut must not also insert an "a".
    expect(await evalIn(tabId, 'document.getElementById("i").value')).toBe('halo dunia');
  });

  // LV-07
  it('function keys are accepted', async () => {
    expect((await post('/render/key?key=f5')).status).toBe(200);
    expect((await post('/render/key?key=f12')).status).toBe(200);
    expect((await post('/render/key?key=f13')).status).toBe(400);
  });
});

describe('Live view — the stream follows ?tab=', () => {
  let proc;

  beforeAll(async () => { proc = await startBrowser(); }, 20000);
  afterAll(() => stopBrowser(proc));

  // LV-08 — the bug this suite was written for. The stream grabbed the
  // container, which is whatever tab is raised, so ?tab= was accepted and then
  // ignored: asking for a background tab silently streamed the active one.
  it('streams the tab that was asked for, not the active one', async () => {
    const first = (await listTabs())[0].anoaTabId;
    const { tabId: second } = await newTab('about:blank');
    expect(second).toBeTruthy();

    await evalIn(first, "document.body.style.background = '#ff0000'; 'ok'");
    await evalIn(second, "document.body.style.background = '#0000ff'; 'ok'");
    await new Promise((r) => setTimeout(r, 500));

    const a = await firstFrame(`?tab=${first}`);
    const b = await firstFrame(`?tab=${second}`);
    const aAgain = await firstFrame(`?tab=${first}`);

    // Same tab twice is byte-identical, which is what makes the comparison
    // below mean "different tab" rather than "different moment".
    expect(aAgain.equals(a)).toBe(true);
    expect(b.equals(a)).toBe(false);
  });

  // LV-09
  it('an unknown tab is refused rather than silently served the active one', async () => {
    const resp = await fetch(`${BASE_URL}/render/stream.mjpeg?tab=t99`);
    expect(resp.status).toBe(404);
  });
});

describe('Live view — tabs from the viewer', () => {
  let proc;

  beforeAll(async () => { proc = await startBrowser(); }, 20000);
  afterAll(() => stopBrowser(proc));

  // LV-10
  it('POST /render/tab/new opens a tab that /json/list then reports', async () => {
    const resp = await post('/render/tab/new');
    expect(resp.status).toBe(200);
    const { id } = await resp.json();
    expect(id).toMatch(/^t[1-9][0-9]*$/);
    const tabs = await listTabs();
    expect(tabs.map((t) => t.anoaTabId)).toContain(id);
  });

  // LV-11
  it('POST /render/tab/close closes that tab and refuses the last one', async () => {
    const { id } = await (await post('/render/tab/new')).json();
    expect((await post(`/render/tab/close?tab=${id}`)).status).toBe(200);
    expect((await listTabs()).map((t) => t.anoaTabId)).not.toContain(id);

    // Down to one: closing it would leave a browser with no page, which the
    // registry refuses for the same reason the CLI does.
    const remaining = await listTabs();
    for (const t of remaining.slice(1))
      await post(`/render/tab/close?tab=${t.anoaTabId}`);
    const last = (await listTabs())[0].anoaTabId;
    expect((await post(`/render/tab/close?tab=${last}`)).status).toBe(409);
  });

  // LV-12
  it('POST /render/tab/close needs a tab named', async () => {
    expect((await post('/render/tab/close')).status).toBe(400);
  });
});

describe('Live view — who may frame it', () => {
  // LV-13 — the view forwards keystrokes as well as showing pixels, so a page
  // that can frame it can read a logged-in session and act inside it.
  it('names an allowed origin when one is configured', async () => {
    const proc = await startBrowser(['--embed-origin', 'https://app.example.com']);
    try {
      const resp = await fetch(`${BASE_URL}/render`);
      expect(resp.headers.get('content-security-policy'))
        .toBe("frame-ancestors 'self' https://app.example.com");
    } finally {
      await stopBrowser(proc);
    }
  }, 20000);

  // LV-14
  it('drops the restriction entirely for --embed-origin "*"', async () => {
    const proc = await startBrowser(['--embed-origin', '*']);
    try {
      const resp = await fetch(`${BASE_URL}/render`);
      expect(resp.headers.get('content-security-policy')).toBeNull();
    } finally {
      await stopBrowser(proc);
    }
  }, 20000);
});

describe('Live view — reading the tab list from the host page', () => {
  const HOST = 'http://127.0.0.1:8080';
  let proc;

  beforeAll(async () => {
    proc = await startBrowser(['--embed-origin', HOST]);
  }, 20000);
  afterAll(() => stopBrowser(proc));

  // LV-15 — an origin trusted to embed the view and drive the browser through
  // it, but not to read /json/list, can show the view and cannot draw a tab bar
  // around it. That is most of what embedding is for, so the two travel
  // together.
  it('answers an allowed origin with its own origin echoed back', async () => {
    const resp = await fetch(`${BASE_URL}/json/list`, { headers: { Origin: HOST } });
    expect(resp.status).toBe(200);
    expect(resp.headers.get('access-control-allow-origin')).toBe(HOST);
    // Or a cache hands one origin's answer to another.
    expect(resp.headers.get('vary')).toBe('Origin');
  });

  // LV-16
  it('says nothing to an origin that was never named', async () => {
    const resp = await fetch(`${BASE_URL}/json/list`,
                             { headers: { Origin: 'https://elsewhere.example' } });
    expect(resp.headers.get('access-control-allow-origin')).toBeNull();
  });

  // LV-17
  it('answers a preflight, which carries no credentials of its own', async () => {
    const resp = await fetch(`${BASE_URL}/render/tab/new`,
                             { method: 'OPTIONS', headers: { Origin: HOST } });
    expect(resp.status).toBe(204);
    expect(resp.headers.get('access-control-allow-methods')).toContain('POST');
  });
});

describe('Live view — CORS stays off until asked for', () => {
  let proc;

  beforeAll(async () => { proc = await startBrowser(); }, 20000);
  afterAll(() => stopBrowser(proc));

  // LV-18 — the default configuration answers exactly as it did before this
  // existed, for every origin.
  it('sends no CORS headers when no origin was configured', async () => {
    const resp = await fetch(`${BASE_URL}/json/list`,
                             { headers: { Origin: 'http://127.0.0.1:8080' } });
    expect(resp.status).toBe(200);
    expect(resp.headers.get('access-control-allow-origin')).toBeNull();
  });
});
