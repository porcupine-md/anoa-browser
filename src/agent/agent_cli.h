#pragma once

// The agent command layer: `anoa open`, `anoa snapshot`, `anoa click @e2`, …
//
// Every one of these is a one-shot process that attaches to an anoa that is
// already running and leaves it running. That is the whole design: an agent
// composes shell commands, and a command that had to boot a browser first
// would cost seconds per step and lose all page state between them. The
// browser is the session; these commands are statements against it.
//
// State that has to survive between invocations lives in the page, not here —
// see agent_script.h for why the @e1/@e2 refs are DOM attributes.

#include <QString>
#include <QStringList>

struct Config;

// True when `verb` is one of the agent commands. main.cpp asks before doing
// anything else, because the answer decides which application class to build.
bool isAgentCommand(const QString &verb);

// Runs one command against the endpoint in `config` (--port / --host).
// Returns the process exit code: 0 on success, 1 on a command failure, 2 on
// usage error, 3 when no browser is listening.
int runAgentCommand(const Config &config, const QString &verb, const QStringList &args);

// ── a connection held open across many commands ─────────────────────────────
//
// `exec` already does this within one process, because attaching costs about
// 130 ms and a twenty-step flow should not pay it twenty times. The MCP
// endpoint needs the same thing for the life of the browser, from a different
// translation unit — hence a facade rather than a shared class: the Session
// type stays private to agent_cli.cpp, where its blocking design is explained.
//
// The commands themselves print rather than return (every cmdX is an int and a
// side effect), so running one for a caller that needs the *text* means
// capturing what it would have written. That is what agentSessionRun does, and
// it is why every existing command works through here unchanged — `--json`
// included.
class AgentSession;

// Null on failure, with `why` set to something a caller can show a user.
AgentSession *agentSessionOpen(const QString &host, int port, const QString &token,
                               QString *why);
void agentSessionClose(AgentSession *session);

// Runs one verb and captures what it printed. Returns the same exit code the
// command line would have produced: 0 ok, 1 failed, 2 usage.
int agentSessionRun(AgentSession *session, const QString &verb, QStringList args,
                    bool json, QString *outText, QString *errText);
