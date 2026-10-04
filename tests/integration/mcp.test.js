/**
 * Suite 10 — the MCP endpoint.
 *
 * MCP is how a client with no shell drives this browser, so these go through
 * HTTP exactly as such a client would: a JSON-RPC message in, a JSON response
 * out. Nothing here reaches into the binary.
 */
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { startBrowser, stopBrowser, BASE_URL, HTTP_PORT } from './helpers.js';

const MCP = `${BASE_URL}/mcp`;

/** One JSON-RPC round trip. Returns the parsed body and the HTTP status. */
async function rpc(message, { token, origin, url = MCP } = {}) {
  const headers = { 'Content-Type': 'application/json' };
  if (token) headers.Authorization = `Bearer ${token}`;
  if (origin) headers.Origin = origin;
  const res = await fetch(url, {
    method: 'POST',
    headers,
    body: typeof message === 'string' ? message : JSON.stringify(message),
  });
  const text = await res.text();
  return { status: res.status, body: text ? JSON.parse(text) : null, raw: text };
}

let id = 0;
const call = (name, args = {}, opts) =>
  rpc({ jsonrpc: '2.0', id: ++id, method: 'tools/call',
        params: { name, arguments: args } }, opts);

describe('MCP endpoint', () => {
  let proc;

  beforeAll(async () => {
    proc = await startBrowser();
  }, 30000);

  afterAll(async () => {
    await stopBrowser(proc);
  });

  // MCP-01: the handshake. A client that cannot negotiate a version never gets
  // as far as asking what tools exist.
  it('initialize reports a protocol version, the server and its capabilities', async () => {
    const { status, body } = await rpc({
      jsonrpc: '2.0', id: ++id, method: 'initialize',
      params: { protocolVersion: '2025-06-18', capabilities: {},
                clientInfo: { name: 'suite', version: '1' } },
    });
    expect(status).toBe(200);
    expect(body.result.protocolVersion).toMatch(/^\d{4}-\d{2}-\d{2}$/);
    expect(body.result.serverInfo.name).toBe('anoa');
    expect(body.result.serverInfo.version).toMatch(/^\d+\.\d+\.\d+$/);
    expect(body.result.capabilities).toHaveProperty('tools');
  });

  // MCP-02: every tool a client is offered has to be one it can actually call,
  // which means a name, something to read, and a schema that parses.
  it('tools/list returns well-formed tools', async () => {
    const { body } = await rpc({ jsonrpc: '2.0', id: ++id, method: 'tools/list' });
    const tools = body.result.tools;
    expect(tools.length).toBeGreaterThanOrEqual(25);

    for (const t of tools) {
      expect(t.name, JSON.stringify(t)).toMatch(/^browser_[a-z_]+$/);
      expect(t.description.length).toBeGreaterThan(20);
      expect(t.inputSchema.type).toBe('object');
      expect(t.inputSchema).toHaveProperty('properties');
    }
    // No duplicates — a client keys its tool map by name.
    expect(new Set(tools.map((t) => t.name)).size).toBe(tools.length);
  });

  // MCP-03: the point of the whole thing — a tool call drives the real browser
  // and the answer comes back as content a model can read.
  it('tools/call drives the page and returns its text', async () => {
    const opened = await call('browser_open', { url: 'example.com' });
    expect(opened.body.result.isError).toBeUndefined();
    expect(opened.body.result.content[0].text).toMatch(/example\.com/);

    const text = await call('browser_get', { what: 'text' });
    expect(text.body.result.content[0].type).toBe('text');
    expect(text.body.result.content[0].text.length).toBeGreaterThan(20);
  });

  // MCP-04: refs have to survive between calls, or the snapshot/act loop that
  // the whole CLI is built around does not work over MCP either.
  it('a ref from snapshot is usable by a later call', async () => {
    await call('browser_open', { url: 'example.com' });
    const snap = await call('browser_snapshot', { interactive: true });
    const ref = snap.body.result.content[0].text.match(/@e\d+/);
    expect(ref, snap.body.result.content[0].text).toBeTruthy();

    const href = await call('browser_get', { what: 'attr', target: ref[0], name: 'href' });
    expect(href.body.result.isError).toBeUndefined();
    expect(href.body.result.content[0].text).toMatch(/^https?:\/\//);
  });

  // MCP-05: a command that failed is a *result*, not a transport error. The
  // protocol reserves errors for malformed calls; a model needs to read the
  // failure and decide what to do, which it cannot do with a JSON-RPC error.
  it('a failing tool returns isError with the reason, not a protocol error', async () => {
    const { body } = await call('browser_click', { target: '#definitely-not-here' });
    expect(body.error).toBeUndefined();
    expect(body.result.isError).toBe(true);
    expect(body.result.content[0].text).toMatch(/no element/i);
  });

  // MCP-06: the error codes a client branches on.
  it('malformed calls get the right JSON-RPC codes', async () => {
    const unknownMethod = await rpc({ jsonrpc: '2.0', id: ++id, method: 'nope' });
    expect(unknownMethod.body.error.code).toBe(-32601);

    const unknownTool = await call('browser_does_not_exist');
    expect(unknownTool.body.error.code).toBe(-32602);

    const missingArg = await call('browser_open', {});
    expect(missingArg.body.error.code).toBe(-32602);
    expect(missingArg.body.error.message).toMatch(/url/);

    const parse = await rpc('{not json');
    expect(parse.status).toBe(400);
    expect(parse.body.error.code).toBe(-32700);
  });

  // MCP-07: a notification has no id and takes no reply — answering one would
  // leave a client correlating a response to a request it never made.
  it('a notification is accepted with no body', async () => {
    const res = await fetch(MCP, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }),
    });
    expect(res.status).toBe(202);
    expect((await res.text()).trim()).toBe('');
  });

  // MCP-08: GET opens the server-initiated stream. This server pushes nothing,
  // and declining is more honest than holding a stream open forever.
  it('GET is declined rather than left hanging', async () => {
    const res = await fetch(MCP);
    expect(res.status).toBe(405);
  });

  // MCP-09: commands run on one connection and each one blocks it. Two at once
  // must be refused rather than interleaved — and refused *quickly*, not by
  // queueing behind a four-second wait.
  it('a second tool call during one in flight is refused, not queued', async () => {
    const slow = call('browser_wait', { ms: 3000 });
    await new Promise((r) => setTimeout(r, 600));

    const started = Date.now();
    const second = await call('browser_status', {});
    const waited = Date.now() - started;

    expect(second.body.error.code).toBe(-32000);
    expect(second.body.error.message).toMatch(/one at a time/i);
    expect(waited, 'the second call queued instead of being refused').toBeLessThan(1500);

    // And the first still finishes properly.
    const first = await slow;
    expect(first.body.result.isError).toBeUndefined();
  }, 20000);

  // MCP-10: an override set through MCP belongs to the tab, like one set from
  // the CLI — the two surfaces drive the same browser and must agree.
  it('an emulation override set over MCP outlives the call', async () => {
    await call('browser_open', { url: 'example.com' });
    await call('browser_set', { what: 'media', values: ['dark'] });

    const dark = await call('browser_eval', {
      expression: `matchMedia('(prefers-color-scheme: dark)').matches`,
    });
    expect(dark.body.result.content[0].text.trim()).toBe('true');

    await call('browser_set', { what: 'media', values: ['light'] });
  });

  // MCP-11: exec takes its script as an argument, because an MCP call has
  // neither a file nor a stdin to read one from.
  it('browser_exec runs a multi-line script', async () => {
    const { body } = await call('browser_exec', {
      script: 'open example.com\nget text\n',
    });
    expect(body.result.isError).toBeUndefined();
    expect(body.result.content[0].text.length).toBeGreaterThan(20);
  });

  // MCP-12: no Origin is a client, not a page. A page in someone's browser can
  // reach localhost through DNS rebinding, and the only thing separating the
  // two is this header — which matters most in the default configuration,
  // where there is no token to stop it either.
  it('rejects a foreign Origin, and allows a request with none', async () => {
    const none = await rpc({ jsonrpc: '2.0', id: ++id, method: 'tools/list' });
    expect(none.status).toBe(200);

    const foreign = await rpc({ jsonrpc: '2.0', id: ++id, method: 'tools/list' },
                              { origin: 'https://evil.example' });
    expect(foreign.status).toBe(403);

    const nullOrigin = await rpc({ jsonrpc: '2.0', id: ++id, method: 'tools/list' },
                                 { origin: 'null' });
    expect(nullOrigin.status).toBe(403);
  });
});

