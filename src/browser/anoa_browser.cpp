#include "anoa_browser.h"

#include <functional>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QWebEngineCookieStore>
#include <QWebEngineDownloadRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineView>

namespace {

// QWebEnginePage's default javaScriptConsoleMessage() writes every console
// message the page produces to stderr. In terminal mode stderr *is* the screen
// the viewer is painting: a single "js: Unrecognized feature: 'web-share'." from
// an ordinary site scrolls the alt screen, which pushes the status bar up and
// leaves a copy of it stranded mid-page. Browser mode keeps the messages, where
// they are a debugging aid and land on a terminal nobody is drawing into.
class AnoaPage : public QWebEnginePage
{
public:
    AnoaPage(bool silenceConsole, QWebEngineProfile *profile, QObject *parent)
        : QWebEnginePage(profile, parent)
        , m_silenceConsole(silenceConsole)
    {
    }

    // What the page asked and what it was told, newest last. An agent cannot
    // see a dialog it never gets to answer, so the answer is recorded instead.
    struct DialogRecord {
        QString kind; // alert | confirm | prompt | beforeunload
        QString message;
        QString answer;
    };
    QList<DialogRecord> takeDialogs()
    {
        QList<DialogRecord> out;
        out.swap(m_dialogs);
        return out;
    }
    void setConfirmAnswer(bool accept) { m_confirmAnswer = accept; }
    void setPromptAnswer(const QString &text) { m_promptAnswer = text; }

    // How this page opens another one. Set by the registry, because a page
    // cannot make a tab on its own and src/browser is the only place that
    // knows how.
    void setTabOpener(std::function<QWebEnginePage *()> opener)
    {
        m_openTab = std::move(opener);
    }
    // Files handed to the next <input type=file> that asks. Armed ahead of the
    // click, because the click is what triggers the request and there is
    // nobody to answer a file dialog.
    void armFiles(const QStringList &paths) { m_armedFiles = paths; }

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString &message,
                                  int lineNumber, const QString &sourceId) override
    {
        if (m_silenceConsole)
            return;
        QWebEnginePage::javaScriptConsoleMessage(level, message, lineNumber, sourceId);
    }

    // The default implementations put a modal dialog on screen and block the
    // renderer until somebody clicks it. Nobody ever does here: headless has no
    // screen, and the agent driving the browser is on the other side of a
    // socket the renderer has stopped serving.
    //
    // One alert() therefore killed the browser outright — not the tab, the
    // whole process. Every later command, down to Target.getTargets at the
    // browser level, timed out, and the only way back was SIGKILL. Any ordinary
    // site that pops a confirm did it.
    //
    // So they answer immediately and record what happened, which is also what
    // an automated browser is expected to do: Chrome's own headless mode
    // dismisses dialogs unless a client has subscribed to Page.javascriptDialog.
    void javaScriptAlert(const QUrl &securityOrigin, const QString &msg) override
    {
        Q_UNUSED(securityOrigin)
        m_dialogs.append({QStringLiteral("alert"), msg, QString()});
    }

    bool javaScriptConfirm(const QUrl &securityOrigin, const QString &msg) override
    {
        Q_UNUSED(securityOrigin)
        m_dialogs.append({QStringLiteral("confirm"), msg,
                          m_confirmAnswer ? QStringLiteral("true") : QStringLiteral("false")});
        return m_confirmAnswer;
    }

    bool javaScriptPrompt(const QUrl &securityOrigin, const QString &msg,
                          const QString &defaultValue, QString *result) override
    {
        Q_UNUSED(securityOrigin)
        // The page's own default unless one was set, so a prompt that offers a
        // sensible value gets it rather than an empty string.
        const QString answer = m_promptAnswer.isNull() ? defaultValue : m_promptAnswer;
        if (result)
            *result = answer;
        m_dialogs.append({QStringLiteral("prompt"), msg, answer});
        return true;
    }

    // window.open and target=_blank. Without this, Qt drops the request and
    // returns nothing — the page sees a null window, or worse, sees a truthy
    // one and waits for a document that never arrives. Now it becomes a real
    // background tab, which is what a browser with tabs should do with it.
    QWebEnginePage *createWindow(WebWindowType type) override
    {
        Q_UNUSED(type)
        if (!m_openTab)
            return nullptr;
        return m_openTab();
    }

    // A file input asked for files. The default puts a native dialog on screen
    // — invisible in headless, and unanswerable by an agent either way, so the
    // upload simply never happened.
    QStringList chooseFiles(FileSelectionMode mode, const QStringList &oldFiles,
                            const QStringList &acceptedMimeTypes) override
    {
        Q_UNUSED(oldFiles)
        Q_UNUSED(acceptedMimeTypes)
        if (m_armedFiles.isEmpty())
            return QStringList();
        // One arming, one use: leaving them set would attach the same file to
        // every later upload on the page.
        QStringList files;
        files.swap(m_armedFiles);
        if (mode == FileSelectOpen && files.size() > 1)
            files = QStringList{files.first()};
        return files;
    }

private:
    bool m_silenceConsole;
    std::function<QWebEnginePage *()> m_openTab;
    QStringList m_armedFiles;
    QList<DialogRecord> m_dialogs;
    bool m_confirmAnswer = true;   // accept: the common intent when driving
    QString m_promptAnswer;        // null = use the page's default
};

} // namespace

