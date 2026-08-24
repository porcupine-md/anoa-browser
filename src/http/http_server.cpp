#include "http/http_server.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QWebEnginePage>
#include <QWebEngineView>

#include "browser/anoa_browser.h"
#include "browser/tab_ids.h"

#include <QBuffer>
#include <QEventLoop>
#include <QHostAddress>
#include <QImage>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QTcpSocket>
#include <QFile>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <memory>

HttpServer::HttpServer(quint16 port, quint16 debuggingPort, quint16 proxyPort,
                       const QString &authToken, AnoaBrowser *browser, QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_port(port)
    , m_debugPort(debuggingPort)
    , m_proxyPort(proxyPort)
    , m_authToken(authToken)
    , m_browser(browser)
{
    connect(m_server, &QTcpServer::newConnection, this, &HttpServer::handleNewConnection);
}

bool HttpServer::start()
{
    return m_server->listen(QHostAddress::Any, m_port);
}

void HttpServer::stop()
{
    m_server->close();
}

// Close a response socket without losing what was just written to it.
//
// The old sequence — write, flush, disconnectFromHost, deleteLater — dropped
// anything that did not fit in the kernel's send buffer. deleteLater() destroys
// the socket on the next return to the event loop, which for a large body is
// long before the write buffer has drained; the peer got a truncated response
// while Content-Length still promised the whole thing. A 1280x720 PPM declared
// 2.7 MB and delivered nothing at all. (bug-004.)
//
// disconnectFromHost() already defers the close until pending writes are done.
// All that was missing is keeping the object alive that long, so deletion is
// driven by `disconnected` instead of by the next event loop turn.
static void closeWhenSent(QTcpSocket *socket)
{
    socket->flush();
    QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    // A peer that vanishes mid-write would otherwise leak the socket, since
    // `disconnected` is not guaranteed to arrive.
    QObject::connect(socket, &QAbstractSocket::errorOccurred, socket,
                     [socket](QAbstractSocket::SocketError) { socket->deleteLater(); });
    socket->disconnectFromHost();
}

// The CORS lines for this connection, decided once when the request is parsed
// and carried on the socket itself.
//
// A dynamic property rather than an argument threaded through every call site,
// and rather than a member: several handlers answer asynchronously, long after
// the request that set it has returned, so the value has to belong to the
// connection and not to the server.
static const char *kCorsProperty = "anoaCors";

static QByteArray corsFor(QTcpSocket *socket)
{
    return socket ? socket->property(kCorsProperty).toByteArray() : QByteArray();
}

static void sendResponse(QTcpSocket *socket, int statusCode, const QByteArray &statusText,
                         const QByteArray &body,
                         const QByteArray &contentType = "application/json")
{
    QByteArray response =
        "HTTP/1.1 " + QByteArray::number(statusCode) + " " + statusText + "\r\n"
        "Content-Type: " + contentType + "\r\n";
    response += corsFor(socket);
    response +=
        "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + body;
    socket->write(response);
    closeWhenSent(socket);
}

struct HtmlCaptureState {
    QString html;
    bool timedOut = true;
    bool waiting = true;
    QEventLoop *loop = nullptr;
};

QByteArray HttpServer::rebuildTargetList(const QByteArray &rewritten,
                                         const QString &hostName) const
{
    if (!m_browser)
        return rewritten;

    const QJsonArray upstream = QJsonDocument::fromJson(rewritten).array();

    // Title and url come from Chromium where it has them: it knows the live
    // document, while the view's own properties lag a navigation slightly.
    QHash<QString, QJsonObject> byTargetId;
    for (const QJsonValue &value : upstream) {
        const QJsonObject target = value.toObject();
        byTargetId.insert(target.value(QStringLiteral("id")).toString(), target);
    }

    QList<TabTargetInfo> tabs;
    const QString activeId = m_browser->activeTabId();
    for (const QString &tabId : m_browser->tabIds()) {
        TabTargetInfo info;
        info.tabId = tabId;
        info.chromiumTargetId = m_browser->chromiumTargetId(tabId);
        info.tabName = m_browser->nameFor(tabId);
        info.active = (tabId == activeId);

        const QJsonObject target = byTargetId.value(info.chromiumTargetId);
        info.title = target.value(QStringLiteral("title")).toString();
        info.url = target.value(QStringLiteral("url")).toString();
        if (info.url.isEmpty()) {
            if (QWebEngineView *view = m_browser->viewFor(tabId)) {
                info.url = view->url().toString();
                info.title = view->title();
            }
        }
        tabs.append(info);
    }

    QJsonArray out = buildTargetList(tabs, hostName, m_proxyPort);
    // Nothing resolved yet — task-004's lookup is still in flight. Answering an
    // empty array here would show a client zero targets where it sees one
    // today, which reads as "the browser has no pages" rather than "ask again".
    if (out.isEmpty())
        return rewritten;

    // Everything that is not a page — service workers, iframes — keeps its
    // upstream entry, already host-rewritten, so no client loses a target it
    // can see today.
    for (const QJsonValue &value : upstream) {
        const QJsonObject target = value.toObject();
        if (target.value(QStringLiteral("type")).toString() != QLatin1String("page"))
            out.append(target);
    }

    return QJsonDocument(out).toJson(QJsonDocument::Compact);
}

// `?mods=ctrl,shift` — comma-separated, order-free, unknown names ignored so a
// client that learns a new modifier before this server does degrades to sending
// the event unmodified rather than losing it to a 400.
static Qt::KeyboardModifiers parseModifiers(const QString &spec)
{
    Qt::KeyboardModifiers mods = Qt::NoModifier;
    const QStringList parts = spec.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &raw : parts) {
        const QString name = raw.trimmed().toLower();
        if (name == QLatin1String("ctrl") || name == QLatin1String("control"))
            mods |= Qt::ControlModifier;
        else if (name == QLatin1String("shift"))
            mods |= Qt::ShiftModifier;
        else if (name == QLatin1String("alt"))
            mods |= Qt::AltModifier;
        else if (name == QLatin1String("meta") || name == QLatin1String("cmd"))
            mods |= Qt::MetaModifier;
    }
    return mods;
}

