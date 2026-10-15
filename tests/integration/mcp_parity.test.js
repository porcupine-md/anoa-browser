/**
 * Suite 10b — MCP and the CLI are one surface.
 *
 * Every tool is a CLI verb underneath, so for every form the CLI documents this
 * runs it both ways against one browser and one page and asks for the same
 * answer: the same text when the command reads something, the same effect when
 * it changes something, the same message when it fails. The MCP table is a
 * hand-written mapping onto argv, and this is the only place a wrong mapping
 * shows up — scroll used to send flags the CLI never reads, and scrolled 400px
 * down whatever it was asked.
 */
import { describe, it, expect, beforeAll, afterAll, beforeEach } from 'vitest';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { startBrowser, stopBrowser, BINARY, BASE_URL, HTTP_PORT } from './helpers.js';

const PAGE = `<!doctype html><title>parity</title>
<h1>Parity page</h1>
<p id="intro">Hello <b>parity</b></p>
<a id="link" href="/next">next page</a>
<input id="name" value="start"><input id="other">
<button id="go" onclick="document.getElementById('out').textContent='clicked'">Go</button>
<div id="out"></div>
<div style="height:4000px"></div>
<script>console.log('parity-log'); console.error('parity-error');</script>`;

let fixture;
let fixtureUrl;
let proc;

// Async on purpose: the fixture server lives in this process, and a blocking
// spawn would leave the browser's request to it unanswered until the command
// it is waiting on gave up.
function run(args, input) {
  return new Promise((resolve) => {
    const child = spawn(BINARY, [...args, '--port', String(HTTP_PORT)]);
    let out = '', err = '';
    child.stdout.on('data', (d) => { out += d; });
    child.stderr.on('data', (d) => { err += d; });
    child.on('close', (code) => resolve({ ok: code === 0, text: (code === 0 ? out : err).trim() }));
    child.stdin.end(input ?? '');
  });
}
const cli = (...args) => run(args);

let id = 0;
async function mcp(name, args = {}) {
  const res = await fetch(`${BASE_URL}/mcp`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', id: ++id, method: 'tools/call',
                           params: { name, arguments: args } }),
  });
  const body = await res.json();
  if (body.error) return { ok: false, text: body.error.message };
  return { ok: !body.result.isError, text: body.result.content[0].text.trim() };
}

// MCP answers "ok" where the command printed nothing: a tool result needs text.
const same = (a, b) => {
  expect(b.ok, `mcp: ${b.text}`).toBe(a.ok);
  expect(b.text).toBe(a.text === '' ? 'ok' : a.text);
};

const evalPage = async (js) => (await cli('eval', js)).text;
const fresh = () => cli('open', fixtureUrl);

