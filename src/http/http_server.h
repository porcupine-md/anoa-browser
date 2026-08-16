#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTcpServer>

class AnoaBrowser;
class QUrlQuery;
class QWebEngineView;

class HttpServer : public QObject {
    Q_OBJECT
public:
    explicit HttpServer(quint16 port, quint16 debuggingPort, quint16 proxyPort,
                       const QString &authToken, AnoaBrowser *browser,
                       QObject *parent = nullptr);

    bool start();
    void stop();

    // Which origins may put the live view in an iframe. Empty is the default
    // and means 'self' only — see frameAncestorsHeader() for why that is the
    // default rather than the permissive thing.
    void setEmbedOrigins(const QStringList &origins) { m_embedOrigins = origins; }

private slots:
    void handleNewConnection();

private:
    // /json and /json/list, rebuilt from the tab registry: a tab id is ours and
    // appears nowhere in Chromium's answer, so the old byte rewrite had nothing
    // to carry it in. Falls back to the already-rewritten upstream body when no
    // tab has resolved its target id yet.
    QByteArray rebuildTargetList(const QByteArray &rewritten, const QString &hostName) const;
    // ?tab=<id> on any /render/* endpoint. Returns the view to act on, or
    // nullptr for an id that is malformed or names no tab — never the active
    // tab as a silent consolation, because a caller that named the wrong tab
    // has to find out.
    QWebEngineView *resolveRenderTab(const QUrlQuery &query, QString *badId) const;
    // The Content-Security-Policy line for the viewer page, terminator
    // included, or empty for no restriction at all.
    QByteArray frameAncestorsHeader() const;
    // The Access-Control-* lines for a request from this Origin, or empty when
    // the origin was never named by --embed-origin.
    QByteArray corsHeadersFor(const QString &origin) const;

    QTcpServer *m_server;
    quint16 m_port;
    quint16 m_debugPort;
    quint16 m_proxyPort;
    QString m_authToken;
    AnoaBrowser *m_browser;
    QStringList m_embedOrigins;
};
