#pragma once

// MCP over Streamable HTTP, served from the browser's own HTTP port.
//
// The smallest thing the spec allows: POST carrying one JSON-RPC message in
// and one JSON response out. No SSE, because this server never pushes
// anything — the spec permits answering a POST with application/json directly,
// and a stream nobody writes to is machinery without a purpose.
//
// The odd part, stated out loud: this runs *inside* the browser process, but
// every command it runs only knows how to work through a CDP session. So it
// attaches a client back to its own process — 127.0.0.1:port, through the
// proxy on port+2. The alternative was reimplementing thirty commands against
// QWebEnginePage and maintaining both copies forever.

#include <QByteArray>
#include <QJsonObject>
#include <QString>

class AgentSession;

class McpServer
{
public:
    // `host`/`port`/`token` are how to reach this same browser.
    McpServer(const QString &host, int port, const QString &token);
    ~McpServer();

    McpServer(const McpServer &) = delete;
    McpServer &operator=(const McpServer &) = delete;

    // Handles one request body. Returns the response body, and sets
    // `httpStatus`. A notification (no id) returns empty with status 202,
    // which is what the spec asks for.
    QByteArray handle(const QByteArray &body, int *httpStatus);

    // The protocol revision this implements, as sent in an initialize reply.
    static const char *protocolVersion();

private:
    QJsonObject dispatch(const QJsonObject &request, bool *isNotification);
    QJsonObject callTool(const QJsonObject &params);
    // Opens the connection to this browser on first use. It cannot happen in
    // the constructor: the HTTP server is built before the CDP proxy is
    // listening, so an attach there would always fail.
    AgentSession *session(QString *why);

    QString m_host;
    int m_port;
    QString m_token;
    AgentSession *m_session = nullptr;
    // One command at a time. Each runs a nested event loop, and a second
    // request entering that loop would interleave two commands on one
    // connection.
    bool m_busy = false;
};
