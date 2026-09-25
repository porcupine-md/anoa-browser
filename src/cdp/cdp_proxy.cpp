#include "cdp/cdp_proxy.h"
#include "cdp/cdp_extensions.h"

#include <QDebug>
#include <QHostAddress>
#include <QJsonDocument>
#include <QPointer>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QWebSocketProtocol>

CdpProxy::CdpProxy(quint16 listenPort, quint16 debuggingPort,
                   const QString &authToken, QObject *parent)
    : QObject(parent)
    , m_server(new QWebSocketServer(QStringLiteral("CdpProxy"),
                                    QWebSocketServer::NonSecureMode, this))
    , m_listenPort(listenPort)
    , m_debugPort(debuggingPort)
    , m_authToken(authToken)
{}

bool CdpProxy::start()
{
    if (!m_server->listen(QHostAddress::Any, m_listenPort)) {
        qWarning() << "CdpProxy: failed to listen on port" << m_listenPort
                   << m_server->errorString();
        return false;
    }
    connect(m_server, &QWebSocketServer::newConnection,
            this, &CdpProxy::onNewConnection);
    qInfo() << "CdpProxy: listening on ws://0.0.0.0:" << m_listenPort;
    return true;
}

void CdpProxy::setPageResolver(std::function<QWebEnginePage *(const QString &)> resolver)
{
    m_pageResolver = std::move(resolver);
}

void CdpProxy::setTabHost(TabHost *tabs)
{
    m_tabs = tabs;
}

void CdpProxy::stop()
{
    m_server->close();
    for (auto *client : m_clientToUpstream.keys()) {
        client->close();
    }
    m_clientToUpstream.clear();
    m_upstreamToClient.clear();
}

std::function<void(const QString &)> CdpProxy::makeDeferredSender(QWebSocket *client) const
{
    // QPointer, not the raw socket: the answer arrives on a later turn of the
    // event loop, by which time the client may have disconnected and been
    // deleted. Dropping a reply on a dead socket is correct; writing to freed
    // memory is not.
    QPointer<QWebSocket> guard(client);
    auto answered = std::make_shared<bool>(false);
    return [guard, answered](const QString &reply) {
        if (*answered)
            return; // one command, one answer
        *answered = true;
        if (guard && guard->state() == QAbstractSocket::ConnectedState)
            guard->sendTextMessage(reply);
    };
}

QWebEnginePage *CdpProxy::pageForClient(QWebSocket *client) const
{
    if (!m_pageResolver)
        return nullptr;
    // Resolved per message, not cached at connect time: task-004's lookup backs
    // off for a few seconds, so a client can arrive before its tab has a target
    // id and would otherwise be stuck on whatever the answer was then.
    return m_pageResolver(m_clientTargetId.value(client));
}

void CdpProxy::onNewConnection()
{
    QWebSocket *client = m_server->nextPendingConnection();
    if (!client)
        return;

    // Auth check: ?token= query param or Authorization: Bearer <token> header.
    if (!m_authToken.isEmpty()) {
        bool authorized = false;
        QUrlQuery query(client->requestUrl());
        if (query.queryItemValue(QStringLiteral("token")) == m_authToken)
            authorized = true;
        if (!authorized) {
            QByteArray authHeader = client->request().rawHeader("Authorization");
            if (!authHeader.isEmpty()) {
                QString authStr = QString::fromUtf8(authHeader);
                if (authStr.startsWith(QStringLiteral("Bearer "), Qt::CaseInsensitive)
                    && authStr.mid(7) == m_authToken)
                    authorized = true;
            }
        }
        if (!authorized) {
            // QWebSocketServer offers no hook to reject during the HTTP
            // upgrade, so the handshake has already completed; 1008 (policy
            // violation) is the closest to an HTTP 401 a client can observe.
            client->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("Unauthorized"));
            client->deleteLater();
            return;
        }
    }

    // Extract target path and open an upstream connection to Chromium DevTools.
    QString path = client->requestUrl().path();
    QUrl upstreamUrl(QStringLiteral("ws://127.0.0.1:%1%2").arg(m_debugPort).arg(path));

    // /devtools/page/<targetId> — anything else (the browser endpoint) leaves
    // this empty and resolves to the active tab, which is what it did before
    // tabs existed.
    static const QLatin1String kPagePrefix("/devtools/page/");
    QString targetId;
    if (path.startsWith(kPagePrefix))
        targetId = path.mid(kPagePrefix.size());
    m_clientTargetId.insert(client, targetId);

    QWebSocket *upstream = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    m_clientToUpstream.insert(client, upstream);
    m_upstreamToClient.insert(upstream, client);

    connect(client, &QWebSocket::textMessageReceived, this, &CdpProxy::onClientMessage);
    connect(client, &QWebSocket::disconnected, this, &CdpProxy::onClientDisconnected);
    connect(upstream, &QWebSocket::textMessageReceived, this, &CdpProxy::onUpstreamMessage);
    connect(upstream, &QWebSocket::disconnected, this, &CdpProxy::onUpstreamDisconnected);
    connect(upstream, &QWebSocket::connected, this, &CdpProxy::onUpstreamConnected);

    upstream->open(upstreamUrl);
}