AnoaBrowser::AnoaBrowser(const Config &config, QWidget *parent)
    : QWidget(parent)
    , m_config(config)
    , m_profile(nullptr)
    , m_nam(new QNetworkAccessManager(this))
{
    // QTWEBENGINE_CHROMIUM_FLAGS must be set before WebEngine initializes its
    // profile/page. Setting it here, before creating QWebEngineProfile and any
    // view, ensures Chromium picks up the remote-debugging port.
    // Chromium DevTools runs on port+1; our HTTP/WS proxy layer listens on port.
    QByteArray flags;
    if (m_config.termEmbedded) {
        // The embedded viewer reaches this browser through a pointer, so there
        // is nothing for a debugging port to serve — and opening one would both
        // collide with the anoa the user may already be running on the
        // default port and print "DevTools listening on ..." onto the alt
        // screen the viewer is drawing. Chromium's own logging goes for the
        // same reason: stderr is the terminal the UI owns.
        flags = "--disable-logging --log-level=3";
    } else {
        flags = "--remote-debugging-port=" + QByteArray::number(m_config.port + 1);
        // Chromium 111+ rejects DevTools WebSocket connections whose Origin
        // header is not allowlisted. Remote CDP clients (tunnels, reverse
        // proxies, browser frontends) connect from arbitrary origins, so allow
        // all; access control is enforced by our own auth-token proxy layer.
        flags += " --remote-allow-origins=*";
    }
    if (m_config.noSandbox)
        flags += " --no-sandbox";
    if (m_config.headless)
        flags += " --disable-gpu";

    // Chromium gives every tab its own renderer process, and that is where the
    // memory goes: nine tabs measured at 1031 MB of renderers against 117 MB
    // for one. Capping the count makes tabs share processes, which is a trade,
    // not a free win — the same nine tabs doing work at once took 469 ms with a
    // process each and 1061 ms sharing four, because a shared process has one
    // main thread. Off by default; the caller decides where on that curve they
    // want to sit.
    if (m_config.maxRenderers > 0)
        flags += " --renderer-process-limit=" + QByteArray::number(m_config.maxRenderers);

    // Keep whatever the caller already put there.
    //
    // This used to be a plain qputenv, which silently discarded it — including
    // the QTWEBENGINE_CHROMIUM_FLAGS our own Dockerfile sets and our own docs
    // tell people to set. So the documented escape hatch for a machine whose GL
    // stack cannot satisfy Chromium ("Could not initialize GLX") did nothing at
    // all, and neither did the container's --disable-gpu.
    //
    // Ours go first so the caller's win on any flag Chromium resolves
    // last-wins; theirs are the deliberate override.
    const QByteArray inherited = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS").trimmed();
    if (!inherited.isEmpty())
        flags += " " + inherited;
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);

    if (m_config.headless)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    // A persistent profile, not Qt's default one.
    //
    // QWebEngineProfile::defaultProfile() is OFF-THE-RECORD: it keeps nothing.
    // So `anoa` with no --profile logged you into a site, and logged you out
    // again the moment the process ended — with no error and nothing on disk to
    // suggest why. A browser you drive across separate commands is exactly the
    // case where that is wrong.
    //
    // Named "default" and stored where the platform keeps application data, so
    // it behaves like any other browser profile. --profile picks a different
    // one; --ephemeral asks for the old off-the-record behaviour back.
    if (m_config.ephemeral) {
        m_profile = QWebEngineProfile::defaultProfile();
    } else {
        const QString base = m_config.profileDir.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            : m_config.profileDir;
        m_profile = new QWebEngineProfile(QStringLiteral("default"), this);
        m_profile->setPersistentStoragePath(QDir(base).filePath(QStringLiteral("default")));
        m_profile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
    }

    // No layout at all. Views are children at identical geometry and the
    // active one is raised — see the header for why hiding them is not an
    // option.
}

AnoaBrowser::~AnoaBrowser()
{
    // See the header: the views hold the pages, the pages hold the profile, and
    // Qt's own child ordering gets it backwards. deleteLater is no good here —
    // there is no event loop left to run it — so the views are deleted outright
    // while the profiles they name are still valid objects.
    for (Tab &tab : m_tabs) {
        delete tab.view;
        tab.view = nullptr;
    }
    m_tabs.clear();
}

void AnoaBrowser::acceptDownloadsOn(QWebEngineProfile *profile)
{
    if (!profile || m_downloadWired.contains(profile))
        return;
    m_downloadWired.insert(profile);
    // Qt cancels a download nobody accepts, silently. A page that offers a file
    // therefore did nothing at all, with no error anywhere to say why.
    connect(profile, &QWebEngineProfile::downloadRequested, this,
            [this](QWebEngineDownloadRequest *item) {
                if (!item)
                    return;
                const QString dir = m_config.downloadDir.isEmpty()
                    ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
                    : m_config.downloadDir;
                QDir().mkpath(dir);
                item->setDownloadDirectory(dir);
                item->accept();
                connect(item, &QWebEngineDownloadRequest::isFinishedChanged, this, [this, item]() {
                    if (!item->isFinished())
                        return;
                    emit downloadFinished(QDir(item->downloadDirectory())
                                              .filePath(item->downloadFileName()),
                                          item->state()
                                              == QWebEngineDownloadRequest::DownloadCompleted);
                });
            });
}


// ── grazing ────────────────────────────────────────────────────────────────
// Chromium keeps a renderer process per tab, and that is where the memory of
// many tabs goes: nine tabs measured at 1031 MB of renderers against 117 MB for
// one. A tab the agent has finished reading does not need one until it comes
// back, and Qt has a first-class way to say so.

void AnoaBrowser::startGrazing(const QString &tabId)
{
    if (m_config.grazeSeconds <= 0 || tabId.isEmpty())
        return;
    if (indexOf(tabId) < 0)
        return;
    // Never the active tab: it is on screen and its renderer is the one working.
    if (tabId == m_activeTabId)
        return;

    stopGrazing(tabId);
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(m_config.grazeSeconds * 1000);
    connect(timer, &QTimer::timeout, this, [this, tabId]() { discardTab(tabId); });
    m_grazeTimers.insert(tabId, timer);
    timer->start();
}

void AnoaBrowser::stopGrazing(const QString &tabId)
{
    if (QTimer *t = m_grazeTimers.take(tabId)) {
        t->stop();
        t->deleteLater();
    }
}

