#include "cdp/cdp_extensions.h"
#include "cdp/tab_host.h"
#include "pdf/pdf_handler.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

static QString stubResult(const QJsonObject &cmd)
{
    QJsonObject resp;
    resp[QStringLiteral("id")] = cmd.value(QStringLiteral("id")).toInt();
    resp[QStringLiteral("result")] = QJsonObject();
    return QJsonDocument(resp).toJson(QJsonDocument::Compact);
}

QString CdpExtensions::processCommand(const QJsonObject &cmd, QWebEnginePage *page,
                                      TabHost *tabs, bool *deferred,
                                      const std::function<void(const QString &)> &sendLater)
{
    // Cleared up front so a caller never reads a stale value off its own stack.
    if (deferred)
        *deferred = false;

    const QString method = cmd.value(QStringLiteral("method")).toString();
    const int dotPos = method.indexOf(QLatin1Char('.'));
    if (dotPos < 0)
        return QString();

    const QString domain = method.left(dotPos);

    if (domain == QLatin1String("Profiler"))
        return handleProfiler(cmd);
    if (domain == QLatin1String("HeapProfiler"))
        return handleHeapProfiler(cmd);
    if (domain == QLatin1String("Security"))
        return handleSecurity(cmd, page);
    if (domain == QLatin1String("Browser"))
        return handleBrowser(cmd);
    if (domain == QLatin1String("Target"))
        return handleTarget(cmd, tabs, deferred, sendLater);
    // anoa's own domain. Downloads are browser state, so Runtime.evaluate — how
    // the agent CLI reads almost everything else — cannot see them. Dispatched
    // here beside the other domains rather than inside one of them, which is
    // where it does not get reached.
    if (domain == QLatin1String("Anoa")) {
        if (method == QLatin1String("Anoa.getDownloads")) {
            // Built here rather than through cdpResult(), which lives in the
            // anonymous namespace further down and is not visible yet.
            QJsonObject result;
            result[QStringLiteral("downloads")] = tabs ? tabs->downloadsJson() : QJsonArray();
            QJsonObject resp;
            resp[QStringLiteral("id")] = cmd.value(QStringLiteral("id")).toInt();
            resp[QStringLiteral("result")] = result;
            return QJsonDocument(resp).toJson(QJsonDocument::Compact);
        }
        return QString(); // unknown Anoa.* falls through and is reported as such
    }
    if (method == QLatin1String("Page.printToPDF")) {
        // Only use Qt's PdfHandler when we have a page reference.
        // Without a page (e.g. in the proxy path), pass through to Chromium which
        // handles Page.printToPDF natively (Chrome 96+).
        if (page) {
            PdfHandler handler(page);
            return handler.handlePrintToPdf(cmd);
        }
        return QString(); // pass through to Chromium
    }

    return QString();
}

QString CdpExtensions::handleProfiler(const QJsonObject &cmd)
{
    // All Profiler commands are stubbed — Qt has no direct V8 profiler API.
    return stubResult(cmd);
}

QString CdpExtensions::handleHeapProfiler(const QJsonObject &cmd)
{
    // All HeapProfiler commands are stubbed — no Qt API maps to these.
    return stubResult(cmd);
}

QString CdpExtensions::handleSecurity(const QJsonObject &cmd, QWebEnginePage *page)
{
    Q_UNUSED(page)
    // No direct QWebEngineProfile API for certificate error ignoring; return stub.
    return stubResult(cmd);
}

QJsonObject CdpExtensions::rewritePassthrough(const QJsonObject &cmd)
{
    // Chromium knows none of the browser context ids anoa mints, so any command
    // still carrying one would be rejected outright. createTarget is answered
    // locally and never reaches here, but Storage and Target commands can carry
    // the same field, so the strip is by field rather than by method.
    const QJsonObject params = cmd.value(QStringLiteral("params")).toObject();
    const QString ctxId = params.value(QStringLiteral("browserContextId")).toString();
    if (ctxId == QLatin1String("__anoa_default__")
        || ctxId.startsWith(QLatin1String("anoa-ctx-"))) {
        QJsonObject modified = cmd;
        QJsonObject p = params;
        p.remove(QStringLiteral("browserContextId"));
        modified[QStringLiteral("params")] = p;
        return modified;
    }
    return QJsonObject(); // no rewrite needed
}