describe('MCP endpoint with --auth-token', () => {
  let proc;
  const TOKEN = 'mcp-suite-token';

  beforeAll(async () => {
    proc = await startBrowser([`--auth-token=${TOKEN}`,
                               '--embed-origin=https://trusted.example']);
  }, 30000);

  afterAll(async () => {
    await stopBrowser(proc);
  });

  // MCP-13: the endpoint sits behind the same token as everything else. It
  // drives a whole browser, so an unauthenticated one would be the widest hole
  // this binary has.
  it('requires the token, and takes it either way the rest of the API does', async () => {
    const list = { jsonrpc: '2.0', id: 1, method: 'tools/list' };

    const anonymous = await fetch(MCP, {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(list),
    });
    expect(anonymous.status).toBe(401);

    const bearer = await rpc(list, { token: TOKEN });
    expect(bearer.status).toBe(200);
    expect(bearer.body.result.tools.length).toBeGreaterThan(0);

    const viaQuery = await rpc(list, { url: `${MCP}?token=${TOKEN}` });
    expect(viaQuery.status).toBe(200);

    const wrong = await rpc(list, { token: 'not-it' });
    expect(wrong.status).toBe(401);
  });

  // MCP-14: an origin named with --embed-origin is allowed through.
  it('allows an origin the browser was told to trust', async () => {
    const list = { jsonrpc: '2.0', id: 2, method: 'tools/list' };
    const trusted = await rpc(list, { token: TOKEN, origin: 'https://trusted.example' });
    expect(trusted.status).toBe(200);
  });
});
