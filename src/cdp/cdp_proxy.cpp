#include "cdp/cdp_proxy.h"
#include "cdp/cdp_extensions.h"
#include "cdp/tab_host.h"

#include <QDebug>
#include <QHostAddress>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
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

    // --graze may have handed this tab's renderer back, taking its engine target
    // with it. The id the client dialled is the stale one, and that is exactly
    // what finds the tab: the registry still maps it. Waking has to happen for
    // the agent CLI to work at all, because it reaches every tab over CDP
    // rather than over /render/*.
    QString grazedTab;
    if (!targetId.isEmpty() && m_tabs)
        grazedTab = m_tabs->tabIdForTargetId(targetId);
    m_clientTargetId.insert(client, targetId);

    QWebSocket *upstream = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    m_clientToUpstream.insert(client, upstream);
    m_upstreamToClient.insert(upstream, client);

    connect(client, &QWebSocket::textMessageReceived, this, &CdpProxy::onClientMessage);
    connect(client, &QWebSocket::disconnected, this, &CdpProxy::onClientDisconnected);
    connect(upstream, &QWebSocket::textMessageReceived, this, &CdpProxy::onUpstreamMessage);
    connect(upstream, &QWebSocket::disconnected, this, &CdpProxy::onUpstreamDisconnected);
    connect(upstream, &QWebSocket::connected, this, &CdpProxy::onUpstreamConnected);

    if (grazedTab.isEmpty()) {
        upstream->open(upstreamUrl);
        return;
    }

    // Deferred by one event loop turn, and this is the whole trick: waking is
    // fast but blocking, and blocking inside the connection handler leaves the
    // socket we were just handed unusable — measured as a client that hangs
    // until its own timeout while the wake itself took a tenth of a second.
    // Off the handler's stack, the same call works.
    QPointer<QWebSocket> up(upstream);
    QPointer<QWebSocket> cl(client);
    const QString dialled = targetId;
    QTimer::singleShot(0, this, [this, up, cl, grazedTab, dialled]() {
        if (!up || !cl)
            return;
        m_tabs->wakeTab(grazedTab);
        // A woken tab came back on a new renderer, so its target id changed.
        QString fresh = m_tabs->targetIdFor(grazedTab);
        if (fresh.isEmpty())
            fresh = dialled;
        m_clientTargetId.insert(cl, fresh);
        up->open(QUrl(QStringLiteral("ws://127.0.0.1:%1/devtools/page/%2")
                          .arg(m_debugPort).arg(fresh)));
    });
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
        upstream->close();
        upstream->deleteLater();
    }
    client->deleteLater();
}

void CdpProxy::onUpstreamMessage(const QString &message)
{
    QWebSocket *upstream = qobject_cast<QWebSocket *>(sender());
    if (!upstream)
        return;
    QWebSocket *client = m_upstreamToClient.value(upstream);
    if (!client)
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
    upstream->deleteLater();
}