QString CdpExtensions::handleBrowser(const QJsonObject &cmd)
{
    // Only stub Browser commands that QtWebEngine Chromium rejects with
    // "Browser context management is not supported". Pass everything else
    // (e.g. Browser.getVersion) through to Chromium.
    const QString method = cmd.value(QStringLiteral("method")).toString();
    if (method == QLatin1String("Browser.close")) {
        // Chromium answers Browser.close with an empty success and then keeps
        // running: the browser it would close is one Qt owns and it has no
        // handle on it. So `anoa close` exited 0 while the process and its
        // renderers stayed alive — the failure mode this codebase specialises
        // in, an endpoint reporting success without acting.
        //
        // Quit on a later turn of the event loop, not now: this reply still has
        // to reach the socket. Unwinding main() from there is what destroys the
        // profile, which is what flushes cookies — the same path SIGTERM takes.
        QTimer::singleShot(150, qApp, &QCoreApplication::quit);
        return stubResult(cmd);
    }
    if (method == QLatin1String("Browser.setDownloadBehavior")
            || method == QLatin1String("Browser.getWindowForTarget")
            || method == QLatin1String("Browser.setWindowBounds")
            || method == QLatin1String("Browser.grantPermissions")
            || method == QLatin1String("Browser.resetPermissions")) {
        return stubResult(cmd);
    }
    return QString(); // pass through
}

namespace {

// { "id": <n>, "result": <result> }
QString cdpResult(const QJsonObject &cmd, const QJsonObject &result)
{
    QJsonObject resp;
    resp[QStringLiteral("id")] = cmd.value(QStringLiteral("id")).toInt();
    resp[QStringLiteral("result")] = result;
    return QJsonDocument(resp).toJson(QJsonDocument::Compact);
}

// { "id": <n>, "error": { "code": -32000, "message": ... } }
QString cdpError(const QJsonObject &cmd, const QString &message)
{
    QJsonObject error;
    error[QStringLiteral("code")] = -32000;
    error[QStringLiteral("message")] = message;
    QJsonObject resp;
    resp[QStringLiteral("id")] = cmd.value(QStringLiteral("id")).toInt();
    resp[QStringLiteral("error")] = error;
    return QJsonDocument(resp).toJson(QJsonDocument::Compact);
}

QJsonObject targetInfoFor(TabHost *tabs, const QString &tabId)
{
    QJsonObject info;
    info[QStringLiteral("targetId")] = tabs->targetIdFor(tabId);
    info[QStringLiteral("type")] = QStringLiteral("page");
    info[QStringLiteral("title")] = tabs->titleFor(tabId);
    info[QStringLiteral("url")] = tabs->urlFor(tabId);
    info[QStringLiteral("attached")] = (tabId == tabs->activeTabId());
    info[QStringLiteral("canAccessOpener")] = false;
    info[QStringLiteral("browserContextId")] = tabs->browserContextIdFor(tabId);
    // anoa's own id, alongside the engine's. A client that does not know the
    // field ignores it; `anoa tab` is the reason it is here, because the id a
    // user types is not the id Chromium mints.
    info[QStringLiteral("anoaTabId")] = tabId;
    const QString name = tabs->nameFor(tabId);
    if (!name.isEmpty())
        info[QStringLiteral("anoaTabName")] = name;
    return info;
}

} // namespace

