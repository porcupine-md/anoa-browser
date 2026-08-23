#pragma once

// The page-side half of the agent commands.
//
// Everything an agent command needs to do inside the page lives here as one
// script, evaluated through Runtime.evaluate. It is a single blob rather than a
// call per command because refs have to survive between commands: `snapshot`
// hands back @e1/@e2 and a later `click @e2` has to resolve the same element in
// a *different* process invocation. The bridge for that is the page itself —
// the refs are written onto the elements as data-anoa-ref, so they live exactly
// as long as the DOM node does, which is the same lifetime the agent assumes.
//
// A ref that no longer resolves is therefore a real answer, not a bug: the page
// moved on and the snapshot is stale. Commands say so in those words.

#include <QLatin1String>

// Installed idempotently: re-evaluating it must not renumber live refs, or a
// second snapshot would silently invalidate the refs an agent is holding.
inline QLatin1String agentScript()
{
    return QLatin1String(R"JS(
(function () {
  // Version, not mere presence: a page carrying an older helper from a previous
  // build has to be upgraded, or every command added since then fails against a
  // page that looks like it already has what it needs.
  if (window.__anoa && window.__anoa.v === 2) return "ready";

  const INTERACTIVE = 'a[href],button,input,select,textarea,summary,' +
    '[role=button],[role=link],[role=checkbox],[role=radio],[role=tab],' +
    '[role=menuitem],[role=switch],[role=textbox],[role=combobox],' +
    '[contenteditable=""],[contenteditable=true],[onclick],[tabindex]';

  function visible(el) {
    if (!(el instanceof Element)) return false;
    const r = el.getBoundingClientRect();
    if (r.width <= 0 || r.height <= 0) return false;
    const s = getComputedStyle(el);
    return s.visibility !== 'hidden' && s.display !== 'none' && s.opacity !== '0';
  }

  // The accessible name, in the order a screen reader would resolve it. Not a
  // full accname implementation — enough that an agent can tell two buttons
  // apart, which is what refs are for.
  function name(el) {
    const aria = el.getAttribute('aria-label');
    if (aria && aria.trim()) return aria.trim();
    const labelledby = el.getAttribute('aria-labelledby');
    if (labelledby) {
      const parts = labelledby.split(/\s+/)
        .map(id => document.getElementById(id))
        .filter(Boolean)
        .map(n => (n.textContent || '').trim())
        .filter(Boolean);
      if (parts.length) return parts.join(' ');
    }
    if (el.id) {
      const lab = document.querySelector('label[for="' + CSS.escape(el.id) + '"]');
      if (lab && lab.textContent.trim()) return lab.textContent.trim();
    }
    const closestLabel = el.closest('label');
    if (closestLabel && closestLabel.textContent.trim()) return closestLabel.textContent.trim();
    if (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA') {
      const ph = el.getAttribute('placeholder');
      if (ph && ph.trim()) return ph.trim();
      if (el.type === 'submit' || el.type === 'button') return el.value || '';
    }
    if (el.tagName === 'IMG') return (el.getAttribute('alt') || '').trim();
    const t = (el.textContent || '').replace(/\s+/g, ' ').trim();
    return t.length > 120 ? t.slice(0, 120) + '…' : t;
  }

  function role(el) {
    const explicit = el.getAttribute('role');
    if (explicit) return explicit;
    switch (el.tagName) {
      case 'A': return el.hasAttribute('href') ? 'link' : 'generic';
      case 'BUTTON': return 'button';
      case 'SELECT': return 'combobox';
      case 'TEXTAREA': return 'textbox';
      case 'SUMMARY': return 'summary';
      case 'INPUT': {
        const t = (el.getAttribute('type') || 'text').toLowerCase();
        if (t === 'checkbox') return 'checkbox';
        if (t === 'radio') return 'radio';
        if (t === 'submit' || t === 'button' || t === 'reset') return 'button';
        return 'textbox';
      }
      default: return 'generic';
    }
  }

  function state(el) {
    const bits = [];
    if (el.disabled) bits.push('disabled');
    if (el.checked) bits.push('checked');
    if (el.getAttribute('aria-expanded') === 'true') bits.push('expanded');
    if (el.required) bits.push('required');
    if (el === document.activeElement) bits.push('focused');
    if (el.value !== undefined && el.value !== '' && el.type !== 'submit' && el.type !== 'button')
      bits.push('value=' + JSON.stringify(String(el.value).slice(0, 40)));
    return bits;
  }

  // Console and network history have the same problem the refs do, in a harder
  // form: a one-shot command attaches long after the interesting message was
  // logged, so subscribing to CDP events at attach time is always too late.
  // The answer is the same — keep the state in the page. These wrappers install
  // once and buffer into window.__anoa, so `anoa console` reports what happened
  // before it ran rather than what happens while it watches.
  const LOG_CAP = 500;
  function installRecorders(api) {
    const push = (list, item) => {
      list.push(item);
      if (list.length > LOG_CAP) list.shift();
    };

    for (const level of ['log', 'info', 'warn', 'error', 'debug']) {
      const original = console[level].bind(console);
      console[level] = function (...args) {
        try {
          push(api.logs, {
            level,
            text: args.map(a => {
              if (typeof a === 'string') return a;
              try { return JSON.stringify(a); } catch (e) { return String(a); }
            }).join(' ').slice(0, 2000),
            t: Date.now(),
          });
        } catch (e) { /* never let recording break the page */ }
        return original(...args);
      };
    }

    addEventListener('error', e => push(api.errors, {
      text: String(e.message || e.error || 'error'),
      source: e.filename || '', line: e.lineno || 0, t: Date.now(),
    }));
    addEventListener('unhandledrejection', e => push(api.errors, {
      text: 'unhandled rejection: ' + String((e.reason && e.reason.message) || e.reason),
      source: '', line: 0, t: Date.now(),
    }));

    // fetch and XHR cover essentially all page traffic that an agent cares
    // about. Document and subresource loads are not here — those need CDP's
    // Network domain, which a one-shot process cannot have been subscribed to.
    const origFetch = window.fetch;
    if (origFetch) {
      window.fetch = function (input, init) {
        const url = typeof input === 'string' ? input : (input && input.url) || '';
        const method = (init && init.method) || (input && input.method) || 'GET';
        const started = Date.now();
        api.inflight++;
        const settle = () => { api.inflight--; api.lastNet = Date.now(); };
        return origFetch.apply(this, arguments).then(res => {
          settle();
          push(api.requests, { url, method, status: res.status, ms: Date.now() - started, t: started });
          return res;
        }, err => {
          settle();
          push(api.requests, { url, method, status: 0, error: String(err), ms: Date.now() - started, t: started });
          throw err;
        });
      };
    }

    const origOpen = XMLHttpRequest.prototype.open;
    const origSend = XMLHttpRequest.prototype.send;
    XMLHttpRequest.prototype.open = function (method, url) {
      this.__anoaReq = { url: String(url), method: String(method) };
      return origOpen.apply(this, arguments);
    };
    XMLHttpRequest.prototype.send = function () {
      const req = this.__anoaReq;
      if (req) {
        const started = Date.now();
        api.inflight++;
        this.addEventListener('loadend', () => {
          api.inflight--;
          api.lastNet = Date.now();
          push(api.requests, { url: req.url, method: req.method, status: this.status,
                               ms: Date.now() - started, t: started });
        });
      }
      return origSend.apply(this, arguments);
    };
  }

)JS"
        // Split here, and only for MSVC's sake: a single string literal may not
        // exceed 16380 bytes, and Windows checkouts turn every newline into
        // CRLF, so 410 lines of script cross the limit there while building
        // fine everywhere else. Adjacent literals concatenate, so this is one
        // string to every compiler and two to the limit.
        R"JS(
  const api = {
    v: 2,
    n: 0,
    logs: [],
    errors: [],
    requests: [],
    // How many fetch/XHR calls are outstanding, and when the last one settled.
    // `wait --network-idle` is these two and nothing else: a page is quiet when
    // nothing is in flight and nothing has finished recently.
    inflight: 0,
    lastNet: 0,

    // Walks the document once, assigning a ref to every interactive element
    // that does not already carry one. Existing refs are preserved so an agent
    // holding @e2 across a re-snapshot still points at the same node.
    snapshot(interactiveOnly) {
      const out = [];
      const seen = new Set();
      const nodes = document.querySelectorAll(INTERACTIVE);
      for (const el of nodes) {
        if (!visible(el) || seen.has(el)) continue;
        seen.add(el);
        let ref = el.getAttribute('data-anoa-ref');
        if (!ref) {
          ref = 'e' + (++api.n);
          el.setAttribute('data-anoa-ref', ref);
        } else {
          const num = parseInt(ref.slice(1), 10);
          if (num > api.n) api.n = num;
        }
        const r = el.getBoundingClientRect();
        out.push({
          ref: '@' + ref,
          role: role(el),
          name: name(el),
          tag: el.tagName.toLowerCase(),
          state: state(el),
          box: { x: Math.round(r.x), y: Math.round(r.y),
                 w: Math.round(r.width), h: Math.round(r.height) },
        });
      }
      const doc = {
        url: location.href,
        title: document.title,
        elements: out,
      };
      if (!interactiveOnly) {
        const heads = [];
        document.querySelectorAll('h1,h2,h3,h4,h5,h6').forEach(h => {
          const t = (h.textContent || '').replace(/\s+/g, ' ').trim();
          if (t) heads.push({ level: Number(h.tagName[1]), text: t });
        });
        doc.headings = heads;
      }
      return doc;
    },

    // "@e2" -> element, or a CSS selector -> first match. One entry point so
    // every command accepts both spellings without repeating the branch.
    resolve(target) {
      if (typeof target !== 'string' || !target) return null;
      if (target[0] === '@') {
        return document.querySelector('[data-anoa-ref="' + CSS.escape(target.slice(1)) + '"]');
      }
      try { return document.querySelector(target); } catch (e) { return null; }
    },

    describe(el) {
      if (!el) return null;
      const r = el.getBoundingClientRect();
      return { tag: el.tagName.toLowerCase(), role: role(el), name: name(el),
               box: { x: Math.round(r.x), y: Math.round(r.y),
                      w: Math.round(r.width), h: Math.round(r.height) } };
    },

    // Centre point in CSS pixels, scrolled into view first. Returned to the
    // caller so the click can be a real Input event rather than el.click():
    // a synthetic click skips hit-testing, so it happily "clicks" a button
    // underneath a consent banner and the agent never learns it was covered.
    clickPoint(target) {
      const el = api.resolve(target);
      if (!el) return { error: 'no element for ' + target };
      el.scrollIntoView({ block: 'center', inline: 'center' });
      const r = el.getBoundingClientRect();
      if (r.width <= 0 || r.height <= 0) return { error: 'element has no box: ' + target };
      const x = r.x + r.width / 2, y = r.y + r.height / 2;
      const top = document.elementFromPoint(x, y);
      if (top && top !== el && !el.contains(top) && !top.contains(el)) {
        return { error: 'covered', covered_by: api.describe(top), x, y };
      }
      return { x, y, el: api.describe(el) };
    },

    fill(target, value) {
      const el = api.resolve(target);
      if (!el) return { error: 'no element for ' + target };
      el.scrollIntoView({ block: 'center' });
      el.focus();
      if (el.isContentEditable) {
        el.textContent = value;
      } else {
        const proto = el instanceof HTMLTextAreaElement
          ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
        const setter = Object.getOwnPropertyDescriptor(proto, 'value');
        // React and friends track the last value they wrote on the node and
        // ignore an event whose value they believe is unchanged. Going through
        // the prototype setter is what makes the framework see this one.
        if (setter && setter.set) setter.set.call(el, value); else el.value = value;
      }
      el.dispatchEvent(new Event('input', { bubbles: true }));
      el.dispatchEvent(new Event('change', { bubbles: true }));
      return { ok: true, el: api.describe(el) };
    },

    get(what, target) {
      const el = target ? api.resolve(target) : document.documentElement;
      if (!el) return { error: 'no element for ' + target };
      if (what === 'text') return { value: (el.innerText || el.textContent || '').trim() };
      if (what === 'html') return { value: el.outerHTML };
      if (what === 'value') return { value: el.value !== undefined ? el.value : null };
      return { error: 'unknown property: ' + what };
    },

    attr(target, key) {
      const el = api.resolve(target);
      if (!el) return { error: 'no element for ' + target };
      return { value: el.getAttribute(key) };
    },

    exists(selector) {
      return { found: !!api.resolve(selector) };
    },

    scroll(dx, dy) {
      window.scrollBy(dx, dy);
      return { x: window.scrollX, y: window.scrollY };
    },

    info() {
      return {
        url: location.href,
        title: document.title,
        ready: document.readyState,
        scroll: { x: window.scrollX, y: window.scrollY },
        size: { w: window.innerWidth, h: window.innerHeight },
      };
    },

    // ── find ────────────────────────────────────────────────────────────────
    //
    // Locating an element by what it *is* rather than by where it sits in the
    // DOM. Returns refs, so the result is usable by every other command.
    find(kind, needle, nth) {
      let pool = [];
      const all = Array.from(document.querySelectorAll('*')).filter(visible);
      if (kind === 'role') {
        pool = all.filter(el => role(el) === needle);
      } else if (kind === 'text') {
        const want = String(needle).toLowerCase();
        // Deepest match wins: a <button> inside a <div> both "contain" the
        // text, and the button is what someone means.
        pool = all.filter(el => {
          const t = (el.textContent || '').toLowerCase();
          if (!t.includes(want)) return false;
          return !Array.from(el.children).some(c =>
            (c.textContent || '').toLowerCase().includes(want));
        });
      } else if (kind === 'selector') {
        try { pool = Array.from(document.querySelectorAll(needle)).filter(visible); }
        catch (e) { return { error: 'bad selector: ' + needle }; }
      } else {
        return { error: 'find takes role, text or selector' };
      }

      const out = pool.map(el => {
        let ref = el.getAttribute('data-anoa-ref');
        if (!ref) {
          ref = 'e' + (++api.n);
          el.setAttribute('data-anoa-ref', ref);
        }
        return { ref: '@' + ref, role: role(el), name: name(el),
                 tag: el.tagName.toLowerCase() };
      });
      if (typeof nth === 'number' && nth > 0) {
        return { matches: out[nth - 1] ? [out[nth - 1]] : [], total: out.length };
      }
      return { matches: out, total: out.length };
    },

    // ── waits that need the page ─────────────────────────────────────────────
    hasText(text) {
      return { found: (document.body.innerText || '').includes(text) };
    },
    isHidden(selector) {
      const el = api.resolve(selector);
      return { hidden: !el || !visible(el) };
    },

    // ── storage ─────────────────────────────────────────────────────────────
    storage(area, action, key, value) {
      const store = area === 'session' ? sessionStorage : localStorage;
      if (action === 'clear') { store.clear(); return { ok: true }; }
      if (action === 'set') { store.setItem(key, value); return { ok: true }; }
      if (action === 'remove') { store.removeItem(key); return { ok: true }; }
      if (key) return { key, value: store.getItem(key) };
      const all = {};
      for (let i = 0; i < store.length; i++) {
        const k = store.key(i);
        all[k] = store.getItem(k);
      }
      return { items: all, count: store.length };
    },

    // ── recorded history ────────────────────────────────────────────────────
    console(level) {
      const list = level ? api.logs.filter(l => l.level === level) : api.logs;
      return { entries: list, count: list.length };
    },
    pageErrors() {
      return { entries: api.errors, count: api.errors.length };
    },
    network() {
      return { entries: api.requests, count: api.requests.length };
    },
    // Quiet for at least `ms`. lastNet starts at 0 so a page that has made no
    // request at all is idle immediately, which is right: there is nothing to
    // wait for.
    netIdle(ms) {
      if (api.inflight > 0) return false;
      return Date.now() - api.lastNet >= ms;
    },
    clearHistory() {
      api.logs.length = 0;
      api.errors.length = 0;
      api.requests.length = 0;
      return { ok: true };
    },
  };

  installRecorders(api);
  window.__anoa = api;
  return "ready";
})()
)JS");
}
