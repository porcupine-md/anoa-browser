#pragma once

// `anoa skills get <name>` — the workflow instructions an agent should read,
// carried inside the binary.
//
// Embedded rather than shipped as a file next to the install, and served at
// runtime rather than copied into a project, for one reason: instructions that
// live anywhere else go stale against the CLI they describe. A stub in
// .claude/skills that runs `anoa skills get core` always describes the binary
// that is actually installed.

#include <QStringList>

class QTextStream;

// Handles `list` and `get <name>`. Returns the process exit code and never
// touches the browser — an agent asks for this before starting one.
// Writes to the caller's streams, so the MCP door gets the text instead of
// the server's stdout.
int runSkillsCommand(const QStringList &args, QTextStream &out, QTextStream &err);