// Shared by click and the three pointer endpoints, so "right" means the same
// button everywhere. Returns false for a name none of them accept.
static bool parseButton(const QString &spec, Qt::MouseButton *out)
{
    const QString name = spec.toLower();
    if (name.isEmpty() || name == QLatin1String("left"))
        *out = Qt::LeftButton;
    else if (name == QLatin1String("right"))
        *out = Qt::RightButton;
    else if (name == QLatin1String("middle"))
        *out = Qt::MiddleButton;
    else
        return false;
    return true;
}

QByteArray HttpServer::corsHeadersFor(const QString &origin) const
{
    // Same list as framing, and for the same reason: --embed-origin says an
    // origin is trusted to embed the view and drive the browser through it. An
    // origin trusted with that but not allowed to read /json/list can show the
    // view and cannot draw a tab bar around it, which is most of what embedding
    // is for. Nothing is opened up that framing had not already opened.
    //
    // Silence unless an origin was named, so the default configuration answers
    // exactly as it did before.
    if (origin.isEmpty() || m_embedOrigins.isEmpty())
        return QByteArray();

    const bool any = m_embedOrigins.contains(QStringLiteral("*"));
    if (!any && !m_embedOrigins.contains(origin))
        return QByteArray();

    // The caller's own origin is echoed rather than "*", so a future credentialed
    // request does not have to be rewritten. Vary: Origin keeps a cache from
    // handing one origin's answer to another.
    return "Access-Control-Allow-Origin: " + origin.toUtf8() + "\r\n"
           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
           "Access-Control-Allow-Headers: Authorization, Content-Type\r\n"
           "Vary: Origin\r\n";
}

QByteArray HttpServer::frameAncestorsHeader() const
{
    // The live view is not a picture of a browser, it is a handle on one: it
    // streams the screen and it forwards clicks and keystrokes. A page that can
    // frame it can read a logged-in session and act inside it, invisibly, from
    // any site the user happens to be visiting. The control endpoints have
    // always been reachable cross-origin, but blind — framing is what turns
    // that into a session someone can see and steer.
    //
    // So the default is 'self': the viewer frames itself and nothing else does,
    // and embedding somewhere real is one flag away. A refused frame says so in
    // the console, which is a better way to find out than never finding out.
    if (m_embedOrigins.contains(QStringLiteral("*")))
        return QByteArray();

    QByteArray value = "'self'";
    for (const QString &origin : m_embedOrigins) {
        const QString trimmed = origin.trimmed();
        if (trimmed.isEmpty())
            continue;
        value += " " + trimmed.toUtf8();
    }
    return "Content-Security-Policy: frame-ancestors " + value + "\r\n";
}