void AnoaBrowser::discardTab(const QString &tabId)
{
    stopGrazing(tabId);
    const int idx = indexOf(tabId);
    if (idx < 0 || tabId == m_activeTabId)
        return;
    QWebEngineView *view = m_tabs.at(idx).view;
    if (!view || !view->page())
        return;
    if (view->page()->lifecycleState() == QWebEnginePage::LifecycleState::Discarded)
        return;

    // Qt refuses both transitions while the page is visible — "failed to
    // transition from Active to Frozen state: page is visible" — and every tab
    // here is visible on purpose, because a hidden QWebEngineView takes no
    // input and that is what makes a background tab drivable.
    //
    // Hiding is therefore part of discarding, not a workaround: the invariant
    // holds because nothing reaches a sleeping tab without waking it first, and
    // waking shows it again.
    m_tabs[idx].sleepUrl = view->url();
    view->hide();
    // Frozen first: Qt will not go straight from Active to Discarded.
    view->page()->setLifecycleState(QWebEnginePage::LifecycleState::Frozen);
    view->page()->setLifecycleState(QWebEnginePage::LifecycleState::Discarded);
    // The cached engine target id is deliberately kept, even though the target
    // it names died with the renderer. It is what /json/list advertises and
    // what a client therefore dials, and dialling it is how the proxy finds
    // this tab in order to wake it. Clearing it here made the tab vanish from
    // discovery instead — unreachable rather than asleep.
}

void AnoaBrowser::wakeTab(const QString &tabId)
{
    if (tabId.isEmpty())
        return;
    const int idx = indexOf(tabId);
    if (idx < 0)
        return;
    stopGrazing(tabId);

    QWebEngineView *view = m_tabs.at(idx).view;
    QWebEnginePage *page = view ? view->page() : nullptr;
    if (!page) {
        startGrazing(tabId);
        return;
    }
    if (page->lifecycleState() == QWebEnginePage::LifecycleState::Active) {
        // Awake already: this call was a touch, so the clock starts over.
        startGrazing(tabId);
        return;
    }

    const bool wasDiscarded =
        page->lifecycleState() == QWebEnginePage::LifecycleState::Discarded;

    // Visible first, then Active — the mirror of how it went to sleep. Qt gates
    // the transitions on visibility in both directions, so asking for Active
    // while the view is hidden is refused and the page never comes back. Sized
    // as well as shown: the container may have been resized while this one was
    // away, and a view that returns at the wrong geometry maps every later
    // click to the wrong place.
    view->setGeometry(rect());
    view->show();
    page->setLifecycleState(QWebEnginePage::LifecycleState::Active);

    if (wasDiscarded) {
        // Qt documents a discarded page as reloading itself once active.
        // Headless it does not, and the page comes back blank — every command
        // then fails for a reason that looks nothing like the cause.
        const QUrl sleepUrl = m_tabs.at(idx).sleepUrl;
        if (sleepUrl.isValid() && !sleepUrl.isEmpty()
            && sleepUrl.toString() != QLatin1String("about:blank")) {
            QEventLoop loop;
            QTimer deadline;
            deadline.setSingleShot(true);
            connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
            QMetaObject::Connection c = connect(page, &QWebEnginePage::loadFinished,
                                                &loop, [&loop](bool) { loop.quit(); });
            view->load(sleepUrl);
            // Never forever: a page that will not come back must not take the
            // process with it.
            deadline.start(15000);
            loop.exec();
            disconnect(c);
        }
        // A new renderer means a new engine target. The stale id has to go
        // first: whenTargetResolved answers immediately from the cache, so
        // waiting while the old value is still there returns the dead target.
        m_tabs[idx].chromiumTargetId.clear();
        resolveTargetId(tabId, 0);

        // Block until the fresh id lands, because the caller — the CDP proxy —
        // is about to read it to decide where to point the upstream socket.
        if (chromiumTargetId(tabId).isEmpty()) {
            QEventLoop idLoop;
            QTimer idDeadline;
            idDeadline.setSingleShot(true);
            connect(&idDeadline, &QTimer::timeout, &idLoop, &QEventLoop::quit);
            whenTargetResolved(tabId, [&idLoop](const QString &) { idLoop.quit(); });
            idDeadline.start(5000);
            idLoop.exec();
        }
    }

    // Not on top: waking a background tab must not cover the active one.
    if (!m_activeTabId.isEmpty() && m_activeTabId != tabId) {
        const int activeIdx = indexOf(m_activeTabId);
        if (activeIdx >= 0 && m_tabs.at(activeIdx).view)
            m_tabs.at(activeIdx).view->raise();
    }
    startGrazing(tabId);
}

QWebEngineView *AnoaBrowser::createView(QWebEngineProfile *profile)
{
    auto *view = new QWebEngineView(this);
    acceptDownloadsOn(profile);
    auto *page = new AnoaPage(m_config.terminalMode, profile, view);
    // A popup becomes a background tab on the same profile: it is the same
    // session the opener belongs to, and putting it in front would interrupt
    // whoever is driving the active tab.
    page->setTabOpener([this, profile]() -> QWebEnginePage * {
        Tab tab;
        tab.id = m_minter.next();
        tab.profile = profile;
        tab.view = createView(profile);
        m_profileUsers[profile] += 1;
        const QString id = finishNewTab(tab, QUrl());
        return id.isEmpty() ? nullptr : pageFor(id);
    });
    view->setPage(page);

    // Every view reports through the same three signals, filtered to whichever
    // tab is active. A filter rather than connect/disconnect on every switch:
    // there is one connection per view for its whole life, so no window exists
    // in which a page is loading with nothing listening.
    connect(view, &QWebEngineView::urlChanged, this, [this, view](const QUrl &url) {
        if (view == activeView())
            emit activeUrlChanged(url);
    });
    connect(view, &QWebEngineView::titleChanged, this, [this, view](const QString &title) {
        if (view == activeView())
            emit activeTitleChanged(title);
    });
    connect(view, &QWebEngineView::loadFinished, this, [this, view](bool ok) {
        if (view == activeView())
            emit activeLoadFinished(ok);
    });

    // A recreated page gets a new DevTools target, so the cached id becomes a
    // dead route. A client dialling a stale id fails in a way that looks like
    // the browser is broken; having no id at all is merely "not yet", which
    // /json/list already knows how to express.
    connect(view->page(), &QWebEnginePage::renderProcessTerminated, this,
            [this, view](QWebEnginePage::RenderProcessTerminationStatus, int) {
                for (Tab &tab : m_tabs) {
                    if (tab.view != view)
                        continue;
                    tab.chromiumTargetId.clear();
                    resolveTargetId(tab.id, 0);
                    break;
                }
            });

    return view;
}

