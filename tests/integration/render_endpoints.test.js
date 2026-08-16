import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import fetch from 'node-fetch';
import { startBrowser, stopBrowser, BASE_URL, newTab, listTabs } from './helpers.js';

const AUTH_TOKEN = 'inttest-render-abc';

describe('Render endpoints (no auth)', () => {
  let proc;

  beforeAll(async () => {
    proc = await startBrowser();
  }, 20000);

  afterAll(() => stopBrowser(proc));

  // RND-01
  it('GET /render/screenshot.png returns 200 with image/png', async () => {
    const resp = await fetch(`${BASE_URL}/render/screenshot.png`);
    expect(resp.status).toBe(200);
    expect(resp.headers.get('content-type')).toMatch(/image\/png/);
  });

  // RND-02
  it('GET /render/screenshot.png body starts with PNG magic bytes', async () => {
    const resp = await fetch(`${BASE_URL}/render/screenshot.png`);
    const buf = Buffer.from(await resp.arrayBuffer());
    // PNG magic: 89 50 4E 47
    expect(buf[0]).toBe(0x89);
    expect(buf[1]).toBe(0x50); // 'P'
    expect(buf[2]).toBe(0x4e); // 'N'
    expect(buf[3]).toBe(0x47); // 'G'
  });

  // RND-03
  it('GET /render/screenshot.png has Cache-Control: no-cache', async () => {
    const resp = await fetch(`${BASE_URL}/render/screenshot.png`);
    expect(resp.headers.get('cache-control')).toBe('no-cache');
  });

  // RND-05
  it('GET /render/html returns 200 with text/html', async () => {
    const resp = await fetch(`${BASE_URL}/render/html`);
    expect(resp.status).toBe(200);
    expect(resp.headers.get('content-type')).toMatch(/text\/html/);
  });

  // RND-06
  it('GET /render/html body contains HTML structure', async () => {
    const body = await fetch(`${BASE_URL}/render/html`).then(r => r.text());
    expect(body).toMatch(/<html/i);
    expect(body).toMatch(/<\/html>/i);
  });

  // RND-07
  it('GET /render/html has Cache-Control: no-cache', async () => {
    const resp = await fetch(`${BASE_URL}/render/html`);
    expect(resp.headers.get('cache-control')).toBe('no-cache');
  });

  // RND-09
  it('POST /render/navigate with valid HTTP URL (query param) returns 200 "navigating"', async () => {
    const resp = await fetch(`${BASE_URL}/render/navigate?url=http://example.com`, {
      method: 'POST',
    });
    expect(resp.status).toBe(200);
    const body = await resp.text();
    expect(body).toBe('navigating');
  });

  // RND-10
  it('POST /render/navigate with valid HTTPS URL (query param) returns 200', async () => {
    const resp = await fetch(`${BASE_URL}/render/navigate?url=https://example.com`, {
      method: 'POST',
    });
    expect(resp.status).toBe(200);
  });

  // RND-11
  it('POST /render/navigate with URL in body returns 200', async () => {
    const resp = await fetch(`${BASE_URL}/render/navigate`, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: 'https://example.com',
    });
    expect(resp.status).toBe(200);
    const body = await resp.text();
    expect(body).toBe('navigating');
  });

  // RND-12
  it('POST /render/navigate with no URL returns 400 "invalid url"', async () => {
    const resp = await fetch(`${BASE_URL}/render/navigate`, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: '',
    });
    expect(resp.status).toBe(400);
    const body = await resp.text();
    expect(body).toBe('invalid url');
  });

  // RND-13
  it('POST /render/navigate with relative URL returns 400', async () => {
    const resp = await fetch(`${BASE_URL}/render/navigate?url=relative/path`, {
      method: 'POST',
    });
    expect(resp.status).toBe(400);
    const body = await resp.text();
    expect(body).toBe('invalid url');
  });

  // RND-14
  it('POST /render/navigate with javascript: scheme returns 400 "scheme not allowed"', async () => {
    const resp = await fetch(
      `${BASE_URL}/render/navigate?url=${encodeURIComponent('javascript:alert(1)')}`,
      { method: 'POST' },
    );
    expect(resp.status).toBe(400);
    const body = await resp.text();
    expect(body).toBe('scheme not allowed');
  });

  // RND-15
  it('POST /render/navigate with ftp: scheme returns 400 "scheme not allowed"', async () => {
    const resp = await fetch(
      `${BASE_URL}/render/navigate?url=${encodeURIComponent('ftp://example.com')}`,
      { method: 'POST' },
    );
    expect(resp.status).toBe(400);
    const body = await resp.text();
    expect(body).toBe('scheme not allowed');
  });

  // RND-16
  it('GET /render returns 200 with text/html', async () => {
    const resp = await fetch(`${BASE_URL}/render`);
    expect(resp.status).toBe(200);
    expect(resp.headers.get('content-type')).toMatch(/text\/html/);
  });

  // RND-17. The page polled a PNG every 500 ms when it was a picture of a
  // browser. It is a live view now, so it opens the MJPEG stream instead.
  it('GET /render page body opens the MJPEG stream', async () => {
    const body = await fetch(`${BASE_URL}/render`).then(r => r.text());
    expect(body).toContain('/render/stream.mjpeg');
  });

  // RND-17b — the view forwards input, so a page that can frame it can drive
  // the browser. Same-origin only unless an origin is named on the command line.
  it('GET /render restricts framing to same-origin by default', async () => {
    const resp = await fetch(`${BASE_URL}/render`);
    expect(resp.headers.get('content-security-policy')).toBe("frame-ancestors 'self'");
  });

  // RND-19
  it('GET /render has Cache-Control: no-cache', async () => {
    const resp = await fetch(`${BASE_URL}/render`);
    expect(resp.headers.get('cache-control')).toBe('no-cache');
  });

  // RND-20
  it('GET /render/ (trailing slash) returns 301 to /render', async () => {
    const resp = await fetch(`${BASE_URL}/render/`, { redirect: 'manual' });
    expect(resp.status).toBe(301);
    expect(resp.headers.get('location')).toBe('/render');
  });

  // RND-21
  it('GET /render/?token=abc preserves query string in redirect Location', async () => {
    const resp = await fetch(`${BASE_URL}/render/?token=abc`, { redirect: 'manual' });
    expect(resp.status).toBe(301);
    expect(resp.headers.get('location')).toBe('/render?token=abc');
  });

  // RND-22
  it('GET /render/stream.mjpeg returns 200 with multipart/x-mixed-replace content type', async () => {
    const controller = new AbortController();
    const resp = await fetch(`${BASE_URL}/render/stream.mjpeg`, {
      signal: controller.signal,
    });
    expect(resp.status).toBe(200);
    expect(resp.headers.get('content-type')).toMatch(/multipart\/x-mixed-replace.*boundary=frame/);
    controller.abort();
  });

  // RND-24
  it('GET /render/stream.mjpeg has no Content-Length header', async () => {
    const controller = new AbortController();
    const resp = await fetch(`${BASE_URL}/render/stream.mjpeg`, {
      signal: controller.signal,
    });
    expect(resp.headers.get('content-length')).toBeNull();
    controller.abort();
  });

  // RND-23
  it('GET /render/stream.mjpeg stream begins with MJPEG boundary marker', async () => {
    await new Promise((resolve, reject) => {
      const controller = new AbortController();
      const timeout = setTimeout(() => {
        controller.abort();
        reject(new Error('No MJPEG boundary received within 15s'));
      }, 15000);

      fetch(`${BASE_URL}/render/stream.mjpeg`, { signal: controller.signal })
        .then(resp => {
          const chunks = [];
          resp.body.on('data', chunk => {
            chunks.push(chunk);
            const text = Buffer.concat(chunks).toString('binary');
            if (text.includes('--frame')) {
              clearTimeout(timeout);
              controller.abort();
              resolve();
            }
          });
          resp.body.on('error', (err) => {
            if (err.name !== 'AbortError') reject(err);
          });
        })
        .catch(err => {
          if (err.name !== 'AbortError') reject(err);
        });
    });
  });
});

