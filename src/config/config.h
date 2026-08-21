#pragma once

#include <QString>
#include <QStringList>

struct Config {
    int port = 9222;
    bool headless = false;
    bool noSandbox = false;
    QString profileDir;
    // Opt back in to Qt's off-the-record default profile: nothing is written
    // and nothing survives the process. The default is persistent, because a
    // browser that forgets every login is not one you can drive across
    // commands.
    bool ephemeral = false;
    // Where accepted downloads land. Empty = the platform's Downloads folder.
    QString downloadDir;
    QString profileName;
    QStringList extensionPaths;
    QString authToken;
    // Cap on Chromium renderer processes. 0 leaves Chromium to decide, which
    // means roughly one per tab. Lower numbers trade parallelism for memory —
    // see the comment in anoa_browser.cpp where it is applied.
    int maxRenderers = 0;
    // --proxy. Held whole, credentials included: Chromium's --proxy-server
    // takes no user:pass, so the host part goes on the command line and the
    // credentials are answered from proxyAuthenticationRequired instead.
    QString proxyUrl;
    // Hosts that skip the proxy. Chromium's own list syntax, passed through.
    QString proxyBypass;
    // Origins allowed to put the live view (/render) in an iframe. Empty means
    // 'self' only, because the view forwards input as well as showing pixels —
    // a page that can frame it can drive the browser. "*" opts out entirely.
    QStringList embedOrigins;
    int width = 1280;
    int height = 720;

    // Terminal mode (`anoa terminal`). CLI-only — never read from the
    // JSON/INI config file. terminalMode is set by the argv pre-scan in
    // main.cpp, not by parseArgs().
    bool terminalMode = false;
    // Set by the same pre-scan when `terminal` was given with no target at all
    // — no --term-host, no --term-port, no --cdp. The viewer then hosts its own
    // browser in-process instead of connecting to one. It cannot be derived
    // from the fields below: their defaults are indistinguishable from the user
    // typing those same values, and `--term-port 9222` must still mean "connect
    // to the anoa already on 9222".
    bool termEmbedded = false;
    QString termHost = "127.0.0.1";
    int termPort = 9222;
    QString termToken;
    int fps = 30;
    QString gfxMode = "auto";
    QString cdpUrl; // empty = use the default /render/* HTTP backend
};

// `terminalMode` is what the argv pre-scan in main.cpp already decided; it is
// passed in rather than re-derived, because the `terminal` word has been
// removed from argv by the time this runs. It lands in Config::terminalMode and
// suppresses the browser-only warnings.
Config parseArgs(int argc, char *argv[], bool terminalMode = false);
Config loadConfigFile(const QString &path);