int AnoaBrowser::indexOf(const QString &id) const
{
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs.at(i).id == id)
            return i;
    }
    return -1;
}

QString AnoaBrowser::newTab(const QUrl &url, const QString &profileName, bool isolated,
                            const QString &name)
{
    // A name is an alias, so it has to be unique or it is not one. Refusing
    // here rather than overwriting: two tabs answering to "search" would make
    // every later --tab search a coin toss.
    if (!name.isEmpty() && !resolveTab(name).isEmpty())
        return QString();

    Tab tab;
    tab.id = m_minter.next();
    tab.name = name;
    tab.profileName = profileName;
    tab.profile = profileFor(profileName, isolated);
    tab.view = createView(tab.profile);
    m_profileUsers[tab.profile] += 1;

    return finishNewTab(tab, url);
}

QString AnoaBrowser::finishNewTab(Tab &tab, const QUrl &url)
{
    // Every profile a client can name needs a stable id, minted once per
    // profile object so two tabs sharing one report the same context.
    if (!m_contextIds.contains(tab.profile)) {
        m_contextIds.insert(tab.profile,
                            tab.profile == m_profile
                                ? QStringLiteral("__anoa_default__")
                                : QStringLiteral("anoa-ctx-%1").arg(++m_nextContextId));
    }

    m_tabs.append(tab);
    // Visible from birth, sized like the container. A tab opened while the
    // window is already up never sees a resize, and a view that was never
    // shown takes no input even once it is raised.
    tab.view->setGeometry(rect());
    tab.view->show();

    // The first tab is the active one by definition; later tabs open in the
    // background, so an agent driving the active tab is not interrupted by
    // another one opening.
    if (m_activeTabId.isEmpty()) {
        m_activeTabId = tab.id;
        tab.view->raise();
    } else {
        // Opened in the background: a tab an agent opens, reads once and then
        // ignores is exactly what --graze is for, so its clock starts now.
        startGrazing(tab.id);
    }

    // about:blank rather than nothing: a page has to exist for the renderer to
    // start and register as a DevTools target, which is what makes the tab
    // reachable over CDP at all.
    tab.view->load(url.isEmpty() ? QUrl(QStringLiteral("about:blank")) : url);

    // The DevTools target does not exist yet, so this answers on a later turn
    // of the event loop and retries until it does.
    resolveTargetId(tab.id, 0);

    emit tabCreated(tab.id);
    if (m_activeTabId == tab.id)
        emit tabActivated(tab.id);
    return tab.id;
}

bool AnoaBrowser::closeTab(const QString &id)
{
    // First: a timer left running would fire at a tab that is no longer there.
    stopGrazing(id);

    const int idx = indexOf(id);
    if (idx < 0)
        return false;
    // One process still means at least one page: HttpServer, the viewer and
    // every /render/* endpoint describe "the page", and there has to be one.
    if (m_tabs.size() <= 1)
        return false;

    const bool wasActive = (m_activeTabId == id);
    QWebEngineView *view = m_tabs.at(idx).view;
    QWebEngineProfile *profile = m_tabs.at(idx).profile;
    m_tabs.removeAt(idx);
    // The view goes first and the profile reference after it. A profile
    // destroyed while a page still holds it is a use-after-free inside
    // Chromium, not a leak we would notice later.
    delete view;
    releaseProfile(profile);

    if (wasActive) {
        // The next tab in creation order, or the previous one if the closed tab
        // was last — the same choice a tabbed browser makes.
        const int next = (idx < m_tabs.size()) ? idx : m_tabs.size() - 1;
        m_activeTabId = m_tabs.at(next).id;
        m_tabs.at(next).view->raise();
        emit tabActivated(m_activeTabId);
        emit activeUrlChanged(m_tabs.at(next).view->url());
        emit activeTitleChanged(m_tabs.at(next).view->title());
    }

    emit tabClosed(id);
    return true;
}

bool AnoaBrowser::selectTab(const QString &id)
{
    const int idx = indexOf(id);
    if (idx < 0)
        return false;
    if (m_activeTabId == id)
        return true;

    // The arriving tab has to be awake before it is raised, or the window
    // shows a blank page; the one being left behind starts its clock.
    const QString previous = m_activeTabId;
    wakeTab(id);
    m_activeTabId = id;
    stopGrazing(id);
    if (!previous.isEmpty() && previous != id)
        startGrazing(previous);
    m_tabs.at(idx).view->raise();
    m_tabs.at(idx).view->setFocus();

    emit tabActivated(id);
    // The window has no other way to learn what it is now showing: the filtered
    // signals above only fire when a page changes, and switching tabs changes
    // no page.
    emit activeUrlChanged(m_tabs.at(idx).view->url());
    emit activeTitleChanged(m_tabs.at(idx).view->title());
    return true;
}

QStringList AnoaBrowser::tabIds() const
{
    QStringList ids;
    ids.reserve(m_tabs.size());
    for (const Tab &tab : m_tabs)
        ids << tab.id;
    return ids;
}

QString AnoaBrowser::activeTabId() const
{
    return m_activeTabId;
}

int AnoaBrowser::tabCount() const
{
    return static_cast<int>(m_tabs.size());
}

QWebEngineView *AnoaBrowser::viewFor(const QString &id) const
{
    const int idx = indexOf(id);
    return idx < 0 ? nullptr : m_tabs.at(idx).view;
}

QWebEnginePage *AnoaBrowser::pageFor(const QString &id) const
{
    QWebEngineView *view = viewFor(id);
    return view ? view->page() : nullptr;
}