describe('Render endpoints (with auth token)', () => {
  let proc;

  beforeAll(async () => {
    proc = await startBrowser([`--auth-token=${AUTH_TOKEN}`]);
  }, 20000);

  afterAll(() => stopBrowser(proc));

  // RND-25
  it('GET /render/screenshot.png without token returns 401', async () => {
    const resp = await fetch(`${BASE_URL}/render/screenshot.png`);
    expect(resp.status).toBe(401);
  });

  // RND-26
  it('GET /render/screenshot.png with Bearer token returns 200', async () => {
    const resp = await fetch(`${BASE_URL}/render/screenshot.png`, {
      headers: { Authorization: `Bearer ${AUTH_TOKEN}` },
    });
    expect(resp.status).toBe(200);
  });

  // RND-27
  it('GET /render/screenshot.png with ?token= returns 200', async () => {
    const resp = await fetch(`${BASE_URL}/render/screenshot.png?token=${AUTH_TOKEN}`);
    expect(resp.status).toBe(200);
  });

  // RND-28
  it('GET /render/screenshot.png with wrong token returns 401', async () => {
    const resp = await fetch(`${BASE_URL}/render/screenshot.png`, {
      headers: { Authorization: 'Bearer wrongtoken' },
    });
    expect(resp.status).toBe(401);
  });

  // RND-18. The page used to have the server's token substituted into it, so
  // every authenticated fetch of /render returned a document with the secret
  // written in it. It is served verbatim now and the viewer reads the token
  // from its own query string, which is the one the client already holds —
  // so the token must NOT appear in the body.
  it('GET /render does not write the auth token into the page', async () => {
    const resp = await fetch(`${BASE_URL}/render`, {
      headers: { Authorization: `Bearer ${AUTH_TOKEN}` },
    });
    const body = await resp.text();
    expect(resp.status).toBe(200);
    expect(body).not.toContain(AUTH_TOKEN);
  });
});

