#include "agent/agent_skill.h"

#include <QTextStream>

namespace {

const char kCore[] = R"(# Driving a browser with `anoa`

`anoa` controls a browser that is already running. Each command is a separate
process that attaches, does one thing, and exits — the browser keeps the page,
the cookies and the scroll position between them. Commands can be chained with
`&&` safely and cheaply.

## Start the browser once

```bash
anoa --headless --port 9222 &
```

Everything below attaches to it. `anoa status` says whether one is listening;
exit code 3 means nothing is.

One port becomes three: `--port 9222` also uses 9223 (Chromium's own DevTools)
and 9224 (the CDP proxy). You always address 9222 — the others are internal, and
seeing 9224 in a log does not mean `--port` was ignored.

## The loop

```bash
anoa open example.com          # 1. go somewhere
anoa snapshot -i               # 2. see what is interactive, with refs
anoa click @e2                 # 3. act by ref
anoa snapshot -i               # 4. look again — the page changed
```

`snapshot` prints one line per interactive element:

```
  @e1   link       Documentation
  @e3   textbox    Search  [required]
  @e7   button     Sign in
```

The `@e1` refs are written onto the DOM nodes, so they stay valid across
commands until the page replaces those nodes. **Re-snapshot after anything that
changes the page** — a navigation, a submit, an expanded menu. A ref that no
longer resolves reports "no element for @e4"; that means the snapshot is stale,
not that the command is wrong.

Any CSS selector works wherever a ref does: `anoa click "#submit"`.

## Reading a page

```bash
anoa get text                  # all visible text
anoa get text @e5              # one element
anoa get attr @e1 href
anoa eval "document.querySelectorAll('article').length"
```

Prefer `get text` over `get html` — HTML costs far more tokens and rarely says
more. Use `snapshot` when you need to *act*, `get text` when you need to *read*.

## Filling forms

```bash
anoa fill @e3 "user@example.com"
anoa fill @e4 "hunter2"
anoa click @e7
anoa wait --url "/dashboard"     # name where you expect to land
```

`fill` sets the value through the native setter and fires `input`/`change`, so
React and other frameworks see it. `type` sends keystrokes to whatever has
focus; `press Enter` submits.

## When a click fails

```
anoa: @e7 is covered by <div> Accept cookies — dismiss it, then re-snapshot
```

Clicks are hit-tested against the point they land on, so a consent banner or
modal is reported rather than clicked through. Deal with the covering element,
take a fresh snapshot, then retry the original ref.

## Waiting

```bash
anoa wait --load                       # navigation finished
anoa wait --selector ".results"        # element appeared
anoa wait --url "/dashboard"           # the url changed
anoa wait --text "Welcome"             # the text arrived
anoa wait --ms 500                     # last resort
```

Prefer `--selector`, `--url` or `--text` over `--ms`: they are faster when the
page is quick and more reliable when it is slow.

After a click that navigates, prefer `--url` or `--text` over `--load` when you
know what the destination should contain. `--load` works — it watches for the
navigation to start rather than trusting the old document's state — but it
spends about 1.5s deciding that nothing is loading, and a check that names the
destination both confirms you arrived *somewhere specific* and returns as soon
as you do.

## Output for programs

Add `--json` to any command for structured output. Exit codes: `0` success,
`1` the command failed, `2` bad usage, `3` no browser is listening.

## More than one page at a time

One browser holds many tabs. Open one and keep the id it prints:

```bash
TAB=$(anoa tab new example.com)
anoa --tab "$TAB" get text
```

Or name it, and skip keeping the id around at all:

```bash
anoa tab new example.com --name search
anoa --tab search get text
```

Or give it a name and stop tracking ids:

```bash
anoa tab new example.com --name search
anoa --tab search get text
```

A name is an alias — the id keeps working, and `--tab` takes either. Names must
be unique and cannot look like an id (`t2`), which is what keeps `--tab t2`
unambiguous.

Without `--tab`, every command acts on the active tab, so nothing you already
do changes. `anoa tab list` shows them all with `*` on the active one.

**Refs do not cross tabs.** They live in the page as `data-anoa-ref`
attributes, so `@e2` in one tab names nothing in another. Snapshot the tab you
are about to act on, with the same `--tab` you will use for the click.

**Logins survive between runs.** Cookies go to a persistent profile, so you can
log in once and use it in later commands and later sessions. `--ephemeral`
starts a browser that keeps nothing.

**Cookies are shared between tabs unless you ask otherwise.** A login in one tab
is a login in all of them. For two accounts on one site at once:

```bash
anoa tab new example.com --isolated       # its own cookies, gone with the tab
anoa tab new example.com --profile work   # its own cookies, kept on disk
```

**A proxy belongs to the browser, not to a tab.** Set it when you start one:

```bash
anoa --headless --port 9222 --proxy http://user:pass@proxy.example:3128 &
anoa --headless --port 9222 --proxy socks5://127.0.0.1:1080 \
     --proxy-bypass "localhost,127.0.0.1,*.internal" &
```

Every tab in that browser goes through it — there is no per-tab proxy, because
Chromium keeps proxy settings per process and Qt exposes no way to vary them per
page. If you need two tabs on two different proxies, start two browsers on
different ports and address them with `--port`.

## Things a page does that used to fail quietly

`alert`, `confirm` and `prompt` are answered and recorded rather than shown:
confirm returns true, prompt returns the page's own default. Nothing blocks.

`window.open` and `target=_blank` open a real background tab — `anoa tab list`
shows it, and the tab you were driving stays active.

Downloads are accepted and saved. `--download-dir` says where.

Clicking a file input opens a dialog nobody can answer, so upload the file
directly instead:

```bash
anoa upload @e2 ./report.pdf
```

That fires the page's `change` handler, which is what a form validating on
change is waiting for.

## Watching it happen

`anoa terminal` renders the live page in the terminal and forwards clicks and
typing. It attaches to the same running browser, so it can be left open in one
pane while commands run in another. `Ctrl-N` cycles tabs.
)";

const char kCommands[] = R"(# `anoa` command reference

Every command attaches to a browser that is already running. Start one with
`anoa --headless --port 9222 &`. Exit codes: `0` ok, `1` the command failed,
`2` bad usage, `3` nothing is listening. Add `--json` to any command for
structured output, and `--port` / `--host` / `--token` to reach a browser
somewhere else. `--tab <id>` picks which tab to act on; without it every command
acts on the active tab.

`<target>` below is either a ref from a snapshot (`@e2`) or any CSS selector.

## Starting the browser

| Flag | Does |
|---|---|
| `--headless` | no window, no display server needed |
| `--port <n>` | which port to listen on (default 9222) |
| `--profile <name>` | a named profile with its own cookies and storage |
| `--ephemeral` | keep nothing; gone when the process ends |
| `--proxy <url>` | `host:port`, or `scheme://user:pass@host:port` with `http`, `https`, `socks5`, `socks4` |
| `--proxy-bypass <list>` | hosts that skip the proxy, comma separated |
| `--max-renderers <n>` | cap renderer processes: less memory, less parallelism |
| `--download-dir <dir>` | where downloads land |

The proxy is a property of the browser, not of a tab: every tab in one browser
uses it, and there is no per-tab proxy. Two proxies means two browsers on two
ports.

## Tabs

One browser holds many pages, each with a stable id (`t1`, `t2`, …) that
survives between commands and between processes.

| Command | Does |
|---|---|
| `anoa tab new [url]` | open a tab and print its id |
| `anoa tab new --name <name>` | name it; `--tab` takes the name afterwards |
| `anoa tab new --profile <name>` | open it with its own persistent cookies |
| `anoa tab new --isolated` | open it with a throwaway jar, gone with the tab |
| `anoa tab list` | every tab; `*` marks the active one |
| `anoa tab select <id>` | make a tab the active one |
| `anoa tab close <id>` | close a tab; the last one cannot be closed |

`tab new` prints the id alone, so it composes:

```bash
TAB=$(anoa tab new example.com)
anoa --tab "$TAB" get text
```

Tabs share one cookie jar by default, so a login in one is a login in all.
`--profile` and `--isolated` are how two tabs hold two different logins to the
same site at once — which is the reason to reach for them.

## Navigate

| Command | Does |
|---|---|
| `anoa open <url>` | go to a url; the scheme is optional |
| `anoa back` / `forward` / `reload` | move through history, one entry at a time |
| `anoa wait --load` | wait for the page to finish loading |
| `anoa wait <css>` | wait for an element to appear |
| `anoa wait <css> --state hidden` | wait for one to go away |
| `anoa wait --text "<text>"` | wait for text to appear anywhere on the page |
| `anoa wait --url "<fragment>"` | wait for the url to contain something |
| `anoa wait --fn "<js>"` | wait for a JS expression to be truthy |
| `anoa wait --ms <n>` | wait a fixed time — the last resort |

`--timeout <ms>` bounds any wait (default 15000). A bare argument is read as a
duration when it is a number and as a selector otherwise.

## Inspect

| Command | Does |
|---|---|
| `anoa snapshot` | page outline plus interactive elements, with refs |
| `anoa snapshot -i` | interactive elements only |
| `anoa find role <role>` | locate by role: button, link, textbox, checkbox, … |
| `anoa find text <text>` | locate by visible text; the deepest match wins |
| `anoa find selector <css>` | locate by CSS, returned as refs |
| `anoa get text [<target>]` | visible text of the page, or of one element |
| `anoa get html <target>` | outer HTML |
| `anoa get value <target>` | current form value |
| `anoa get attr <target> <name>` | one attribute |
| `anoa eval "<js>"` | evaluate an expression in the page |
| `anoa status` | what the browser is attached to right now |

`find` takes `--nth <n>` to keep only the nth match, 1-based.

Prefer `get text` over `get html`: HTML costs far more tokens and rarely says
more. Use `snapshot` when you need to *act*, `get text` when you need to *read*.

## Interact

| Command | Does |
|---|---|
| `anoa click <target>` | click, hit-tested — refuses if something covers it |
| `anoa fill <target> <text>` | write into a field and fire input/change |
| `anoa type <text>` | type into whatever has focus |
| `anoa upload <target> <file...>` | put files into a file input |
| `anoa press <key>` | Enter, Tab, Escape, ArrowDown, … |
| `anoa scroll [--up] [--by <px>]` | scroll the page |
| `anoa scroll --top` / `--bottom` | jump to either end |
| `anoa mouse move <x> <y>` | move the pointer |
| `anoa mouse down` / `up [x] [y]` | press or release — for drags |
| `anoa mouse wheel <dy> [x] [y]` | wheel at a position |

## State

| Command | Does |
|---|---|
| `anoa cookies` | list cookies |
| `anoa cookies set <name> <value>` | write one, scoped to the current page |
| `anoa cookies clear` | clear them all |
| `anoa storage local` | everything in localStorage |
| `anoa storage local <key>` | one key |
| `anoa storage local set <k> <v>` | write one |
| `anoa storage local remove <k>` | delete one |
| `anoa storage local clear` | empty it |
| `anoa storage session …` | the same, for sessionStorage |
| `anoa set viewport <w> <h> [scale]` | resize the page |
| `anoa set device [name]` | a preset; with no name, lists them |
| `anoa set geo <lat> <lng>` | override geolocation |
| `anoa set offline [on\|off]` | cut the page off from the network |
| `anoa set headers '<json>'` | extra HTTP headers on every request |
| `anoa set media dark\|light` | emulate prefers-color-scheme |

`cookies set` scopes to the page you are on; pass `--url` to scope it elsewhere.

## Debug

| Command | Does |
|---|---|
| `anoa console [--level <lvl>]` | console output, newest last |
| `anoa errors` | uncaught exceptions and rejections |
| `anoa network` | fetch/XHR the page made: method, status, ms |

All three take `--clear` to forget what has been recorded.

These are recorded **inside the page**, which is what lets them report what
happened *before* the command ran — a one-shot process could never have
subscribed to the events in time. Consequences worth knowing:

- The buffer starts empty on every page load. "nothing recorded since the page
  loaded" means exactly that, not that nothing happened earlier.
- It holds the last 500 entries of each kind.
- Only `fetch` and `XMLHttpRequest` are seen. Document navigations, images,
  stylesheets and other subresource loads are not.

## Capture

| Command | Does |
|---|---|
| `anoa screenshot [file]` | PNG of the viewport (default screenshot.png) |
| `anoa pdf [file]` | PDF of the page (default page.pdf) |

## Agents

| Command | Does |
|---|---|
| `anoa help [group]` | grouped command list, or one group |
| `anoa skills list` | what this binary carries |
| `anoa skills get core` | the workflow: how to drive a page |
| `anoa skills get commands` | this document |
| `anoa close` | ask the browser to exit |

## Not implemented

Worth knowing so you do not reach for them: there is no React introspection, no
Web Vitals, no accessibility audit, no credential vault, no MCP server, no
plugin system, and no request interception — `anoa network` observes, it cannot
block or rewrite.
)";

const char kIndex[] = R"(core       the workflow: start a browser, snapshot, act by ref
commands   every command, with its arguments
)";

QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}

} // namespace

int runSkillsCommand(const QStringList &args)
{
    QTextStream err(stderr);

    if (args.isEmpty() || args.first() == QStringLiteral("list")) {
        out() << QLatin1String(kIndex);
        return 0;
    }
    if (args.first() == QStringLiteral("get")) {
        const QString name = args.size() > 1 ? args.at(1) : QStringLiteral("core");
        if (name == QStringLiteral("core")) {
            out() << QLatin1String(kCore);
            return 0;
        }
        if (name == QStringLiteral("commands")) {
            out() << QLatin1String(kCommands);
            return 0;
        }
        err << "anoa: no skill named '" << name << "' — try: anoa skills list" << Qt::endl;
        return 1;
    }
    err << "anoa: usage: anoa skills list | anoa skills get core" << Qt::endl;
    return 2;
}
