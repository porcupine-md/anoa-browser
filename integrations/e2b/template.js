// The E2B template that carries anoa.
//
// E2B hands an agent an isolated Linux machine that boots from a template in
// about a second. anoa belongs in one whenever the browser should not be the
// host's: each sandbox gets its own Chromium, and every port anoa listens on is
// reachable from outside at its own https URL.
//
//   node template.js                      # newest release, template name "anoa"
//   node template.js --version v0.17.0
//   node template.js --name anoa-dev --cpu 2 --memory 4096 --skip-cache
//
// E2B_API_KEY must be in the environment. `node --env-file=.env template.js`
// reads one from a .env beside this file; .env is gitignored, a key never is.
//
// The build runs on E2B, not here — no Docker, no registry push. What it
// installs is the published release tarball, the same artifact the Dockerfile
// and the install script use, so a sandbox runs the binary users run.

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Template, defaultBuildLogger } from 'e2b';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = join(here, '..', '..');

export const DEFAULT_TEMPLATE_NAME = 'anoa';
export const RELEASES = 'https://github.com/porcupine-md/anoa-browser/releases';

// Only x86_64 is published, and E2B sandboxes are x86_64. Both halves of that
// sentence have to stay true; if either changes this is the line that says so.
const ASSET = 'anoa-linux-x86_64.tar.gz';

/**
 * The system libraries Chromium dlopen()s, read out of the Dockerfile instead
 * of copied next to it.
 *
 * Two lists that must agree is the kind of drift that surfaces as a dlopen
 * error inside a sandbox an hour later, so there is one list and this reads it.
 * A parse that goes wrong throws here, at build time, naming the problem.
 */
export function chromiumRuntimePackages(
  dockerfile = readFileSync(join(repoRoot, 'Dockerfile'), 'utf8')
) {
  const marker = 'apt-get install -y --no-install-recommends';
  const at = dockerfile.indexOf(marker);
  if (at === -1) throw new Error(`${marker} not found in Dockerfile — the package list moved`);

  const packages = [];
  for (const raw of dockerfile.slice(at + marker.length).split('\n').slice(1)) {
    const line = raw.replace(/\\\s*$/, '').trim();
    if (line.startsWith(';')) break; // end of the apt-get invocation
    if (!line || line.startsWith('#')) continue;
    packages.push(line);
  }

  // Asserted rather than trusted: an empty or truncated list still builds a
  // template, and the browser then fails to start with a message about a
  // library rather than about this.
  for (const needed of ['libnss3', 'libgbm1', 'fonts-liberation']) {
    if (!packages.includes(needed))
      throw new Error(`parsed ${packages.length} packages but not ${needed} — check the Dockerfile`);
  }
  return packages;
}

/** The newest published release tag, resolved the same way the Dockerfile does. */
export async function latestRelease() {
  const res = await fetch('https://api.github.com/repos/porcupine-md/anoa-browser/releases/latest');
  if (!res.ok) throw new Error(`GitHub says ${res.status} for the latest release`);
  const { tag_name: tag } = await res.json();
  if (!tag) throw new Error('the latest release has no tag_name');
  return tag;
}

/**
 * @param {string} version a release tag, with or without the leading v.
 */
export function anoaTemplate(version, packages = chromiumRuntimePackages()) {
  const tag = version.startsWith('v') ? version : `v${version}`;
  const tarball = `${RELEASES}/download/${tag}/${ASSET}`;

  // No start command, though E2B can snapshot one into the template: it would
  // carry a single browser process and a single --auth-token into every sandbox
  // anyone boots from it. startAnoa() in anoa.js starts the browser per sandbox
  // with a token that belongs to that sandbox alone.
  return Template()
    .fromBaseImage()
    // An array is joined with && into a single step, which is why no step here
    // needs a shell guard: the first failure ends it and fails the build.
    .runCmd(
      [
        'apt-get update',
        `DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ${packages.join(' ')}`,
        'rm -rf /var/lib/apt/lists/*',
      ],
      { user: 'root' }
    )
    .runCmd(
      [
        `curl -fsSL -o /tmp/anoa.tar.gz ${tarball}`,
        // A 404 arrives as an HTML page, and tar would blame a truncated
        // archive rather than a missing release.
        `head -c2 /tmp/anoa.tar.gz | od -An -tx1 | tr -d ' \\n' | grep -q '^1f8b'`,
        'mkdir -p /opt',
        'tar xzf /tmp/anoa.tar.gz -C /opt',
        'rm /tmp/anoa.tar.gz',
        // The launcher, never the raw binary: it sets LD_LIBRARY_PATH and the
        // QtWebEngine paths that make the bundle self-contained.
        'ln -sf /opt/anoa/anoa.sh /usr/local/bin/anoa',
        // Proves it links and starts before any sandbox boots from this.
        'QT_QPA_PLATFORM=offscreen anoa --version',
      ],
      { user: 'root' }
    )
    .setEnvs({
      // Qt prints a four-line complaint on every invocation when the locale is
      // not UTF-8, in the middle of whatever a caller is capturing.
      LANG: 'C.UTF-8',
      LC_ALL: 'C.UTF-8',
      // There is no GPU, and Chromium's own sandbox needs privileges this
      // microVM does not hand a process. The isolation is the microVM.
      QTWEBENGINE_CHROMIUM_FLAGS: '--disable-gpu --no-sandbox',
    });
}

async function main() {
  const argv = process.argv.slice(2);
  const flag = (name, fallback) => {
    const at = argv.indexOf(`--${name}`);
    return at === -1 ? fallback : argv[at + 1];
  };

  if (!process.env.E2B_API_KEY) {
    console.error('E2B_API_KEY is not set. Create a key at https://e2b.dev/dashboard?tab=keys');
    process.exit(2);
  }

  const version = flag('version') ?? (await latestRelease());
  const name = flag('name', DEFAULT_TEMPLATE_NAME);
  const packages = chromiumRuntimePackages();

  console.log(`building template "${name}" with anoa ${version}`);
  console.log(`${packages.length} runtime packages from the Dockerfile`);

  const info = await Template.build(anoaTemplate(version, packages), name, {
    cpuCount: Number(flag('cpu', 2)),
    memoryMB: Number(flag('memory', 2048)),
    // 'default' is the tag Sandbox.create('anoa') resolves to, and passing any
    // tags replaces it — a build carrying only a version tag produces a
    // template that nothing can boot by name.
    tags: ['default', version.replace(/^v/, '')],
    skipCache: argv.includes('--skip-cache'),
    onBuildLogs: defaultBuildLogger(),
  });

  console.log(`\ntemplate ${info.name} built — Sandbox.create('${info.name}')`);
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  main().catch((err) => {
    console.error(err.message ?? err);
    process.exit(1);
  });
}
