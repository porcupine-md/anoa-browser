#pragma once

#include <functional>

#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <QUrl>

// What the Target domain needs from whoever owns the tabs.
//
// A pure interface rather than an AnoaBrowser include: src/cdp knows nothing
// about src/browser, and everything on the other side of this seam is a
// QWebEngine type the CDP layer has no business naming.
class TabHost
{
public:
    virtual ~TabHost() = default;

    // `name` is optional and, when given, becomes an alias for the minted id:
    // both resolve to the same tab, and an agent can use whichever it finds
    // easier to keep track of. Returns empty if the name is already taken.
    virtual QString newTab(const QUrl &url, const QString &profileName, bool isolated,
                           const QString &name = QString()) = 0;
    virtual bool closeTab(const QString &tabId) = 0;
    virtual bool selectTab(const QString &tabId) = 0;

    virtual QStringList tabIds() const = 0;
    virtual QString activeTabId() const = 0;

    // Our id and the engine's, which are not the same thing and must not be
    // confused: a Chromium target id changes when a page is recreated.
    virtual QString targetIdFor(const QString &tabId) const = 0;
    virtual QString tabIdForTargetId(const QString &targetId) const = 0;

    // Downloads this browser has accepted, newest last, as the JSON both the
    // HTTP endpoint and the CDP extension hand back. JSON rather than a struct
    // so this header keeps naming nothing from WebEngine.
    virtual QJsonArray downloadsJson() const = 0;

    // An id or a name to the id it means, or empty for neither.
    virtual QString resolveTab(const QString &idOrName) const = 0;
    virtual QString nameFor(const QString &tabId) const = 0;

    virtual QString titleFor(const QString &tabId) const = 0;
    virtual QString urlFor(const QString &tabId) const = 0;
    virtual QString browserContextIdFor(const QString &tabId) const = 0;
    // A context id this registry minted, and a tab created inside it. Returns
    // empty for an id we never issued, so the caller can say so rather than
    // quietly opening the tab somewhere else.
    virtual bool knowsBrowserContext(const QString &contextId) const = 0;
    virtual QString newTabInBrowserContext(const QUrl &url, const QString &contextId) = 0;

    // Calls back once the tab has a Chromium target id — immediately if it
    // already has one. This is what makes Target.createTarget answerable at
    // all: the page exists before its target does.
    virtual void whenTargetResolved(const QString &tabId,
                                    std::function<void(const QString &targetId)> cb) = 0;

    // ── what the Browser domain needs ───────────────────────────────────────
    // These exist because the alternative was worse. Each of the commands
    // below used to be answered with an empty success and no action, which is
    // the same lie `anoa close` was telling before #30: a Playwright script
    // granting geolocation or setting a download directory got `{}` back and
    // nothing happened.

    // `behavior` is CDP's: "allow", "deny", "default" (and "allowAndName",
    // treated as allow). An empty path leaves the directory alone.
    virtual bool setDownloadBehavior(const QString &behavior, const QString &path) = 0;

    // CDP permission names. Anything this engine cannot express is returned in
    // `unsupported` rather than quietly dropped, so the caller is told which
    // of the permissions it asked for did not happen.
    virtual void grantPermissions(const QUrl &origin, const QStringList &permissions,
                                  QStringList *unsupported) = 0;
    // False where the engine cannot enumerate what was granted, which is
    // every Qt before 6.8 — a success there would be a fresh instance of the
    // lie this interface exists to remove.
    virtual bool resetPermissions() = 0;

    // Logical pixels, and the whole window rather than a tab: Chromium has one
    // window per browser here, so every target reports the same bounds.
    virtual QJsonObject windowBounds() const = 0;
    virtual bool setWindowBounds(int width, int height) = 0;
};