QWebEngineView *AnoaBrowser::activeView() const
{
    return viewFor(m_activeTabId);
}

QWebEngineProfile *AnoaBrowser::profileFor(const QString &name, bool isolated)
{
    // The shared profile's storage root, so a named tab profile lands beside it
    // rather than in the working directory.
    // Off-the-record and unnamed: a fresh jar per tab, gone when the tab goes.
    // Asked for explicitly, because it is the opposite of what a browser
    // normally does with a login.
    if (isolated)
        return new QWebEngineProfile(this);

    if (name.isEmpty())
        return m_profile; // the shared default

    // Created once per name. Two QWebEngineProfile objects over one on-disk
    // path corrupt each other's storage; this is not an optimisation.
    if (QWebEngineProfile *existing = m_profilesByName.value(name))
        return existing;

    const QString base = m_config.profileDir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        : m_config.profileDir;
    auto *profile = new QWebEngineProfile(name, this);
    profile->setPersistentStoragePath(QDir(base).filePath(name));
    profile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
    m_profilesByName.insert(name, profile);
    return profile;
}

void AnoaBrowser::releaseProfile(QWebEngineProfile *profile)
{
    if (!profile)
        return;
    const int left = m_profileUsers.value(profile) - 1;
    if (left > 0) {
        m_profileUsers[profile] = left;
        return;
    }
    m_profileUsers.remove(profile);
    // The default outlives every tab: the process still holds it, and
    // setupNamedProfile may have made it the --profile one.
    if (profile == m_profile)
        return;
    m_profilesByName.remove(m_profilesByName.key(profile));
    m_contextIds.remove(profile);
    profile->deleteLater();
}

QString AnoaBrowser::profileNameFor(const QString &tabId) const
{
    const int idx = indexOf(tabId);
    return idx < 0 ? QString() : m_tabs.at(idx).profileName;
}

bool AnoaBrowser::tabsShareProfile(const QString &a, const QString &b) const
{
    const int ia = indexOf(a);
    const int ib = indexOf(b);
    if (ia < 0 || ib < 0)
        return false;
    return m_tabs.at(ia).profile == m_tabs.at(ib).profile;
}

void AnoaBrowser::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // Nothing lays these out but this: every tab is sized like the container,
    // not just the one on top. /render/screenshot.png reports a view's geometry
    // as the coordinate space clicks are measured in, so a background tab left
    // at its birth size would answer with a viewport a hundred pixels wide and
    // every click against it would land somewhere else entirely.
    for (const Tab &tab : m_tabs) {
        if (tab.view)
            tab.view->setGeometry(rect());
    }
}

QWebEngineView *AnoaBrowser::viewForOrActive(const QString &tabId) const
{
    return tabId.isEmpty() ? activeView() : viewFor(tabId);
}

QString AnoaBrowser::chromiumTargetId(const QString &tabId) const
{
    const int idx = indexOf(tabId);
    return idx < 0 ? QString() : m_tabs.at(idx).chromiumTargetId;
}

QString AnoaBrowser::resolveTab(const QString &idOrName) const
{
    if (idOrName.isEmpty())
        return QString();
    // Ids first. They are minted, unique and cannot collide with a name —
    // isValidTabName rejects anything shaped like one — so this order is a
    // statement of which namespace owns the string, not a tie-break.
    if (indexOf(idOrName) >= 0)
        return idOrName;
    for (const Tab &tab : m_tabs) {
        if (!tab.name.isEmpty() && tab.name == idOrName)
            return tab.id;
    }
    return QString();
}

QString AnoaBrowser::nameFor(const QString &tabId) const
{
    const int idx = indexOf(tabId);
    return idx < 0 ? QString() : m_tabs.at(idx).name;
}

QString AnoaBrowser::targetIdFor(const QString &tabId) const
{
    return chromiumTargetId(tabId);
}

QString AnoaBrowser::titleFor(const QString &tabId) const
{
    QWebEngineView *view = viewFor(tabId);
    return view ? view->title() : QString();
}

QString AnoaBrowser::urlFor(const QString &tabId) const
{
    QWebEngineView *view = viewFor(tabId);
    return view ? view->url().toString() : QString();
}

QString AnoaBrowser::browserContextIdFor(const QString &tabId) const
{
    const int idx = indexOf(tabId);
    if (idx < 0)
        return QStringLiteral("__anoa_default__");
    return m_contextIds.value(m_tabs.at(idx).profile,
                              QStringLiteral("__anoa_default__"));
}

bool AnoaBrowser::knowsBrowserContext(const QString &contextId) const
{
    if (contextId == QLatin1String("__anoa_default__"))
        return true;
    for (auto it = m_contextIds.constBegin(); it != m_contextIds.constEnd(); ++it) {
        if (it.value() == contextId)
            return true;
    }
    return false;
}

QString AnoaBrowser::newTabInBrowserContext(const QUrl &url, const QString &contextId)
{
    if (contextId.isEmpty() || contextId == QLatin1String("__anoa_default__"))
        return newTab(url);

    // The context names a profile we already hold, so the new tab joins it
    // rather than creating a second object over the same storage.
    for (auto it = m_contextIds.constBegin(); it != m_contextIds.constEnd(); ++it) {
        if (it.value() != contextId)
            continue;
        QWebEngineProfile *profile = it.key();
        Tab tab;
        tab.id = m_minter.next();
        tab.profile = profile;
        tab.profileName = m_profilesByName.key(profile);
        tab.view = createView(profile);
        m_profileUsers[profile] += 1;
        return finishNewTab(tab, url);
    }
    return QString(); // not ours
}

void AnoaBrowser::whenTargetResolved(const QString &tabId,
                                     std::function<void(const QString &)> cb)
{
    const QString already = chromiumTargetId(tabId);
    if (!already.isEmpty()) {
        cb(already);
        return;
    }
    // One shot: disconnected as soon as this tab's id lands, so a later tab
    // resolving does not fire someone else's callback.
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(this, &AnoaBrowser::tabTargetResolved, this,
                    [tabId, cb, conn](const QString &resolvedTab,
                                      const QString &targetId) {
                        if (resolvedTab != tabId)
                            return;
                        QObject::disconnect(*conn);
                        cb(targetId);
                    });
}

