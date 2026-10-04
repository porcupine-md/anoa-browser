#include "mcp/mcp_server.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTemporaryDir>

#include "agent/agent_cli.h"
#include "mcp/mcp_tools.h"

namespace {

// JSON-RPC 2.0 error codes. The first four are the spec's; -32000 is in the
// implementation-defined range and is what this server uses for "I could not
// do that right now", which is different from "you asked wrongly".
enum RpcError {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    ServerError = -32000,
};

QJsonObject errorReply(const QJsonValue &id, int code, const QString &message)
{
    QJsonObject error;
    error[QStringLiteral("code")] = code;
    error[QStringLiteral("message")] = message;

    QJsonObject reply;
    reply[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    reply[QStringLiteral("id")] = id;
    reply[QStringLiteral("error")] = error;
    return reply;
}

QJsonObject resultReply(const QJsonValue &id, const QJsonObject &result)
{
    QJsonObject reply;
    reply[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    reply[QStringLiteral("id")] = id;
    reply[QStringLiteral("result")] = result;
    return reply;
}

// A tool's answer. Text, because that is what a model reads — and a failure is
// *not* a JSON-RPC error: the protocol reserves those for "the call itself was
// malformed". A command that failed is a result the model should see and act
// on, which is what isError means.
QJsonObject toolContent(const QString &text, bool isError = false)
{
    QJsonObject entry;
    entry[QStringLiteral("type")] = QStringLiteral("text");
    entry[QStringLiteral("text")] = text;

    QJsonArray content;
    content.append(entry);

    QJsonObject result;
    result[QStringLiteral("content")] = content;
    if (isError)
        result[QStringLiteral("isError")] = true;
    return result;
}

} // namespace

const char *McpServer::protocolVersion()
{
    return "2025-06-18";
}

McpServer::McpServer(const QString &host, int port, const QString &token)
    : m_host(host)
    , m_port(port)
    , m_token(token)
{
}

McpServer::~McpServer()
{
    if (m_session)
        agentSessionClose(m_session);
}

AgentSession *McpServer::session(QString *why)
{
    if (!m_session)
        m_session = agentSessionOpen(m_host, m_port, m_token, why);
    return m_session;
}

QByteArray McpServer::handle(const QByteArray &body, int *httpStatus)
{
    const auto status = [httpStatus](int code) {
        if (httpStatus)
            *httpStatus = code;
    };

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        status(400);
        return QJsonDocument(errorReply(QJsonValue(), ParseError,
                                        parseError.errorString()))
            .toJson(QJsonDocument::Compact);
    }

    bool isNotification = false;
    const QJsonObject reply = dispatch(doc.object(), &isNotification);
    if (isNotification) {
        // Nothing to answer. 202 is the spec's "accepted, no body".
        status(202);
        return QByteArray();
    }
    status(200);
    return QJsonDocument(reply).toJson(QJsonDocument::Compact);
}

QJsonObject McpServer::dispatch(const QJsonObject &request, bool *isNotification)
{
    const QString method = request.value(QStringLiteral("method")).toString();
    const QJsonValue id = request.value(QStringLiteral("id"));
    // A request without an id is a notification: it gets no reply at all, ever,
    // including when it is wrong.
    if (isNotification)
        *isNotification = !request.contains(QStringLiteral("id"));

    if (method.isEmpty())
        return errorReply(id, InvalidRequest, QStringLiteral("no method"));

    if (method == QLatin1String("initialize")) {
        QJsonObject tools; // an empty object advertises the capability
        QJsonObject capabilities;
        capabilities[QStringLiteral("tools")] = tools;

        QJsonObject info;
        info[QStringLiteral("name")] = QStringLiteral("anoa");
        info[QStringLiteral("version")] = QStringLiteral(ANOA_VERSION);

        QJsonObject result;
        result[QStringLiteral("protocolVersion")] = QLatin1String(protocolVersion());
        result[QStringLiteral("capabilities")] = capabilities;
        result[QStringLiteral("serverInfo")] = info;
        result[QStringLiteral("instructions")] = QStringLiteral(
            "A real browser that outlives these calls. snapshot returns refs like @e2 "
            "that later calls target; clicks are hit-tested, so a button under a banner "
            "is reported rather than clicked through. Emulation set with browser_set "
            "belongs to the tab and persists.");
        return resultReply(id, result);
    }

    if (method == QLatin1String("ping"))
        return resultReply(id, QJsonObject());

    if (method.startsWith(QLatin1String("notifications/"))) {
        // initialized, cancelled, and anything else the client tells us about.
        // Nothing here acts on them, and a notification takes no reply.
        if (isNotification)
            *isNotification = true;
        return QJsonObject();
    }

    if (method == QLatin1String("tools/list")) {
        QJsonArray list;
        for (const McpTool &tool : mcpTools()) {
            QJsonObject entry;
            entry[QStringLiteral("name")] = tool.name;
            entry[QStringLiteral("description")] = tool.description;
            entry[QStringLiteral("inputSchema")] = tool.inputSchema;
            list.append(entry);
        }
        QJsonObject result;
        result[QStringLiteral("tools")] = list;
        return resultReply(id, result);
    }

    if (method == QLatin1String("tools/call")) {
        if (m_busy) {
            return errorReply(id, ServerError,
                              QStringLiteral("another tool call is in flight — this "
                                             "browser runs one at a time"));
        }
        m_busy = true;
        const QJsonObject result =
            callTool(request.value(QStringLiteral("params")).toObject());
        m_busy = false;

        if (result.contains(QStringLiteral("__rpcError"))) {
            const QJsonObject e = result.value(QStringLiteral("__rpcError")).toObject();
            return errorReply(id, e.value(QStringLiteral("code")).toInt(),
                              e.value(QStringLiteral("message")).toString());
        }
        return resultReply(id, result);
    }

    return errorReply(id, MethodNotFound, QStringLiteral("unknown method: ") + method);
}

QJsonObject McpServer::callTool(const QJsonObject &params)
{
    const auto rpcError = [](int code, const QString &message) {
        QJsonObject e;
        e[QStringLiteral("code")] = code;
        e[QStringLiteral("message")] = message;
        QJsonObject wrapper;
        wrapper[QStringLiteral("__rpcError")] = e;
        return wrapper;
    };

    const QString name = params.value(QStringLiteral("name")).toString();
    const McpTool *tool = mcpToolNamed(name);
    if (!tool)
        return rpcError(InvalidParams, QStringLiteral("unknown tool: ") + name);

    const QJsonObject args = params.value(QStringLiteral("arguments")).toObject();
    QStringList argv;
    QString mapError;
    if (!mcpArgvFor(*tool, args, &argv, &mapError))
        return rpcError(InvalidParams, mapError);

    QString why;
    AgentSession *handle = session(&why);
    if (!handle) {
        return rpcError(ServerError,
                        QStringLiteral("cannot reach this browser: ") + why);
    }

    // exec is the one verb whose input is not an argument: it reads a file or
    // stdin, and an MCP call has neither. The script becomes a temp file for
    // the length of the call.
    QTemporaryDir scratch;
    if (tool->verb == QLatin1String("exec")) {
        const QString script = args.value(QStringLiteral("script")).toString();
        if (!scratch.isValid())
            return rpcError(ServerError, QStringLiteral("no temporary directory"));
        const QString path = scratch.filePath(QStringLiteral("batch.anoa"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
            return rpcError(ServerError, QStringLiteral("cannot write the batch"));
        file.write(script.toUtf8());
        file.close();
        argv = QStringList{path};
    }

    QString outText, errText;
    const int rc = agentSessionRun(handle, tool->verb, argv, /*json=*/false,
                                   &outText, &errText);

    if (rc != 0) {
        // The command's own message, which already says what went wrong in the
        // words the CLI would have used. Falling back to the exit code alone
        // would be the kind of answer a model cannot act on.
        const QString message = errText.trimmed().isEmpty()
            ? QStringLiteral("%1 failed with exit code %2").arg(tool->name).arg(rc)
            : errText.trimmed();
        return toolContent(message, /*isError=*/true);
    }

    const QString text = outText.trimmed().isEmpty()
        ? QStringLiteral("ok")
        : outText.trimmed();
    return toolContent(text);
}
