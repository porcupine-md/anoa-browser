#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QtTest/QtTest>

// Pull in the function under test directly.
// config.cpp is compiled into a static lib (anoa-config-lib) by the CMake target.
#include "config/config.h"

// 4x2 RGB PNG, row 0 = red/green/blue/white, row 1 = black/yellow/cyan/magenta.
// Embedded rather than generated so the test exercises decoding only — the CDP
// backend receives PNG bytes it did not produce.
static const char kTinyPngBase64[] =
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAACCAIAAADwyuo0AAAAFklEQVR42mP4z8DA"
    "AMZAwACmwWyG/wCJkAv1L5VDcAAAAABJRU5ErkJggg==";

class TestConfig : public QObject
{
    Q_OBJECT

private:
    // Result of running parseArgs in a subprocess (see the parse_args harness
    // mode below). parseArgs validates by calling ::exit(1), so it cannot be
    // driven in-process — and QCommandLineParser reads argv from the live
    // QCoreApplication, which this test binary already owns for QTest.
    struct ParseResult {
        int exitCode = -1;
        QJsonObject cfg;
    };

    static ParseResult runParseArgs(const QStringList &args)
    {
        QProcess proc;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("ANOA_TEST_HARNESS", "parse_args");
        proc.setProcessEnvironment(env);
        proc.start(QCoreApplication::applicationFilePath(), args);

        ParseResult r;
        if (!proc.waitForFinished(10000)) {
            proc.kill();
            proc.waitForFinished(2000);
            return r; // exitCode stays -1 → any QCOMPARE against 0 fails loudly
        }
        if (proc.exitStatus() != QProcess::NormalExit)
            return r;

        r.exitCode = proc.exitCode();
        r.cfg = QJsonDocument::fromJson(proc.readAllStandardOutput()).object();
        return r;
    }