QString AnoaBrowser::tabIdForTargetId(const QString &targetId) const
{
    if (targetId.isEmpty())
        return QString();
    for (const Tab &tab : m_tabs) {
        if (tab.chromiumTargetId == targetId)
            return tab.id;
    }
    return QString();
}

void AnoaBrowser::resolveTargetId(const QString &tabId, int attempt)
{
    // The embedded viewer opens no debugging port at all, so there is nothing
    // to ask and nothing that could ever answer.
    if (m_config.termEmbedded)
        return;
    const int idx = indexOf(tabId);
    if (idx < 0)
        return; // the tab was closed while we were waiting

    // 100, 200, 400, 800, 1600ms — about 3s in total, then give up and leave
    // the id empty. Advertising a target that cannot be dialled is worse than
    // advertising it a moment later, and /json/list omits unresolved tabs.
    constexpr int kMaxAttempts = 6;
    if (attempt >= kMaxAttempts)
        return;

    const QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/json/list")
                            .arg(m_config.port + 1));
    QNetworkReply *reply = m_nam->get(QNetworkRequest(endpoint));
    connect(reply, &QNetworkReply::finished, this, [this, reply, tabId, attempt]() {
        reply->deleteLater();

        QString found;
        if (reply->error() == QNetworkReply::NoError) {
            const QJsonArray targets =
                QJsonDocument::fromJson(reply->readAll()).array();
            const int idx = indexOf(tabId);
            if (idx < 0)
                return; // closed while the request was in flight

            const QWebEngineView *view = m_tabs.at(idx).view;
            const QString wantUrl = view ? view->url().toString() : QString();

            // Only entries no other tab has already claimed can be ours. With
            // one unclaimed entry that settles it; with several — tabs opened
            // close together, all still on about:blank — prefer the one whose
            // url matches this view's.
            QString firstUnclaimed;
            for (const QJsonValue &value : targets) {
                const QJsonObject target = value.toObject();
                if (target.value(QStringLiteral("type")).toString()
                    != QLatin1String("page"))
                    continue;
                const QString id = target.value(QStringLiteral("id")).toString();
                if (id.isEmpty() || !tabIdForTargetId(id).isEmpty())
                    continue;
                if (firstUnclaimed.isEmpty())
                    firstUnclaimed = id;
                if (!wantUrl.isEmpty()
                    && target.value(QStringLiteral("url")).toString() == wantUrl) {
                    found = id;
                    break;
                }
            }
            if (found.isEmpty())
                found = firstUnclaimed;
        }

        if (found.isEmpty()) {
            const int delayMs = 100 << attempt;
            QTimer::singleShot(delayMs, this, [this, tabId, attempt]() {
                resolveTargetId(tabId, attempt + 1);
            });
            return;
        }

        const int idx = indexOf(tabId);
        if (idx < 0)
            return;
        m_tabs[idx].chromiumTargetId = found;
        emit tabTargetResolved(tabId, found);
    });
}

QWebEnginePage *AnoaBrowser::page() const
{
    QWebEngineView *view = activeView();
    return view ? view->page() : nullptr;
}

void AnoaBrowser::load(const QUrl &url)
{
    if (QWebEngineView *view = activeView())
        view->load(url);
}

void AnoaBrowser::back()
{
    if (QWebEngineView *view = activeView())
        view->back();
}

void AnoaBrowser::forward()
{
    if (QWebEngineView *view = activeView())
        view->forward();
}

void AnoaBrowser::reload()
{
    if (QWebEngineView *view = activeView())
        view->reload();
}

QUrl AnoaBrowser::url() const
{
    QWebEngineView *view = activeView();
    return view ? view->url() : QUrl();
}

QString AnoaBrowser::title() const
{
    QWebEngineView *view = activeView();
    return view ? view->title() : QString();
}

void AnoaBrowser::init()
{
    resize(m_config.width, m_config.height);
    // show() is required in both headed and headless (offscreen) mode: without it
    // the widget has no backing surface and QWebEngineView reports a 0×0 viewport.
    // With QPA_PLATFORM=offscreen the call creates an invisible surface, not a window.
    show();
    // Exactly one tab, so nothing observable changes for a caller that has
    // never heard of tabs.
    newTab();
}

void AnoaBrowser::loadExtensions(const QStringList &paths)
{
    // Process-wide, against the default profile only. Extensions per tab are
    // out of scope, so a tab on a named or isolated profile gets none of these
    // scripts — which is why this reads m_profile rather than walking the
    // profile registry.
    for (const QString &path : paths) {
        if (!QDir(path).exists()) {
            qWarning("Extension path does not exist, skipping: %s", qPrintable(path));
            continue;
        }
        const QString manifestPath = path + "/manifest.json";
        if (!QFile::exists(manifestPath)) {
            qWarning("No manifest.json found in extension path, skipping: %s", qPrintable(path));
            continue;
        }
        QFile f(manifestPath);
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning("Cannot read manifest.json, skipping: %s", qPrintable(path));
            continue;
        }
        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
        if (doc.isNull()) {
            qWarning("Invalid manifest.json (%s), skipping: %s",
                     qPrintable(parseErr.errorString()), qPrintable(path));
            continue;
        }
        const QJsonObject manifest = doc.object();
        const int manifestVersion = manifest["manifest_version"].toInt();
        if (manifestVersion == 3) {
            qWarning("Manifest v3 not supported in Qt6 WebEngine, skipping: %s", qPrintable(path));
            continue;
        }

        // Inject content scripts via QWebEngineScript (manifest v1/v2 only)
        const QJsonArray contentScripts = manifest["content_scripts"].toArray();
        for (const auto &csVal : contentScripts) {
            const QJsonObject cs = csVal.toObject();
            const QJsonArray jsFiles = cs["js"].toArray();
            for (const auto &jsVal : jsFiles) {
                const QString jsPath = path + "/" + jsVal.toString();
                QFile jsFile(jsPath);
                if (!jsFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    qWarning("Cannot read content script, skipping: %s", qPrintable(jsPath));
                    continue;
                }
                QWebEngineScript script;
                script.setName(jsPath);
                script.setSourceCode(QString::fromUtf8(jsFile.readAll()));
                script.setInjectionPoint(QWebEngineScript::DocumentReady);
                script.setWorldId(QWebEngineScript::MainWorld);
                m_profile->scripts()->insert(script);
            }
        }
    }
}

