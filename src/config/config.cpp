#include "config.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTextStream>
#include <QUrl>

static void validatePort(int port)
{
    if (port < 1 || port > 65535) {
        QTextStream err(stderr);
        err << "Error: --port must be between 1 and 65535, got " << port << Qt::endl;
        ::exit(1);
    }
}

// Terminal-mode validators. Kept separate from validatePort() so the error
// message names the flag the user actually typed.
static void validateTermPort(int port)
{
    if (port < 1 || port > 65535) {
        QTextStream err(stderr);
        err << "Error: --term-port must be between 1 and 65535, got " << port << Qt::endl;
        ::exit(1);
    }
}

static void validateFps(int fps)
{
    if (fps < 1 || fps > 120) {
        QTextStream err(stderr);
        err << "Error: --fps must be between 1 and 120, got " << fps << Qt::endl;
        ::exit(1);
    }
}

static void validateGfxMode(const QString &mode)
{
    static const QStringList kModes{"auto", "halfblock", "iterm", "kitty"};
    if (!kModes.contains(mode)) {
        QTextStream err(stderr);
        err << "Error: --gfx must be one of auto, halfblock, iterm, kitty; got " << mode << Qt::endl;
        ::exit(1);
    }
}

static void validateCdpUrl(const QString &url)
{
    if (url.isEmpty())
        return;

    const QUrl parsed(url);
    const QString scheme = parsed.scheme().toLower();

    if (scheme == "wss") {
        QTextStream err(stderr);
        err << "Error: --cdp does not support wss://: TLS CDP endpoints are unsupported "
               "because Qt is not built with OpenSSL here; use ws:// or http://" << Qt::endl;
        ::exit(1);
    }

    if (!parsed.isValid() || parsed.host().isEmpty()
        || (scheme != "http" && scheme != "https" && scheme != "ws")) {
        QTextStream err(stderr);
        err << "Error: --cdp must be a URL with scheme http, https or ws, got " << url << Qt::endl;
        ::exit(1);
    }
}

static void validateExtensionPaths(const QStringList &paths)
{
    for (const QString &p : paths) {
        if (!QFileInfo(p).isDir()) {
            QTextStream err(stderr);
            err << "Error: extension path is not an existing directory: " << p << Qt::endl;
            ::exit(1);
        }
    }
}

Config loadConfigFile(const QString &path)
{
    Config cfg;
    QFileInfo fi(path);
    if (!fi.exists()) {
        QTextStream err(stderr);
        err << "Error: config file not found: " << path << Qt::endl;
        ::exit(1);
    }

    QString suffix = fi.suffix().toLower();
    if (suffix == "json") {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            QTextStream err(stderr);
            err << "Error: cannot open config file: " << path << Qt::endl;
            ::exit(1);
        }
        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
        if (doc.isNull()) {
            QTextStream err(stderr);
            err << "Error: invalid JSON in config file: " << parseErr.errorString() << Qt::endl;
            ::exit(1);
        }
        QJsonObject obj = doc.object();
        if (obj.contains("port"))
            cfg.port = obj["port"].toInt(9222);
        if (obj.contains("headless"))
            cfg.headless = obj["headless"].toBool(false);
        if (obj.contains("noSandbox"))
            cfg.noSandbox = obj["noSandbox"].toBool(false);
        if (obj.contains("profileDir"))
            cfg.profileDir = obj["profileDir"].toString();
        if (obj.contains("profileName"))
            cfg.profileName = obj["profileName"].toString();
        if (obj.contains("authToken"))
            cfg.authToken = obj["authToken"].toString();
        if (obj.contains("extensionPaths")) {
            for (const auto &v : obj["extensionPaths"].toArray())
                cfg.extensionPaths << v.toString();
        }
        if (obj.contains("width"))
            cfg.width = obj["width"].toInt(1280);
        if (obj.contains("height"))
            cfg.height = obj["height"].toInt(720);
    } else {
        // INI format via QSettings
        QSettings ini(path, QSettings::IniFormat);
        cfg.port = ini.value("port", 9222).toInt();
        cfg.headless = ini.value("headless", false).toBool();
        cfg.noSandbox = ini.value("noSandbox", false).toBool();
        cfg.profileDir = ini.value("profileDir").toString();
        cfg.profileName = ini.value("profileName").toString();
        cfg.authToken = ini.value("authToken").toString();
        cfg.extensionPaths = ini.value("extensionPaths").toStringList();
        cfg.extensionPaths.removeAll(QString());
        cfg.width = ini.value("width", 1280).toInt();
        cfg.height = ini.value("height", 720).toInt();
    }
    return cfg;
}