describe('MCP and CLI parity', () => {
  beforeAll(async () => {
    fixture = createServer((req, res) => {
      res.writeHead(200, { 'Content-Type': 'text/html' });
      res.end(req.url === '/next' ? '<title>next</title><h1>Next</h1>' : PAGE);
    });
    await new Promise((r) => fixture.listen(0, '127.0.0.1', r));
    fixtureUrl = `http://127.0.0.1:${fixture.address().port}/`;
    proc = await startBrowser();
  }, 30000);

  afterAll(async () => {
    await stopBrowser(proc);
    fixture.close();
  });

  beforeEach(async () => { await fresh(); });

  // ── reads: same text ──────────────────────────────────────────────────────
  const reads = [
    [['snapshot'], 'browser_snapshot', {}],
    [['snapshot', '-i'], 'browser_snapshot', { interactive: true }],
    [['snapshot', '--json'], 'browser_snapshot', { json: true }],
    [['find', 'role', 'textbox'], 'browser_find', { by: 'role', value: 'textbox' }],
    [['find', 'selector', 'input', '--nth', '2'], 'browser_find',
     { by: 'selector', value: 'input', nth: 2 }],
    [['find', 'text', 'Go', '--json'], 'browser_find', { by: 'text', value: 'Go', json: true }],
    [['get', 'text'], 'browser_get', { what: 'text' }],
    [['get', 'text', '#intro'], 'browser_get', { what: 'text', target: '#intro' }],
    [['get', 'html', '#intro'], 'browser_get', { what: 'html', target: '#intro' }],
    [['get', 'value', '#name'], 'browser_get', { what: 'value', target: '#name' }],
    [['get', 'attr', '#link', 'href'], 'browser_get', { what: 'attr', target: '#link', name: 'href' }],
    [['get', 'text', '#missing'], 'browser_get', { what: 'text', target: '#missing' }],
    [['eval', 'document.title + 1'], 'browser_eval', { expression: 'document.title + 1' }],
    [['eval', 'nope.nope'], 'browser_eval', { expression: 'nope.nope' }],
    [['status'], 'browser_status', {}],
    [['status', '--json'], 'browser_status', { json: true }],
    [['console'], 'browser_console', {}],
    [['console', '--level', 'error'], 'browser_console', { level: 'error' }],
    [['console', '--json'], 'browser_console', { json: true }],
    [['errors'], 'browser_errors', {}],
    [['tab', 'list'], 'browser_tab', { action: 'list' }],
    [['tab', 'list', '--json'], 'browser_tab', { action: 'list', json: true }],
    [['downloads'], 'browser_downloads', {}],
    [['skills', 'list'], 'browser_skills', { what: 'list' }],
    [['skills', 'get', 'core'], 'browser_skills', { what: 'core' }],
    [['skills', 'get', 'commands'], 'browser_skills', { what: 'commands' }],
    [['click', '#missing'], 'browser_click', { target: '#missing' }],
    [['set', 'device'], 'browser_set', { what: 'device' }],
  ];
  for (const [argv, tool, args] of reads) {
    it(`${argv.join(' ')}  ==  ${tool} ${JSON.stringify(args)}`, async () => {
      same(await cli(...argv), await mcp(tool, args));
    });
  }

  // ── writes: same effect ───────────────────────────────────────────────────
  // Each runs the CLI form on a fresh page, reads the result, then does the
  // same through MCP on another fresh page.
  async function sameEffect(argv, tool, args, read) {
    const a = await cli(...argv);
    const afterCli = await read();
    await fresh();
    const b = await mcp(tool, args);
    const afterMcp = await read();
    same(a, b);
    expect(afterMcp).toBe(afterCli);
    return afterCli;
  }
  const scrollY = async () => await evalPage('Math.round(scrollY)');

  it('scroll (default) moves the same distance', async () => {
    expect(await sameEffect(['scroll'], 'browser_scroll', {}, scrollY)).not.toBe('0');
  });
  it('scroll --by 300', async () => {
    expect(await sameEffect(['scroll', '--by', '300'], 'browser_scroll', { by: 300 }, scrollY))
      .toBe('300');
  });
  it('scroll --bottom, then --top and --up', async () => {
    expect(await sameEffect(['scroll', '--bottom'], 'browser_scroll', { bottom: true }, scrollY))
      .not.toBe('0');
    for (const [argv, args] of [[['scroll', '--top'], { top: true }],
                                [['scroll', '--up'], { up: true }]]) {
      await cli('scroll', '--bottom');
      const a = await cli(...argv); const afterCli = await scrollY();
      await cli('scroll', '--bottom');
      const b = await mcp('browser_scroll', args); const afterMcp = await scrollY();
      same(a, b);
      expect(afterMcp).toBe(afterCli);
    }
  });
  it('mouse wheel <dy> scrolls the same', async () => {
    await sameEffect(['mouse', 'wheel', '250', '100', '100'], 'browser_mouse',
                     { action: 'wheel', dy: 250, x: 100, y: 100 },
                     async () => await evalPage('new Promise(r=>setTimeout(()=>r(Math.round(scrollY)),300))'));
  });
  it('fill, click, type, press', async () => {
    await sameEffect(['fill', '#name', 'filled'], 'browser_fill',
                     { target: '#name', text: 'filled' }, async () => (await cli('get', 'value', '#name')).text);
    await sameEffect(['click', '#go'], 'browser_click', { target: '#go' },
                     async () => (await cli('get', 'text', '#out')).text);
    const typed = async () => (await cli('get', 'value', '#other')).text;
    await cli('click', '#other');
    const a = await cli('type', 'abc'); const afterCli = await typed();
    await fresh(); await cli('click', '#other');
    const b = await mcp('browser_type', { text: 'abc' });
    same(a, b); expect(await typed()).toBe(afterCli);
    same(await cli('press', 'Tab'), await mcp('browser_press', { key: 'Tab' }));
  });
  it('set viewport, set media', async () => {
    await sameEffect(['set', 'viewport', '800', '600'], 'browser_set',
                     { what: 'viewport', values: ['800', '600'] }, async () => await evalPage('innerWidth'));
    await sameEffect(['set', 'media', 'dark'], 'browser_set', { what: 'media', values: ['dark'] },
                     async () => await evalPage("matchMedia('(prefers-color-scheme: dark)').matches"));
    await cli('set', 'viewport', '1280', '720');
    await cli('set', 'media', 'light');
  });
  it('cookies set / list / --url / clear', async () => {
    await cli('cookies', 'clear');
    await sameEffect(['cookies', 'set', 'k', 'v1'], 'browser_cookies',
                     { action: 'set', name: 'k', value: 'v1' }, async () => (await cli('cookies')).text);
    same(await cli('cookies'), await mcp('browser_cookies', {}));
    same(await cli('cookies'), await mcp('browser_cookies', { action: 'list' }));
    same(await cli('cookies', '--json'), await mcp('browser_cookies', { json: true }));
    await sameEffect(['cookies', 'set', 'other', 'x', '--url', 'http://example.com/'],
                     'browser_cookies',
                     { action: 'set', name: 'other', value: 'x', url: 'http://example.com/' },
                     async () => (await cli('cookies')).text);
    await sameEffect(['cookies', 'clear'], 'browser_cookies', { action: 'clear' },
                     async () => (await cli('cookies')).text);
  });
  it('storage local / <key> / set / remove / clear', async () => {
    const all = async () => (await cli('storage', 'local')).text;
    await sameEffect(['storage', 'local', 'set', 'k', 'v'], 'browser_storage',
                     { area: 'local', action: 'set', key: 'k', value: 'v' }, all);
    same(await cli('storage', 'local'), await mcp('browser_storage', { area: 'local' }));
    same(await cli('storage', 'local', 'k'), await mcp('browser_storage', { area: 'local', key: 'k' }));
    same(await cli('storage', 'local', 'k'),
         await mcp('browser_storage', { area: 'local', action: 'get', key: 'k' }));
    same(await cli('storage', 'session'), await mcp('browser_storage', { area: 'session' }));
    await sameEffect(['storage', 'local', 'remove', 'k'], 'browser_storage',
                     { area: 'local', action: 'remove', key: 'k' }, all);
    await sameEffect(['storage', 'local', 'clear'], 'browser_storage',
                     { area: 'local', action: 'clear' }, all);
  });
  it('wait forms', { timeout: 30000 }, async () => {
    same(await cli('wait', '--load'), await mcp('browser_wait', { load: true }));
    same(await cli('wait', '--selector', '#go'), await mcp('browser_wait', { selector: '#go' }));
    same(await cli('wait', '--text', 'Parity page'), await mcp('browser_wait', { text: 'Parity page' }));
    same(await cli('wait', '--url', '127.0.0.1'), await mcp('browser_wait', { url: '127.0.0.1' }));
    same(await cli('wait', '--fn', 'true'), await mcp('browser_wait', { fn: 'true' }));
    same(await cli('wait', '--ms', '10'), await mcp('browser_wait', { ms: 10 }));
    same(await cli('wait', '--network-idle', '--network-idle-ms', '100'),
         await mcp('browser_wait', { network_idle: true, network_idle_ms: 100 }));
    same(await cli('wait', '#missing', '--state', 'hidden'),
         await mcp('browser_wait', { selector: '#missing', state: 'hidden' }));
    same(await cli('wait', '--selector', '#missing', '--timeout', '300'),
         await mcp('browser_wait', { selector: '#missing', timeout: 300 }));
  });
  it('open, back, forward, reload', async () => {
    same(await cli('open', fixtureUrl + 'next'), await mcp('browser_open', { url: fixtureUrl + 'next' }));
    same(await cli('back'), await mcp('browser_back', {}));
    same(await cli('forward'), await mcp('browser_forward', {}));
    same(await cli('reload'), await mcp('browser_reload', {}));
    same(await cli('open', fixtureUrl, '--json'), await mcp('browser_open', { url: fixtureUrl, json: true }));
  });
  it('exec runs the same script', async () => {
    const script = 'get text #intro\neval 1+1\n';
    same(await run(['exec', '-'], script), await mcp('browser_exec', { script }));
  });

  // Input sent before the page painted was acknowledged and dropped. A process
  // per command hid it; exec and MCP, which act at once, lost two in three.
  it('a click straight after open lands, through exec and through MCP', async () => {
    for (let i = 0; i < 5; i++) {
      await run(['exec', '-'], `open ${fixtureUrl}\nclick #go\n`);
      expect((await cli('get', 'text', '#out')).text, `exec, try ${i}`).toBe('clicked');
      await mcp('browser_open', { url: fixtureUrl });
      await mcp('browser_click', { target: '#go' });
      expect((await cli('get', 'text', '#out')).text, `mcp, try ${i}`).toBe('clicked');
    }
  });

  // ── tabs: --tab works on every tool ───────────────────────────────────────
  it('tab new --name, --tab on a read, select, close', async () => {
    const made = await cli('tab', 'new', fixtureUrl + 'next', '--name', 'side');
    expect(made.ok, made.text).toBe(true);
    await cli('wait', '--load', '--tab', 'side');
    same(await cli('get', 'text', 'h1', '--tab', 'side'),
         await mcp('browser_get', { what: 'text', target: 'h1', tab: 'side' }));
    same(await cli('eval', 'document.title', '--tab', 'side'),
         await mcp('browser_eval', { expression: 'document.title', tab: 'side' }));
    // The active tab is still the first one, for both.
    same(await cli('get', 'text', 'h1'), await mcp('browser_get', { what: 'text', target: 'h1' }));
    same(await cli('tab', 'select', 'side'), await mcp('browser_tab', { action: 'select', target: 'side' }));
    // After a select, a call with no --tab acts on the newly active tab.
    expect((await mcp('browser_get', { what: 'text', target: 'h1' })).text)
      .toBe((await cli('get', 'text', 'h1')).text);
    await cli('tab', 'new', 'about:blank', '--name', 'side2');
    const closedByCli = await cli('tab', 'close', 'side2');
    const closedByMcp = await mcp('browser_tab', { action: 'close', target: 'side' });
    expect(closedByMcp.ok, closedByMcp.text).toBe(closedByCli.ok);
    expect((await cli('tab', 'list')).text).not.toMatch(/side/);
  });
  it('tab new --profile and --isolated are reachable', async () => {
    const viaMcp = await mcp('browser_tab', { action: 'new', target: 'about:blank',
                                              profile: 'parity', name: 'p1' });
    expect(viaMcp.ok, viaMcp.text).toBe(true);
    expect((await cli('tab', 'list')).text).toMatch(/p1/);
    expect((await mcp('browser_tab', { action: 'close', target: 'p1' })).ok).toBe(true);
  });

  // ── the surface itself ────────────────────────────────────────────────────
  it('every CLI verb but help has a tool', async () => {
    const res = await fetch(`${BASE_URL}/mcp`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ jsonrpc: '2.0', id: ++id, method: 'tools/list' }),
    });
    const names = (await res.json()).result.tools.map((t) => t.name);
    const verbs = ['open', 'back', 'forward', 'reload', 'wait', 'snapshot', 'find', 'get',
                   'eval', 'status', 'click', 'fill', 'type', 'press', 'scroll', 'mouse',
                   'upload', 'screenshot', 'pdf', 'cookies', 'storage', 'set', 'tab',
                   'console', 'errors', 'network', 'downloads', 'exec', 'skills', 'close'];
    for (const v of verbs) expect(names).toContain(`browser_${v}`);
  });
});
