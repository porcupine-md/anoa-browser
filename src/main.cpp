#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <memory>

#include <QtGlobal>

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QTimer>

#include "agent/agent_cli.h"
#include "agent/agent_help.h"
#include "browser/anoa_browser.h"
#include "browser/browser_window.h"
#include "cdp/cdp_proxy.h"
#include "config/config.h"
#include "http/http_server.h"

#ifndef Q_OS_WIN
#include "terminal/terminal_app.h"
#endif

#ifndef Q_OS_WIN
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// Is something already listening there? A bounded, non-blocking connect,
// because this runs before any application object exists and must not be able
// to hang the process on a host that swallows SYNs.
bool portIsOpen(quint16 port, int timeoutMs)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    bool open = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
    if (!open && errno == EINPROGRESS) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(fd, &w);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (::select(fd + 1, nullptr, &w, nullptr, &tv) > 0) {
            int soErr = 0;
            socklen_t len = sizeof(soErr);
            open = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len) == 0 && soErr == 0;
        }
    }
    ::close(fd);
    return open;
}

} // namespace
#endif

int main(int argc, char *argv[])
{
    // Pre-scan raw argv for the two things that must be known before any
    // application object exists: --headless (QT_QPA_PLATFORM has to be set
    // before QApplication) and the `terminal` subcommand (it selects the
    // application class itself). QCommandLineParser, used by parseArgs, needs a
    // live QCoreApplication, so neither decision can wait for the full parse —
    // and `terminal` is a positional word the parser never registers.
    bool terminalMode = false;
    // Whether the viewer was given a target to connect to. Also a pre-scan
    // question, and for the same reason: with no target the viewer hosts its
    // own browser, which decides the application class just as `terminal` does.
    // Presence is all that matters, so the values are not parsed here — this
    // only has to agree with QCommandLineParser about which words are options,
    // including the --opt=value spelling it accepts.
    bool hasTarget = false;
    // Set when the line is a statement against a running browser rather than an
    // instruction to start one. Everything after the verb belongs to it.
    QString agentVerb;
    QStringList agentArgs;
    const auto isOption = [](const char *arg, const char *name) {
        const size_t n = std::strlen(name);
        return std::strncmp(arg, name, n) == 0 && (arg[n] == '\0' || arg[n] == '=');
    };

    // A mistyped subcommand used to start a browser. Nothing reads
    // positionalArguments() and Config has no URL field, so QCommandLineParser
    // discarded the word and the process carried on as if the line had been
    // bare `anoa` — opening a window on a desktop, and on a headless box dying
    // as "Could not load the Qt platform plugin xcb", which describes nothing
    // the user did. Only the first argument is examined: a later bare word can
    // be an option's value (`--port 9222`), and telling those apart would mean
    // duplicating every option's arity here.
    if (argc > 1 && argv[1][0] != '-' && std::strcmp(argv[1], "terminal") != 0
        && !isAgentCommand(QString::fromLocal8Bit(argv[1]))) {
        QTextStream(stderr) << "anoa: unknown command '" << argv[1] << "' — try: anoa help"
                            << Qt::endl;
        return 2;
    }

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        } else if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0
                   || std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            // QCommandLineParser handles both of these, and it needs a live
            // application object — which on a machine with no display aborts
            // before it can print anything. `anoa --version` therefore
            // died with SIGABRT on any headless Linux box, which is also the
            // one command the Homebrew Linux formula runs as its test.
            //
            // Neither path ever puts a pixel on a screen, so the offscreen
            // platform is not a compromise here: it is simply the honest
            // description of what the process is about to do.
            qputenv("QT_QPA_PLATFORM", "offscreen");
        } else if (isOption(argv[i], "--term-host") || isOption(argv[i], "--term-port")
                   || isOption(argv[i], "--cdp")) {
            hasTarget = true;
        } else if (std::strcmp(argv[i], "terminal") == 0) {
            terminalMode = true;
            // Drop the subcommand word so QCommandLineParser only ever sees
            // options: process() would otherwise leave it in
            // positionalArguments() and echo it back in --help.
            for (int j = i; j < argc - 1; ++j)
                argv[j] = argv[j + 1];
            argv[--argc] = nullptr;
            --i; // re-examine the argument shifted into this slot
        } else if (isAgentCommand(QString::fromLocal8Bit(argv[i]))) {
            // An agent verb takes the rest of the line with it — arguments and
            // flags alike. Splitting them between this pre-scan and
            // QCommandLineParser is what would go wrong: `anoa open a.com
            // --port 9222` has a bare "9222" that belongs to --port, and
            // `anoa snapshot --json` has a flag the browser parser has never
            // heard of. Neither can be classified without knowing every
            // option's arity, which is precisely what a subcommand boundary
            // exists to avoid.
            agentVerb = QString::fromLocal8Bit(argv[i]);
            for (int j = i + 1; j < argc; ++j)
                agentArgs << QString::fromLocal8Bit(argv[j]);
            // Anything before the verb goes with it too, so both orders work:
            //
            //   anoa get text --tab t2      (always did)
            //   anoa --tab t2 get text      (used to be "Unknown option 'tab'")
            //
            // The second is the form our own README, help and skill documents
            // told agents to use, and it is what a person reaches for — `git
            // --no-pager log` reads the same way. It failed loudly, which is
            // better than acting on the wrong tab, but "loudly wrong" is still
            // wrong when the documentation said to type it.
            //
            // Only these five are moved, and only with their value. They are
            // the options that address a browser rather than describe one, so
            // they mean the same thing whichever side of the verb they sit;
            // every other flag before a verb still belongs to the browser
            // parser. The arity problem the comment above describes is why
            // this is a fixed list rather than a general rule.
            for (int j = 1; j < i; ++j) {
                // Already taken as the previous option's value.
                if (!argv[j])
                    continue;
                const bool addressing =
                    isOption(argv[j], "--tab") || isOption(argv[j], "--port")
                    || isOption(argv[j], "--host") || isOption(argv[j], "--token")
                    || isOption(argv[j], "--auth-token");
                if (!addressing)
                    continue;
                agentArgs << QString::fromLocal8Bit(argv[j]);
                // --opt=value carries its own value; --opt value takes the next
                // word, which must not be left behind for the browser parser to
                // trip over.
                const bool joined = std::strchr(argv[j], '=') != nullptr;
                if (!joined && j + 1 < i) {
                    agentArgs << QString::fromLocal8Bit(argv[j + 1]);
                    argv[j + 1] = nullptr;
                }
                argv[j] = nullptr;
            }
            // Compact what is left: parseArgs still runs on this argv, and a
            // hole in the middle of it is not something QCommandLineParser
            // survives.
            int keep = 1;
            for (int j = 1; j < i; ++j) {
                if (argv[j])
                    argv[keep++] = argv[j];
            }
            argc = keep;
            argv[argc] = nullptr;
            break;
        }
    }
    bool embeddedTerminal = terminalMode && !hasTarget;
