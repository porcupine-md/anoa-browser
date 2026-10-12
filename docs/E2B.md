# anoa in an E2B sandbox

[E2B](https://e2b.dev) boots a Firecracker microVM from a template in about a
second and publishes every port it listens on at its own https URL. Putting anoa
in one gives an agent a browser that is not the machine the agent runs on: a
fresh Chromium per sandbox, thrown away when the sandbox ends, reachable from
anywhere with an HTTPS client.

That is the whole integration. anoa needs no code to run there — it is the same
release tarball the Dockerfile and `install-linux.sh` use. What this directory
adds is the template that installs it and the helper that starts it with a token
that belongs to one sandbox.

```
integrations/e2b/template.js   builds the template on E2B — no Docker, no registry
integrations/e2b/anoa.js       startAnoa(sandbox) → the URLs that reach it
tests/e2e/e2b.test.js          ten checks against a real sandbox
```

---

## Before you start

- An E2B account and an API key from <https://e2b.dev/dashboard?tab=keys>. The
  key starts with `e2b_`; a dashed value without that prefix is an ID, not a key.
- Node 20.18.1 or newer (not 21), which the E2B SDK requires.
- Nothing else. The template is built by E2B from a base image it pulls itself,
  so no Docker daemon and no registry push are involved.

```bash
cd integrations/e2b
npm install
printf 'E2B_API_KEY=e2b_your_key\n' > .env     # .env is gitignored; a key is not a file you commit
```

Every command below reads that file with `node --env-file=.env`. An exported
`E2B_API_KEY` works just as well.

---

## 1. Build the template

```bash
node --env-file=.env template.js                    # newest release, template "anoa"
node --env-file=.env template.js --version v0.17.0  # pin a release
node --env-file=.env template.js --name anoa-dev --cpu 2 --memory 4096
```

A first build takes about 90 seconds; a rebuild with nothing changed takes about
12, because every step is cached. The result is a template named `anoa` tagged
both `default` and with the anoa version it carries, so

```js
await Sandbox.create('anoa')        // whatever was built last
await Sandbox.create('anoa:0.17.0') // that release, forever
```

**What is in it.** The E2B base image, the two dozen system libraries Chromium
`dlopen()`s, and `/opt/anoa` unpacked from the release tarball with
`/usr/local/bin/anoa` pointing at the launcher. The library list is not written
down twice — `template.js` reads it out of the repository's `Dockerfile`, which
is where it was worked out, and throws if the parse comes back without
`libnss3`, `libgbm1` and a font. The build ends by running `anoa --version`, so a
template that cannot start anoa never finishes building.

**No start command.** E2B can snapshot a running process into a template so it
is already up when a sandbox boots. anoa deliberately does not: the snapshot
would carry one browser process and one `--auth-token` into every sandbox anyone
boots from the template. Starting it per sandbox costs about three seconds and
gives each one a token nobody else has.

---

## 2. Start anoa in a sandbox

```js
import { Sandbox } from 'e2b'
import { startAnoa } from './integrations/e2b/anoa.js'

const sandbox = await Sandbox.create('anoa', { timeoutMs: 300_000 })
const anoa = await startAnoa(sandbox)

console.log(anoa.url)        // https://9222-<sandbox-id>.e2b.app
console.log(anoa.mcpUrl)     // …/mcp
console.log(anoa.cdpUrl)     // wss://9224-<sandbox-id>.e2b.app/devtools/browser/…?token=…
console.log(anoa.authToken)  // a fresh token for this sandbox

// …drive it…

await sandbox.kill()
```

`startAnoa` starts the browser detached, waits for `/json/version` to answer over
the public URL, and comes back with every address you need. It takes `port`
(default 9222), `authToken` (default: a fresh 24-byte token), `flags` for extra
anoa arguments such as `--ephemeral`, and `readyTimeoutMs`.

If the browser never comes up, the error carries `/tmp/anoa.log` from inside the
sandbox rather than a timeout with no cause.

`anoa.js` is not published to npm and imports nothing but `node:crypto` and the
E2B SDK, so copy it into your project if you would rather not depend on a
checkout of this repository. It is a hundred lines, and the only part that is
not obvious is the WebSocket rewrite below.

---

## 3. Three ways to drive it

### MCP — for an agent

The whole point. anoa serves MCP over Streamable HTTP at `/mcp`, so any MCP
client that can set an `Authorization` header can drive the browser:

```json
{
  "mcpServers": {
    "anoa": {
      "type": "http",
      "url": "https://9222-<sandbox-id>.e2b.app/mcp",
      "headers": { "Authorization": "Bearer <token>" }
    }
  }
}
```

From code, with the helpers in `anoa.js`:

```js
import { mcp, callTool } from './integrations/e2b/anoa.js'

const { tools } = await mcp(anoa, 'tools/list')
await callTool(anoa, 'browser_open', { url: 'https://example.com' })
const title = await callTool(anoa, 'browser_eval', { expression: 'document.title' })
```

Or with nothing but curl:

```bash
curl -s https://9222-$ID.e2b.app/mcp \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

Every verb of the CLI is a tool; see the MCP section of the README for the list.

### CDP — for Puppeteer and Playwright

```js
import puppeteer from 'puppeteer-core'

const browser = await puppeteer.connect({ browserWSEndpoint: anoa.cdpUrl })
const page = (await browser.pages())[0]
await page.goto('https://example.com')
```

`anoa.cdpUrl` is not what the browser advertises. anoa rewrites
`webSocketDebuggerUrl` to the host a caller asked on — correct when ports are
ports, wrong on E2B, where port 9224 is a different *hostname* rather than a
different port on the same one. `startAnoa` keeps the path, swaps the host for
`getHost(port + 2)`, and appends `?token=` because a WebSocket client cannot
send an `Authorization` header.

### The CLI — inside the sandbox

The same browser, driven from a shell in the sandbox:

```js
await sandbox.commands.run(
  `anoa --port 9222 --auth-token ${anoa.authToken} open https://example.com`
)
const { stdout } = await sandbox.commands.run(
  `anoa --port 9222 --auth-token ${anoa.authToken} eval "document.title"`
)
```

Useful when the output should stay in the sandbox — a PDF, a screenshot, a
download — and be read back with `sandbox.files.read`.

---

## Ports

anoa listens on a triplet, and each one gets its own hostname out here.

| in the sandbox | outside | what it is |
|---|---|---|
| 9222 | `https://9222-<id>.e2b.app` | discovery, `/render/*`, `/mcp` |
| 9223 | — | Chromium's own DevTools port; nothing should reach it |
| 9224 | `wss://9224-<id>.e2b.app` | the CDP proxy, which is what the token guards |

Pass `port` to `startAnoa` to move the whole triplet.

---

## The token is not optional

A sandbox URL is on the public internet. It is unguessable, and that is all it
is — anyone who learns it can reach whatever is listening. `startAnoa` therefore
always starts anoa with `--auth-token`, and every request needs it:
`Authorization: Bearer <token>` for HTTP, `?token=` for the WebSocket.

Without it, a URL in a log line is a browser anyone can drive — with
`browser_eval` on it, and with your sandbox's network. The e2e suite checks all
three doors answer 401 unauthenticated, because this is the assertion that keeps
being true only by accident otherwise.

---

## Lifetime and cost

A sandbox is billed while it runs and killed when its timeout expires — five
minutes by default, which is shorter than most work.

```js
const sandbox = await Sandbox.create('anoa', { timeoutMs: 900_000 })
await sandbox.setTimeout(1_800_000)   // extend, counted from now
await sandbox.kill()                  // stop paying immediately
```

Kill the sandbox in a `finally`. An orphaned one keeps a browser running and
billing until its timeout, and the sandbox is also the only thing holding that
browser — `anoa close` or killing the process leaves the sandbox up.

To find the ones you forgot — `Sandbox.list()` hands back a paginator, not an
array, which is a five-minute mistake to make once:

```js
for (const s of await Sandbox.list().nextItems()) await Sandbox.kill(s.sandboxId);
```

`e2b sandbox list` and `e2b sandbox kill <id>` do the same from a terminal.

---

## Running the tests

`tests/e2e/e2b.test.js` boots a real sandbox and drives it; it is not a mock of
anything. It skips, rather than fails, when `E2B_API_KEY` is absent, so a
checkout without a key still runs the suite.

```bash
cd tests/e2e
npm install
node --env-file=../../integrations/e2b/.env --test e2b.test.js
```

About eighty seconds and one sandbox. The ten checks are: the template carries
the release build; the public URL reaches the browser; all three doors reject a
missing or wrong token; the MCP handshake and tool list answer over the edge; a
tool call drives the page; `/render/screenshot.png` returns real PNG bytes;
Puppeteer and Playwright each connect over `wss://`; the in-sandbox CLI drives
the same browser the outside is driving; and the browser is still answering 75
seconds in.

That last check is most of the runtime, and it is not padding. A command started
with the SDK's `background: true` is reaped at the 60s command timeout — alive at
+30s, gone by +65s, measured rather than assumed — so every other check passes
either way, and only one that waits can tell the difference.

The GitHub workflow **E2B** (`workflow_dispatch`) does the same thing against
the repository's own key: it builds the template from the newest release and
runs the suite. Like the OBS and tap workflows it warns and exits 0 when the
secret is not configured, so it never fails a release for being unset.

---

## When it goes wrong

**`404: tag 'default' does not exist for template 'team/anoa'`** — the template
was built under another name, or a build passed tags that replaced `default`.
Build it again with `template.js`, which tags both.

**`anoa did not answer at https://9222-… within 90000ms`** — the message carries
`/tmp/anoa.log` from the sandbox. A missing system library is the usual cause and
names itself there; add it to the `Dockerfile` list, which is the one the
template reads.

**`403` from `/mcp`** — an `Origin` header the browser does not allow. Server-side
clients send none; a page in a browser does, and needs `--embed-origin` passed
through `startAnoa`'s `flags`.

**Puppeteer cannot connect** — check the URL is the `wss://9224-…` one with
`?token=`, not the `https://9222-…` one. `connect` with the wrong host fails as a
timeout rather than as a refusal.

**`Sandbox not found` mid-run** — the timeout expired. It counts from creation,
not from the last request.

---

## Known limits

- **x86_64 only**, because that is the only published tarball. E2B sandboxes are
  x86_64, so this costs nothing today and would matter the day one is not.
- **One browser per sandbox.** Several are possible on other ports — nothing
  stops it — but `startAnoa` tracks one pidfile and one log.
- **No GPU.** `QTWEBENGINE_CHROMIUM_FLAGS` carries `--disable-gpu`; WebGL falls
  back to software rendering.