void CdpProxy::onClientMessage(const QString &message)
{
    QWebSocket *client = qobject_cast<QWebSocket *>(sender());
    if (!client)
        return;
    QWebSocket *upstream = m_clientToUpstream.value(client);
    if (!upstream)
        return;

    // If the upstream handshake is not yet complete, queue the message.
    // onUpstreamConnected() will flush the queue when the connection is ready.
    if (upstream->state() != QAbstractSocket::ConnectedState) {
        m_pendingMessages[upstream].append(message);
        return;
    }

    QJsonObject cmd = QJsonDocument::fromJson(message.toUtf8()).object();
    bool deferred = false;
    const QString handled = CdpExtensions::processCommand(cmd, pageForClient(client), m_tabs,
                                                          &deferred,
                                                          makeDeferredSender(client));
    if (!handled.isEmpty()) {
        client->sendTextMessage(handled);
        return;
    }
    // Ours, but not answerable yet. Nothing goes upstream: the reply will come
    // through the callback above.
    if (deferred)
        return;
    // Remembered before forwarding, not after: the client may disconnect the
    // moment it has its reply, and a one-shot CLI process does exactly that.
    rememberOverride(m_clientTargetId.value(client), cmd);
    // Optionally rewrite the command before forwarding (e.g. strip synthetic context IDs).
    const QJsonObject rewritten = CdpExtensions::rewritePassthrough(cmd);
    if (!rewritten.isEmpty()) {
        upstream->sendTextMessage(
            QString::fromUtf8(QJsonDocument(rewritten).toJson(QJsonDocument::Compact)));
    } else {
        upstream->sendTextMessage(message);
    }
}

void CdpProxy::onUpstreamConnected()
{
    QWebSocket *upstream = qobject_cast<QWebSocket *>(sender());
    if (!upstream)
        return;
    // Before anything the client sent: a session starts with no overrides, and
    // the first command down this socket may well be the screenshot that has
    // to come out themed.
    if (QWebSocket *client = m_upstreamToClient.value(upstream))
        replayOverrides(upstream, m_clientTargetId.value(client));

    // Flush any messages that arrived before the upstream handshake completed.
    const QStringList pending = m_pendingMessages.take(upstream);
    for (const QString &message : pending) {
        QWebSocket *client = m_upstreamToClient.value(upstream);
        if (!client)
            continue;
        QJsonObject cmd = QJsonDocument::fromJson(message.toUtf8()).object();
        bool deferred = false;
        const QString handled = CdpExtensions::processCommand(cmd, pageForClient(client), m_tabs,
                                                              &deferred,
                                                              makeDeferredSender(client));
        if (deferred)
            continue;
        if (!handled.isEmpty()) {
            client->sendTextMessage(handled);
            continue;
        }
        rememberOverride(m_clientTargetId.value(client), cmd);
        const QJsonObject rewritten = CdpExtensions::rewritePassthrough(cmd);
        if (!rewritten.isEmpty()) {
            upstream->sendTextMessage(
                QString::fromUtf8(QJsonDocument(rewritten).toJson(QJsonDocument::Compact)));
        } else {
            upstream->sendTextMessage(message);
        }
    }
}

void CdpProxy::onClientDisconnected()
{
    QWebSocket *client = qobject_cast<QWebSocket *>(sender());
    if (!client)
        return;
    m_clientTargetId.remove(client);
    QWebSocket *upstream = m_clientToUpstream.take(client);
    if (upstream) {
        m_upstreamToClient.remove(upstream);
        m_pendingMessages.remove(upstream);
        m_replayIds.remove(upstream);
        upstream->close();
        upstream->deleteLater();
    }
    client->deleteLater();
}