QString CdpExtensions::handleTarget(const QJsonObject &cmd, TabHost *tabs, bool *deferred,
                                    const std::function<void(const QString &)> &sendLater)
{
    const QString method = cmd.value(QStringLiteral("method")).toString();
    const QJsonObject params = cmd.value(QStringLiteral("params")).toObject();

    // QtWebEngine Chromium does not support multiple browser contexts
    // (incognito). A synthetic id lets clients like Playwright proceed.
    if (method == QLatin1String("Target.createBrowserContext")) {
        QJsonObject result;
        result[QStringLiteral("browserContextId")] = QStringLiteral("__anoa_default__");
        return cdpResult(cmd, result);
    }
    if (method == QLatin1String("Target.disposeBrowserContext"))
        return stubResult(cmd);

    // Everything below answers from the registry. Without one there is nothing
    // to answer from, so it passes upstream exactly as it did before.
    if (!tabs)
        return QString();

    if (method == QLatin1String("Target.getTargets")) {
        QJsonArray infos;
        for (const QString &tabId : tabs->tabIds()) {
            if (tabs->targetIdFor(tabId).isEmpty())
                continue; // not attachable yet, the same rule /json/list follows
            infos.append(targetInfoFor(tabs, tabId));
        }
        QJsonObject result;
        result[QStringLiteral("targetInfos")] = infos;
        return cdpResult(cmd, result);
    }

    if (method == QLatin1String("Target.getTargetInfo")) {
        const QString tabId =
            tabs->tabIdForTargetId(params.value(QStringLiteral("targetId")).toString());
        // An id we do not know is not an error: Chromium owns targets we never
        // registered — the browser target itself, service workers, a popup
        // opened by window.open — and it is authoritative for those.
        //
        // Answering "No target with given id" here broke connectOverCDP
        // outright: Playwright asks about the browser target during handshake,
        // and a client cannot attach to a browser that denies its own existence.
        if (tabId.isEmpty())
            return QString(); // pass through
        QJsonObject result;
        result[QStringLiteral("targetInfo")] = targetInfoFor(tabs, tabId);
        return cdpResult(cmd, result);
    }

    if (method == QLatin1String("Target.activateTarget")) {
        const QString tabId =
            tabs->tabIdForTargetId(params.value(QStringLiteral("targetId")).toString());
        if (tabId.isEmpty() || !tabs->selectTab(tabId))
            return cdpError(cmd, QStringLiteral("No target with given id"));
        return cdpResult(cmd, QJsonObject());
    }

    if (method == QLatin1String("Target.closeTarget")) {
        const QString tabId =
            tabs->tabIdForTargetId(params.value(QStringLiteral("targetId")).toString());
        if (tabId.isEmpty())
            return cdpError(cmd, QStringLiteral("No target with given id"));
        // Refusing the last tab is not an error: the registry declines, and the
        // client is told plainly that the close did not happen.
        QJsonObject result;
        result[QStringLiteral("success")] = tabs->closeTab(tabId);
        return cdpResult(cmd, result);
    }

    if (method == QLatin1String("Target.createTarget")) {
        // The one command that cannot answer in this turn: the page exists
        // before its DevTools target does. Nothing goes upstream, and the reply
        // arrives through sendLater once the id lands.
        if (!deferred || !sendLater)
            return QString();

        const QUrl url(params.value(QStringLiteral("url")).toString());
        // anoa-only parameters, sent by our own CLI. A client that does not know
        // them gets the shared profile, which is what every client expects.
        const QString profileName = params.value(QStringLiteral("anoaProfile")).toString();
        const bool isolated = params.value(QStringLiteral("anoaIsolated")).toBool();
        const QString wantName = params.value(QStringLiteral("anoaName")).toString();

        // A context we minted means "open it beside the tabs already there";
        // one we never issued is a mistake worth saying out loud, rather than
        // quietly opening the tab in the default profile instead.
        const QString contextId = params.value(QStringLiteral("browserContextId")).toString();
        QString tabId;
        if (!contextId.isEmpty()) {
            if (!tabs->knowsBrowserContext(contextId))
                return cdpError(cmd, QStringLiteral("Failed to find browser context with id ")
                                         + contextId);
            tabId = tabs->newTabInBrowserContext(url, contextId);
        } else {
            tabId = tabs->newTab(url, profileName, isolated, wantName);
        }
        if (tabId.isEmpty()) {
            // The one failure worth naming: a name already in use is a caller
            // mistake with an obvious fix, not a browser that would not open a
            // tab.
            if (!wantName.isEmpty() && !tabs->resolveTab(wantName).isEmpty())
                return cdpError(cmd, QStringLiteral("Tab name already in use: ") + wantName);
            return cdpError(cmd, QStringLiteral("Could not create target"));
        }

        *deferred = true;
        const QJsonObject request = cmd;
        tabs->whenTargetResolved(tabId, [request, sendLater](const QString &targetId) {
            if (targetId.isEmpty()) {
                sendLater(cdpError(request, QStringLiteral("Target did not register")));
                return;
            }
            QJsonObject result;
            result[QStringLiteral("targetId")] = targetId;
            sendLater(cdpResult(request, result));
        });
        return QString();
    }

    return QString(); // pass through
}