    // Write content to a temp file and return its path.
    static QString writeTempJson(QTemporaryDir &dir, const QByteArray &content,
                                 const QString &name = "config.json")
    {
        QString path = dir.filePath(name);
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(content);
        f.close();
        return path;
    }

private slots:
    // CFG-01: default port when JSON has no port key
    void testDefaultPort()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({})");
        Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.port, 9222);
    }

    // CFG-02: custom port from JSON
    void testCustomPort()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"port":8080})");
        Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.port, 8080);
    }

    // CFG-05: headless flag from JSON
    void testHeadlessFlag()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"headless":true})");
        Config cfg = loadConfigFile(path);
        QVERIFY(cfg.headless);
    }

    // CFG-06: no-sandbox flag from JSON
    void testNoSandboxFlag()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"noSandbox":true})");
        Config cfg = loadConfigFile(path);
        QVERIFY(cfg.noSandbox);
    }

    // CFG-07: profile name from JSON
    void testProfileName()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"profileName":"myprofile"})");
        Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.profileName, QString("myprofile"));
    }

    // CFG-08: profile directory from JSON
    void testProfileDir()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"profileDir":"/tmp/p"})");
        Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.profileDir, QString("/tmp/p"));
    }

    // CFG-09: auth token from JSON
    void testAuthToken()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"authToken":"abc123"})");
        Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.authToken, QString("abc123"));
    }

    // CFG-10: multiple extension paths from JSON
    void testMultipleExtensionPaths()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"extensionPaths":["/p/a","/p/b"]})");
        Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.extensionPaths.size(), 2);
        QCOMPARE(cfg.extensionPaths.at(0), QString("/p/a"));
        QCOMPARE(cfg.extensionPaths.at(1), QString("/p/b"));
    }

    // CFG-11: port read from JSON config file
    void testPortFromConfigFile()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"port":8081})");
        Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.port, 8081);
    }

    // CFG-13: missing config file → process exits with code 1
    //
    // The message is asserted, not just the code. loadConfigFile() has three
    // exit(1) branches — not found, cannot open (CFG-23) and invalid JSON
    // (CFG-14) — so an exit-code-only assertion passes whichever one fires and
    // would not notice the not-exists check being dropped in favour of letting
    // the open() fail.
    void testMissingConfigFileExits()
    {
        // Run a subprocess that calls loadConfigFile on a nonexistent path.
        // The subprocess is this test binary itself invoked with a special
        // environment variable that triggers the helper slot below.
        QProcess proc;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("ANOA_TEST_HARNESS", "missing_config");
        proc.setProcessEnvironment(env);
        proc.start(QCoreApplication::applicationFilePath(), {});
        QVERIFY(proc.waitForFinished(5000));
        QCOMPARE(proc.exitCode(), 1);
        QVERIFY2(proc.readAllStandardError().contains("config file not found"),
                 "a nonexistent path reported some other error");
    }

    // CFG-14: malformed JSON config → process exits with code 1
    //
    // Same reasoning as CFG-13, and pointed at the branch next door: CFG-23
    // drives this very harness mode down the "cannot open" path, so without the
    // message check the two cases are indistinguishable.
    void testMalformedJsonConfigExits()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, "{invalid json}");

        QProcess proc;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("ANOA_TEST_HARNESS", "bad_json");
        env.insert("ANOA_TEST_HARNESS_PATH", path);
        proc.setProcessEnvironment(env);
        proc.start(QCoreApplication::applicationFilePath(), {});
        QVERIFY(proc.waitForFinished(5000));
        QCOMPARE(proc.exitCode(), 1);
        QVERIFY2(proc.readAllStandardError().contains("invalid JSON in config file"),
                 "a syntactically broken file reported some other error");
    }

    // CFG-03 & CFG-04: invalid ports — tested via shell (port validation is in
    // parseArgs which requires the full binary CLI pipeline; see
    // tests/integration/port_layout.test.sh PORT-INVALID-* cases).

    // ── Terminal mode options (`anoa terminal`) ────────────────────
    // These live on the shared parser, so an unregistered flag would make
    // QCommandLineParser::process() hard-exit with "Unknown option".

    // TERM-CFG-01: every terminal flag is accepted and lands in its own field
    void testTerminalOptionsParsed()
    {
        ParseResult r = runParseArgs({"--term-host", "10.0.0.5",
                                      "--term-port", "9333",
                                      "--term-token", "termsecret",
                                      "--fps", "60",
                                      "--gfx", "kitty",
                                      "--cdp", "ws://example.test:9222/devtools/page/ABC"});
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.cfg["termHost"].toString(), QString("10.0.0.5"));
        QCOMPARE(r.cfg["termPort"].toInt(), 9333);
        QCOMPARE(r.cfg["termToken"].toString(), QString("termsecret"));
        QCOMPARE(r.cfg["fps"].toInt(), 60);
        QCOMPARE(r.cfg["gfxMode"].toString(), QString("kitty"));
        QCOMPARE(r.cfg["cdpUrl"].toString(),
                 QString("ws://example.test:9222/devtools/page/ABC"));
    }

    // TERM-CFG-02: struct defaults stand in when the flags are omitted
    void testTerminalDefaults()
    {
        ParseResult r = runParseArgs({});
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.cfg["termHost"].toString(), QString("127.0.0.1"));
        QCOMPARE(r.cfg["termPort"].toInt(), 9222);
        QCOMPARE(r.cfg["termToken"].toString(), QString());
        QCOMPARE(r.cfg["fps"].toInt(), 30);
        QCOMPARE(r.cfg["gfxMode"].toString(), QString("auto"));
        QCOMPARE(r.cfg["cdpUrl"].toString(), QString());
    }

    // TERM-CFG-03: --port / --auth-token keep their browser meaning and are
    // NOT aliased onto the terminal connection fields
    void testBrowserOptionsAreNotAliasedToTerminalFields()
    {
        ParseResult r = runParseArgs({"--port", "8080", "--auth-token", "browsersecret"});
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.cfg["port"].toInt(), 8080);
        QCOMPARE(r.cfg["authToken"].toString(), QString("browsersecret"));
        QCOMPARE(r.cfg["termPort"].toInt(), 9222);
        QCOMPARE(r.cfg["termToken"].toString(), QString());
    }

    // TERM-CFG-04: ...and the reverse — terminal flags never touch the
    // browser's listen port or server token
    void testTerminalOptionsDoNotAffectBrowserFields()
    {
        ParseResult r = runParseArgs({"--term-port", "9333", "--term-token", "termsecret"});
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.cfg["port"].toInt(), 9222);
        QCOMPARE(r.cfg["authToken"].toString(), QString());
    }

    // TERM-CFG-05: --term-port 0 is rejected
    void testTermPortZeroRejected()
    {
        ParseResult r = runParseArgs({"--term-port", "0"});
        QVERIFY(r.exitCode != 0);
    }

    // TERM-CFG-06: --term-port above 65535 is rejected
    void testTermPortTooLargeRejected()
    {
        ParseResult r = runParseArgs({"--term-port", "70000"});
        QVERIFY(r.exitCode != 0);
    }

    // TERM-CFG-07: --fps 0 is rejected
    void testFpsZeroRejected()
    {
        ParseResult r = runParseArgs({"--fps", "0"});
        QVERIFY(r.exitCode != 0);
    }

    // TERM-CFG-08: an unknown --gfx backend is rejected
    void testUnknownGfxModeRejected()
    {
        ParseResult r = runParseArgs({"--gfx", "bogus"});
        QVERIFY(r.exitCode != 0);
    }

    // TERM-CFG-09: wss:// is rejected — Qt is not built with OpenSSL here
    void testCdpWssRejected()
    {
        ParseResult r = runParseArgs({"--cdp", "wss://example.test:9222"});
        QVERIFY(r.exitCode != 0);
    }

    // TERM-CFG-10: the ws:// target form is accepted verbatim
    void testCdpWsUrlAccepted()
    {
        ParseResult r = runParseArgs({"--cdp", "ws://example.test:9222/devtools/page/ABC"});
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.cfg["cdpUrl"].toString(),
                 QString("ws://example.test:9222/devtools/page/ABC"));
    }

    // TERM-CFG-11: the http:// discovery form is accepted verbatim
    void testCdpHttpUrlAccepted()
    {
        ParseResult r = runParseArgs({"--cdp", "http://example.test:9222"});
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.cfg["cdpUrl"].toString(), QString("http://example.test:9222"));
    }

    // ── parseArgs validation ──────────────────────────────────────────────
    //
    // Every validator below ends in ::exit(1), so each case is a subprocess.
    // port_layout.test.sh covers some of the same refusals against the shipped
    // binary; these run against config.cpp directly, which is what makes them
    // available on a machine with no WebEngine build and what puts the
    // validators under the sanitizer and coverage builds of this target.

    // CFG-15: --port outside 1-65535 is refused at both ends.
    void testInvalidPortIsRefused()
    {
        QCOMPARE(runParseArgs({"--port", "0"}).exitCode, 1);
        QCOMPARE(runParseArgs({"--port", "70000"}).exitCode, 1);
        QCOMPARE(runParseArgs({"--port", "-1"}).exitCode, 1);
        // The boundaries themselves are valid.
        QCOMPARE(runParseArgs({"--port", "1"}).exitCode, 0);
        QCOMPARE(runParseArgs({"--port", "65535"}).exitCode, 0);
    }

    // CFG-16: --width / --height must be positive integers. Non-numeric and
    // zero take different branches of the same guard (toInt's ok flag vs the
    // range test), so both are exercised.
    void testInvalidWidthAndHeightAreRefused()
    {
        QCOMPARE(runParseArgs({"--width", "abc"}).exitCode, 1);
        QCOMPARE(runParseArgs({"--width", "0"}).exitCode, 1);
        QCOMPARE(runParseArgs({"--height", "abc"}).exitCode, 1);
        QCOMPARE(runParseArgs({"--height", "0"}).exitCode, 1);

        const ParseResult ok = runParseArgs({"--width", "1024", "--height", "768"});
        QCOMPARE(ok.exitCode, 0);
        QCOMPARE(ok.cfg["width"].toInt(), 1024);
        QCOMPARE(ok.cfg["height"].toInt(), 768);
    }

    // CFG-24: --max-renderers caps Chromium renderer processes. Zero and
    // negative are refused rather than passed through, because
    // --renderer-process-limit=0 is not "no limit" to Chromium, and a browser
    // that starts and then cannot open a page is worse than one that refuses
    // the flag. The absent case must stay 0, which is what leaves Chromium to
    // decide.
    void testMaxRenderersIsValidated()
    {
        QCOMPARE(runParseArgs({"--max-renderers", "abc"}).exitCode, 1);
        QCOMPARE(runParseArgs({"--max-renderers", "0"}).exitCode, 1);
        QCOMPARE(runParseArgs({"--max-renderers", "-3"}).exitCode, 1);

        const ParseResult ok = runParseArgs({"--max-renderers", "4"});
        QCOMPARE(ok.exitCode, 0);
        QCOMPARE(ok.cfg["maxRenderers"].toInt(), 4);

        QCOMPARE(runParseArgs({}).cfg["maxRenderers"].toInt(), 0);
    }

    // CFG-25: --proxy. A bare host:port is what people type and what Chromium
    // accepts, but QUrl reads "127.0.0.1:8080" as scheme "127.0.0.1", so the
    // scheme is filled in before validating and the stored value is normalised.
    // Anything that is not a proxy scheme is refused up front: Chromium would
    // take it, fail every request, and blame the site.
    void testProxyIsValidatedAndNormalised()
    {
        QCOMPARE(runParseArgs({"--proxy", "ftp://h:1"}).exitCode, 1);
        QCOMPARE(runParseArgs({"--proxy", "://broken"}).exitCode, 1);
        QCOMPARE(runParseArgs({"--proxy", ""}).exitCode, 1);

        const ParseResult bare = runParseArgs({"--proxy", "1.2.3.4:8080"});
        QCOMPARE(bare.exitCode, 0);
        QCOMPARE(bare.cfg["proxyUrl"].toString(), QString("http://1.2.3.4:8080"));

        const ParseResult socks = runParseArgs({"--proxy", "socks5://1.2.3.4:1080"});
        QCOMPARE(socks.exitCode, 0);
        QCOMPARE(socks.cfg["proxyUrl"].toString(), QString("socks5://1.2.3.4:1080"));

        // Credentials survive parsing: they are answered from the auth signal
        // rather than put on Chromium's command line, so they must be kept.
        const ParseResult creds = runParseArgs({"--proxy", "http://bob:s3cret@p.example:3128"});
        QCOMPARE(creds.exitCode, 0);
        QCOMPARE(creds.cfg["proxyUrl"].toString(),
                 QString("http://bob:s3cret@p.example:3128"));

        const ParseResult bypass =
            runParseArgs({"--proxy", "1.2.3.4:8080", "--proxy-bypass", "localhost,*.internal"});
        QCOMPARE(bypass.cfg["proxyBypass"].toString(), QString("localhost,*.internal"));

        QCOMPARE(runParseArgs({}).cfg["proxyUrl"].toString(), QString());
    }

    // TERM-CFG-12: --term-port and --fps reject a non-numeric value before the
    // range check ever runs. TERM-CFG-05/06/07 already cover the range refusals
    // (--term-port 0 and 70000, --fps 0); this is the branch above them.
    void testNonNumericTermPortAndFpsAreRefused()
    {
        QCOMPARE(runParseArgs({"--term-port", "abc"}).exitCode, 1);
        QCOMPARE(runParseArgs({"--fps", "abc"}).exitCode, 1);
    }

    // TERM-CFG-13: a --cdp URL that parses but carries an unusable scheme is
    // refused by the general validator rather than the wss:// special case
    // (TERM-CFG-09), as is a string that is not a URL at all.
    void testCdpUrlWithUnusableSchemeIsRefused()
    {
        QCOMPARE(runParseArgs({"--cdp", "ftp://example.test:9222"}).exitCode, 1);
        QCOMPARE(runParseArgs({"--cdp", "example.test:9222"}).exitCode, 1);
        QCOMPARE(runParseArgs({"--cdp", "http://"}).exitCode, 1);
    }

    // CFG-17: --extension must name an existing directory. A file is not a
    // directory, so the same guard rejects it.
    void testExtensionPathMustBeAnExistingDirectory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = writeTempJson(dir, "{}", "not-a-dir.json");

        QCOMPARE(runParseArgs({"--extension", dir.filePath("nope")}).exitCode, 1);
        QCOMPARE(runParseArgs({"--extension", file}).exitCode, 1);

        // A real directory is accepted and replaces (not appends to) whatever
        // the config file supplied.
        const ParseResult ok = runParseArgs({"--extension", dir.path()});
        QCOMPARE(ok.exitCode, 0);
        QCOMPARE(ok.cfg["extensionPaths"].toArray().size(), 1);
        QCOMPARE(ok.cfg["extensionPaths"].toArray().at(0).toString(), dir.path());
    }

    // CFG-18: the plain CLI overrides. These have no validator of their own,
    // which is exactly why they are worth pinning — a flag silently not
    // reaching its Config field looks identical to one that does.
    void testBrowserCliOverridesReachConfig()
    {
        // The named-profile flag is --profile, not --profile-name; the Config
        // field it fills is profileName. Driving config.cpp directly also means
        // there is no argv pre-scan here, so "--profile work" parses normally
        // (see AGENTS.md on why "--profile terminal" cannot, in the real binary).
        const ParseResult r = runParseArgs({"--headless", "--no-sandbox",
                                            "--profile-dir", "/tmp/anoa-profile",
                                            "--profile", "work"});
        QCOMPARE(r.exitCode, 0);
        QVERIFY(r.cfg["headless"].toBool());
        QVERIFY(r.cfg["noSandbox"].toBool());
        QCOMPARE(r.cfg["profileDir"].toString(), QString("/tmp/anoa-profile"));
        QCOMPARE(r.cfg["profileName"].toString(), QString("work"));
    }

    // CFG-19: --config is read first and CLI options layer on top of it. This
    // is the only test that drives loadConfigFile() through parseArgs rather
    // than calling it directly.
    void testConfigFileIsLoadedThenOverriddenByCli()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTempJson(dir, R"({"port":8081,"authToken":"fromfile"})");

        const ParseResult fromFile = runParseArgs({"--config", path});
        QCOMPARE(fromFile.exitCode, 0);
        QCOMPARE(fromFile.cfg["port"].toInt(), 8081);
        QCOMPARE(fromFile.cfg["authToken"].toString(), QString("fromfile"));

        const ParseResult overridden = runParseArgs({"--config", path, "--port", "9333"});
        QCOMPARE(overridden.exitCode, 0);
        QCOMPARE(overridden.cfg["port"].toInt(), 9333);
        QCOMPARE(overridden.cfg["authToken"].toString(), QString("fromfile"));
    }

    // ── loadConfigFile formats ────────────────────────────────────────────

    // CFG-20: width and height from a JSON config file. Every other JSON key
    // is covered above; these two were the gap.
    void testWidthAndHeightFromJsonConfigFile()
    {
        QTemporaryDir dir;
        const QString path = writeTempJson(dir, R"({"width":1600,"height":900})");
        const Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.width, 1600);
        QCOMPARE(cfg.height, 900);
    }

    // CFG-21: any suffix that is not .json is read as INI via QSettings — a
    // whole second parser that had no test at all. Note the defaults differ in
    // kind from the JSON branch: QSettings substitutes them for absent keys
    // rather than leaving the Config member untouched.
    void testIniConfigFileIsParsed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("config.ini");
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("port=8082\n"
                    "headless=true\n"
                    "noSandbox=true\n"
                    "profileDir=/tmp/ini-profile\n"
                    "profileName=ini\n"
                    "authToken=inisecret\n"
                    "extensionPaths=/tmp/ext-a, /tmp/ext-b\n"
                    "width=1440\n"
                    "height=810\n");
        }

        const Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.port, 8082);
        QVERIFY(cfg.headless);
        QVERIFY(cfg.noSandbox);
        QCOMPARE(cfg.profileDir, QString("/tmp/ini-profile"));
        QCOMPARE(cfg.profileName, QString("ini"));
        QCOMPARE(cfg.authToken, QString("inisecret"));
        QCOMPARE(cfg.extensionPaths, QStringList({"/tmp/ext-a", "/tmp/ext-b"}));
        QCOMPARE(cfg.width, 1440);
        QCOMPARE(cfg.height, 810);
    }

    // CFG-22: an empty INI file falls back to the documented defaults rather
    // than to zeroes, which is what QSettings::value's second argument is for.
    void testEmptyIniConfigFileUsesDefaults()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("empty.ini");
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
        }

        const Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.port, 9222);
        QVERIFY(!cfg.headless);
        QCOMPARE(cfg.width, 1280);
        QCOMPARE(cfg.height, 720);
        QVERIFY2(cfg.extensionPaths.isEmpty(),
                 "an absent extensionPaths key produced a list with an empty entry");
    }

    // CFG-23: a path that exists but cannot be opened is a different failure
    // from one that does not exist (CFG-13), and it has its own message. A
    // directory named *.json reaches it on any platform and without depending
    // on the test user's privileges — a chmod 000 file would still open as root.
    void testUnreadableJsonConfigExits()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("adirectory.json");
        QVERIFY(QDir().mkpath(path));

        QProcess proc;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("ANOA_TEST_HARNESS", "bad_json");
        env.insert("ANOA_TEST_HARNESS_PATH", path);
        proc.setProcessEnvironment(env);
        proc.start(QCoreApplication::applicationFilePath(), {});
        QVERIFY(proc.waitForFinished(5000));
        QCOMPARE(proc.exitCode(), 1);
        QVERIFY2(proc.readAllStandardError().contains("cannot open config file"),
                 "the unreadable-file path reported some other error");
    }

    // ── QCoreApplication assumption ───────────────────────────────────────
    // Terminal mode constructs QCoreApplication, not QApplication, so that it
    // works over SSH with no display. The plan flags "QImage decodes PNG
    // without a GUI application object" as asserted-but-unverified; this test
    // is the verification, and it runs on every CI platform. This binary's
    // main() deliberately uses a plain QCoreApplication for the same reason.
    // If it ever fails, the fallback is QApplication with QT_QPA_PLATFORM
    // forced to offscreen from the argv pre-scan in src/main.cpp.

    // TERM-GUI-01: no GUI application object is alive in this process
    void testApplicationObjectIsPlainQCoreApplication()
    {
        QVERIFY(QCoreApplication::instance() != nullptr);
        QCOMPARE(QString::fromLatin1(QCoreApplication::instance()->metaObject()->className()),
                 QString("QCoreApplication"));
    }

    // TERM-GUI-02: QImage decodes PNG bytes under that plain QCoreApplication
    void testQImageDecodesPngWithoutGuiApplication()
    {
        QVERIFY2(QImageReader::supportedImageFormats().contains(QByteArrayLiteral("png")),
                 "QtGui reports no PNG reader without a GUI application object");

        const QByteArray png = QByteArray::fromBase64(QByteArray(kTinyPngBase64));
        QVERIFY(!png.isEmpty());

        QImage img;
        QVERIFY2(img.loadFromData(png, "PNG"), "QImage::loadFromData failed on a valid PNG");
        QCOMPARE(img.width(), 4);
        QCOMPARE(img.height(), 2);
        QCOMPARE(img.pixelColor(0, 0), QColor(255, 0, 0));
        QCOMPARE(img.pixelColor(3, 1), QColor(255, 0, 255));
    }

    // TERM-GUI-03: the halfblock path's downscale + RGB888 conversion also
    // works headless — this is the http_server.cpp scaling recipe in miniature
    void testQImageScaleAndConvertWithoutGuiApplication()
    {
        QImage img;
        QVERIFY(img.loadFromData(QByteArray::fromBase64(QByteArray(kTinyPngBase64)), "PNG"));

        const QImage scaled = img.scaled(2, 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QCOMPARE(scaled.size(), QSize(2, 1));

        const QImage rgb = scaled.convertToFormat(QImage::Format_RGB888);
        QCOMPARE(rgb.format(), QImage::Format_RGB888);
        QVERIFY(!rgb.isNull());
    }
};