QWebEngineView *HttpServer::resolveRenderTab(const QUrlQuery &query, QString *badId) const
{
    if (!m_browser)
        return nullptr;
    const QString asked = query.queryItemValue(QStringLiteral("tab"));
    if (asked.isEmpty())
        return m_browser->activeView();
    // An id or a name — whichever the caller finds easier to keep track of.
    if (!isValidTabId(asked) && !isValidTabName(asked)) {
        *badId = asked;
        return nullptr;
    }
    const QString tabId = m_browser->resolveTab(asked);
    QWebEngineView *view = tabId.isEmpty() ? nullptr : m_browser->viewFor(tabId);
    if (!view)
        *badId = asked;
    return view;
}

void HttpServer::handleNewConnection()
{
    QTcpSocket *socket = m_server->nextPendingConnection();
    if (!socket)
        return;

    QByteArray requestData;
    while (!requestData.contains("\r\n\r\n")) {
        if (!socket->waitForReadyRead(5000)) {
            socket->disconnectFromHost();
            socket->deleteLater();
            return;
        }
        requestData += socket->readAll();
    }

    int firstLineEnd = requestData.indexOf("\r\n");
    QList<QByteArray> requestLineParts = requestData.left(firstLineEnd).split(' ');
    if (requestLineParts.size() < 2) {
        socket->disconnectFromHost();
        socket->deleteLater();
        return;
    }

    QString method = QString::fromUtf8(requestLineParts[0]);
    QString rawPath = QString::fromUtf8(requestLineParts[1]);

    QMap<QString, QString> headers;
    int headerEnd = requestData.indexOf("\r\n\r\n");
    QByteArray headerSection = requestData.mid(firstLineEnd + 2, headerEnd - firstLineEnd - 2);
    for (const QByteArray &line : headerSection.split('\n')) {
        QByteArray trimmed = line.trimmed();
        int colonPos = trimmed.indexOf(':');
        if (colonPos > 0) {
            QString key = QString::fromUtf8(trimmed.left(colonPos)).trimmed().toLower();
            QString value = QString::fromUtf8(trimmed.mid(colonPos + 1)).trimmed();
            headers[key] = value;
        }
    }

    QUrl url(rawPath);
    QString path = url.path();
    QUrlQuery query(url.query());
    QString hostHeader = headers.value(QStringLiteral("host"),
                                       QStringLiteral("127.0.0.1:%1").arg(m_port));

    // Decided before anything can answer, and carried on the connection, so the
    // handlers that reply asynchronously still send it.
    socket->setProperty(kCorsProperty,
                        corsHeadersFor(headers.value(QStringLiteral("origin"))));

    // A preflight is asked before the request it is about, so it cannot be
    // behind the auth check — the browser sends no credentials with it, and
    // answering 401 here fails the actual request with a CORS error that names
    // no cause. An origin we do not allow gets no CORS lines and the browser
    // stops it on its own, which is the correct outcome and a clear one.
    if (method == QLatin1String("OPTIONS")) {
        sendResponse(socket, 204, "No Content", QByteArray(), "text/plain");
        return;
    }

    if (!m_authToken.isEmpty()) {
        QString authHeader = headers.value(QStringLiteral("authorization"));
        bool viaBearer = authHeader.startsWith(QStringLiteral("Bearer "), Qt::CaseInsensitive)
                         && authHeader.mid(7) == m_authToken;
        bool viaQuery = query.queryItemValue(QStringLiteral("token")) == m_authToken;
        if (!viaBearer && !viaQuery) {
            sendResponse(socket, 401, "Unauthorized", R"({"error":"unauthorized"})");
            return;
        }
    }

    // Which tab this request means, resolved once. A caller that names a tab
    // that is not there gets told so — falling back to the active tab would
    // send clicks to the wrong page and look like the page was wrong.
    QWebEngineView *renderView = nullptr;
    // Normalised to the minted id, because the input helpers below look a tab
    // up by id. Passing a name straight through would find the right view for
    // a screenshot and no view at all for a click.
    QString renderTabId;
    if (path.startsWith(QStringLiteral("/render"))) {
        QString badId;
        renderView = resolveRenderTab(query, &badId);
        if (!badId.isEmpty()) {
            sendResponse(socket, 404, "Not Found",
                         QByteArray("{\"error\":\"no tab ") + badId.toUtf8() + "\"}");
            return;
        }
        const QString asked = query.queryItemValue(QStringLiteral("tab"));
        if (!asked.isEmpty() && m_browser)
            renderTabId = m_browser->resolveTab(asked);
    }

    // Redirect /render/ (trailing slash) to /render, preserving query string.
    if (method == QLatin1String("GET") && path == QLatin1String("/render/")) {
        QString location = QStringLiteral("/render");
        QString origQuery = url.query();
        if (!origQuery.isEmpty())
            location += QStringLiteral("?") + origQuery;
        QByteArray response =
            "HTTP/1.1 301 Moved Permanently\r\n"
            "Location: " + location.toUtf8() + "\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        socket->write(response);
        closeWhenSent(socket);
        return;
    }

    // Route CDP discovery paths to the internal Chromium debugging port.
    // Normalize trailing slash: Playwright requests /json/version/ with a slash.
    if (path.endsWith('/') && path.size() > 1)
        path.chop(1);
    bool isDiscovery = (path == QLatin1String("/json")
                        || path == QLatin1String("/json/list")
                        || path == QLatin1String("/json/version"));

    if (method == QLatin1String("GET") && isDiscovery) {
        QNetworkAccessManager nam;
        QUrl targetUrl(QString("http://127.0.0.1:%1%2").arg(m_debugPort).arg(path));
        QNetworkReply *reply = nam.get(QNetworkRequest(targetUrl));

        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(5000);
        loop.exec();

        QByteArray body;
        int statusCode = 200;
        if (reply->error() == QNetworkReply::NoError) {
            body = reply->readAll();
            // Strip port from Host header to get bare hostname.
            QString hostName = hostHeader;
            int colonIdx = hostName.lastIndexOf(':');
            if (colonIdx != -1) {
                bool ok = false;
                hostName.mid(colonIdx + 1).toUShort(&ok);
                if (ok)
                    hostName = hostName.left(colonIdx);
            }
            // Rewrite "127.0.0.1:<debugPort>" to "hostname:<proxyPort>" first so
            // that webSocketDebuggerUrl points at the CDP proxy (port+2) rather than
            // the raw Chromium DevTools port (port+1).
            body.replace(
                QByteArrayLiteral("127.0.0.1:") + QByteArray::number(m_debugPort),
                hostName.toUtf8() + ":" + QByteArray::number(m_proxyPort)
            );
            // Rewrite any remaining bare 127.0.0.1 references.
            body.replace(QByteArrayLiteral("127.0.0.1"), hostName.toUtf8());

            // The target list is rebuilt from the registry rather than
            // byte-patched, because a tab id is ours and appears nowhere in
            // Chromium's answer. /json/version is left alone: its
            // webSocketDebuggerUrl is the browser endpoint, not a page.
            if (path != QLatin1String("/json/version"))
                body = rebuildTargetList(body, hostName);
        } else {
            body = R"({"error":"upstream unavailable"})";
            statusCode = 503;
        }
        reply->deleteLater();

        QByteArray statusText = (statusCode == 200) ? "OK" : "Service Unavailable";
        sendResponse(socket, statusCode, statusText, body);
    } else if (method == QLatin1String("GET")
               && path == QLatin1String("/render/screenshot.png")) {
        QByteArray pngBytes;
        bool ok = false;
        if (m_browser) {
            QPixmap pixmap = renderView->grab();
            if (!pixmap.isNull()) {
                QBuffer buf(&pngBytes);
                buf.open(QIODevice::WriteOnly);
                ok = pixmap.save(&buf, "PNG");
            }
        }
        if (!ok) {
            sendResponse(socket, 503, "Service Unavailable", "capture failed", "text/plain");
        } else {
            QByteArray response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/png\r\n"
                "Cache-Control: no-cache\r\n"
                "X-Anoa-Viewport-Width: " + QByteArray::number(renderView->width()) + "\r\n"
                "X-Anoa-Viewport-Height: " + QByteArray::number(renderView->height()) + "\r\n"
                "Content-Length: " + QByteArray::number(pngBytes.size()) + "\r\n"
                "Connection: close\r\n"
                "\r\n";
            response += pngBytes;
            socket->write(response);
            closeWhenSent(socket);
        }
    } else if (method == QLatin1String("GET") && path == QLatin1String("/render/html")) {
        auto state = std::make_shared<HtmlCaptureState>();

        if (renderView && renderView->page()) {
            QEventLoop loop;
            QTimer timer;
            state->loop = &loop;
            timer.setSingleShot(true);
            connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            timer.start(5000);
            renderView->page()->toHtml([state](const QString &html) {
                if (!state->waiting)
                    return;
                state->html = html;
                state->timedOut = false;
                if (state->loop)
                    state->loop->quit();
            });
            loop.exec();
            state->waiting = false;
            state->loop = nullptr;
        }

        if (state->timedOut) {
            sendResponse(socket, 504, "Gateway Timeout", "html capture timeout", "text/plain");
        } else {
            QByteArray htmlBytes = state->html.toUtf8();
            QByteArray response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Cache-Control: no-cache\r\n"
                "Content-Length: " + QByteArray::number(htmlBytes.size()) + "\r\n"
                "Connection: close\r\n"
                "\r\n";
            response += htmlBytes;
            socket->write(response);
            closeWhenSent(socket);
        }
    } else if (method == QLatin1String("POST") && path == QLatin1String("/render/navigate")) {
        // Prefer url from query string; fall back to plain-text request body.
        QString navUrl = query.queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded);
        if (navUrl.isEmpty()) {
            QByteArray bodyBytes = requestData.mid(headerEnd + 4);
            bool lengthOk = false;
            int contentLength = headers.value(QStringLiteral("content-length")).toInt(&lengthOk);
            if (lengthOk && contentLength > bodyBytes.size()) {
                while (bodyBytes.size() < contentLength) {
                    if (!socket->waitForReadyRead(5000))
                        break;
                    bodyBytes += socket->readAll();
                }
            }
            navUrl = QString::fromUtf8(bodyBytes.trimmed());
        }


        QUrl parsedUrl(navUrl);
        if (navUrl.isEmpty() || !parsedUrl.isValid() || parsedUrl.isRelative()) {
            sendResponse(socket, 400, "Bad Request", "invalid url", "text/plain");
            return;
        }

        QString scheme = parsedUrl.scheme().toLower();
        if (scheme != QLatin1String("http") && scheme != QLatin1String("https")
            && scheme != QLatin1String("file")) {
            sendResponse(socket, 400, "Bad Request", "scheme not allowed", "text/plain");
            return;
        }

        if (m_browser)
            renderView->load(parsedUrl);

        sendResponse(socket, 200, "OK", "navigating", "text/plain");
    } else if (method == QLatin1String("POST")
               && (path == QLatin1String("/render/back")
                   || path == QLatin1String("/render/forward")
                   || path == QLatin1String("/render/reload"))) {
        // History control. Deliberately not one endpoint with a direction
        // parameter: these are three verbs a caller either has or does not,
        // and a typo in a parameter would silently do the wrong one.
        //
        // back/forward at the end of history are no-ops in Chromium, so there
        // is nothing to report but acceptance — answering 200 for a request
        // that changed nothing is the honest description of what happened.
        if (m_browser) {
            if (path.endsWith(QLatin1String("back")))
                renderView->back();
            else if (path.endsWith(QLatin1String("forward")))
                renderView->forward();
            else
                renderView->reload();
        }
        sendResponse(socket, 200, "OK", "ok", "text/plain");
    } else if (method == QLatin1String("GET") && path == QLatin1String("/render")) {
        // The page used to be a two-line poller built here as a format string.
        // It is now a real viewer — tabs, a URL bar and every input event —
        // which is more markup than belongs in a C++ literal, so it lives in
        // the resource bundle and is served verbatim. The token and the tab
        // travel in the query string the client already has, so nothing has to
        // be substituted on the way out.
        QFile page(QStringLiteral(":/viewer.html"));
        if (!page.open(QIODevice::ReadOnly)) {
            sendResponse(socket, 500, "Internal Server Error", "viewer missing", "text/plain");
            return;
        }
        const QByteArray htmlBytes = page.readAll();
        QByteArray response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Cache-Control: no-cache\r\n";
        response += frameAncestorsHeader();
        response +=
            "Content-Length: " + QByteArray::number(htmlBytes.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n";
        response += htmlBytes;
        socket->write(response);
        closeWhenSent(socket);
    } else if (method == QLatin1String("GET") && path == QLatin1String("/render/downloads")) {
        // Browser state, not page state, so it cannot come back through CDP's
        // Runtime.evaluate like most of what the agent CLI reads.
        if (!m_browser) {
            sendResponse(socket, 503, "Service Unavailable", R"({"error":"no browser"})");
            return;
        }
        sendResponse(socket, 200, "OK",
                     QJsonDocument(m_browser->downloadsJson()).toJson(QJsonDocument::Compact));
    } else if (method == QLatin1String("GET") && path == QLatin1String("/render/viewport")) {
        // The logical size of the tab, which is the coordinate space every
        // input endpoint speaks. A client cannot infer it from the frame:
        // grab() returns device pixels, so on a HiDPI display the image is
        // twice as wide as the coordinates that drive it.
        if (!renderView) {
            sendResponse(socket, 503, "Service Unavailable", R"({"error":"no browser"})");
            return;
        }
        QJsonObject obj;
        obj[QStringLiteral("width")] = renderView->width();
        obj[QStringLiteral("height")] = renderView->height();
        obj[QStringLiteral("dpr")] = renderView->devicePixelRatioF();
        sendResponse(socket, 200, "OK", QJsonDocument(obj).toJson(QJsonDocument::Compact));
    } else if (method == QLatin1String("POST") && path == QLatin1String("/render/tab/new")) {
        if (!m_browser) {
            sendResponse(socket, 503, "Service Unavailable", R"({"error":"no browser"})");
            return;
        }
        const QString target = query.queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded);
        const QString wanted = query.queryItemValue(QStringLiteral("name"));
        const QString id = m_browser->newTab(target.isEmpty() ? QUrl() : QUrl(target),
                                             QString(), false, wanted);
        if (id.isEmpty()) {
            sendResponse(socket, 400, "Bad Request", R"({"error":"could not open tab"})");
            return;
        }
        QJsonObject obj;
        obj[QStringLiteral("id")] = id;
        sendResponse(socket, 200, "OK", QJsonDocument(obj).toJson(QJsonDocument::Compact));
    } else if (method == QLatin1String("POST") && path == QLatin1String("/render/tab/close")) {
        // The tab was already resolved and 404'd above if it does not exist, so
        // reaching here with an empty id means no ?tab= was given at all.
        if (!m_browser || renderTabId.isEmpty()) {
            sendResponse(socket, 400, "Bad Request", R"({"error":"tab required"})");
            return;
        }
        // Refuses the last tab, by the same rule the CLI follows: one process
        // still means at least one page.
        if (!m_browser->closeTab(renderTabId)) {
            sendResponse(socket, 409, "Conflict", R"({"error":"cannot close the last tab"})");
            return;
        }
        sendResponse(socket, 200, "OK", R"({"closed":true})");
    } else if (method == QLatin1String("GET")
               && path == QLatin1String("/render/screenshot.ppm")) {
        // Binary PPM (P6) frame, optionally scaled server-side via ?w=&h=.
        // Terminal clients parse PPM with ~20 lines of code — no image decoder
        // needed. X-Anoa-Viewport-* headers carry the logical widget size so
        // clients can map image pixels back to click coordinates.
        QByteArray ppmBytes;
        int viewportW = 0, viewportH = 0;
        if (m_browser) {
            QPixmap pixmap = renderView->grab();
            if (!pixmap.isNull()) {
                viewportW = renderView->width();
                viewportH = renderView->height();
                QImage img = pixmap.toImage();
                int reqW = query.queryItemValue(QStringLiteral("w")).toInt();
                int reqH = query.queryItemValue(QStringLiteral("h")).toInt();
                if (reqW > 0 && reqH > 0)
                    img = img.scaled(reqW, reqH, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
                img = img.convertToFormat(QImage::Format_RGB888);
                ppmBytes = "P6\n" + QByteArray::number(img.width()) + " "
                           + QByteArray::number(img.height()) + "\n255\n";
                // Copy width*3 bytes per row — scanlines are 4-byte aligned so
                // bytesPerLine() may include padding that must not be emitted.
                for (int y = 0; y < img.height(); ++y)
                    ppmBytes.append(reinterpret_cast<const char *>(img.constScanLine(y)),
                                    img.width() * 3);
            }
        }
        if (ppmBytes.isEmpty()) {
            sendResponse(socket, 503, "Service Unavailable", "capture failed", "text/plain");
        } else {
            QByteArray response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/x-portable-pixmap\r\n"
                "Cache-Control: no-cache\r\n"
                "X-Anoa-Viewport-Width: " + QByteArray::number(viewportW) + "\r\n"
                "X-Anoa-Viewport-Height: " + QByteArray::number(viewportH) + "\r\n"
                "Content-Length: " + QByteArray::number(ppmBytes.size()) + "\r\n"
                "Connection: close\r\n"
                "\r\n";
            response += ppmBytes;
            socket->write(response);
            closeWhenSent(socket);
        }
    } else if (method == QLatin1String("POST") && path == QLatin1String("/render/click")) {
        bool okX = false, okY = false;
        int x = query.queryItemValue(QStringLiteral("x")).toInt(&okX);
        int y = query.queryItemValue(QStringLiteral("y")).toInt(&okY);
        if (!okX || !okY || x < 0 || y < 0) {
            sendResponse(socket, 400, "Bad Request", "invalid coordinates", "text/plain");
            return;
        }

        Qt::MouseButton button = Qt::LeftButton;
        if (!parseButton(query.queryItemValue(QStringLiteral("button")), &button)) {
            sendResponse(socket, 400, "Bad Request", "invalid button", "text/plain");
            return;
        }

        if (!m_browser) {
            sendResponse(socket, 503, "Service Unavailable", "no browser", "text/plain");
            return;
        }

        m_browser->sendClick(QPoint(x, y), button, renderTabId,
                             parseModifiers(query.queryItemValue(QStringLiteral("mods"))));
        sendResponse(socket, 200, "OK", "clicked", "text/plain");
    } else if (method == QLatin1String("POST")
               && (path == QLatin1String("/render/move")
                   || path == QLatin1String("/render/mousedown")
                   || path == QLatin1String("/render/mouseup"))) {
        // Press, move and release as separate events. A click endpoint can
        // drive a page from a script; it cannot hover a menu open or drag a
        // selection across one, because both of those are a state that spans
        // more than one event.
        bool okX = false, okY = false;
        const int x = query.queryItemValue(QStringLiteral("x")).toInt(&okX);
        const int y = query.queryItemValue(QStringLiteral("y")).toInt(&okY);
        if (!okX || !okY || x < 0 || y < 0) {
            sendResponse(socket, 400, "Bad Request", "invalid coordinates", "text/plain");
            return;
        }

        Qt::MouseButton button = Qt::LeftButton;
        if (!parseButton(query.queryItemValue(QStringLiteral("button")), &button)) {
            sendResponse(socket, 400, "Bad Request", "invalid button", "text/plain");
            return;
        }

        if (!m_browser) {
            sendResponse(socket, 503, "Service Unavailable", "no browser", "text/plain");
            return;
        }

        const Qt::KeyboardModifiers mods =
            parseModifiers(query.queryItemValue(QStringLiteral("mods")));
        const QPoint pos(x, y);

        if (path == QLatin1String("/render/mousedown")) {
            m_browser->sendMouseDown(pos, button, mods, renderTabId);
            sendResponse(socket, 200, "OK", "pressed", "text/plain");
        } else if (path == QLatin1String("/render/mouseup")) {
            m_browser->sendMouseUp(pos, button, mods, renderTabId);
            sendResponse(socket, 200, "OK", "released", "text/plain");
        } else {
            // `?buttons=` is what is still held during the move, which is what
            // separates a drag from a hover. Absent means nothing is down.
            Qt::MouseButtons held = Qt::NoButton;
            const QString heldSpec = query.queryItemValue(QStringLiteral("buttons"));
            const QStringList heldNames = heldSpec.split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QString &name : heldNames) {
                Qt::MouseButton b = Qt::LeftButton;
                if (parseButton(name.trimmed(), &b))
                    held |= b;
            }
            m_browser->sendMouseMove(pos, held, mods, renderTabId);
            sendResponse(socket, 200, "OK", "moved", "text/plain");
        }
    } else if (method == QLatin1String("POST") && path == QLatin1String("/render/scroll")) {
        bool okDy = false;
        int dy = query.queryItemValue(QStringLiteral("dy")).toInt(&okDy);
        if (!okDy || dy == 0) {
            sendResponse(socket, 400, "Bad Request", "invalid dy", "text/plain");
            return;
        }

        if (!m_browser) {
            sendResponse(socket, 503, "Service Unavailable", "no browser", "text/plain");
            return;
        }

        // x/y optional — default to the viewport center.
        bool okX = false, okY = false;
        int x = query.queryItemValue(QStringLiteral("x")).toInt(&okX);
        int y = query.queryItemValue(QStringLiteral("y")).toInt(&okY);
        QPoint pos(okX && x >= 0 ? x : renderView->width() / 2,
                   okY && y >= 0 ? y : renderView->height() / 2);

        m_browser->sendScroll(pos, dy, renderTabId);
        sendResponse(socket, 200, "OK", "scrolled", "text/plain");
    } else if (method == QLatin1String("POST") && path == QLatin1String("/render/type")) {
        // Prefer text from the query string; fall back to the request body
        // (same pattern as /render/navigate) for long payloads.
        QString text = query.queryItemValue(QStringLiteral("text"), QUrl::FullyDecoded);
        if (text.isEmpty()) {
            QByteArray bodyBytes = requestData.mid(headerEnd + 4);
            bool lengthOk = false;
            int contentLength = headers.value(QStringLiteral("content-length")).toInt(&lengthOk);
            if (lengthOk && contentLength > bodyBytes.size()) {
                while (bodyBytes.size() < contentLength) {
                    if (!socket->waitForReadyRead(5000))
                        break;
                    bodyBytes += socket->readAll();
                }
            }
            text = QString::fromUtf8(bodyBytes);
        }

        if (text.isEmpty()) {
            sendResponse(socket, 400, "Bad Request", "empty text", "text/plain");
            return;
        }
        if (!m_browser) {
            sendResponse(socket, 503, "Service Unavailable", "no browser", "text/plain");
            return;
        }

        m_browser->sendText(text, renderTabId);
        sendResponse(socket, 200, "OK", "typed", "text/plain");
    } else if (method == QLatin1String("POST") && path == QLatin1String("/render/key")) {
        const QString keyName = query.queryItemValue(QStringLiteral("key"));
        if (keyName.isEmpty()) {
            sendResponse(socket, 400, "Bad Request", "missing key", "text/plain");
            return;
        }
        if (!m_browser) {
            sendResponse(socket, 503, "Service Unavailable", "no browser", "text/plain");
            return;
        }

        if (!m_browser->sendKey(keyName, renderTabId,
                                parseModifiers(query.queryItemValue(QStringLiteral("mods"))))) {
            sendResponse(socket, 400, "Bad Request", "unknown key", "text/plain");
            return;
        }
        sendResponse(socket, 200, "OK", "key sent", "text/plain");
    } else if (method == QLatin1String("GET")
               && path == QLatin1String("/render/stream.mjpeg")) {
        // Send MJPEG stream headers — keep socket open, no Content-Length.
        QByteArray streamHeader =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        socket->write(streamHeader);
        socket->flush();

        // Timer fires every 100 ms (~10 fps). Parented to socket so it is
        // cleaned up automatically if socket is deleted before disconnected fires.
        QTimer *frameTimer = new QTimer(socket);
        frameTimer->setInterval(100);

        // The tab resolved for this request, not the container. Grabbing the
        // container gives whatever is raised, so `?tab=` was accepted and then
        // ignored: asking for a background tab streamed the active one, with
        // nothing anywhere to say so. Held as a QPointer because the stream
        // outlives the request and a tab can be closed while it runs.
        QPointer<QWebEngineView> streamView(renderView);
        connect(frameTimer, &QTimer::timeout, socket, [streamView, socket]() {
            // Skip this frame if the write buffer is backed up beyond 512 KB.
            if (socket->bytesToWrite() > 512 * 1024)
                return;
            if (!streamView)
                return;

            QPixmap pixmap = streamView->grab();
            if (pixmap.isNull())
                return;

            QByteArray jpegBytes;
            QBuffer buf(&jpegBytes);
            buf.open(QIODevice::WriteOnly);
            if (!pixmap.save(&buf, "JPEG", 70))
                return;
            buf.close();

            QByteArray part;
            part += "--frame\r\n";
            part += "Content-Type: image/jpeg\r\n";
            part += "Content-Length: " + QByteArray::number(jpegBytes.size()) + "\r\n";
            part += "\r\n";
            part += jpegBytes;
            part += "\r\n";

            socket->write(part);
            socket->flush();
        });

        connect(socket, &QTcpSocket::disconnected, socket, [socket, frameTimer]() {
            frameTimer->stop();
            socket->deleteLater();
        });

        frameTimer->start();
        // Do not close socket — stream runs until client disconnects.
    } else {
        sendResponse(socket, 404, "Not Found", R"({"error":"not found"})");
    }
}
