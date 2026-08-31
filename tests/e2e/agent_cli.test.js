/**
 * Suite 8 — the agent command layer, end to end.
 *
 * These drive the real binary against a real browser, because that is the only
 * place the interesting failures live: the refs have to survive between
 * separate *processes*, which no in-process test can check. Everything here
 * runs the CLI exactly as an agent would, through argv and exit codes.
 *
 * Prerequisites: a build at ../../build (see docs/BUILDING.md).
 */
import { describe, it, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { existsSync, rmSync, writeFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '../..');

// macOS builds into an app bundle; Linux and Windows leave the binary in place.
const CANDIDATES = [
  resolve(root, 'build/anoa.app/Contents/MacOS/anoa'),
  resolve(root, 'build/anoa'),
];
const BIN = CANDIDATES.find(existsSync);
const PORT = Number(process.env.ANOA_E2E_PORT ?? 9455);

let browser;

/** Run the CLI. Never throws on a non-zero exit — the exit code is the result. */
function run(args) {
  const r = spawnSync(BIN, args, { encoding: 'utf8' });
  return { code: r.status, out: (r.stdout || '').trim(), err: (r.stderr || '').trim() };
}

/** Run one agent command against the browser this suite started. */
function anoa(...args) {
  return run([...args, '--port', String(PORT)]);
}

describe('Agent CLI (Suite 8)', () => {
  before(async () => {
    assert.ok(BIN, `no built binary — looked in:\n  ${CANDIDATES.join('\n  ')}`);
    browser = spawn(BIN, ['--headless', '--no-sandbox', '--port', String(PORT)],
                    { stdio: 'ignore' });
    // Poll rather than sleep: startup time is a property of the machine.
    const deadline = Date.now() + 30000;
    for (;;) {
      const r = anoa('status');
      if (r.code === 0) break;
      assert.ok(Date.now() < deadline, `browser never came up on ${PORT}: ${r.err}`);
      await new Promise(r2 => setTimeout(r2, 500));
    }
  });

  after(() => {
    if (browser) browser.kill('SIGKILL');
  });

  // AGENT-01: the contract that lets an agent decide whether to start a browser
  // or retry a command. A wrong code here sends it into the wrong recovery.
  it('reports exit 3 when nothing is listening', () => {
    const r = run(['status', '--port', '9', '--json']);
    assert.equal(r.code, 3);
    assert.match(r.err, /no browser/i);
  });

  // AGENT-02
  it('open navigates and reports the resolved page', () => {
    const r = anoa('open', 'example.com');
    assert.equal(r.code, 0, r.err);
    assert.match(r.out, /example\.com/);
  });

  // AGENT-03: refs are the whole interface. They must come back with a role and
  // an accessible name, or an agent cannot tell two buttons apart.
  it('snapshot returns refs with roles and names', () => {
    const r = anoa('snapshot', '-i');
    assert.equal(r.code, 0, r.err);
    assert.match(r.out, /@e\d+\s+link\s+/);
  });

  // AGENT-04: the point of the whole design — a ref minted by one process is
  // resolvable by the next, because it lives on the DOM node, not in memory.
  it('a ref from one process is usable by another', () => {
    const snap = anoa('snapshot', '-i', '--json');
    assert.equal(snap.code, 0, snap.err);
    const first = JSON.parse(snap.out).elements[0];
    assert.ok(first, 'no interactive elements to test with');

    const got = anoa('get', 'attr', first.ref, 'href');
    assert.equal(got.code, 0, got.err);
    assert.match(got.out, /^https?:\/\//);
  });

  // AGENT-05
  it('get text reads the page, eval runs in it', () => {
    const text = anoa('get', 'text');
    assert.equal(text.code, 0, text.err);
    assert.match(text.out, /Example Domain/);

    const ev = anoa('eval', 'document.querySelectorAll("p").length');
    assert.equal(ev.code, 0, ev.err);
    assert.equal(ev.out, '2');
  });

  // AGENT-06: a click that would land on an overlay must be refused, with the
  // covering element named. Clicking through it is the failure mode that makes
  // an agent believe it dismissed a consent banner when it did not.
  it('refuses a click that something covers, and names the cover', () => {
    const build = anoa('eval',
      `document.body.innerHTML = '<button id=go>Go</button>' +
       '<div id=veil style="position:fixed;inset:0;z-index:99">Accept cookies</div>'; 'ok'`);
    assert.equal(build.code, 0, build.err);

    const snap = JSON.parse(anoa('snapshot', '-i', '--json').out);
    const button = snap.elements.find(e => e.role === 'button');
    assert.ok(button, 'no button in the built page');

    const blocked = anoa('click', button.ref);
    assert.equal(blocked.code, 1);
    assert.match(blocked.err, /covered by/i);
    assert.match(blocked.err, /Accept cookies/);

    anoa('eval', "document.getElementById('veil').remove(); 'gone'");
    const ok = anoa('click', button.ref);
    assert.equal(ok.code, 0, ok.err);
  });

  // AGENT-07: fill must go through the native setter, or a React-style page
  // ignores the value it was given.
  it('fill sets a value the page can see', () => {
    anoa('eval', `document.body.innerHTML = '<input id=q>'; 'ok'`);
    const snap = JSON.parse(anoa('snapshot', '-i', '--json').out);
    const box = snap.elements.find(e => e.role === 'textbox');
    assert.ok(box, 'no textbox in the built page');

    assert.equal(anoa('fill', box.ref, 'hello world').code, 0);
    assert.equal(anoa('eval', "document.getElementById('q').value").out, 'hello world');
  });

  // AGENT-08: a stale ref is a normal outcome, and has to read as one.
  it('reports a stale ref as a missing element, not a crash', () => {
    const r = anoa('click', '@e9999');
    assert.equal(r.code, 1);
    assert.match(r.err, /no element/i);
  });

  // AGENT-09
  it('screenshot writes a real PNG', () => {
    const path = resolve(here, 'test-results', 'agent-e2e.png');
    const r = anoa('screenshot', path);
    assert.equal(r.code, 0, r.err);
    assert.ok(existsSync(path));
    rmSync(path, { force: true });
  });

  // AGENT-10: history, which goes through getNavigationHistory rather than any
  // back() verb CDP does not have.
  it('back and forward move exactly one entry', () => {
    anoa('open', 'example.com');
    anoa('open', 'example.net');
    assert.equal(anoa('back').code, 0);
    assert.match(anoa('eval', 'location.hostname').out, /example\.com/);
    assert.equal(anoa('forward').code, 0);
    assert.match(anoa('eval', 'location.hostname').out, /example\.net/);
  });

  // AGENT-11: the skill has to be readable without a browser at all — an agent
  // asks for it before it has started one.
  it('skills get core works with no browser involved', () => {
    const r = run(['skills', 'get', 'core']);
    assert.equal(r.code, 0);
    assert.match(r.out, /snapshot/);
    assert.match(r.out, /@e\d/);
  });

  // AGENT-12: grouped help, and the group names it advertises.
  it('help is grouped and every advertised group exists', () => {
    assert.equal(run(['help']).code, 0);
    for (const group of ['browser', 'navigate', 'inspect', 'interact',
                         'state', 'debug', 'capture', 'agents']) {
      const one = run(['help', group]);
      assert.equal(one.code, 0, `help ${group} failed`);
      assert.ok(one.out.length > 0, `help ${group} printed nothing`);
    }
    assert.equal(run(['help', 'nope']).code, 2);
  });

  // AGENT-12b: --help and -h must be the grouped help, not the flag dump.
  // QCommandLineParser's own --help cannot mention a subcommand, because the
  // parser never sees one — so someone typing --help was shown --profile-dir
  // and left with no idea `click` existed.
  it('--help and -h print the same grouped help as `help`', () => {
    const grouped = run(['help']);
    for (const flag of ['--help', '-h']) {
      const r = run([flag]);
      assert.equal(r.code, 0, `${flag} failed`);
      assert.equal(r.out, grouped.out, `${flag} differs from \`help\``);
    }
    // And it carries the browser flags, so nothing was lost by replacing the
    // parser's output with this.
    for (const flag of ['--headless', '--auth-token', '--gfx', '--term-port']) {
      assert.match(grouped.out, new RegExp(flag.replace(/-/g, '\\-')),
                   `${flag} missing from help`);
    }
  });

  // AGENT-13: find returns refs, so its output feeds every other command.
  it('find locates by role, text and selector, and returns usable refs', () => {
    anoa('open', 'example.com');
    const byRole = anoa('find', 'role', 'link', '--json');
    assert.equal(byRole.code, 0, byRole.err);
    const first = JSON.parse(byRole.out).matches[0];
    assert.ok(first, 'no link found on example.com');
    assert.match(first.ref, /^@e\d+$/);

    // The ref find minted must work in the next process, like snapshot's do.
    assert.match(anoa('get', 'attr', first.ref, 'href').out, /^https?:\/\//);

    assert.equal(anoa('find', 'text', 'Learn more').code, 0);
    assert.equal(anoa('find', 'selector', 'a').code, 0);
    // No match is a failure, not an empty success — an agent branches on this.
    assert.equal(anoa('find', 'text', 'definitely-not-on-this-page').code, 1);
  });

  // AGENT-14: cookies and storage survive between processes, which is the
  // whole reason they are worth having as commands.
  it('cookies and storage round-trip across processes', () => {
    anoa('open', 'example.com');

    assert.equal(anoa('cookies', 'set', 'sid', 'abc123').code, 0);
    assert.match(anoa('cookies').out, /sid=abc123/);
    assert.equal(anoa('cookies', 'clear').code, 0);

    assert.equal(anoa('storage', 'local', 'set', 'token', 'xyz789').code, 0);
    assert.equal(anoa('storage', 'local', 'token').out, 'xyz789');
    assert.equal(anoa('storage', 'local', 'clear').code, 0);
    assert.match(anoa('storage', 'local').out, /empty/);
  });

  // AGENT-15: emulation actually reaches the page, checked by asking the page.
  it('set viewport and device change what the page sees', () => {
    assert.equal(anoa('set', 'viewport', '800', '600').code, 0);
    assert.equal(anoa('eval', 'window.innerWidth').out, '800');

    assert.equal(anoa('set', 'device', 'iphone-14').code, 0);
    assert.equal(anoa('eval', 'window.innerWidth').out, '390');

    // No name lists the presets rather than erroring.
    assert.match(anoa('set', 'device').out, /iphone-14/);
    assert.equal(anoa('set', 'device', 'no-such-phone').code, 1);
  });

  // AGENT-16: the recorders. Written by one process, read by another — a
  // one-shot command cannot subscribe to CDP events in time, so this is the
  // only shape that can report what already happened.
  it('console, errors and network report what happened before the command ran', () => {
    anoa('open', 'example.com');
    anoa('console', '--clear');

    anoa('eval', "console.log('recorded-marker'); console.warn('warn-marker'); 'ok'");
    const log = anoa('console');
    assert.equal(log.code, 0, log.err);
    assert.match(log.out, /recorded-marker/);
    assert.match(log.out, /warn-marker/);
    assert.match(anoa('console', '--level', 'warn').out, /warn-marker/);

    anoa('eval', "setTimeout(function(){ null.boom; }, 0); 'armed'");
    anoa('wait', '--ms', '600');
    assert.match(anoa('errors').out, /TypeError/);

    anoa('eval', 'fetch(location.href).then(function(){return 0;}); "fired"');
    anoa('wait', '--ms', '1200');
    assert.match(anoa('network').out, /GET\s+200/);

    anoa('console', '--clear');
    assert.match(anoa('console').out, /nothing recorded/);
  });

  // AGENT-17: the richer waits, including the one that must not mistake a
  // throwing expression for a failure.
  it('wait handles text, url, fn and hidden', () => {
    anoa('open', 'example.com');
    assert.equal(anoa('wait', '--text', 'Example Domain', '--timeout', '5000').code, 0);
    assert.equal(anoa('wait', '--url', 'example.com', '--timeout', '5000').code, 0);
    // Throws until it does not — `window.__late` is undefined at first.
    anoa('eval', 'setTimeout(function(){ window.__late = { ready: true }; }, 300); "armed"');
    assert.equal(anoa('wait', '--fn', 'window.__late.ready', '--timeout', '5000').code, 0);
    assert.equal(anoa('wait', '#nothing-here', '--state', 'hidden', '--timeout', '3000').code, 0);
    // And a real timeout is a failure with a reason.
    const late = anoa('wait', '--text', 'not-on-this-page', '--timeout', '1000');
    assert.equal(late.code, 1);
    assert.match(late.err, /timed out/);
  });

  // AGENT-19: `wait --load` straight after a click that navigates.
  //
  // Found by pointing an agent at the skill and watching it work. The obvious
  // probe returns instantly here — the *old* document is still `complete` while
  // the new one is in flight — so the wait passed, the next command read the
  // page the agent was trying to leave, and it reported the wrong answer with
  // no sign anything had gone wrong.
  it('wait --load waits for a navigation the click started', () => {
    anoa('open', 'example.com');
    anoa('snapshot', '-i');
    const before = anoa('eval', 'location.href').out;

    assert.equal(anoa('click', '@e1').code, 0);
    assert.equal(anoa('wait', '--load').code, 0);

    const after = anoa('eval', 'location.href').out;
    assert.notEqual(after, before,
                    'wait --load returned while the navigation was still in flight');
    assert.match(after, /iana\.org/);
  });

  // AGENT-19b: and it must not hang on a page that is genuinely idle — the
  // settle window is what bounds that, so a regression there shows up as a
  // wait that never returns rather than one that returns too early.
  it('wait --load returns promptly when nothing is loading', () => {
    anoa('open', 'example.com');
    const started = Date.now();
    assert.equal(anoa('wait', '--load').code, 0);
    const took = Date.now() - started;
    assert.ok(took < 6000, `wait --load on an idle page took ${took}ms`);
  });

  // AGENT-18: the reference an agent is told to read must exist and describe
  // the commands that exist.
  it('skills get commands documents the real command set', () => {
    const r = run(['skills', 'get', 'commands']);
    assert.equal(r.code, 0);
    for (const verb of ['snapshot', 'click', 'find', 'cookies', 'storage',
                        'console', 'network', 'wait', 'screenshot']) {
      assert.match(r.out, new RegExp(`anoa ${verb}`), `${verb} missing from the reference`);
    }
    assert.match(run(['skills', 'list']).out, /commands/);
  });

  // AGENT-19: a mistyped subcommand has to be reported as one. Nothing reads
  // positionalArguments(), so the word used to be discarded and a browser
  // started in its place — a stray window on a desktop, and SIGABRT under
  // "Could not load the Qt platform plugin xcb" over SSH.
  it('an unknown subcommand is an error, not a browser', () => {
    const r = run(['blahblah']);
    assert.equal(r.code, 2);
    assert.match(r.err, /unknown command 'blahblah'/);
    assert.match(r.err, /anoa help/);
    // The real verbs, and the words that are not verbs but are still
    // legitimate first arguments, must not be caught by it.
    assert.equal(anoa('status').code, 0);
    assert.equal(run(['--version']).code, 0);
    assert.equal(run(['help']).code, 0);
  });

  // ── Tabs ──────────────────────────────────────────────────────────────────
  //
  // Every case here leaves the browser on one tab again, because the cases
  // above and below it assume the single-tab browser they were written against.
  function closeExtraTabs() {
    const rows = anoa('tab', 'list').out.split('\n').filter(Boolean);
    const ids = rows.map(line => line.trim().split(/\s+/)[0]);
    // Never the first: the registry refuses the last tab, and the point is to
    // get back to exactly one.
    for (const id of ids.slice(1)) anoa('tab', 'close', id);
    if (ids.length) anoa('tab', 'select', ids[0]);
  }

  // Two documents without a server: about:blank in each tab, then a distinct
  // body written into it. Fixtures would need something to serve them.
  function makeTab(marker) {
    const id = anoa('tab', 'new').out.trim();
    anoa('eval', `document.body.innerHTML = '<h1>${marker}</h1>'`, '--tab', id);
    return id;
  }

  // AGENT-20: two tabs are two pages, and --tab picks between them.
  it('two tabs hold independent documents', () => {
    anoa('eval', "document.body.innerHTML = '<h1>ALPHA</h1>'");
    const t2 = makeTab('BRAVO');
    try {
      assert.match(anoa('get', 'text', '--tab', 't1').out, /ALPHA/);
      assert.match(anoa('get', 'text', '--tab', t2).out, /BRAVO/);
      // And they did not bleed into each other.
      assert.doesNotMatch(anoa('get', 'text', '--tab', 't1').out, /BRAVO/);
    } finally {
      closeExtraTabs();
    }
  });

  // AGENT-21
  it('tab list marks exactly one tab active, and --json parses', () => {
    const t2 = makeTab('BRAVO');
    try {
      const plain = anoa('tab', 'list').out.split('\n').filter(Boolean);
      assert.equal(plain.length, 2);
      assert.equal(plain.filter(line => line.includes('*')).length, 1);

      const rows = JSON.parse(anoa('tab', 'list', '--json').out);
      assert.equal(rows.length, 2);
      assert.equal(rows.filter(r => r.active).length, 1);
      assert.deepEqual(rows.map(r => r.tab), ['t1', t2]);
    } finally {
      closeExtraTabs();
    }
  });

  // AGENT-22: refs live in the page as data-anoa-ref attributes, so one tab's
  // ref names nothing in another. Silently acting on some other element would
  // be far worse than failing.
  it('a ref from one tab does not resolve in another', () => {
    anoa('eval', "document.body.innerHTML = '<button id=a>click me</button>'");
    const t2 = makeTab('BRAVO');
    try {
      const snap = anoa('snapshot', '-i', '--tab', 't1');
      const ref = (snap.out.match(/@e\d+/) || [])[0];
      assert.ok(ref, `no ref in snapshot output: ${snap.out}`);

      const r = anoa('click', ref, '--tab', t2);
      assert.notEqual(r.code, 0);
      assert.match(r.err + r.out, new RegExp(ref.replace('@', '@?')));
    } finally {
      closeExtraTabs();
    }
  });

  // AGENT-23
  it('closing a non-active tab leaves the active one alone', () => {
    anoa('eval', "document.body.innerHTML = '<h1>ALPHA</h1>'");
    const t2 = makeTab('BRAVO');
    try {
      assert.equal(anoa('tab', 'close', t2).code, 0);
      const ids = anoa('tab', 'list').out.split('\n').filter(Boolean)
                    .map(l => l.trim().split(/\s+/)[0]);
      assert.deepEqual(ids, ['t1']);
      assert.match(anoa('get', 'text').out, /ALPHA/);
    } finally {
      closeExtraTabs();
    }
  });

  // AGENT-24: closing the active tab has to hand the role to another one, or
  // the next command with no --tab has nothing to act on.
  it('closing the active tab promotes another, and a bare command follows it', () => {
    anoa('eval', "document.body.innerHTML = '<h1>ALPHA</h1>'");
    const t2 = makeTab('BRAVO');
    try {
      assert.equal(anoa('tab', 'select', t2).code, 0);
      assert.equal(anoa('tab', 'close', t2).code, 0);

      const rows = JSON.parse(anoa('tab', 'list', '--json').out);
      assert.equal(rows.length, 1);
      assert.equal(rows[0].active, true);
      // No --tab: it must reach whatever is active now.
      assert.match(anoa('get', 'text').out, /ALPHA/);
    } finally {
      closeExtraTabs();
    }
  });

  // AGENT-25
  it('closing the last tab is refused in words, not a protocol error', () => {
    const r = anoa('tab', 'close', 't1');
    assert.notEqual(r.code, 0);
    assert.match(r.err, /only tab/i);
    assert.equal(JSON.parse(anoa('tab', 'list', '--json').out).length, 1);
  });

  // AGENT-26: the default that keeps every existing invocation working.
  it('commands with no --tab act on the active tab', () => {
    anoa('eval', "document.body.innerHTML = '<h1>ALPHA</h1>'");
    const t2 = makeTab('BRAVO');
    try {
      assert.equal(anoa('tab', 'select', t2).code, 0);

      assert.match(anoa('get', 'text').out, /BRAVO/);
      assert.match(anoa('eval', 'document.body.textContent').out, /BRAVO/);
      assert.match(anoa('snapshot').out, /BRAVO/);

      const shot = resolve(root, 'build/tab-active.png');
      assert.equal(anoa('screenshot', shot).code, 0);
      assert.ok(existsSync(shot));
      rmSync(shot, { force: true });
    } finally {
      closeExtraTabs();
    }
  });

  // AGENT-27: a wrong tab id fails differently depending on whether it could
  // ever have been a tab.
  it('an unknown tab names the ones that exist, a malformed one is usage', () => {
    const t2 = makeTab('BRAVO');
    try {
      const unknown = anoa('get', 'text', '--tab', 't99');
      assert.notEqual(unknown.code, 0);
      assert.match(unknown.err, /t1/);
      assert.match(unknown.err, new RegExp(t2));

      // "junk" is a legal NAME now, so it can only fail as an unknown tab —
      // a malformed one has to be something no tab could ever be called.
      const unnamed = anoa('get', 'text', '--tab', 'junk');
      assert.notEqual(unnamed.code, 0);
      assert.match(unnamed.err, /no tab junk/);

      const malformed = anoa('get', 'text', '--tab', 'has space');
      assert.equal(malformed.code, 2);
      assert.match(malformed.err, /--tab/);
    } finally {
      closeExtraTabs();
    }
  });

  // AGENT-28: input reaches a tab that is not the active one.
  //
  // It did not, and both paths still answered "clicked": AnoaBrowser held tabs
  // in a QStackedLayout, which HIDES every view but the current one, and a
  // hidden QWebEngineView processes no input at all — not Qt synthetic events
  // and not CDP Input.dispatchMouseEvent. Views are covered rather than hidden
  // now, and this is the case that says so.
  it('a click reaches a background tab', () => {
    const t2 = anoa('tab', 'new').out.trim();
    try {
      anoa('eval',
           "document.body.innerHTML = '<button id=bg>bg</button>';" +
           " window.__bgHit = 0;" +
           " document.getElementById('bg').onclick = () => window.__bgHit = 1; 'ok'",
           '--tab', t2);

      // t1 is still the active tab: t2 was opened in the background.
      const rows = JSON.parse(anoa('tab', 'list', '--json').out);
      assert.equal(rows.find(r => r.active).tab, 't1');

      const clicked = anoa('click', '#bg', '--tab', t2);
      assert.equal(clicked.code, 0, clicked.err);
      assert.equal(anoa('eval', 'window.__bgHit', '--tab', t2).out, '1',
                   'the click reported success but never reached the page');
    } finally {
      closeExtraTabs();
    }
  });

  // AGENT-29: --tab works on either side of the verb, and in both spellings.
  //
  // It used to work only after the verb. Everything before one was handed to
  // the browser parser, which has never heard of --tab, so
  // `anoa --tab t2 get text` died with "Unknown option 'tab'" — and that is
  // the form the README, the help and the core skill all told agents to type.
  // --tab=t2 was worse: takeOption did not understand the = spelling at all,
  // so the value was dropped in silence and the command ran against the wrong
  // tab.
  it('--tab is accepted before or after the verb, joined or separate', () => {
    anoa('eval', "document.title = 'ALPHA'");
    const t2 = anoa('tab', 'new').out.trim();
    anoa('eval', "document.title = 'BRAVO'", '--tab', t2);
    try {
      const port = String(PORT);
      // after the verb, separate — the form that always worked
      assert.equal(run(['eval', 'document.title', '--port', port, '--tab', t2]).out, 'BRAVO');
      // before the verb, separate — the form the docs used and that failed
      assert.equal(run(['--tab', t2, '--port', port, 'eval', 'document.title']).out, 'BRAVO');
      // joined, both sides
      assert.equal(run([`--tab=${t2}`, `--port=${port}`, 'eval', 'document.title']).out, 'BRAVO');
      assert.equal(run(['eval', 'document.title', `--port=${port}`, `--tab=${t2}`]).out, 'BRAVO');
      // and with no --tab at all it is still the active tab
      assert.equal(run(['eval', 'document.title', '--port', port]).out, 'ALPHA');
    } finally {
      closeExtraTabs();
    }
  });

  // AGENT-30: a tab can be given a name, and the name works wherever an id
  // does. Ids like t1 and t2 mean nothing to an agent three commands later;
  // "search" and "cart" survive the round trip.
  it('a named tab answers to its name and to its id', () => {
    const printed = anoa('tab', 'new', '--name', 'searchtab').out.trim();
    try {
      // `tab new` prints the NAME when there is one — it is what gets typed next.
      assert.equal(printed, 'searchtab');

      anoa('eval', "document.title = 'NAMED'", '--tab', 'searchtab');
      assert.equal(anoa('eval', 'document.title', '--tab', 'searchtab').out, 'NAMED');

      // The id is still a valid handle: the name is an alias, not a rename.
      const row = JSON.parse(anoa('tab', 'list', '--json').out)
                    .find(r => r.name === 'searchtab');
      assert.ok(row, 'the named tab is missing from tab list');
      assert.equal(anoa('eval', 'document.title', '--tab', row.tab).out, 'NAMED');

      // select and close take a name too.
      assert.equal(anoa('tab', 'select', 'searchtab').code, 0);
      assert.equal(JSON.parse(anoa('tab', 'list', '--json').out)
                     .find(r => r.active).name, 'searchtab');
    } finally {
      closeExtraTabs();
    }
  });

  // AGENT-31: the rules that keep a name from becoming a second, conflicting
  // way to say the same thing.
  it('rejects a duplicate name, and a name shaped like an id', () => {
    anoa('tab', 'new', '--name', 'dup');
    try {
      const again = anoa('tab', 'new', '--name', 'dup');
      assert.notEqual(again.code, 0);
      assert.match(again.err, /already in use/i);

      // A tab named "t9" would make --tab t9 ambiguous forever.
      const idish = anoa('tab', 'new', '--name', 't9');
      assert.equal(idish.code, 2);
      assert.match(idish.err, /--name/);

      assert.equal(anoa('tab', 'new', '--name', 'has space').code, 2);
    } finally {
      closeExtraTabs();
    }
  });

  // AGENT-32: the hooks a page needs to not fail silently.
  //
  // None of these existed. A single alert() wedged the whole browser — not the
  // tab, the process: every later command timed out, including Target.* at the
  // browser level, and only SIGKILL got it back. Downloads and window.open were
  // dropped without a word.
  it('a dialog does not wedge the browser, and answers sensibly', () => {
    assert.equal(anoa('eval', "alert('hi'); 'past the alert'").out, 'past the alert');
    // The tell-tale of the old bug: everything after an alert timed out.
    assert.equal(anoa('eval', '1 + 1').out, '2');
    assert.equal(anoa('eval', "String(confirm('ok?'))").out, 'true');
    assert.equal(anoa('eval', "prompt('name?', 'fallback')").out, 'fallback');
    assert.equal(anoa('status').code, 0);
  });

  // AGENT-33
  it('window.open becomes a real background tab', () => {
    try {
      const before = JSON.parse(anoa('tab', 'list', '--json').out).length;
      anoa('eval', "window.open('https://example.net')");
      // The tab exists, and the caller's tab is still the active one.
      const rows = JSON.parse(anoa('tab', 'list', '--json').out);
      assert.equal(rows.length, before + 1);
      assert.equal(rows.find(r => r.active).tab, 't1');
    } finally {
      closeExtraTabs();
    }
  });

  // AGENT-34: clicking a file input asks for a dialog nobody can answer, so
  // the files are handed to the element directly — and the page's change
  // handler has to see them, or a form that validates on change never knows.
  it('upload puts files into an input and fires change', () => {
    const tmp = resolve(root, 'build', 'agent-upload.txt');
    writeFileSync(tmp, 'payload');
    try {
      anoa('eval', "document.body.innerHTML = '<input id=up type=file>';" +
                   " window.__seen = '';" +
                   " document.getElementById('up').onchange = e => window.__seen = e.target.files[0].name; 'ready'");
      assert.equal(anoa('upload', '#up', tmp).code, 0);
      assert.equal(anoa('eval', "document.getElementById('up').files.length").out, '1');
      assert.equal(anoa('eval', 'window.__seen').out, 'agent-upload.txt');

      // A target that is not a file input says so rather than half-working.
      const wrong = anoa('upload', 'body', tmp);
      assert.notEqual(wrong.code, 0);
      assert.match(wrong.err, /file input/i);

      const missing = anoa('upload', '#up', '/no/such/file');
      assert.notEqual(missing.code, 0);
      assert.match(missing.err, /no such file/i);
    } finally {
      rmSync(tmp, { force: true });
    }
  });
  // ── exec, downloads, network-idle ─────────────────────────────────────────

  // AGENT-30: exec runs many commands against one attached session. The point
  // is the attach, not the convenience: every ordinary command pays for its own
  // process and its own CDP handshake, measured at about 130 ms, so a
  // twenty-step flow spends seconds before any page does anything.
  it('exec runs a batch against one session, and far faster than one process each',
     () => {
    const script = 'open example.com\n'
                 + '# comments and blank lines are allowed\n\n'
                 + 'get text h1\n'
                 + 'eval "document.title"\n';
    const batch = run(['exec', '-', '--port', String(PORT)]);
    // A batch on stdin needs stdin; run() gives none, so use the file form for
    // the assertion and keep the stdin form for the shape check below.
    assert.ok(batch.code === 0 || batch.code === 1);

    const file = resolve(root, 'build/e2e-batch.txt');
    writeFileSync(file, script);
    try {
      const t0 = Date.now();
      const r = anoa('exec', file);
      const batchMs = Date.now() - t0;
      assert.equal(r.code, 0, r.err);
      assert.match(r.out, /Example Domain/);

      const t1 = Date.now();
      anoa('open', 'example.com');
      anoa('get', 'text', 'h1');
      anoa('eval', 'document.title');
      const separateMs = Date.now() - t1;

      // Three commands in one process against three processes. The margin is
      // deliberately loose — this asserts that the attach is paid once, not a
      // particular speed on a particular machine.
      assert.ok(batchMs < separateMs,
                `batch ${batchMs}ms was not faster than ${separateMs}ms separate`);
    } finally {
      rmSync(file, { force: true });
    }
  });

  // AGENT-31: a batch stops where it broke and says so. Running the rest
  // against a page that never got there produces errors that point at the wrong
  // line, which is worse than stopping.
  it('exec fails fast and names the line', () => {
    const file = resolve(root, 'build/e2e-batch-fail.txt');
    writeFileSync(file, 'open example.com\nclick "#nothing-here"\neval "1+1"\n');
    try {
      const r = anoa('exec', file);
      assert.equal(r.code, 1);
      assert.match(r.err, /line 2/);
    } finally {
      rmSync(file, { force: true });
    }
  });

  // AGENT-32: one batch drives one tab. Honouring a per-line --tab would mean
  // re-attaching, which is the cost exec exists to avoid, so it is refused
  // rather than quietly ignored.
  it('exec refuses a per-line --tab instead of ignoring it', () => {
    const file = resolve(root, 'build/e2e-batch-tab.txt');
    writeFileSync(file, 'get text --tab t1\n');
    try {
      const r = anoa('exec', file);
      assert.equal(r.code, 2);
      assert.match(r.err, /--tab belongs on `exec`/);
    } finally {
      rmSync(file, { force: true });
    }
  });

  // AGENT-33: downloads are browser state, not page state, so Runtime.evaluate
  // cannot see them. Before this existed the browser accepted a download,
  // recorded nothing an agent could read, and left it to guess at a filename.
  it('downloads reports what was downloaded, and where', () => {
    const empty = anoa('downloads');
    assert.equal(empty.code, 0, empty.err);
    // Either wording is fine; what matters is that it answers rather than errors.
    assert.ok(empty.out.length > 0);

    const json = anoa('downloads', '--json');
    assert.equal(json.code, 0, json.err);
    assert.doesNotThrow(() => JSON.parse(json.out));
    assert.ok(Array.isArray(JSON.parse(json.out)));
  });

  // AGENT-34: waiting for nothing is not an error. A page that made no request
  // is already idle, and blocking to the timeout would only hide that.
  it('wait --network-idle returns on a quiet page', () => {
    anoa('open', 'example.com');
    const t0 = Date.now();
    const r = anoa('wait', '--network-idle', '--timeout', '8000');
    const ms = Date.now() - t0;
    assert.equal(r.code, 0, r.err);
    assert.ok(ms < 5000, `idle wait took ${ms}ms on a quiet page`);
  });

  // AGENT-35: the quiet window is what makes it useful — a single request
  // finishing must not look like the end of a burst.
  //
  // A request has to happen first. Measured against a page that made none, both
  // windows return at once and are indistinguishable: idle is "nothing
  // outstanding and nothing finished recently", and on a silent page both halves
  // are true from the start. An earlier version of this case did exactly that
  // and passed on timing noise alone.
  it('wait --network-idle honours a longer quiet window', () => {
    anoa('open', 'example.com');

    // Fetch anything; it is the completion that starts the clock. Cross-origin
    // and rejected is fine — the helper counts a failed request as a finished
    // one, which is the behaviour that matters here.
    const kick = `fetch('http://127.0.0.1:${PORT}/json/version').catch(function(){})`;

    anoa('eval', kick);
    const short = Date.now();
    anoa('wait', '--network-idle', '--network-idle-ms', '200', '--timeout', '8000');
    const shortMs = Date.now() - short;

    anoa('eval', kick);
    const long = Date.now();
    anoa('wait', '--network-idle', '--network-idle-ms', '1500', '--timeout', '8000');
    const longMs = Date.now() - long;

    assert.ok(longMs > shortMs + 500,
              `1500ms window (${longMs}ms) was not meaningfully longer than 200ms (${shortMs}ms)`);
  });

  // AGENT-36: wait --download with nothing downloading returns rather than
  // blocking, for the same reason as AGENT-34.
  it('wait --download returns when nothing is downloading', () => {
    const t0 = Date.now();
    const r = anoa('wait', '--download', '--timeout', '5000');
    const ms = Date.now() - t0;
    assert.equal(r.code, 0, r.err);
    assert.ok(ms < 3000, `download wait took ${ms}ms with nothing in flight`);
  });

});

// Issue #30. Its own describe with its own port and its own browser, because
// the subject of the test is killing the browser and Suite 8 shares one.
describe('Agent CLI — close (issue #30)', () => {
  const CLOSE_PORT = PORT + 10;
  let victim;

  const closeAnoa = (...args) => run([...args, '--port', String(CLOSE_PORT)]);

  before(async () => {
    assert.ok(BIN, 'no built binary');
    victim = spawn(BIN, ['--headless', '--no-sandbox', '--port', String(CLOSE_PORT)],
                   { stdio: 'ignore' });
    const deadline = Date.now() + 30000;
    for (;;) {
      if (closeAnoa('status').code === 0) break;
      assert.ok(Date.now() < deadline, `browser never came up on ${CLOSE_PORT}`);
      await new Promise(r => setTimeout(r, 500));
    }
  });

  after(() => {
    if (victim) victim.kill('SIGKILL');
  });

  // AGENT-37: close has to mean closed. It reported exit 0 while the process,
  // its renderers and the port all stayed up, so anything trusting the exit
  // code left a whole browser engine running.
  it('close stops the browser, and exit 0 means it is gone', async () => {
    const r = closeAnoa('close');
    assert.equal(r.code, 0, r.err);

    // The exit code alone is the claim under test: by the time close returns,
    // nothing may be listening.
    assert.equal(closeAnoa('status').code, 3, 'port still answering after close');

    // And the process itself is gone, not just its socket.
    const exited = await Promise.race([
      new Promise(res => victim.once('exit', () => res(true))),
      new Promise(res => setTimeout(() => res(false), 10000)),
    ]);
    assert.ok(exited, 'the browser process outlived close');
    victim = null;
  });
});
