// Start anoa inside an E2B sandbox and hand back the URLs that reach it.
//
// A sandbox port is published at its own hostname — `9222-<id>.e2b.app`, not
// `<id>.e2b.app:9222` — so anoa's three ports become three hostnames, and the
// webSocketDebuggerUrl the browser advertises for a local client has to be
// rebuilt for a remote one. That rebuild, and a token that is not shared with
// anyone else's sandbox, is most of what this file is.

import { randomBytes } from 'node:crypto';

export const DEFAULT_PORT = 9222;
export const LOG_PATH = '/tmp/anoa.log';
const PID_PATH = '/tmp/anoa.pid';

/**
 * @param {import('e2b').Sandbox} sandbox a sandbox booted from the anoa template
 * @param {object} [opts]
 * @param {number} [opts.port] the HTTP port; CDP lands on port+2 as everywhere else
 * @param {string} [opts.authToken] defaults to a fresh token for this sandbox
 * @param {string[]} [opts.flags] extra anoa flags, e.g. ['--ephemeral']
 * @param {number} [opts.readyTimeoutMs]
 */
export async function startAnoa(sandbox, opts = {}) {
  const port = opts.port ?? DEFAULT_PORT;
  // The URL is public to anyone who learns it, so there is no unauthenticated
  // mode here. A caller who wants one passes their own empty token knowingly.
  const authToken = opts.authToken ?? randomBytes(24).toString('hex');

  const argv = [
    'anoa',
    '--headless',
    '--no-sandbox',
    '--port',
    String(port),
    '--auth-token',
    authToken,
    ...(opts.flags ?? []),
  ];

  // nohup and & rather than the SDK's background command, which is supervised
  // and killed at the 60s command timeout — measured, not assumed. The browser
  // has to outlive the call that started it, and a log on disk is what explains
  // a browser that never came up.
  const started = await sandbox.commands.run(
    `nohup ${argv.join(' ')} > ${LOG_PATH} 2>&1 & echo $! > ${PID_PATH}`
  );
  if (started.exitCode !== 0)
    throw new Error(`could not start anoa: ${started.stderr || started.stdout}`);

  const url = `https://${sandbox.getHost(port)}`;
  const version = await waitForBrowser(sandbox, url, authToken, opts.readyTimeoutMs ?? 90_000);

  return {
    /** Discovery, /render/*, and /mcp. */
    url,
    mcpUrl: `${url}/mcp`,
    /** Ready for puppeteer.connect({ browserWSEndpoint }). */
    cdpUrl: remoteWsEndpoint(version.webSocketDebuggerUrl, sandbox.getHost(port + 2), authToken),
    authToken,
    port,
    browser: version.Browser,
    pid: Number((await sandbox.files.read(PID_PATH)).trim()),
    logPath: LOG_PATH,
  };
}

/** The log anoa wrote, for when it did not come up. */
export const anoaLog = (sandbox) => sandbox.files.read(LOG_PATH).catch(() => '(no log)');

/** One JSON-RPC round trip against the sandbox's MCP endpoint. */
export async function mcp(session, method, params) {
  const res = await fetch(session.mcpUrl, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      Authorization: `Bearer ${session.authToken}`,
    },
    body: JSON.stringify({ jsonrpc: '2.0', id: ++mcp.id, method, params }),
  });
  const body = await res.json();
  if (body.error) throw new Error(`${method}: ${body.error.message}`);
  return body.result;
}
mcp.id = 0;

/** Call one MCP tool. Returns the text content, which is the shape a model reads. */
export async function callTool(session, name, args = {}) {
  const result = await mcp(session, 'tools/call', { name, arguments: args });
  const text = (result.content ?? []).map((c) => c.text ?? '').join('\n');
  if (result.isError) throw new Error(`${name} failed: ${text}`);
  return text;
}

/**
 * The browser advertises a loopback endpoint rewritten to the host it was asked
 * on — right for a caller on port 9222, wrong for one that reaches the CDP
 * proxy at a different hostname entirely. Keep the path, replace the rest.
 */
function remoteWsEndpoint(advertised, proxyHost, token) {
  const { pathname } = new URL(advertised.replace(/^ws/, 'http'));
  return `wss://${proxyHost}${pathname}?token=${encodeURIComponent(token)}`;
}

async function waitForBrowser(sandbox, url, token, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let last = 'no response yet';
  while (Date.now() < deadline) {
    try {
      const res = await fetch(`${url}/json/version`, {
        headers: { Authorization: `Bearer ${token}` },
      });
      // The edge answers 502 until something listens on the port, and anoa
      // answers 401 if the token is wrong — worth saying out loud rather than
      // retrying until the deadline.
      if (res.ok) return await res.json();
      if (res.status === 401) throw new Error('anoa rejected the token');
      last = `HTTP ${res.status}`;
    } catch (err) {
      if (err.message === 'anoa rejected the token') throw err;
      last = err.message;
    }
    await new Promise((r) => setTimeout(r, 500));
  }
  throw new Error(
    `anoa did not answer at ${url} within ${timeoutMs}ms (${last})\n${await anoaLog(sandbox)}`
  );
}
