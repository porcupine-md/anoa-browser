import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { mkdtempSync, existsSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { startBrowser, stopBrowser, openDevtoolsWs, sendCdp, listTabs, BASE_URL } from './helpers.js';

describe('CDP Extension Stubs', () => {
  let proc;
  let ws;
  let cmdId = 0;

  beforeAll(async () => {
    proc = await startBrowser();
    ({ ws } = await openDevtoolsWs());
  }, 20000);

  afterAll(async () => {
    ws?.close();
    await stopBrowser(proc);
  });

  function nextId() { return ++cmdId; }

  async function expectStub(method, params = {}) {
    const id = nextId();
    const resp = await sendCdp(ws, method, params, id);
    expect(resp.id).toBe(id);
    expect(resp).toHaveProperty('result');
    expect(resp.error).toBeUndefined();
    return resp;
  }

  // EXT-01
  it('Profiler.enable returns stub {}', () => expectStub('Profiler.enable'));
  // EXT-02
  it('Profiler.disable returns stub {}', () => expectStub('Profiler.disable'));
  // EXT-03
  it('Profiler.start returns stub {}', () => expectStub('Profiler.start'));
  // EXT-04
  it('Profiler.stop returns stub {}', () => expectStub('Profiler.stop'));
  // EXT-05
  it('Profiler.setSamplingInterval returns stub {}', () =>
    expectStub('Profiler.setSamplingInterval', { interval: 100 }));

  // EXT-06
  it('HeapProfiler.enable returns stub {}', () => expectStub('HeapProfiler.enable'));
  // EXT-07
  it('HeapProfiler.disable returns stub {}', () => expectStub('HeapProfiler.disable'));
  // EXT-08
  it('HeapProfiler.startTrackingHeapObjects returns stub {}', () =>
    expectStub('HeapProfiler.startTrackingHeapObjects'));
  // EXT-09
  it('HeapProfiler.stopTrackingHeapObjects returns stub {}', () =>
    expectStub('HeapProfiler.stopTrackingHeapObjects'));
  // EXT-10
  it('HeapProfiler.takeHeapSnapshot returns stub {}', () =>
    expectStub('HeapProfiler.takeHeapSnapshot'));

  // EXT-11
  it('Security.enable returns stub {}', () => expectStub('Security.enable'));
  // EXT-12
  it('Security.disable returns stub {}', () => expectStub('Security.disable'));
  // EXT-13
  it('Security.setIgnoreCertificateErrors returns stub {}', () =>
    expectStub('Security.setIgnoreCertificateErrors', { ignore: true }));

  // EXT-14: these used to be stubs — an empty success that changed nothing,
  // the same lie Browser.close was telling before #30. A wrong behavior name
  // is now an error rather than a cheerful {}.
  it('Browser.setDownloadBehavior is accepted, and a bad behavior is refused', async () => {
    const ok = await sendCdp(ws, 'Browser.setDownloadBehavior',
                             { behavior: 'allow', downloadPath: '/tmp' }, nextId());
    expect(ok.result).toBeDefined();
    expect(ok.error).toBeUndefined();

    const bad = await sendCdp(ws, 'Browser.setDownloadBehavior',
                              { behavior: 'nonsense' }, nextId());
    expect(bad.error?.message).toMatch(/behavior/i);
  });

  // EXT-15: it answered {} — no windowId at all — so a client doing
  // `const {windowId} = await getWindowForTarget()` passed undefined straight
  // back into setWindowBounds.
  it('Browser.getWindowForTarget returns a real windowId and bounds', async () => {
    const r = await sendCdp(ws, 'Browser.getWindowForTarget', {}, nextId());
    expect(r.result.windowId).toBe(1);
    expect(r.result.bounds.width).toBeGreaterThan(0);
    expect(r.result.bounds.height).toBeGreaterThan(0);
  });

  // EXT-16
  it('Target.createBrowserContext returns synthetic context ID', async () => {
    const id = nextId();
    const resp = await sendCdp(ws, 'Target.createBrowserContext', {}, id);
    expect(resp.id).toBe(id);
    expect(resp.result?.browserContextId).toBe('__anoa_default__');
  });

  // EXT-17
  it('Target.disposeBrowserContext returns stub {}', () =>
    expectStub('Target.disposeBrowserContext', { browserContextId: '__anoa_default__' }));

  // EXT-18
  it('Each response carries the correct matching id', async () => {
    const ids = [100, 200, 300];
    const responses = await Promise.all(
      ids.map((id) => sendCdp(ws, 'Profiler.enable', {}, id)),
    );
    for (let i = 0; i < ids.length; i++) {
      expect(responses[i].id).toBe(ids[i]);
    }
  });

  // ── the rest of the Browser domain, which used to be four more stubs ──────

  // EXT-20: it answered {} and left the window exactly as it was. Asserted
  // through two independent readings — the browser's own bounds and the
  // renderer's innerWidth — because the first alone would pass on a widget
  // that resized without the page ever hearing about it.
  it('Browser.setWindowBounds actually resizes, and the page sees it', async () => {
    const before = (await sendCdp(ws, 'Browser.getWindowForTarget', {}, nextId())).result.bounds;
    const target = { width: before.width - 200, height: before.height - 100 };

    const set = await sendCdp(ws, 'Browser.setWindowBounds',
                              { windowId: 1, bounds: target }, nextId());
    expect(set.error).toBeUndefined();

    // The resize crosses a process boundary on its way to the renderer.
    let inner = '';
    for (let i = 0; i < 40 && inner !== `${target.width}x${target.height}`; i++) {
      await new Promise((r) => setTimeout(r, 100));
      const ev = await sendCdp(ws, 'Runtime.evaluate',
        { expression: `[innerWidth,innerHeight].join('x')`, returnByValue: true }, nextId());
      inner = ev.result?.result?.value ?? '';
    }
    expect(inner).toBe(`${target.width}x${target.height}`);

    const after = (await sendCdp(ws, 'Browser.getWindowForTarget', {}, nextId())).result.bounds;
    expect(after.width).toBe(target.width);
    expect(after.height).toBe(target.height);

    // Put it back: every later case in this file shares this browser.
    await sendCdp(ws, 'Browser.setWindowBounds',
                  { windowId: 1, bounds: { width: before.width, height: before.height } },
                  nextId());
  }, 20000);

  // EXT-21: a bare bounds object with only one side named must leave the other
  // alone. CDP sends partial bounds, and reading a missing width as 0 would
  // collapse the window.
  it('Browser.setWindowBounds leaves an unnamed dimension alone', async () => {
    const before = (await sendCdp(ws, 'Browser.getWindowForTarget', {}, nextId())).result.bounds;
    await sendCdp(ws, 'Browser.setWindowBounds',
                  { windowId: 1, bounds: { width: before.width - 50 } }, nextId());
    const after = (await sendCdp(ws, 'Browser.getWindowForTarget', {}, nextId())).result.bounds;
    expect(after.width).toBe(before.width - 50);
    expect(after.height).toBe(before.height);

    await sendCdp(ws, 'Browser.setWindowBounds',
                  { windowId: 1, bounds: { width: before.width } }, nextId());
  });

  // EXT-24: the one claim the others do not reach — that "deny" refuses. A
  // stub answered yes to this and downloaded the file anyway, which is the
  // worst shape of the bug: a script that thought it had turned downloads off.
  //
  // A blob rather than a URL, so the case needs no server and no network.
  it('Browser.setDownloadBehavior deny actually refuses a download', async () => {
    const dir = mkdtempSync(join(tmpdir(), 'anoa-dl-'));
    const trigger = (name) => `(function(){` +
      `var b=new Blob(["x"],{type:"text/plain"});` +
      `var a=document.createElement("a");a.href=URL.createObjectURL(b);` +
      `a.download=${JSON.stringify(name)};document.body.appendChild(a);a.click();})()`;
    const downloads = async () =>
      (await sendCdp(ws, 'Anoa.getDownloads', {}, nextId())).result.downloads;

    const before = (await downloads()).length;

    await sendCdp(ws, 'Browser.setDownloadBehavior',
                  { behavior: 'deny' }, nextId());
    await sendCdp(ws, 'Runtime.evaluate', { expression: trigger('denied.txt') }, nextId());
    await new Promise((r) => setTimeout(r, 1500));

    const denied = (await downloads()).slice(before);
    expect(denied.length).toBe(1);
    expect(denied[0].state).toBe('cancelled');
    expect(existsSync(join(dir, 'denied.txt'))).toBe(false);

    // And allow again, into a directory this test names, so the path half of
    // the command is checked too rather than assumed.
    await sendCdp(ws, 'Browser.setDownloadBehavior',
                  { behavior: 'allow', downloadPath: dir }, nextId());
    await sendCdp(ws, 'Runtime.evaluate', { expression: trigger('allowed.txt') }, nextId());

    for (let i = 0; i < 40 && !existsSync(join(dir, 'allowed.txt')); i++)
      await new Promise((r) => setTimeout(r, 100));
    expect(existsSync(join(dir, 'allowed.txt'))).toBe(true);

    rmSync(dir, { recursive: true, force: true });
  }, 20000);

  // EXT-19
  it('Unknown domain (DOM.getDocument) is forwarded to Chromium and returns result or error', async () => {
    const id = nextId();
    const resp = await sendCdp(ws, 'DOM.getDocument', {}, id);
    expect(resp.id).toBe(id);
    // Proxy must not swallow the response: must carry either result or error from Chromium
    expect(resp.result !== undefined || resp.error !== undefined).toBe(true);
  });
});

describe('Target domain against the tab registry', () => {
  let proc;
  let ws;
  let id = 9000;
  const call = (method, params = {}) => sendCdp(ws, method, params, ++id);

  beforeAll(async () => {
    proc = await startBrowser();
    ({ ws } = await openDevtoolsWs());
  }, 20000);

  afterAll(() => { if (ws) ws.close(); stopBrowser(proc); });

  // TGT-01: every tab, with both ids — ours and the engine's — and a browser
  // context per profile object.
  it('getTargets lists each tab, with a context per profile', async () => {
    const shared = await call('Target.createTarget', { url: 'about:blank' });
    const isolated = await call('Target.createTarget',
                                { url: 'about:blank', anoaIsolated: true });
    expect(shared.result.targetId).toBeTruthy();
    expect(isolated.result.targetId).toBeTruthy();

    const infos = (await call('Target.getTargets')).result.targetInfos;
    expect(infos.length).toBeGreaterThanOrEqual(3);
    for (const info of infos) {
      expect(info.type).toBe('page');
      expect(info.targetId).toBeTruthy();
      expect(info.anoaTabId).toMatch(/^t[1-9][0-9]*$/);
      expect(info.browserContextId).toBeTruthy();
    }

    // The two tabs on the shared profile agree on a context; the isolated one
    // has its own.
    const isolatedInfo = infos.find((i) => i.targetId === isolated.result.targetId);
    const others = infos.filter((i) => i.targetId !== isolated.result.targetId);
    expect(new Set(others.map((i) => i.browserContextId)).size).toBe(1);
    expect(isolatedInfo.browserContextId)
      .not.toBe(others[0].browserContextId);
  }, 30000);

  // TGT-02: createTarget answers with an id the discovery document then reports,
  // which is what a CDP client dials next.
  it('a created target appears in /json/list with a fresh tab id', async () => {
    const before = (await listTabs()).map((t) => t.anoaTabId);
    const created = await call('Target.createTarget', { url: 'about:blank' });
    const after = await listTabs();

    const entry = after.find((t) => t.id === created.result.targetId);
    expect(entry).toBeTruthy();
    expect(before).not.toContain(entry.anoaTabId);
  }, 20000);

  // TGT-03
  it('activateTarget moves the active marker', async () => {
    const tabs = await listTabs();
    const target = tabs.find((t) => !t.anoaActive);
    expect(target).toBeTruthy();

    const r = await call('Target.activateTarget', { targetId: target.id });
    expect(r.error).toBeUndefined();

    const after = await listTabs();
    expect(after.find((t) => t.anoaActive).anoaTabId).toBe(target.anoaTabId);
    expect(after.filter((t) => t.anoaActive).length).toBe(1);
  }, 20000);

  // TGT-04: closing answers true until the registry refuses the last tab, and
  // then false — not an error, because the client asked a fair question.
  it('closeTarget succeeds until the last tab, which is refused', async () => {
    let tabs = await listTabs();
    expect(tabs.length).toBeGreaterThan(1);

    while (tabs.length > 1) {
      const victim = tabs[tabs.length - 1];
      const r = await call('Target.closeTarget', { targetId: victim.id });
      expect(r.result.success).toBe(true);
      tabs = await listTabs();
    }

    const last = await call('Target.closeTarget', { targetId: tabs[0].id });
    expect(last.error).toBeUndefined();
    expect(last.result.success).toBe(false);
    expect((await listTabs()).length).toBe(1);
  }, 30000);

  // TGT-05: an id we never issued is refused rather than quietly opening the
  // tab somewhere else.
  it('an unknown browser context is refused', async () => {
    const r = await call('Target.createTarget',
                         { url: 'about:blank', browserContextId: 'never-issued' });
    expect(r.error).toBeTruthy();
    expect(r.error.message).toMatch(/browser context/i);
  }, 20000);
});

// Their own browser, and an ephemeral one. A granted permission is written to
// the profile, and below Qt 6.8 nothing can take it back — so on a persistent
// profile these cases pass once and then start from 'granted' forever. CI
// builds 6.7.3 and found exactly that; --ephemeral keeps nothing, so every run
// starts where the last one did not leave anything.
describe('Browser.grantPermissions (ephemeral profile)', () => {
  let proc;
  let ws;
  let cmdId = 7000;
  const nextId = () => ++cmdId;

  beforeAll(async () => {
    proc = await startBrowser(['--ephemeral']);
    ({ ws } = await openDevtoolsWs());
  }, 20000);

  afterAll(async () => {
    ws?.close();
    await stopBrowser(proc);
  });

  // EXT-22: the stub reported every permission granted. A page asking the
  // Permissions API disagreed, which is the only way anyone would have found
  // out. Needs a real origin: a permission belongs to one, and about:blank
  // has none to speak of.
  it('Browser.grantPermissions really grants one', async () => {
    await sendCdp(ws, 'Page.navigate', { url: `${BASE_URL}/json/version` }, nextId());
    for (let i = 0; i < 40; i++) {
      const ev = await sendCdp(ws, 'Runtime.evaluate',
        { expression: 'location.origin', returnByValue: true }, nextId());
      if ((ev.result?.result?.value ?? '').startsWith('http')) break;
      await new Promise((r) => setTimeout(r, 100));
    }

    const state = async () => {
      const ev = await sendCdp(ws, 'Runtime.evaluate', {
        expression: `navigator.permissions.query({name:'geolocation'}).then(p=>p.state)`,
        awaitPromise: true, returnByValue: true,
      }, nextId());
      return ev.result?.result?.value;
    };

    expect(await state()).toBe('prompt');

    const granted = await sendCdp(ws, 'Browser.grantPermissions',
                                  { permissions: ['geolocation'] }, nextId());
    expect(granted.error).toBeUndefined();
    expect(await state()).toBe('granted');

    // Reset needs Qt 6.8 to enumerate what was granted. Below that it reports
    // the limitation rather than a success it cannot deliver, and either
    // answer is correct here — what must never happen is a plain {} with the
    // permission still on.
    const reset = await sendCdp(ws, 'Browser.resetPermissions', {}, nextId());
    if (reset.error) {
      expect(reset.error.message).toMatch(/cannot.*reset|Qt 6\.8/i);
    } else {
      expect(await state()).toBe('prompt');
    }
  }, 20000);

  // EXT-23: the honest half. QtWebEngine has no expression for most of CDP's
  // permission names, and saying so beats granting four of five and reporting
  // success — a script would go on believing it had camera access.
  it('Browser.grantPermissions names the permissions it cannot grant', async () => {
    // notifications rather than geolocation, and "unchanged" rather than
    // "prompt": EXT-22 grants geolocation just before this and below Qt 6.8
    // cannot put it back. A case that depends on the one before it having
    // cleaned up is a case that passes on one machine and not another, which
    // is exactly what happened.
    const notifications = async () => {
      const ev = await sendCdp(ws, 'Runtime.evaluate', {
        expression: `navigator.permissions.query({name:'notifications'}).then(p=>p.state)`,
        awaitPromise: true, returnByValue: true,
      }, nextId());
      return ev.result?.result?.value;
    };
    const before = await notifications();

    const r = await sendCdp(ws, 'Browser.grantPermissions',
                            { permissions: ['notifications', 'midiSysex'] }, nextId());
    expect(r.error).toBeDefined();
    expect(r.error.message).toMatch(/midiSysex/);
    expect(r.error.message).not.toMatch(/notifications/);

    // And the half it *could* do must not have happened either. Granting some
    // of a list and then reporting failure leaves a permission on that the
    // caller has every reason to believe is off.
    expect(await notifications()).toBe(before);
  }, 20000);

});