// When invoked as a harness subprocess, run the specific exit-code scenario.
static bool runHarnessIfRequested(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QString mode = qEnvironmentVariable("ANOA_TEST_HARNESS");
    if (mode.isEmpty())
        return false;

    if (mode == "missing_config") {
        loadConfigFile("/this/path/does/not/exist/config.json");
    } else if (mode == "bad_json") {
        const QString path = qEnvironmentVariable("ANOA_TEST_HARNESS_PATH");
        loadConfigFile(path);
    } else if (mode == "parse_args") {
        // Everything after argv[0] is the CLI under test; parseArgs reads it
        // back off the QCoreApplication constructed above. On a validation
        // failure parseArgs calls ::exit(1) and nothing is printed.
        const Config cfg = parseArgs(argc, argv);
        const QJsonObject obj{
            {"port", cfg.port},
            {"authToken", cfg.authToken},
            {"termHost", cfg.termHost},
            {"termPort", cfg.termPort},
            {"termToken", cfg.termToken},
            {"fps", cfg.fps},
            {"gfxMode", cfg.gfxMode},
            {"cdpUrl", cfg.cdpUrl},
            // Browser-mode fields, reported so the CLI-override and validation
            // cases can assert on the value that survived rather than only on
            // the exit code.
            {"headless", cfg.headless},
            {"noSandbox", cfg.noSandbox},
            {"profileDir", cfg.profileDir},
            {"profileName", cfg.profileName},
            {"width", cfg.width},
            {"maxRenderers", cfg.maxRenderers},
            {"proxyUrl", cfg.proxyUrl},
            {"proxyBypass", cfg.proxyBypass},
            {"height", cfg.height},
            {"extensionPaths", QJsonArray::fromStringList(cfg.extensionPaths)},
        };
        QTextStream out(stdout);
        out << QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)) << Qt::endl;
    }
    // Reached only when nothing exited: the success path for parse_args, and an
    // unexpected success for the modes that are supposed to exit(1).
    return true; // will fall through to normal exit(0)
}

int main(int argc, char *argv[])
{
    if (runHarnessIfRequested(argc, argv))
        return 0;

    QCoreApplication app(argc, argv);
    TestConfig tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "test_config.moc"