void AnoaBrowser::setupNamedProfile(const QString &name, const QString &baseDir)
{
    if (m_profile != QWebEngineProfile::defaultProfile())
        m_profile->deleteLater();

    const QString base = baseDir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        : baseDir;
    m_profile = new QWebEngineProfile(name, this);
    m_profile->setPersistentStoragePath(QDir(base).filePath(name));
    m_profile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);

    // Called before init() in practice, so there is usually nothing to move.
    // Any tab that does exist gets a page on the new profile, which is what
    // this call meant when there was only ever one.
    for (Tab &tab : m_tabs) {
        auto *oldPage = tab.view->page();
        tab.profile = m_profile;
        tab.view->setPage(new AnoaPage(m_config.terminalMode, m_profile, tab.view));
        oldPage->deleteLater();
    }
}

QList<QNetworkCookie> AnoaBrowser::getCookies(const QUrl &origin)
{
    Q_UNUSED(origin)
    QList<QNetworkCookie> cookies;
    auto *store = m_profile->cookieStore();

    QEventLoop loop;
    auto conn = connect(store, &QWebEngineCookieStore::cookieAdded,
                        [&cookies](const QNetworkCookie &cookie) {
                            cookies.append(cookie);
                        });
    QTimer::singleShot(500, &loop, &QEventLoop::quit);
    store->loadAllCookies();
    loop.exec();
    disconnect(conn);
    return cookies;
}

void AnoaBrowser::setCookie(const QNetworkCookie &cookie, const QUrl &origin)
{
    m_profile->cookieStore()->setCookie(cookie, origin);
}

void AnoaBrowser::clearStorage(const QUrl &origin)
{
    Q_UNUSED(origin)
    m_profile->cookieStore()->deleteAllCookies();
    m_profile->clearAllVisitedLinks();
    if (QWebEnginePage *p = page())
        p->triggerAction(QWebEnginePage::Stop);
}

// Synthetic input must go to the render widget (focusProxy), not the
// QWebEngineView itself — events posted to the view are not forwarded
// to Chromium. postEvent (not sendEvent) keeps this callable from HTTP
// handler code without re-entering the widget stack synchronously.
static QWidget *inputTarget(QWebEngineView *view)
{
    if (!view)
        return nullptr;
    QWidget *proxy = view->focusProxy();
    return proxy ? proxy : static_cast<QWidget *>(view);
}

void AnoaBrowser::sendClick(const QPoint &pos, Qt::MouseButton button, const QString &tabId,
                            Qt::KeyboardModifiers mods)
{
    QWidget *target = inputTarget(viewForOrActive(tabId));
    if (!target)
        return;
    const QPointF posF(pos);
    const QPointF globalF(target->mapToGlobal(pos));
    // Chromium's click-count logic compares event timestamps; leaving them at 0
    // makes every synthetic click look simultaneous (double/triple-click runs).
    const auto stamp = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
    // Leading move puts the pointer at the click position so hover/hit-testing
    // state matches a real interaction before the press arrives.
    auto *move = new QMouseEvent(QEvent::MouseMove, posF, posF, globalF,
                                 Qt::NoButton, Qt::NoButton, mods);
    move->setTimestamp(stamp);
    auto *press = new QMouseEvent(QEvent::MouseButtonPress, posF, posF, globalF,
                                  button, button, mods);
    press->setTimestamp(stamp + 1);
    auto *release = new QMouseEvent(QEvent::MouseButtonRelease, posF, posF, globalF,
                                    button, Qt::NoButton, mods);
    release->setTimestamp(stamp + 50);
    QCoreApplication::postEvent(target, move);
    QCoreApplication::postEvent(target, press);
    QCoreApplication::postEvent(target, release);
}

// The three below share everything except the event type and which buttons are
// reported held, so they post through one helper rather than three copies that
// would drift.
static void postMouse(QWidget *target, QEvent::Type type, const QPoint &pos,
                      Qt::MouseButton button, Qt::MouseButtons held,
                      Qt::KeyboardModifiers mods)
{
    if (!target)
        return;
    const QPointF posF(pos);
    const QPointF globalF(target->mapToGlobal(pos));
    auto *ev = new QMouseEvent(type, posF, posF, globalF, button, held, mods);
    // Same reason as sendClick: a zero timestamp reads as "same instant as the
    // last one", and a stream of those is a multi-click, not a drag.
    ev->setTimestamp(static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()));
    QCoreApplication::postEvent(target, ev);
}

void AnoaBrowser::sendMouseMove(const QPoint &pos, Qt::MouseButtons heldButtons,
                                Qt::KeyboardModifiers mods, const QString &tabId)
{
    // Qt reports no *triggering* button on a move — the buttons still down go
    // in the third argument. Passing the held set as the trigger instead makes
    // Chromium read every move as a fresh press.
    postMouse(inputTarget(viewForOrActive(tabId)), QEvent::MouseMove, pos,
              Qt::NoButton, heldButtons, mods);
}

void AnoaBrowser::sendMouseDown(const QPoint &pos, Qt::MouseButton button,
                                Qt::KeyboardModifiers mods, const QString &tabId)
{
    postMouse(inputTarget(viewForOrActive(tabId)), QEvent::MouseButtonPress, pos,
              button, button, mods);
}