#ifndef Q_OS_WIN
    // A bare `anoa terminal` prefers a browser that is already running.
    //
    // Hosting its own was the fallback for "there is nothing to attach to", not
    // a preference: starting a second browser beside a live one gives the user
    // a viewer showing a blank page next to the session they meant to watch,
    // and doubles the memory for it. Anything that names a target
    // (--term-port, --cdp) has already opted out of both behaviours above.
    if (embeddedTerminal && portIsOpen(9222, 300))
        embeddedTerminal = false;
#endif

    // `anoa help [group]` and `anoa --help` are the same thing.
    //
    // QCommandLineParser's own --help lists flags and cannot mention a
    // subcommand, because the parser never sees one — so a user typing --help
    // was shown --profile-dir and left with no idea `click` existed. The
    // grouped help carries both: the commands, and the flags under BROWSER.
    const bool wantsHelp =
        agentVerb == QLatin1String("help")
        || (agentVerb.isEmpty()
            && std::any_of(argv + 1, argv + argc, [](const char *a) {
                   return a && (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0);
               }));
    if (wantsHelp) {
        const QString group = agentArgs.isEmpty() ? QString() : agentArgs.first();
        if (group.isEmpty()) {
            printAgentHelp();
            return 0;
        }
        if (printAgentHelpGroup(group))
            return 0;
        QTextStream(stderr) << "anoa: no help group '" << group << "' — try: anoa help"
                            << Qt::endl;
        return 2;
    }

    if (!agentVerb.isEmpty()) {
        // QCoreApplication is enough: an agent command speaks JSON over a
        // WebSocket and never touches the widget stack.
        QCoreApplication app(argc, argv);
        app.setApplicationVersion(QStringLiteral(ANOA_VERSION));
        Config config = parseArgs(argc, argv, /*terminalMode=*/true);
        return runAgentCommand(config, agentVerb, agentArgs);
    }

    if (terminalMode) {
#ifdef Q_OS_WIN
        // No terminal source is compiled on Windows at all (see CMakeLists.txt),
        // so this has to be a clean runtime error rather than a link failure.
        // The embedded decision is POSIX-only for the same reason, and would
        // otherwise be an unused variable in a build that treats warnings as
        // something to keep at zero.
        Q_UNUSED(embeddedTerminal)
        QTextStream err(stderr);
        err << "Error: terminal mode is not supported on Windows" << Qt::endl;
        return 1;
#else
        if (embeddedTerminal) {
            // The one terminal case that is not a thin client, and so the one
            // that cannot use QCoreApplication: it hosts a QWebEngineView, and
            // that needs the widget stack. Offscreen keeps the "works over SSH
            // with no display" property that motivated QCoreApplication in the
            // first place — QApplication only aborts on a missing display when
            // it is left to pick a platform itself.
            qputenv("QT_QPA_PLATFORM", "offscreen");
            QApplication app(argc, argv);
            app.setApplicationVersion(QStringLiteral(ANOA_VERSION));

            Config config = parseArgs(argc, argv, /*terminalMode=*/true);
            config.termEmbedded = true;
            // Not a copy of the --headless flag: there is no window either way,
            // and AnoaBrowser reads this to add --disable-gpu and to create an
            // offscreen surface rather than look for a display.
            config.headless = true;

            AnoaBrowser browser(config);
            if (!config.profileName.isEmpty())
                browser.setupNamedProfile(config.profileName, config.profileDir);
            browser.loadExtensions(config.extensionPaths);
            browser.init();

            // Still no HttpServer and no CdpProxy: nothing outside this process
            // is meant to reach this browser, and binding ports for a viewer
            // that talks to it through a pointer would be surface for nothing.
            return runTerminal(config, &browser);
        }

        // QCoreApplication, not QApplication: the primary use case is SSH with
        // no display, where QApplication aborts unless QT_QPA_PLATFORM is set.
        QCoreApplication app(argc, argv);
        app.setApplicationVersion(QStringLiteral(ANOA_VERSION));

        Config config = parseArgs(argc, argv, /*terminalMode=*/true);

        // Pointed at a browser elsewhere: no AnoaBrowser, HttpServer or CdpProxy.
        return runTerminal(config);
#endif
    }

    QApplication app(argc, argv);
    app.setApplicationVersion(QStringLiteral(ANOA_VERSION));
    // Read from the binary's own resources, so it is there wherever the bundle
    // was unpacked to. macOS takes the dock icon from the .icns in the bundle
    // instead and ignores this; X11 and Windows use it for the window and the
    // task switcher.
    app.setWindowIcon(QIcon(QStringLiteral(":/anoa.png")));

    Config config = parseArgs(argc, argv);

    AnoaBrowser browser(config);
    if (!config.profileName.isEmpty())
        browser.setupNamedProfile(config.profileName, config.profileDir);
    browser.loadExtensions(config.extensionPaths);

    // Declared after `browser` so it is destroyed first: its destructor
    // releases the tab container it borrowed, and every view inside goes with
    // it. The container itself is a stack object here and must survive that
    // release. Headless mode gets no window at all —
    // there is nothing to show chrome on, and wrapping the view would change
    // the geometry that /render/* reports as the viewport.
    std::unique_ptr<BrowserWindow> window;
    if (!config.headless)
        window = std::make_unique<BrowserWindow>(&browser, config);

    browser.init();
    if (window)
        window->show();

    // Port layout:
    //   config.port     (e.g. 9222) – HTTP discovery (HttpServer)
    //   config.port + 1 (e.g. 9223) – Chromium DevTools internal (set via QTWEBENGINE_CHROMIUM_FLAGS)
    //   config.port + 2 (e.g. 9224) – WebSocket CDP proxy (CdpProxy)
    //
    // HttpServer rewrites webSocketDebuggerUrl from port+1 to port+2 so that
    // CDP clients connect through the authenticated proxy.
    const auto httpPort  = static_cast<quint16>(config.port);
    const auto debugPort = static_cast<quint16>(config.port + 1);
    const auto wsPort    = static_cast<quint16>(config.port + 2);

    HttpServer httpServer(httpPort, debugPort, wsPort, config.authToken, &browser, &app);
    httpServer.setEmbedOrigins(config.embedOrigins);
    if (!httpServer.start()) {
        qCritical("Failed to bind HTTP server to port %u (already in use?)", httpPort);
        return 1;
    }

    CdpProxy cdpProxy(wsPort, debugPort, config.authToken, &app);
    // Provide the initial page for commands handled locally (e.g. Page.printToPDF).
    // The proxy answers some commands itself and needs the page the client is
    // actually attached to, not the first tab. It is handed a lookup rather
    // than a pointer so src/cdp keeps knowing nothing about src/browser.
    cdpProxy.setTabHost(&browser);
    cdpProxy.setPageResolver([&browser](const QString &targetId) -> QWebEnginePage * {
        if (targetId.isEmpty())
            return browser.page(); // browser-level endpoint: the active tab
        const QString tabId = browser.tabIdForTargetId(targetId);
        // An id we do not recognise is not necessarily wrong — resolution may
        // still be in flight — so fall back to the active tab rather than
        // failing the command outright.
        return tabId.isEmpty() ? browser.page() : browser.pageFor(tabId);
    });
    if (!cdpProxy.start()) {
        qCritical("Failed to bind CDP proxy to port %u (already in use?)", wsPort);
        return 1;
    }

#ifndef Q_OS_WIN
    // Ctrl-C and SIGTERM have to end in a clean Qt shutdown, or the session is
    // lost.
    //
    // Chromium writes cookies and local storage lazily and flushes them when
    // the profile is destroyed. Until this existed, a signal killed the process
    // outright: `--profile work` created its directory, wrote Favicons and
    // Session Storage, and never produced a Cookies file at all — so a user who
    // logged in, stopped the browser and started it again found themselves
    // logged out, with a profile directory that looked like it was working.
    //
    // `anoa close` was always clean, which is why this went unnoticed: the
    // documented way out flushed, and every other way did not.
    //
    // The handler only sets a flag. Calling into Qt from a signal handler is
    // not safe, so a timer notices it on the event loop and asks the
    // application to quit, which unwinds main() and destroys the profile.
    static volatile std::sig_atomic_t quitRequested = 0;
    std::signal(SIGINT, [](int) { quitRequested = 1; });
    std::signal(SIGTERM, [](int) { quitRequested = 1; });
    QTimer signalPoll;
    QObject::connect(&signalPoll, &QTimer::timeout, &app, [&app]() {
        if (quitRequested)
            app.quit();
    });
    signalPoll.start(100);
#endif

    return app.exec();
}
