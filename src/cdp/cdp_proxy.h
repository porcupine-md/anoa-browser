#pragma once

#include <functional>

#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWebSocket>
#include <QWebSocketServer>

class QWebEnginePage;
class TabHost;

class CdpProxy : public QObject {
    Q_OBJECT
public:
    explicit CdpProxy(quint16 listenPort, quint16 debuggingPort,
                      const QString &authToken, QObject *parent = nullptr);

    bool start();
    void stop();
    // How to find the page a client is talking about, for the commands this
    // proxy answers itself (Page.printToPDF via QWebEnginePage::printToPdf,
    // Security.*). A std::function rather than an AnoaBrowser pointer on
    // purpose: src/cdp knows nothing about src/browser, and the seam stays a
    // callable the owner installs. An empty target id means the browser-level
    // endpoint, which resolves to the active tab.
    void setPageResolver(std::function<QWebEnginePage *(const QString &targetId)> resolver);
    // The tab registry, for the Target domain. Also an interface rather than a
    // browser type, for the same reason the resolver is a callable.
    void setTabHost(TabHost *tabs);

private:
    QWebEnginePage *pageForClient(QWebSocket *client) const;
    // CDP emulation is *session*-scoped: Chromium drops every override when
    // the socket that set it goes away. Every anoa command is its own process
    // and its own socket, so `set media dark` applied an override and took it
    // with it on exit — reported in #39, and true of the whole `set` family.
    //
    // The browser is meant to be the session here ("the page, the cookies and
    // the scroll position survive between commands"), so the overrides are
    // remembered per target and replayed onto each new upstream connection.
    // The proxy is the only place that sees every attach, which is what makes
    // this one hook instead of one per command.
    void rememberOverride(const QString &targetId, const QJsonObject &cmd);
    void replayOverrides(QWebSocket *upstream, const QString &targetId);
    // True for a reply to a command this proxy injected rather than the client
    // sending it — the client never saw the request and must never see the
    // answer.
    bool isReplayReply(QWebSocket *upstream, const QJsonObject &message);
    // A reply the handler will produce later, aimed at one client. Guarded by a
    // QPointer and a state check because the client may well be gone by then,
    // and by a one-shot flag because a command gets exactly one answer.
    std::function<void(const QString &)> makeDeferredSender(QWebSocket *client) const;

private slots:
    void onNewConnection();
    void onClientMessage(const QString &message);
    void onClientDisconnected();
    void onUpstreamMessage(const QString &message);
    void onUpstreamDisconnected();
    void onUpstreamConnected();

private:
    QWebSocketServer *m_server;
    QMap<QWebSocket *, QWebSocket *> m_clientToUpstream;
    QMap<QWebSocket *, QWebSocket *> m_upstreamToClient;
    // Messages queued while the upstream WS handshake is in progress
    QMap<QWebSocket *, QStringList> m_pendingMessages;
    // The target id each client dialled, taken from /devtools/page/<id>. Kept
    // per connection and resolved on every message rather than once at connect
    // time, because a mapping can still be resolving when the client arrives.
    QMap<QWebSocket *, QString> m_clientTargetId;
    // targetId -> method -> the params it was last called with. Insertion
    // order is not preserved and does not need to be: each override is
    // independent of the others, and a method set twice keeps only the last
    // value, which is what a later caller asked for.
    QHash<QString, QMap<QString, QJsonObject>> m_overrides;
    // Ids this proxy used for replayed commands, per upstream, so their
    // replies can be swallowed instead of confusing a client that never asked.
    QHash<QWebSocket *, QSet<int>> m_replayIds;
    int m_nextReplayId = 1;
    quint16 m_listenPort;
    quint16 m_debugPort;
    QString m_authToken;
    std::function<QWebEnginePage *(const QString &)> m_pageResolver;
    TabHost *m_tabs = nullptr;
};