void AnoaBrowser::sendMouseUp(const QPoint &pos, Qt::MouseButton button,
                              Qt::KeyboardModifiers mods, const QString &tabId)
{
    // The button is released, so it is the trigger but no longer held.
    postMouse(inputTarget(viewForOrActive(tabId)), QEvent::MouseButtonRelease, pos,
              button, Qt::NoButton, mods);
}

void AnoaBrowser::sendScroll(const QPoint &pos, int angleDeltaY, const QString &tabId)
{
    QWidget *target = inputTarget(viewForOrActive(tabId));
    if (!target)
        return;
    const QPointF posF(pos);
    const QPointF globalF(target->mapToGlobal(pos));
    const auto stamp = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());

    auto post = [&](const QPoint &angle, const QPoint &pixels, Qt::ScrollPhase phase,
                    quint64 at) {
        auto *wheel = new QWheelEvent(posF, globalF, pixels, angle,
                                      Qt::NoButton, Qt::NoModifier, phase, false);
        wheel->setTimestamp(at);
        QCoreApplication::postEvent(target, wheel);
    };

#ifdef Q_OS_MACOS
    // macOS has no phaseless wheel. Qt delivers real scroll input there as a
    // gesture — begin, update, end — and QtWebEngine maps Qt::NoScrollPhase to
    // a Blink phase Chromium drops on the floor. The event was posted, accepted
    // and discarded: /render/scroll answered "scrolled", the page never saw a
    // wheel event at all, and nothing moved. Verified against Linux/aarch64,
    // where the same phaseless event scrolls correctly.
    //
    // pixelDelta is what a phased event is steered by, and it has to be filled
    // in or the gesture is a no-op with a different shape. angleDelta/2 is the
    // ratio measured on Linux — 600 angle units moved the page 300 px — so the
    // two platforms scroll the same distance for the same request.
    const QPoint angle(0, angleDeltaY);
    const QPoint pixels(0, angleDeltaY / 2);
    post(QPoint(), QPoint(), Qt::ScrollBegin, stamp);
    post(angle, pixels, Qt::ScrollUpdate, stamp + 1);
    post(QPoint(), QPoint(), Qt::ScrollEnd, stamp + 2);
#else
    // Null pixelDelta = classic notched wheel; angleDelta is in 1/8 degree,
    // one wheel notch = 120.
    post(QPoint(0, angleDeltaY), QPoint(), Qt::NoScrollPhase, stamp);
#endif
}

void AnoaBrowser::sendText(const QString &text, const QString &tabId)
{
    QWidget *target = inputTarget(viewForOrActive(tabId));
    if (!target)
        return;
    for (const QChar &ch : text) {
        // key = 0 (unknown) + non-empty text: Chromium takes the character from
        // the text payload, which handles any unicode without a key-code table.
        QCoreApplication::postEvent(target,
            new QKeyEvent(QEvent::KeyPress, 0, Qt::NoModifier, QString(ch)));
        QCoreApplication::postEvent(target,
            new QKeyEvent(QEvent::KeyRelease, 0, Qt::NoModifier, QString(ch)));
    }
}

bool AnoaBrowser::sendKey(const QString &keyName, const QString &tabId,
                          Qt::KeyboardModifiers mods)
{
    struct NamedKey {
        const char *name;
        Qt::Key key;
        const char *text; // control chars Chromium expects alongside the key
    };
    static const NamedKey keys[] = {
        {"enter", Qt::Key_Return, "\r"},
        {"tab", Qt::Key_Tab, "\t"},
        {"backspace", Qt::Key_Backspace, ""},
        {"delete", Qt::Key_Delete, ""},
        {"escape", Qt::Key_Escape, ""},
        {"space", Qt::Key_Space, " "},
        {"up", Qt::Key_Up, ""},
        {"down", Qt::Key_Down, ""},
        {"left", Qt::Key_Left, ""},
        {"right", Qt::Key_Right, ""},
        {"home", Qt::Key_Home, ""},
        {"end", Qt::Key_End, ""},
        {"pageup", Qt::Key_PageUp, ""},
        {"pagedown", Qt::Key_PageDown, ""},
        {"insert", Qt::Key_Insert, ""},
    };

    QWidget *target = inputTarget(viewForOrActive(tabId));
    if (!target)
        return false;

    // A shortcut carries no text. Chromium turns text into an insertion, so
    // Ctrl+A with text "a" both selects all and types an "a".
    const bool isShortcut = mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);

    auto post = [&](Qt::Key key, const QString &text) {
        const QString payload = isShortcut ? QString() : text;
        QCoreApplication::postEvent(target,
            new QKeyEvent(QEvent::KeyPress, key, mods, payload));
        QCoreApplication::postEvent(target,
            new QKeyEvent(QEvent::KeyRelease, key, mods, payload));
    };

    const QString wanted = keyName.toLower();
    for (const NamedKey &k : keys) {
        if (wanted == QLatin1String(k.name)) {
            post(k.key, QString::fromLatin1(k.text));
            return true;
        }
    }

    // f1..f12. Spelled out rather than tabulated because the enum is contiguous
    // and twelve more table rows would say less than this does.
    if (wanted.size() >= 2 && wanted.at(0) == QLatin1Char('f')) {
        bool ok = false;
        const int n = QStringView{wanted}.mid(1).toInt(&ok);
        if (ok && n >= 1 && n <= 12) {
            post(static_cast<Qt::Key>(Qt::Key_F1 + n - 1), QString());
            return true;
        }
    }

    // A single printable character, which is what a shortcut needs: "a" alone
    // is better served by sendText, but Ctrl+"a" has no other way in.
    if (keyName.size() == 1) {
        const QChar ch = keyName.at(0);
        if (ch.isPrint()) {
            // Qt's key codes for letters and digits are their uppercase ASCII
            // values; anything else printable maps to its own code point.
            const QChar upper = ch.toUpper();
            post(static_cast<Qt::Key>(upper.unicode()), keyName);
            return true;
        }
    }

    return false;
}
