/**
 * Suite 11 — anoa in an E2B sandbox.
 *
 * Every other suite drives a browser on the machine running the tests. This one
 * drives one that is not here at all: a Firecracker microVM booted from the
 * template in integrations/e2b, reached over the public https URL E2B publishes
 * for each of its ports. What is under test is the whole path an agent would
 * use — the template, the tarball inside it, the token on the wire, and the
 * three surfaces anoa exposes once it is up.
 *
 * Prerequisites:
 *   1. E2B_API_KEY in the environment  (the suite skips without it)
 *   2. the template exists:  node integrations/e2b/template.js
 *   3. npm install in this directory
 *
 * Run: node --test e2b.test.js
 *      node --env-file=../../integrations/e2b/.env --test e2b.test.js
 */
import { describe, it, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { Sandbox } from 'e2b';
import puppeteer from 'puppeteer-core';
import { chromium } from '@playwright/test';
import { startAnoa, mcp, callTool, anoaLog } from '../../integrations/e2b/anoa.js';

const TEMPLATE = process.env.ANOA_E2B_TEMPLATE ?? 'anoa';
// Five minutes is the sandbox default and more than this suite needs; it is
// named here because an expired sandbox fails as a connection error, which
// reads like a bug in the browser rather than a clock running out.
const SANDBOX_TIMEOUT_MS = 300_000;

const skip = process.env.E2B_API_KEY
  ? false
  : 'E2B_API_KEY is not set — see docs/E2B.md';

describe('anoa in an E2B sandbox', { skip, timeout: 600_000 }, () => {
  let sandbox;
  let anoa;
  let startedAt;

  before(async () => {
    startedAt = Date.now();
    try {
      sandbox = await Sandbox.create(TEMPLATE, { timeoutMs: SANDBOX_TIMEOUT_MS });
    } catch (err) {
      throw new Error(
        `could not boot template "${TEMPLATE}": ${err.message}\n` +
          'Build it first:  node integrations/e2b/template.js'
      );
    }
    anoa = await startAnoa(sandbox);
  });

  after(async () => {
    // A sandbox left running bills until its timeout, so this is not optional
    // cleanup — and it runs even when the suite failed, which is when a
    // sandbox is most likely to still be up.
    if (sandbox) await sandbox.kill();
  });

  // E2B-01: the template carries the published release, installed through the
  // launcher rather than the raw binary — the launcher is what makes the
  // bundled Qt findable, and a symlink to the wrong file starts nothing.
  it('the template has the release build installed', async () => {
    const { stdout } = await sandbox.commands.run('anoa --version');
    assert.match(stdout.trim(), /^anoa \d+\.\d+\.\d+$/);

    const { stdout: target } = await sandbox.commands.run('readlink -f "$(command -v anoa)"');
    assert.equal(target.trim(), '/opt/anoa/anoa.sh');
  });

  // E2B-02: the browser really is running in there, and the sandbox's public
  // URL reaches it. startAnoa() waits for this, so a failure here means the
  // wait is lying rather than that the browser is slow.
  it('the public URL reaches the browser', async () => {
    assert.match(anoa.url, /^https:\/\/9222-\w+\.e2b\.app$/);
    // /json/version answers with anoa's own name, not Chromium's, so this is
    // also the check that the sandbox URL reached anoa and not something else
    // E2B happens to serve on that port.
    assert.match(anoa.browser, /^anoa\/\d+\.\d+\.\d+$/);

    const res = await fetch(`${anoa.url}/json/version`, {
      headers: { Authorization: `Bearer ${anoa.authToken}` },
    });
    assert.equal(res.status, 200);
  });

  // E2B-03: the URL above is on the public internet and needs no credential to
  // reach, which makes the token the only thing between a stranger and a
  // browser. Both ways in are checked, because only one of them is a header.
  it('rejects requests without the token', async () => {
    const bare = await fetch(`${anoa.url}/json/version`);
    assert.equal(bare.status, 401);

    const mcpBare = await fetch(anoa.mcpUrl, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'tools/list' }),
    });
    assert.equal(mcpBare.status, 401);

    const wrong = await fetch(`${anoa.url}/json/version`, {
      headers: { Authorization: 'Bearer not-the-token' },
    });
    assert.equal(wrong.status, 401);
  });

  // E2B-04: the handshake, over the sandbox edge rather than over loopback.
  it('MCP initialize and tools/list answer over the sandbox URL', async () => {
    const hello = await mcp(anoa, 'initialize', {
      protocolVersion: '2025-06-18',
      capabilities: {},
      clientInfo: { name: 'e2b-suite', version: '1' },
    });
    assert.equal(hello.serverInfo.name, 'anoa');
    assert.match(hello.protocolVersion, /^\d{4}-\d{2}-\d{2}$/);

    const { tools } = await mcp(anoa, 'tools/list');
    assert.ok(tools.length >= 25, `only ${tools.length} tools`);
    const names = tools.map((t) => t.name);
    for (const expected of ['browser_open', 'browser_eval', 'browser_screenshot'])
      assert.ok(names.includes(expected), `${expected} missing from tools/list`);
  });

  // E2B-05: the point of the integration — an agent with nothing but an HTTPS
  // client drives a browser that is nowhere near it.
  it('an MCP tool call drives the page', async () => {
    await callTool(anoa, 'browser_open', { url: 'https://example.com' });
    const title = await callTool(anoa, 'browser_eval', { expression: 'document.title' });
    assert.match(title, /Example Domain/);
  });

  // E2B-06: the HTTP surface, which is what a client that speaks no MCP uses.
  // The magic bytes rather than the content type: a 200 with an error page
  // would pass a header check.
  it('serves a PNG from the render endpoint', async () => {
    const res = await fetch(`${anoa.url}/render/screenshot.png`, {
      headers: { Authorization: `Bearer ${anoa.authToken}` },
    });
    assert.equal(res.status, 200);
    const png = Buffer.from(await res.arrayBuffer());
    assert.deepEqual([...png.subarray(0, 4)], [0x89, 0x50, 0x4e, 0x47]);
    assert.ok(png.length > 1000, `${png.length} bytes is not a screenshot`);
  });

  // E2B-07: the CDP proxy on port+2, which is a different hostname out here
  // than the browser advertises in there. Puppeteer connecting is the proof
  // that the rewrite produced an endpoint a real client accepts.
  it('puppeteer connects to the sandbox over wss', async () => {
    assert.match(anoa.cdpUrl, /^wss:\/\/9224-\w+\.e2b\.app\/devtools\/browser\/.+token=/);

    const browser = await puppeteer.connect({ browserWSEndpoint: anoa.cdpUrl });
    try {
      const page = (await browser.pages())[0] ?? (await browser.newPage());
      await page.goto('https://example.com', { waitUntil: 'domcontentloaded' });
      assert.match(await page.title(), /Example Domain/);
    } finally {
      await browser.disconnect();
    }
  });

  // E2B-08: the other CDP client people bring. Playwright takes the same
  // endpoint through a different code path — it opens its own browser context
  // over the connection — so one passing does not imply the other.
  it('playwright connects to the sandbox over wss', async () => {
    const browser = await chromium.connectOverCDP(anoa.cdpUrl);
    try {
      const context = browser.contexts()[0] ?? (await browser.newContext());
      const page = context.pages()[0] ?? (await context.newPage());
      await page.goto('https://example.com', { waitUntil: 'domcontentloaded' });
      assert.match(await page.title(), /Example Domain/);
    } finally {
      await browser.close();
    }
  });

  // E2B-09: the agent CLI inside the sandbox talks to the same browser the
  // outside is talking to. One browser per sandbox, two ways to drive it.
  it('the CLI inside the sandbox drives the same browser', async () => {
    const run = (verb) =>
      sandbox.commands.run(
        `anoa --port ${anoa.port} --auth-token ${anoa.authToken} ${verb}`
      );

    await run('open https://example.org');
    const { stdout } = await run('eval "document.title"');
    assert.match(stdout, /Example Domain/);

    // And the outside sees that navigation, not a second browser's idea of it.
    const url = await callTool(anoa, 'browser_eval', { expression: 'location.hostname' });
    assert.match(url, /example\.org/);
  });

  // E2B-10: the browser outlives the command that started it.
  //
  // This one waits, and the waiting is the test. A command started with the
  // SDK's `background: true` is supervised, and the supervisor kills it at the
  // default 60s command timeout — measured, not assumed: alive at +30s, gone
  // by +65s, which is why startAnoa uses nohup instead. Every check above
  // finishes inside that minute and passes either way, so without this one the
  // suite would be green against a browser that dies a minute into every
  // session.
  it('the browser outlives the 60s command timeout', async () => {
    const until = startedAt + 75_000;
    while (Date.now() < until) await new Promise((r) => setTimeout(r, 1000));

    const { stdout } = await sandbox.commands.run(`ps -p ${anoa.pid} -o comm=`);
    assert.ok(
      stdout.trim().length > 0,
      `pid ${anoa.pid} is gone ${Math.round((Date.now() - startedAt) / 1000)}s in\n` +
        (await anoaLog(sandbox))
    );

    // Still answering, not merely still a process.
    const res = await fetch(`${anoa.url}/json/version`, {
      headers: { Authorization: `Bearer ${anoa.authToken}` },
    });
    assert.equal(res.status, 200);
  });
});
