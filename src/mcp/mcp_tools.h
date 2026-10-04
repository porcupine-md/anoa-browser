#pragma once

// The command surface, declared once as data.
//
// Everywhere else in this repo the commands are described in prose: the help
// groups in agent_help.cpp, the markdown tables in agent_skill.cpp, and the
// website. None of those can be read by a machine, and MCP needs a JSON Schema
// per tool — so this is the first table that states a verb, its arguments and
// their types in a form something else could consume.
//
// That makes it the third description of the same surface, which is a real
// debt: the three can drift. The intended end state is that help and skills
// are generated from this table. That is not this change.

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct McpTool {
    // MCP tool names are namespaced so a client holding several servers can
    // tell whose tool is whose.
    QString name;
    // The anoa verb it runs. Several tools share one verb where the verb takes
    // a subcommand (tab, get, set, storage, …) — the subcommand is then a
    // fixed leading argument rather than something the model has to guess.
    QString verb;
    QString description;
    // JSON Schema for the arguments object, as the client will see it.
    QJsonObject inputSchema;
    // Schema property names, in the order they become positional arguments.
    QStringList positional;
    // Arguments that are fixed for this tool, prepended before the positional
    // ones. This is what lets `tab new` and `tab close` be two tools.
    QStringList fixed;
    // Schema property -> the flag it becomes. A boolean property emits the flag
    // alone; anything else emits the flag and its value.
    QVector<QPair<QString, QString>> flags;
};

// Every tool, built once.
const QVector<McpTool> &mcpTools();

// The tool with this name, or nullptr.
const McpTool *mcpToolNamed(const QString &name);

// Turns an arguments object into the argv a command expects. Pure, and the
// place where most of the risk in 30-odd hand-written schemas lives, so it is
// unit tested without a browser.
//
// Returns false and sets `error` when a required property is missing.
bool mcpArgvFor(const McpTool &tool, const QJsonObject &args, QStringList *argv,
                QString *error);