namespace {

// The overrides Chromium forgets when the socket that set them closes, and
// which of them cancels which. Everything else a client sends is left alone:
// replaying a navigation or a click would be a different program running
// itself twice, and only state-setting commands are idempotent enough to
// repeat against a fresh session.
//
// Nothing here is guesswork about what *should* be session-scoped — these are
// the commands `anoa set` issues, plus the clears that undo them.
struct OverrideRule {
    const char *method;
    const char *cancels; // the setter this one clears, or nullptr if it is one
};
const OverrideRule kOverrides[] = {
    {"Emulation.setEmulatedMedia", nullptr},
    {"Emulation.setDeviceMetricsOverride", nullptr},
    {"Emulation.setGeolocationOverride", nullptr},
    {"Emulation.setUserAgentOverride", nullptr},
    {"Emulation.setTimezoneOverride", nullptr},
    {"Emulation.setLocaleOverride", nullptr},
    {"Network.emulateNetworkConditions", nullptr},
    {"Network.setExtraHTTPHeaders", nullptr},
    {"Emulation.clearDeviceMetricsOverride", "Emulation.setDeviceMetricsOverride"},
    {"Emulation.clearGeolocationOverride", "Emulation.setGeolocationOverride"},
};

} // namespace

void CdpProxy::rememberOverride(const QString &targetId, const QJsonObject &cmd)
{
    const QString method = cmd.value(QStringLiteral("method")).toString();
    for (const OverrideRule &rule : kOverrides) {
        if (method != QLatin1String(rule.method))
            continue;
        if (rule.cancels) {
            // A clear is not worth replaying — the fresh session starts
            // cleared. Forgetting the setter is the whole of its effect here.
            m_overrides[targetId].remove(QLatin1String(rule.cancels));
            return;
        }
        m_overrides[targetId].insert(method, cmd.value(QStringLiteral("params")).toObject());
        return;
    }
}

void CdpProxy::replayOverrides(QWebSocket *upstream, const QString &targetId)
{
    const auto it = m_overrides.constFind(targetId);
    if (it == m_overrides.constEnd())
        return;

    for (auto o = it->constBegin(); o != it->constEnd(); ++o) {
        // Ids from a range no client realistically uses, and recorded so the
        // reply can be dropped. A client numbering its own commands from 1
        // would otherwise see an answer to a question it never asked.
        const int id = 1000000000 + (m_nextReplayId++);
        m_replayIds[upstream].insert(id);

        QJsonObject cmd;
        cmd[QStringLiteral("id")] = id;
        cmd[QStringLiteral("method")] = o.key();
        cmd[QStringLiteral("params")] = o.value();
        upstream->sendTextMessage(
            QString::fromUtf8(QJsonDocument(cmd).toJson(QJsonDocument::Compact)));
    }
}

bool CdpProxy::isReplayReply(QWebSocket *upstream, const QJsonObject &message)
{
    // Only replies carry an id we issued; events have no id at all and must
    // reach the client untouched.
    const QJsonValue id = message.value(QStringLiteral("id"));
    if (!id.isDouble())
        return false;
    auto it = m_replayIds.find(upstream);
    if (it == m_replayIds.end())
        return false;
    return it->remove(id.toInt());
}

void CdpProxy::onUpstreamMessage(const QString &message)
{
    QWebSocket *upstream = qobject_cast<QWebSocket *>(sender());
    if (!upstream)
        return;
    QWebSocket *client = m_upstreamToClient.value(upstream);
    if (!client)
        return;
    const QJsonObject parsed = QJsonDocument::fromJson(message.toUtf8()).object();
    if (isReplayReply(upstream, parsed))
        return;
    client->sendTextMessage(message);
}

void CdpProxy::onUpstreamDisconnected()
{
    QWebSocket *upstream = qobject_cast<QWebSocket *>(sender());
    if (!upstream)
        return;
    QWebSocket *client = m_upstreamToClient.take(upstream);
    if (client) {
        m_clientToUpstream.remove(client);
        client->close();
        client->deleteLater();
    }
    m_replayIds.remove(upstream);
    upstream->deleteLater();
}