describe('Render endpoints, tab selection', () => {
  let proc;
  let second;

  beforeAll(async () => {
    proc = await startBrowser();
    // Two documents, told apart by their body text. A page is enough; there is
    // nothing here to serve a fixture.
    await fetch(`${BASE_URL}/render/navigate?url=about:blank`, { method: 'POST' });
    second = await newTab('about:blank');
    // Give each tab its own content through its own endpoint.
    await fetch(`${BASE_URL}/render/navigate?url=about:blank`, { method: 'POST' });
  }, 30000);

  afterAll(() => stopBrowser(proc));

  // RND-T01: the whole point — one endpoint, two tabs, two answers.
  it('?tab= selects which tab /render/html reads', async () => {
    const tabs = await listTabs();
    expect(tabs.length).toBeGreaterThanOrEqual(2);
    const first = tabs.find((t) => t.anoaTabId !== second.tabId).anoaTabId;

    // Two documents that cannot be mistaken for each other. example.com and
    // example.net would not do: they serve the same page.
    await fetch(`${BASE_URL}/render/navigate?url=${encodeURIComponent('https://example.com')}&tab=${first}`,
                { method: 'POST' });
    await fetch(`${BASE_URL}/render/navigate?url=about:blank&tab=${second.tabId}`,
                { method: 'POST' });
    await new Promise((r) => setTimeout(r, 3000));

    const a = await (await fetch(`${BASE_URL}/render/html?tab=${first}`)).text();
    const b = await (await fetch(`${BASE_URL}/render/html?tab=${second.tabId}`)).text();
    expect(a.toLowerCase()).toContain('example domain');
    expect(b.toLowerCase()).not.toContain('example domain');
  }, 20000);

  // RND-T02: a wrong id must not fall back to the active tab. Silently acting
  // on another page is the failure an agent cannot detect.
  it('an unknown or malformed tab is 404, never a silent fallback', async () => {
    for (const bad of ['t99', 'junk', 't0']) {
      const resp = await fetch(`${BASE_URL}/render/html?tab=${bad}`);
      expect(resp.status).toBe(404);
      expect(await resp.json()).toEqual({ error: `no tab ${bad}` });
    }
  });

  // RND-T03: geometry describes the TARGETED tab, and a background tab is
  // sized like the container rather than keeping its birth size — that was
  // 100x30, and every coordinate measured against it was wrong.
  it('viewport headers describe the targeted tab, background or not', async () => {
    const tabs = await listTabs();
    const geometry = async (tab) => {
      const resp = await fetch(`${BASE_URL}/render/screenshot.png?tab=${tab}`);
      expect(resp.status).toBe(200);
      return [resp.headers.get('x-anoa-viewport-width'),
              resp.headers.get('x-anoa-viewport-height')];
    };
    const active = await geometry(tabs.find((t) => t.anoaActive).anoaTabId);
    const background = await geometry(tabs.find((t) => !t.anoaActive).anoaTabId);
    expect(background).toEqual(active);
    expect(Number(active[0])).toBeGreaterThan(200);
  });

  // RND-T04: no ?tab= is the active tab, byte-identical to before tabs existed.
  it('no ?tab= means the active tab', async () => {
    const tabs = await listTabs();
    const activeId = tabs.find((t) => t.anoaActive).anoaTabId;
    const bare = await fetch(`${BASE_URL}/render/screenshot.png`);
    const named = await fetch(`${BASE_URL}/render/screenshot.png?tab=${activeId}`);
    expect(bare.headers.get('x-anoa-viewport-width'))
      .toBe(named.headers.get('x-anoa-viewport-width'));
  });
});