Config parseArgs(int /*argc*/, char * /*argv*/[], bool terminalMode)
{
    // QCommandLineParser requires a QCoreApplication to exist.
    // Callers must construct QApplication before calling parseArgs.
    QCommandLineParser parser;
    parser.setApplicationDescription("Anoa headless browser with CDP support");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption portOpt({"p", "port"}, "CDP listen port (1-65535, default 9222)", "port", "9222");
    QCommandLineOption headlessOpt("headless", "Run in offscreen/headless mode");
    QCommandLineOption noSandboxOpt("no-sandbox", "Disable Chromium sandbox");
    QCommandLineOption profileDirOpt("profile-dir", "Base directory for browser profiles", "dir");
    QCommandLineOption downloadDirOpt("download-dir",
        "Where downloads are saved (default: the platform Downloads folder)", "dir");
    QCommandLineOption ephemeralOpt("ephemeral",
        "Keep nothing: no cookies, no storage, gone when the process ends");
    QCommandLineOption profileNameOpt("profile", "Named profile to activate", "name");
    QCommandLineOption extensionOpt("extension", "Path to an unpacked extension directory (repeatable)", "path");
    QCommandLineOption authTokenOpt("auth-token", "Bearer token required for CDP WebSocket connections", "token");
    QCommandLineOption configOpt("config", "Path to JSON or INI config file", "file");
    QCommandLineOption embedOriginOpt("embed-origin",
        "Origin allowed to iframe the live view, e.g. https://app.example.com "
        "(repeatable; \"*\" allows any, default is same-origin only)", "origin");
    QCommandLineOption widthOpt(QStringList{"width"}, "Browser viewport/window width in pixels (default 1280)", "width");
    QCommandLineOption heightOpt(QStringList{"height"}, "Browser viewport/window height in pixels (default 720)", "height");

    // Terminal mode (`anoa terminal`). Registered on the same parser so
    // process() accepts them; deliberately named apart from --port/--auth-token,
    // which keep their browser meaning in every mode.
    QCommandLineOption termHostOpt("term-host", "Terminal mode: host of the anoa to view (default 127.0.0.1)", "host");
    QCommandLineOption termPortOpt("term-port", "Terminal mode: port of the anoa to view (1-65535, default 9222)", "port");
    QCommandLineOption termTokenOpt("term-token", "Terminal mode: bearer token for the viewed endpoint", "token");
    QCommandLineOption fpsOpt("fps", "Terminal mode: frame rate (1-120, default 30)", "n");
    QCommandLineOption gfxOpt("gfx", "Terminal mode: rendering backend (auto|halfblock|iterm|kitty, default auto)", "mode");
    QCommandLineOption cdpOpt("cdp", "Terminal mode: attach to an external CDP endpoint (http://, https:// or ws:// URL) instead of /render/*", "url");

    parser.addOption(portOpt);
    parser.addOption(headlessOpt);
    parser.addOption(noSandboxOpt);
    parser.addOption(profileDirOpt);
    parser.addOption(downloadDirOpt);
    parser.addOption(ephemeralOpt);
    parser.addOption(profileNameOpt);
    parser.addOption(extensionOpt);
    parser.addOption(authTokenOpt);
    parser.addOption(embedOriginOpt);
    parser.addOption(configOpt);
    parser.addOption(widthOpt);
    parser.addOption(heightOpt);
    parser.addOption(termHostOpt);
    parser.addOption(termPortOpt);
    parser.addOption(termTokenOpt);
    parser.addOption(fpsOpt);
    parser.addOption(gfxOpt);
    parser.addOption(cdpOpt);

    parser.process(*QCoreApplication::instance());

    // Start from file config (if given), then let CLI values override.
    Config cfg;
    if (parser.isSet(configOpt)) {
        cfg = loadConfigFile(parser.value(configOpt));
    }

    if (parser.isSet(portOpt))
        cfg.port = parser.value(portOpt).toInt();
    if (parser.isSet(headlessOpt))
        cfg.headless = true;
    if (parser.isSet(noSandboxOpt))
        cfg.noSandbox = true;
    if (parser.isSet(profileDirOpt))
        cfg.profileDir = parser.value(profileDirOpt);
    if (parser.isSet(downloadDirOpt))
        cfg.downloadDir = parser.value(downloadDirOpt);
    if (parser.isSet(ephemeralOpt))
        cfg.ephemeral = true;
    if (parser.isSet(profileNameOpt))
        cfg.profileName = parser.value(profileNameOpt);
    if (parser.isSet(authTokenOpt))
        cfg.authToken = parser.value(authTokenOpt);
    if (parser.isSet(embedOriginOpt))
        cfg.embedOrigins = parser.values(embedOriginOpt);

    if (parser.isSet(widthOpt)) {
        bool ok = false;
        int w = parser.value(widthOpt).toInt(&ok);
        if (!ok || w <= 0) {
            QTextStream err(stderr);
            err << "Error: --width must be a positive integer, got " << parser.value(widthOpt) << Qt::endl;
            ::exit(1);
        }
        cfg.width = w;
    }
    if (parser.isSet(heightOpt)) {
        bool ok = false;
        int h = parser.value(heightOpt).toInt(&ok);
        if (!ok || h <= 0) {
            QTextStream err(stderr);
            err << "Error: --height must be a positive integer, got " << parser.value(heightOpt) << Qt::endl;
            ::exit(1);
        }
        cfg.height = h;
    }

    // Terminal-mode options are CLI-only: loadConfigFile() never sets them, so
    // the struct defaults stand in when the flags are omitted.
    if (parser.isSet(termHostOpt))
        cfg.termHost = parser.value(termHostOpt);
    if (parser.isSet(termTokenOpt))
        cfg.termToken = parser.value(termTokenOpt);
    if (parser.isSet(gfxOpt))
        cfg.gfxMode = parser.value(gfxOpt);
    if (parser.isSet(cdpOpt))
        cfg.cdpUrl = parser.value(cdpOpt);

    if (parser.isSet(termPortOpt)) {
        bool ok = false;
        int p = parser.value(termPortOpt).toInt(&ok);
        if (!ok) {
            QTextStream err(stderr);
            err << "Error: --term-port must be an integer, got " << parser.value(termPortOpt) << Qt::endl;
            ::exit(1);
        }
        cfg.termPort = p;
    }
    if (parser.isSet(fpsOpt)) {
        bool ok = false;
        int f = parser.value(fpsOpt).toInt(&ok);
        if (!ok) {
            QTextStream err(stderr);
            err << "Error: --fps must be an integer, got " << parser.value(fpsOpt) << Qt::endl;
            ::exit(1);
        }
        cfg.fps = f;
    }

    // --extension is repeatable; append CLI entries on top of file config entries.
    const QStringList cliExtensions = parser.values(extensionOpt);
    if (!cliExtensions.isEmpty())
        cfg.extensionPaths = cliExtensions;

    // Validate
    validatePort(cfg.port);
    validateExtensionPaths(cfg.extensionPaths);
    validateTermPort(cfg.termPort);
    validateFps(cfg.fps);
    validateGfxMode(cfg.gfxMode);
    validateCdpUrl(cfg.cdpUrl);

    cfg.terminalMode = terminalMode;

    // Only browser mode serves a CDP WebSocket. Terminal mode is a viewer —
    // embedded or not, it exposes nothing and demands no token — so warning
    // about an unauthenticated endpoint there names a port that was never
    // opened, and does it by writing over the viewer's first frame.
    if (cfg.authToken.isEmpty() && !terminalMode) {
        QTextStream err(stderr);
        err << "Warning: --auth-token is not set; CDP WebSocket will be unauthenticated\n";
    }

    return cfg;
}
