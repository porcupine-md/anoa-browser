#include "agent/agent_cli.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include "agent/agent_help.h"
#include "agent/agent_script.h"
#include "agent/agent_skill.h"
#include <QFileInfo>

#include "browser/tab_ids.h"
#include "cdp/cdp_client.h"
#include "config/config.h"

namespace {

QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}
QTextStream &err()
{
    static QTextStream s(stderr);
    return s;
}

// Exit codes. Distinct on purpose: an agent retrying a flaky click should not
// retry "nothing is listening", and a shell script should be able to tell the
// two apart without parsing prose.
enum ExitCode { Ok = 0, Failed = 1, Usage = 2, NoBrowser = 3 };

int fail(const QString &message, int code = Failed)
{
    err() << "anoa: " << message << Qt::endl;
    return code;
}

// ── A synchronous view of an asynchronous client ────────────────────────────
//
// CdpClient is built for the viewer, where nothing may block the frame loop.
// A command-line invocation is the opposite: it does one thing and exits, and
// the only sane shape for that is a blocking call. Each call runs a nested
// event loop, which is safe here precisely because there is no other work in
// this process to starve.
class Session
{
public:
    bool attach(const QString &host, int port, const QString &token, int timeoutMs,
                const QString &tabId = QString())
    {
        // Constructed here, not as a plain member: the bearer token is a
        // constructor argument and is not known until the config is parsed.
        m_client = std::make_unique<CdpClient>(token);
        m_client->setRequestTimeout(timeoutMs);
        // One pick, narrowed. Every command reaches its tab through the target
        // the client chooses at discovery, so nothing downstream has to know a
        // tab exists.
        m_client->setTabFilter(tabId);
        m_client->setExitOnDiscoveryFailure(false);

        QEventLoop loop;
        bool ok = false;
        QObject::connect(m_client.get(), &CdpClient::connected, &loop, [&]() {
            ok = true;
            loop.quit();
        });
        QObject::connect(m_client.get(), &CdpClient::discoveryFailed, &loop,
                         [&](const QString &why) {
                             m_why = why;
                             loop.quit();
                         });
        // One attempt only. The viewer retries forever because a session is
        // worth reconnecting; a command has a caller waiting on it.
        QObject::connect(m_client.get(), &CdpClient::retryScheduled, &loop, [&](int, int) {
            if (m_why.isEmpty())
                m_why = QStringLiteral("no answer from %1:%2").arg(host).arg(port);
            loop.quit();
        });
        QTimer::singleShot(timeoutMs, &loop, [&]() {
            if (m_why.isEmpty())
                m_why = QStringLiteral("timed out attaching to %1:%2").arg(host).arg(port);
            loop.quit();
        });

        m_client->connectToEndpoint(
            QUrl(QStringLiteral("http://%1:%2").arg(host).arg(port)));
        if (!m_client->isConnected())
            loop.exec();
        return ok || m_client->isConnected();
    }

    QString why() const { return m_why; }
    bool tabNotFound() const { return m_client && m_client->tabNotFound(); }

    CdpResult call(const QString &method, const QJsonObject &params = QJsonObject())
    {
        QEventLoop loop;
        CdpResult result;
        m_client->send(method, params, [&](const CdpResult &r) {
            result = r;
            loop.quit();
        });
        loop.exec();
        return result;
    }

    // Runtime.evaluate that keeps the OBJECT rather than its value, with the
    // helper script installed first. DOM.requestNode needs a handle, and
    // returnByValue would have serialised the element into a plain object and
    // thrown the handle away.
    CdpResult evaluateHandle(const QString &expression)
    {
        installScript();
        QJsonObject params;
        params[QStringLiteral("expression")] = expression;
        return call(QStringLiteral("Runtime.evaluate"), params);
    }

    // Runtime.evaluate with the helper script guaranteed to be installed.
    // Installing on every call rather than once is deliberate: the page may
    // have navigated since the last command, and this process has no way to
    // know that without asking. The script returns early when it is already
    // there, so the cost is a property read.
    bool installScript()
    {
        if (m_installed)
            return true;
        QJsonObject boot;
        boot[QStringLiteral("expression")] = agentScript();
        boot[QStringLiteral("returnByValue")] = true;
        const CdpResult r = call(QStringLiteral("Runtime.evaluate"), boot);
        m_installed = r.ok;
        return m_installed;
    }

    QJsonValue evaluate(const QString &expression, QString *error)
    {
        if (!installScript()) {
            if (error)
                *error = QStringLiteral("could not install the page helper");
            return QJsonValue();
        }

        QJsonObject params;
        params[QStringLiteral("expression")] = expression;
        params[QStringLiteral("returnByValue")] = true;
        params[QStringLiteral("awaitPromise")] = true;
        const CdpResult r = call(QStringLiteral("Runtime.evaluate"), params);
        if (!r.ok) {
            if (error)
                *error = r.errorMessage;
            return QJsonValue();
        }
        // A thrown exception is a result, not a transport error, so it arrives
        // in the payload and has to be dug out or it reads as success.
        const QJsonObject details =
            r.result.value(QStringLiteral("exceptionDetails")).toObject();
        if (!details.isEmpty()) {
            if (error) {
                const QJsonObject ex = details.value(QStringLiteral("exception")).toObject();
                *error = ex.value(QStringLiteral("description")).toString(
                    details.value(QStringLiteral("text")).toString(
                        QStringLiteral("evaluation failed")));
            }
            return QJsonValue();
        }
        return r.result.value(QStringLiteral("result")).toObject().value(QStringLiteral("value"));
    }

    CdpClient &client() { return *m_client; }

private:
    // Owned, and created by attach() rather than by the constructor. Held as a
    // pointer and not a reference-to-member: attach() replaces it, and a
    // reference bound to the old object would dangle the moment it did.
    std::unique_ptr<CdpClient> m_client;
    QString m_why;
    bool m_installed = false;
};

// ── argument helpers ────────────────────────────────────────────────────────

bool takeFlag(QStringList &args, const QString &name)
{
    const int i = args.indexOf(name);
    if (i < 0)
        return false;
    args.removeAt(i);
    return true;
}

QString takeOption(QStringList &args, const QString &name, const QString &fallback = QString())
{
    // --name=value first. QCommandLineParser accepts that spelling for every
    // browser option, so a user who writes --port=9222 out of habit and then
    // writes --tab=t2 has no reason to expect the second to be ignored — and
    // ignored is what it was: the value never applied and nothing said so.
    const QString joined = name + QLatin1Char('=');
    for (int i = 0; i < args.size(); ++i) {
        if (!args.at(i).startsWith(joined))
            continue;
        const QString value = args.at(i).mid(joined.size());
        args.removeAt(i);
        return value;
    }

    const int i = args.indexOf(name);
    if (i < 0 || i + 1 >= args.size())
        return fallback;
    const QString value = args.at(i + 1);
    args.removeAt(i + 1);
    args.removeAt(i);
    return value;
}

QString jsString(const QString &s)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact))
        .mid(1)
        .chopped(1); // ["..."] -> "..."
}

void printJson(const QJsonValue &v)
{
    const QJsonDocument doc = v.isArray() ? QJsonDocument(v.toArray())
                                          : QJsonDocument(v.toObject());
    out() << QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed() << Qt::endl;
}

// The default, human/agent readable rendering of a snapshot. One element per
// line so an agent can grep it, and refs first because they are what the next
// command needs.
void printSnapshot(const QJsonObject &snap)
{
    out() << snap.value(QStringLiteral("title")).toString() << Qt::endl;
    out() << snap.value(QStringLiteral("url")).toString() << Qt::endl;

    const QJsonArray headings = snap.value(QStringLiteral("headings")).toArray();
    if (!headings.isEmpty()) {
        out() << Qt::endl;
        for (const QJsonValue &h : headings) {
            const QJsonObject o = h.toObject();
            out() << QString(2 * o.value(QStringLiteral("level")).toInt(1), QLatin1Char(' '))
                  << o.value(QStringLiteral("text")).toString() << Qt::endl;
        }
    }

    const QJsonArray els = snap.value(QStringLiteral("elements")).toArray();
    out() << Qt::endl << els.size() << " interactive element"
          << (els.size() == 1 ? "" : "s") << Qt::endl;
    for (const QJsonValue &e : els) {
        const QJsonObject o = e.toObject();
        QString line = QStringLiteral("  %1  %2")
                           .arg(o.value(QStringLiteral("ref")).toString(), -5)
                           .arg(o.value(QStringLiteral("role")).toString(), -9);
        const QString name = o.value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            line += QStringLiteral("  ") + name;
        const QJsonArray st = o.value(QStringLiteral("state")).toArray();
        if (!st.isEmpty()) {
            QStringList bits;
            for (const QJsonValue &s : st)
                bits << s.toString();
            line += QStringLiteral("  [") + bits.join(QStringLiteral(" ")) + QStringLiteral("]");
        }
        out() << line << Qt::endl;
    }
}

// ── commands ────────────────────────────────────────────────────────────────

// `anoa tab ...` acts on the BROWSER, not on a page: every subcommand is a
// Target.* call the proxy answers from the tab registry, whichever page this
// client happens to be attached to. That is why nothing here honours --tab.
// `anoa upload <target> <file...>` — put files into a file input.
//
// Clicking one does nothing useful: the click asks the browser for a file
// dialog, and there is nobody to answer it. DOM.setFileInputFiles hands the
// files straight to the element and fires the change event the page is
// listening for, which is what Playwright and Puppeteer do underneath their
// own upload helpers.
int cmdUpload(Session &s, QStringList args, bool json)
{
    if (args.size() < 2) {
        return fail(QStringLiteral("upload needs a target and a file — "
                                   "try: anoa upload @e2 ./report.pdf"),
                    Usage);
    }
    const QString target = args.takeFirst();

    QJsonArray files;
    for (const QString &path : args) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile())
            return fail(QStringLiteral("no such file: %1").arg(path));
        // Chromium resolves these in the browser process, which has its own
        // working directory; a relative path would land somewhere else.
        files.append(info.absoluteFilePath());
    }

    // The element, as an object the DOM domain can turn into a node id. This
    // goes through __anoa.resolve so a ref (@e2) works exactly as a selector
    // does, rather than upload being the one command that only takes CSS.
    QString why;
    Q_UNUSED(why)
    const CdpResult handle =
        s.evaluateHandle(QStringLiteral("__anoa.resolve(%1)").arg(jsString(target)));
    const QString objectId =
        handle.result.value(QStringLiteral("result")).toObject()
              .value(QStringLiteral("objectId")).toString();
    if (objectId.isEmpty())
        return fail(QStringLiteral("no element for %1").arg(target));

    // DOM.requestNode answers out of the node map, and the map does not exist
    // until the domain has been enabled and the document walked once. Without
    // these two it fails with an empty error, which reads like the element was
    // wrong rather than the domain being asleep.
    s.call(QStringLiteral("DOM.enable"));
    s.call(QStringLiteral("DOM.getDocument"));

    const CdpResult node = s.call(QStringLiteral("DOM.requestNode"),
                                  QJsonObject{{QStringLiteral("objectId"), objectId}});
    const int nodeId = node.result.value(QStringLiteral("nodeId")).toInt();
    if (!node.ok || nodeId == 0)
        return fail(QStringLiteral("could not address %1: %2").arg(target, node.errorMessage));

    const CdpResult set = s.call(QStringLiteral("DOM.setFileInputFiles"),
                                 QJsonObject{{QStringLiteral("nodeId"), nodeId},
                                             {QStringLiteral("files"), files}});
    if (!set.ok) {
        // The error Chromium gives for a non-input element is worth passing on
        // rather than flattening: "Node is not a file input element".
        return fail(QStringLiteral("upload failed: %1").arg(set.errorMessage));
    }

    if (json) {
        QJsonObject o;
        o[QStringLiteral("target")] = target;
        o[QStringLiteral("files")] = files;
        out() << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << Qt::endl;
    } else {
        out() << "uploaded " << files.size() << " file(s) to " << target << Qt::endl;
    }
    return Ok;
}

int cmdTab(Session &s, QStringList args, bool json)
{
    static const QString kUsage =
        QStringLiteral("tab needs a subcommand: new, list, close or select");

    if (args.isEmpty())
        return fail(kUsage, Usage);
    const QString sub = args.takeFirst();

    if (sub == QLatin1String("new")) {
        const bool isolated = takeFlag(args, QStringLiteral("--isolated"));
        const QString profile = takeOption(args, QStringLiteral("--profile"));
        // A name an agent chooses beats an id it has to remember: t1 and t2
        // mean nothing three commands later, "search" and "cart" do.
        const QString name = takeOption(args, QStringLiteral("--name"));
        if (!name.isEmpty() && !isValidTabName(name)) {
            return fail(QStringLiteral("--name takes letters, digits, - and _ (max 32) "
                                       "and cannot look like an id: '") + name + QLatin1Char('\''),
                        Usage);
        }
        // Two ways of saying "not the shared jar" that mean different things:
        // a named profile persists, an isolated one does not.
        if (isolated && !profile.isEmpty())
            return fail(QStringLiteral("--profile and --isolated cannot be combined"), Usage);

        QJsonObject p;
        QString url = args.isEmpty() ? QStringLiteral("about:blank") : args.first();
        if (!url.contains(QStringLiteral("://")) && !url.startsWith(QStringLiteral("about:")))
            url = QStringLiteral("https://") + url;
        p[QStringLiteral("url")] = url;
        if (!profile.isEmpty())
            p[QStringLiteral("anoaProfile")] = profile;
        if (isolated)
            p[QStringLiteral("anoaIsolated")] = true;
        if (!name.isEmpty())
            p[QStringLiteral("anoaName")] = name;

        const CdpResult r = s.call(QStringLiteral("Target.createTarget"), p);
        if (!r.ok)
            return fail(QStringLiteral("could not open a tab: %1").arg(r.errorMessage));
        const QString targetId =
            r.result.value(QStringLiteral("targetId")).toString();

        // createTarget answers with the engine's id; the tab id is what the
        // user types next, so it is looked up rather than guessed.
        const CdpResult listed = s.call(QStringLiteral("Target.getTargets"));
        QString tabId;
        const QJsonArray infos =
            listed.result.value(QStringLiteral("targetInfos")).toArray();
        for (const QJsonValue &value : infos) {
            const QJsonObject info = value.toObject();
            if (info.value(QStringLiteral("targetId")).toString() == targetId) {
                tabId = info.value(QStringLiteral("anoaTabId")).toString();
                break;
            }
        }

        if (json) {
            QJsonObject o;
            o[QStringLiteral("tab")] = tabId;
            if (!name.isEmpty())
                o[QStringLiteral("name")] = name;
            o[QStringLiteral("targetId")] = targetId;
            out() << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact))
                  << Qt::endl;
        } else {
            // The name when there is one — it is what the caller passes to
            // --tab next, and printing the id instead would make them look it
            // up again.
            out() << (name.isEmpty() ? tabId : name) << Qt::endl;
        }
        return Ok;
    }

    if (sub == QLatin1String("list")) {
        const CdpResult r = s.call(QStringLiteral("Target.getTargets"));
        if (!r.ok)
            return fail(QStringLiteral("could not list tabs: %1").arg(r.errorMessage));
        const QJsonArray infos =
            r.result.value(QStringLiteral("targetInfos")).toArray();

        if (json) {
            QJsonArray rows;
            for (const QJsonValue &value : infos) {
                const QJsonObject info = value.toObject();
                QJsonObject row;
                row[QStringLiteral("tab")] = info.value(QStringLiteral("anoaTabId"));
                row[QStringLiteral("name")] = info.value(QStringLiteral("anoaTabName"));
                row[QStringLiteral("active")] = info.value(QStringLiteral("attached"));
                row[QStringLiteral("url")] = info.value(QStringLiteral("url"));
                row[QStringLiteral("title")] = info.value(QStringLiteral("title"));
                row[QStringLiteral("profile")] = info.value(QStringLiteral("browserContextId"));
                rows.append(row);
            }
            out() << QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact))
                  << Qt::endl;
            return Ok;
        }

        for (const QJsonValue &value : infos) {
            const QJsonObject info = value.toObject();
            out() << info.value(QStringLiteral("anoaTabId")).toString()
                  << (info.value(QStringLiteral("attached")).toBool()
                          ? QStringLiteral(" * ")
                          : QStringLiteral("   "));
            const QString tabName = info.value(QStringLiteral("anoaTabName")).toString();
            if (!tabName.isEmpty())
                out() << tabName << " ";
            out() << info.value(QStringLiteral("url")).toString();
            const QString title = info.value(QStringLiteral("title")).toString();
            if (!title.isEmpty())
                out() << " - " << title;
            out() << Qt::endl;
        }
        return Ok;
    }

    if (sub == QLatin1String("close") || sub == QLatin1String("select")) {
        if (args.isEmpty())
            return fail(QStringLiteral("tab %1 needs an id — try: anoa tab %1 t2").arg(sub),
                        Usage);
        const QString tabId = args.first();
        if (!isValidTabId(tabId) && !isValidTabName(tabId)) {
            return fail(QStringLiteral("tab takes an id like t1 or a name, not '%1'")
                            .arg(tabId), Usage);
        }

        // The registry speaks target ids, so the tab id is translated first —
        // and an id no tab answers to is caught here rather than upstream.
        const CdpResult listed = s.call(QStringLiteral("Target.getTargets"));
        QString targetId;
        const QJsonArray infos =
            listed.result.value(QStringLiteral("targetInfos")).toArray();
        for (const QJsonValue &value : infos) {
            const QJsonObject info = value.toObject();
            if (info.value(QStringLiteral("anoaTabId")).toString() == tabId
                || info.value(QStringLiteral("anoaTabName")).toString() == tabId) {
                targetId = info.value(QStringLiteral("targetId")).toString();
                break;
            }
        }
        if (targetId.isEmpty())
            return fail(QStringLiteral("no tab %1 — try: anoa tab list").arg(tabId));

        QJsonObject p;
        p[QStringLiteral("targetId")] = targetId;

        if (sub == QLatin1String("select")) {
            const CdpResult r = s.call(QStringLiteral("Target.activateTarget"), p);
            if (!r.ok)
                return fail(QStringLiteral("could not select %1: %2")
                                .arg(tabId, r.errorMessage));
            return Ok;
        }

        const CdpResult r = s.call(QStringLiteral("Target.closeTarget"), p);
        if (!r.ok)
            return fail(QStringLiteral("could not close %1: %2").arg(tabId, r.errorMessage));
        // The registry refuses the last tab. Saying so plainly beats handing
        // back a protocol result the user cannot act on.
        if (!r.result.value(QStringLiteral("success")).toBool()) {
            return fail(QStringLiteral("%1 is the only tab; stop the browser instead")
                            .arg(tabId));
        }
        return Ok;
    }

    return fail(kUsage, Usage);
}

int cmdOpen(Session &s, QStringList args, bool json)
{
    if (args.isEmpty())
        return fail(QStringLiteral("open needs a url — try: anoa open example.com"), Usage);
    QString url = args.first();
    if (!url.contains(QStringLiteral("://")) && !url.startsWith(QStringLiteral("about:")))
        url = QStringLiteral("https://") + url;

    QJsonObject p;
    p[QStringLiteral("url")] = url;
    const CdpResult r = s.call(QStringLiteral("Page.navigate"), p);
    if (!r.ok)
        return fail(QStringLiteral("navigate failed: %1").arg(r.errorMessage));

    // Page.navigate returns as soon as the navigation is committed, which is
    // before there is anything to snapshot. Poll rather than subscribe to
    // Page.loadEventFired: this process attached a moment ago and may well
    // have missed the event already.
    QElapsedTimer clock;
    clock.start();
    QString e;
    while (clock.elapsed() < 15000) {
        const QJsonValue v = s.evaluate(QStringLiteral("document.readyState"), &e);
        if (v.toString() == QStringLiteral("complete"))
            break;
        QEventLoop wait;
        QTimer::singleShot(100, &wait, &QEventLoop::quit);
        wait.exec();
    }

    const QJsonValue info = s.evaluate(QStringLiteral("__anoa.info()"), &e);
    if (json) {
        printJson(info);
    } else {
        const QJsonObject o = info.toObject();
        out() << o.value(QStringLiteral("title")).toString() << Qt::endl
              << o.value(QStringLiteral("url")).toString() << Qt::endl;
    }
    return Ok;
}

int cmdSnapshot(Session &s, QStringList args, bool json)
{
    const bool interactive = takeFlag(args, QStringLiteral("-i"))
                             || takeFlag(args, QStringLiteral("--interactive"));
    QString e;
    const QJsonValue v = s.evaluate(
        QStringLiteral("__anoa.snapshot(%1)").arg(interactive ? "true" : "false"), &e);
    if (v.isNull() || v.isUndefined())
        return fail(e.isEmpty() ? QStringLiteral("snapshot failed") : e);
    if (json)
        printJson(v);
    else
        printSnapshot(v.toObject());
    return Ok;
}

int cmdClick(Session &s, QStringList args, bool json)
{
    if (args.isEmpty())
        return fail(QStringLiteral("click needs a ref or selector — try: anoa click @e2"), Usage);
    QString e;
    const QJsonValue v =
        s.evaluate(QStringLiteral("__anoa.clickPoint(%1)").arg(jsString(args.first())), &e);
    if (v.isNull() || v.isUndefined())
        return fail(e.isEmpty() ? QStringLiteral("click failed") : e);

    const QJsonObject o = v.toObject();
    if (o.contains(QStringLiteral("error"))) {
        const QString kind = o.value(QStringLiteral("error")).toString();
        if (kind == QStringLiteral("covered")) {
            const QJsonObject by = o.value(QStringLiteral("covered_by")).toObject();
            return fail(QStringLiteral("%1 is covered by <%2> %3 — dismiss it, then re-snapshot")
                            .arg(args.first(),
                                 by.value(QStringLiteral("tag")).toString(),
                                 by.value(QStringLiteral("name")).toString()));
        }
        return fail(kind);
    }

    // A real Input event, not element.click(): the synthetic one skips hit
    // testing, so it would happily "click" through an overlay the user can see.
    QJsonObject p;
    p[QStringLiteral("x")] = o.value(QStringLiteral("x")).toDouble();
    p[QStringLiteral("y")] = o.value(QStringLiteral("y")).toDouble();
    p[QStringLiteral("button")] = QStringLiteral("left");
    p[QStringLiteral("clickCount")] = 1;
    p[QStringLiteral("type")] = QStringLiteral("mousePressed");
    p[QStringLiteral("buttons")] = 1;
    CdpResult r = s.call(QStringLiteral("Input.dispatchMouseEvent"), p);
    if (r.ok) {
        p[QStringLiteral("type")] = QStringLiteral("mouseReleased");
        p[QStringLiteral("buttons")] = 0;
        r = s.call(QStringLiteral("Input.dispatchMouseEvent"), p);
    }
    if (!r.ok)
        return fail(QStringLiteral("click failed: %1").arg(r.errorMessage));

    if (json)
        printJson(o.value(QStringLiteral("el")));
    else
        out() << "clicked " << args.first() << Qt::endl;
    return Ok;
}

int cmdFill(Session &s, QStringList args, bool json)
{
    if (args.size() < 2)
        return fail(QStringLiteral("fill needs a target and a value — "
                                   "try: anoa fill @e3 \"text\""),
                    Usage);
    QString e;
    const QJsonValue v = s.evaluate(QStringLiteral("__anoa.fill(%1, %2)")
                                        .arg(jsString(args.at(0)), jsString(args.at(1))),
                                    &e);
    const QJsonObject o = v.toObject();
    if (o.contains(QStringLiteral("error")))
        return fail(o.value(QStringLiteral("error")).toString());
    if (v.isNull() || v.isUndefined())
        return fail(e.isEmpty() ? QStringLiteral("fill failed") : e);
    if (json)
        printJson(o.value(QStringLiteral("el")));
    else
        out() << "filled " << args.at(0) << Qt::endl;
    return Ok;
}

int cmdType(Session &s, QStringList args)
{
    if (args.isEmpty())
        return fail(QStringLiteral("type needs text"), Usage);
    QJsonObject p;
    p[QStringLiteral("text")] = args.join(QLatin1Char(' '));
    const CdpResult r = s.call(QStringLiteral("Input.insertText"), p);
    return r.ok ? Ok : fail(QStringLiteral("type failed: %1").arg(r.errorMessage));
}

int cmdPress(Session &s, QStringList args)
{
    if (args.isEmpty())
        return fail(QStringLiteral("press needs a key — e.g. Enter, Tab, ArrowDown"), Usage);
    const QString key = args.first();
    // The few keys worth spelling out; anything else is passed through and
    // Chromium decides. Windows virtual key codes are what CDP wants.
    static const QHash<QString, int> codes{
        {QStringLiteral("Enter"), 13},     {QStringLiteral("Tab"), 9},
        {QStringLiteral("Escape"), 27},    {QStringLiteral("Backspace"), 8},
        {QStringLiteral("Delete"), 46},    {QStringLiteral("ArrowUp"), 38},
        {QStringLiteral("ArrowDown"), 40}, {QStringLiteral("ArrowLeft"), 37},
        {QStringLiteral("ArrowRight"), 39}, {QStringLiteral("Home"), 36},
        {QStringLiteral("End"), 35},       {QStringLiteral("PageUp"), 33},
        {QStringLiteral("PageDown"), 34},
    };
    QJsonObject p;
    p[QStringLiteral("key")] = key;
    if (codes.contains(key)) {
        p[QStringLiteral("windowsVirtualKeyCode")] = codes.value(key);
        p[QStringLiteral("nativeVirtualKeyCode")] = codes.value(key);
    }
    p[QStringLiteral("type")] = QStringLiteral("rawKeyDown");
    CdpResult r = s.call(QStringLiteral("Input.dispatchKeyEvent"), p);
    if (r.ok) {
        p[QStringLiteral("type")] = QStringLiteral("keyUp");
        r = s.call(QStringLiteral("Input.dispatchKeyEvent"), p);
    }
    return r.ok ? Ok : fail(QStringLiteral("press failed: %1").arg(r.errorMessage));
}

int cmdGet(Session &s, QStringList args, bool json)
{
    if (args.isEmpty())
        return fail(QStringLiteral("get needs a property: text, html, value or attr"), Usage);
    const QString what = args.takeFirst();
    QString e;
    QJsonValue v;
    if (what == QStringLiteral("attr")) {
        if (args.size() < 2)
            return fail(QStringLiteral("get attr needs a target and an attribute name"), Usage);
        v = s.evaluate(QStringLiteral("__anoa.attr(%1, %2)")
                           .arg(jsString(args.at(0)), jsString(args.at(1))),
                       &e);
    } else {
        const QString target = args.isEmpty() ? QString() : args.first();
        v = s.evaluate(QStringLiteral("__anoa.get(%1, %2)")
                           .arg(jsString(what), target.isEmpty() ? QStringLiteral("null")
                                                                 : jsString(target)),
                       &e);
    }
    const QJsonObject o = v.toObject();
    if (o.contains(QStringLiteral("error")))
        return fail(o.value(QStringLiteral("error")).toString());
    if (v.isNull() || v.isUndefined())
        return fail(e.isEmpty() ? QStringLiteral("get failed") : e);
    if (json)
        printJson(o);
    else
        out() << o.value(QStringLiteral("value")).toVariant().toString() << Qt::endl;
    return Ok;
}

int cmdEval(Session &s, QStringList args, bool json)
{
    if (args.isEmpty())
        return fail(QStringLiteral("eval needs an expression"), Usage);
    QString e;
    const QJsonValue v = s.evaluate(args.join(QLatin1Char(' ')), &e);
    if (!e.isEmpty())
        return fail(e);
    if (json || v.isObject() || v.isArray())
        printJson(v.isObject() || v.isArray() ? v : QJsonValue(QJsonObject{{"value", v}}));
    else
        out() << v.toVariant().toString() << Qt::endl;
    return Ok;
}

int cmdWait(Session &s, QStringList args)
{
    const int budget = takeOption(args, QStringLiteral("--timeout"), QStringLiteral("15000")).toInt();
    const QString state = takeOption(args, QStringLiteral("--state"));
    const QString text = takeOption(args, QStringLiteral("--text"));
    const QString url = takeOption(args, QStringLiteral("--url"));
    const QString fn = takeOption(args, QStringLiteral("--fn"));
    QString selector = takeOption(args, QStringLiteral("--selector"));
    QString msText = takeOption(args, QStringLiteral("--ms"));
    const bool loadFlag = takeFlag(args, QStringLiteral("--load"));
    // --network-idle takes an optional quiet window; bare means 500 ms, which
    // is long enough that one request finishing does not look like the end of a
    // burst, and short enough not to dominate the wait.
    QString netIdleMs;
    const bool netIdleFlag = takeFlag(args, QStringLiteral("--network-idle"));
    if (netIdleFlag)
        netIdleMs = takeOption(args, QStringLiteral("--network-idle-ms"), QStringLiteral("500"));
    // --download waits for every download to leave in_progress.
    const bool downloadFlag = takeFlag(args, QStringLiteral("--download"));

    // A bare positional is whichever of the two it looks like: `wait 500` is a
    // duration, `wait "#results"` is a selector. Safe to guess, because a
    // selector made only of digits would match nothing anyway.
    if (!args.isEmpty() && selector.isEmpty() && msText.isEmpty()) {
        bool numeric = false;
        args.first().toInt(&numeric);
        if (numeric)
            msText = args.takeFirst();
        else
            selector = args.takeFirst();
    }

    const bool load = loadFlag
                      || (!netIdleFlag && !downloadFlag && selector.isEmpty()
                          && msText.isEmpty() && text.isEmpty()
                          && url.isEmpty() && fn.isEmpty());

    // Downloads are browser state, so this one asks the browser rather than the
    // page — Runtime.evaluate cannot see them.
    if (downloadFlag) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < budget) {
            const CdpResult r = s.call(QStringLiteral("Anoa.getDownloads"));
            if (!r.ok)
                return fail(QStringLiteral("could not read downloads"));
            const QJsonArray list = r.result.value(QStringLiteral("downloads")).toArray();
            bool busy = false;
            for (const QJsonValue &v : list) {
                if (v.toObject().value(QStringLiteral("state")).toString()
                    == QStringLiteral("in_progress")) {
                    busy = true;
                    break;
                }
            }
            // No downloads at all counts as done: there is nothing to wait for,
            // and blocking until the timeout would only hide that.
            if (!busy)
                return Ok;
            QEventLoop loop;
            QTimer::singleShot(100, &loop, &QEventLoop::quit);
            loop.exec();
        }
        return fail(QStringLiteral("timed out after %1ms waiting for downloads").arg(budget));
    }

    if (!msText.isEmpty()) {
        QEventLoop loop;
        QTimer::singleShot(msText.toInt(), &loop, &QEventLoop::quit);
        loop.exec();
        return Ok;
    }

    // One condition expressed as JavaScript, whichever flag asked for it, so
    // the poll loop below stays a single thing rather than a branch per form.
    QString probe, what;
    if (netIdleFlag) {
        probe = QStringLiteral("__anoa.netIdle(%1)").arg(netIdleMs.toInt());
        what = QStringLiteral("the network to go quiet for %1ms").arg(netIdleMs);
    } else if (load) {
        // `wait --load` after something that triggers a navigation is the case
        // that matters, and the obvious probe gets it wrong: the *old* document
        // is still `complete` while the new one is in flight, so the wait
        // returns instantly and the next command reads the page the user was
        // trying to leave. An agent that trusts it reports the wrong page.
        //
        // So a page that is already loaded is watched for a transition first —
        // the url changing, or readyState dropping out of `complete`. Seeing
        // one means a navigation is under way and the wait continues into it.
        // Seeing none within the settle window means the page really was idle,
        // which is also a correct answer to "wait for the load".
        QString e;
        const QJsonObject before =
            s.evaluate(QStringLiteral("__anoa.info()"), &e).toObject();
        const QString startUrl = before.value(QStringLiteral("url")).toString();
        const bool startedComplete =
            before.value(QStringLiteral("ready")).toString() == QStringLiteral("complete");

        if (startedComplete) {
            static constexpr int kSettleMs = 1500;
            QElapsedTimer settle;
            settle.start();
            bool moved = false;
            while (settle.elapsed() < kSettleMs && !moved) {
                const QJsonObject now =
                    s.evaluate(QStringLiteral("__anoa.info()"), &e).toObject();
                moved = now.value(QStringLiteral("url")).toString() != startUrl
                        || now.value(QStringLiteral("ready")).toString()
                               != QStringLiteral("complete");
                if (moved)
                    break;
                QEventLoop tick;
                QTimer::singleShot(80, &tick, &QEventLoop::quit);
                tick.exec();
            }
            if (!moved)
                return Ok; // nothing was loading, and nothing started
        }

        probe = QStringLiteral("document.readyState === 'complete'");
        what = QStringLiteral("the page to finish loading");
    } else if (!text.isEmpty()) {
        probe = QStringLiteral("__anoa.hasText(%1).found").arg(jsString(text));
        what = QStringLiteral("the text \"%1\"").arg(text);
    } else if (!url.isEmpty()) {
        probe = QStringLiteral("location.href.includes(%1)").arg(jsString(url));
        what = QStringLiteral("the url to contain \"%1\"").arg(url);
    } else if (!fn.isEmpty()) {
        probe = QStringLiteral("!!(%1)").arg(fn);
        what = QStringLiteral("the condition");
    } else if (state == QStringLiteral("hidden")) {
        probe = QStringLiteral("__anoa.isHidden(%1).hidden").arg(jsString(selector));
        what = QStringLiteral("%1 to disappear").arg(selector);
    } else {
        probe = QStringLiteral("__anoa.exists(%1).found").arg(jsString(selector));
        what = QStringLiteral("%1 to appear").arg(selector);
    }

    QElapsedTimer clock;
    clock.start();
    QString e;
    while (clock.elapsed() < budget) {
        // A throwing --fn means "not yet", not "broken": `window.app.ready`
        // throws until `app` exists, which is precisely what is being awaited.
        if (s.evaluate(probe, &e).toBool())
            return Ok;
        QEventLoop wait;
        QTimer::singleShot(100, &wait, &QEventLoop::quit);
        wait.exec();
    }
    return fail(QStringLiteral("timed out after %1ms waiting for %2").arg(budget).arg(what));
}

int cmdScroll(Session &s, QStringList args)
{
    int dy = 400, dx = 0;
    if (takeFlag(args, QStringLiteral("--up")))
        dy = -400;
    const QString by = takeOption(args, QStringLiteral("--by"));
    if (!by.isEmpty())
        dy = by.toInt();
    if (takeFlag(args, QStringLiteral("--top")))
        dy = -1000000;
    if (takeFlag(args, QStringLiteral("--bottom")))
        dy = 1000000;
    QString e;
    s.evaluate(QStringLiteral("__anoa.scroll(%1, %2)").arg(dx).arg(dy), &e);
    return e.isEmpty() ? Ok : fail(e);
}

int cmdHistory(Session &s, const QString &verb)
{
    if (verb == QStringLiteral("reload")) {
        const CdpResult r = s.call(QStringLiteral("Page.reload"));
        return r.ok ? Ok : fail(QStringLiteral("reload failed: %1").arg(r.errorMessage));
    }
    const CdpResult hist = s.call(QStringLiteral("Page.getNavigationHistory"));
    if (!hist.ok)
        return fail(QStringLiteral("history unavailable: %1").arg(hist.errorMessage));
    const QJsonArray entries = hist.result.value(QStringLiteral("entries")).toArray();
    const int current = hist.result.value(QStringLiteral("currentIndex")).toInt(-1);
    const int target = current + (verb == QStringLiteral("back") ? -1 : 1);
    if (current < 0 || target < 0 || target >= entries.size())
        return fail(QStringLiteral("no %1 entry").arg(verb));
    QJsonObject p;
    p[QStringLiteral("entryId")] =
        entries.at(target).toObject().value(QStringLiteral("id")).toInt();
    const CdpResult r = s.call(QStringLiteral("Page.navigateToHistoryEntry"), p);
    return r.ok ? Ok : fail(QStringLiteral("%1 failed: %2").arg(verb, r.errorMessage));
}

int writeDecoded(const QString &path, const QString &base64, const char *what)
{
    const QByteArray bytes = QByteArray::fromBase64(base64.toLatin1());
    if (bytes.isEmpty())
        return fail(QStringLiteral("%1 came back empty").arg(QLatin1String(what)));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("cannot write %1: %2").arg(path, f.errorString()));
    f.write(bytes);
    f.close();
    out() << path << " (" << bytes.size() << " bytes)" << Qt::endl;
    return Ok;
}

int cmdScreenshot(Session &s, QStringList args)
{
    const QString path = args.isEmpty() ? QStringLiteral("screenshot.png") : args.first();
    QJsonObject p;
    p[QStringLiteral("format")] = QStringLiteral("png");
    const CdpResult r = s.call(QStringLiteral("Page.captureScreenshot"), p);
    if (!r.ok)
        return fail(QStringLiteral("screenshot failed: %1").arg(r.errorMessage));
    return writeDecoded(path, r.result.value(QStringLiteral("data")).toString(), "screenshot");
}

int cmdPdf(Session &s, QStringList args)
{
    const QString path = args.isEmpty() ? QStringLiteral("page.pdf") : args.first();
    const CdpResult r = s.call(QStringLiteral("Page.printToPDF"));
    if (!r.ok)
        return fail(QStringLiteral("pdf failed: %1").arg(r.errorMessage));
    return writeDecoded(path, r.result.value(QStringLiteral("data")).toString(), "pdf");
}

// ── cookies ─────────────────────────────────────────────────────────────────

int cmdCookies(Session &s, QStringList args, bool json)
{
    const QString action = args.isEmpty() ? QStringLiteral("get") : args.takeFirst();

    if (action == QStringLiteral("clear")) {
        const CdpResult r = s.call(QStringLiteral("Network.clearBrowserCookies"));
        return r.ok ? Ok : fail(QStringLiteral("clear failed: %1").arg(r.errorMessage));
    }

    if (action == QStringLiteral("set")) {
        if (args.size() < 2)
            return fail(QStringLiteral("cookies set needs a name and a value"), Usage);
        QString e;
        const QJsonValue info = s.evaluate(QStringLiteral("__anoa.info()"), &e);
        QJsonObject p;
        p[QStringLiteral("name")] = args.at(0);
        p[QStringLiteral("value")] = args.at(1);
        // Scope it to the page being viewed unless told otherwise — a cookie
        // with no url is rejected, and guessing a domain is worse than using
        // the one the user is looking at.
        p[QStringLiteral("url")] = takeOption(args, QStringLiteral("--url"),
                                              info.toObject().value(QStringLiteral("url")).toString());
        const CdpResult r = s.call(QStringLiteral("Network.setCookie"), p);
        if (!r.ok)
            return fail(QStringLiteral("set failed: %1").arg(r.errorMessage));
        if (!r.result.value(QStringLiteral("success")).toBool(true))
            return fail(QStringLiteral("the browser refused the cookie"));
        out() << "set " << args.at(0) << Qt::endl;
        return Ok;
    }

    if (action != QStringLiteral("get"))
        return fail(QStringLiteral("cookies takes get, set or clear"), Usage);

    const CdpResult r = s.call(QStringLiteral("Network.getCookies"));
    if (!r.ok)
        return fail(QStringLiteral("cookies failed: %1").arg(r.errorMessage));
    const QJsonArray cookies = r.result.value(QStringLiteral("cookies")).toArray();
    if (json) {
        printJson(QJsonObject{{"cookies", cookies}, {"count", cookies.size()}});
    } else {
        for (const QJsonValue &c : cookies) {
            const QJsonObject o = c.toObject();
            out() << QStringLiteral("%1  %2=%3")
                         .arg(o.value(QStringLiteral("domain")).toString(), -28)
                         .arg(o.value(QStringLiteral("name")).toString(),
                              o.value(QStringLiteral("value")).toString().left(60))
                  << Qt::endl;
        }
        if (cookies.isEmpty())
            out() << "no cookies" << Qt::endl;
    }
    return Ok;
}

// ── storage ─────────────────────────────────────────────────────────────────

int cmdStorage(Session &s, QStringList args, bool json)
{
    if (args.isEmpty())
        return fail(QStringLiteral("storage needs an area: local or session"), Usage);
    const QString area = args.takeFirst();
    if (area != QStringLiteral("local") && area != QStringLiteral("session"))
        return fail(QStringLiteral("storage area must be local or session"), Usage);

    QString action = QStringLiteral("get"), key, value;
    if (!args.isEmpty()) {
        if (args.first() == QStringLiteral("set") || args.first() == QStringLiteral("clear")
            || args.first() == QStringLiteral("remove")) {
            action = args.takeFirst();
        }
        if (!args.isEmpty())
            key = args.takeFirst();
        if (!args.isEmpty())
            value = args.takeFirst();
    }
    if (action == QStringLiteral("set") && (key.isEmpty() || value.isNull()))
        return fail(QStringLiteral("storage set needs a key and a value"), Usage);

    QString e;
    const QJsonValue v = s.evaluate(QStringLiteral("__anoa.storage(%1, %2, %3, %4)")
                                        .arg(jsString(area), jsString(action),
                                             key.isEmpty() ? QStringLiteral("null") : jsString(key),
                                             value.isNull() ? QStringLiteral("null")
                                                            : jsString(value)),
                                    &e);
    if (v.isNull() || v.isUndefined())
        return fail(e.isEmpty() ? QStringLiteral("storage failed") : e);
    const QJsonObject o = v.toObject();
    if (json) {
        printJson(o);
    } else if (o.contains(QStringLiteral("items"))) {
        const QJsonObject items = o.value(QStringLiteral("items")).toObject();
        for (auto it = items.begin(); it != items.end(); ++it)
            out() << QStringLiteral("%1  %2").arg(it.key(), -24).arg(it.value().toString().left(80))
                  << Qt::endl;
        if (items.isEmpty())
            out() << "empty" << Qt::endl;
    } else if (o.contains(QStringLiteral("value"))) {
        out() << o.value(QStringLiteral("value")).toVariant().toString() << Qt::endl;
    } else {
        out() << "ok" << Qt::endl;
    }
    return Ok;
}

// ── set: viewport, device, geolocation, offline, headers, media ─────────────

// A small table rather than a device database. These are the ones people
// actually name; anything else is `set viewport w h scale`, which is what the
// presets expand to anyway.
struct DevicePreset {
    const char *name;
    int width;
    int height;
    double scale;
    bool mobile;
};
const DevicePreset kDevices[] = {
    {"iphone-se", 375, 667, 2.0, true},   {"iphone-14", 390, 844, 3.0, true},
    {"iphone-14-pro-max", 430, 932, 3.0, true}, {"pixel-7", 412, 915, 2.625, true},
    {"ipad", 810, 1080, 2.0, true},       {"ipad-pro", 1024, 1366, 2.0, true},
    {"desktop", 1280, 720, 1.0, false},   {"desktop-hidpi", 1440, 900, 2.0, false},
};

int applyViewport(Session &s, int w, int h, double scale, bool mobile)
{
    QJsonObject p;
    p[QStringLiteral("width")] = w;
    p[QStringLiteral("height")] = h;
    p[QStringLiteral("deviceScaleFactor")] = scale;
    p[QStringLiteral("mobile")] = mobile;
    const CdpResult r = s.call(QStringLiteral("Emulation.setDeviceMetricsOverride"), p);
    if (!r.ok)
        return fail(QStringLiteral("viewport failed: %1").arg(r.errorMessage));
    out() << w << "x" << h << " @" << scale << (mobile ? " mobile" : "") << Qt::endl;
    return Ok;
}

int cmdSet(Session &s, QStringList args)
{
    if (args.isEmpty())
        return fail(QStringLiteral("set takes viewport, device, geo, offline, headers or media"),
                    Usage);
    const QString what = args.takeFirst();

    if (what == QStringLiteral("viewport")) {
        if (args.size() < 2)
            return fail(QStringLiteral("set viewport needs a width and a height"), Usage);
        const double scale = args.size() > 2 ? args.at(2).toDouble() : 1.0;
        return applyViewport(s, args.at(0).toInt(), args.at(1).toInt(),
                             scale > 0 ? scale : 1.0, false);
    }

    if (what == QStringLiteral("device")) {
        if (args.isEmpty()) {
            for (const DevicePreset &d : kDevices)
                out() << QStringLiteral("  %1  %2x%3 @%4")
                             .arg(QLatin1String(d.name), -20)
                             .arg(d.width).arg(d.height).arg(d.scale)
                      << Qt::endl;
            return Ok;
        }
        const QString wanted = args.first().toLower().replace(QLatin1Char(' '), QLatin1Char('-'));
        for (const DevicePreset &d : kDevices) {
            if (wanted == QLatin1String(d.name))
                return applyViewport(s, d.width, d.height, d.scale, d.mobile);
        }
        return fail(QStringLiteral("no device preset '%1' — run `anoa set device` for the list")
                        .arg(args.first()));
    }

    if (what == QStringLiteral("geo")) {
        if (args.size() < 2)
            return fail(QStringLiteral("set geo needs a latitude and a longitude"), Usage);
        QJsonObject p;
        p[QStringLiteral("latitude")] = args.at(0).toDouble();
        p[QStringLiteral("longitude")] = args.at(1).toDouble();
        p[QStringLiteral("accuracy")] = 10;
        const CdpResult r = s.call(QStringLiteral("Emulation.setGeolocationOverride"), p);
        return r.ok ? Ok : fail(QStringLiteral("geo failed: %1").arg(r.errorMessage));
    }

    if (what == QStringLiteral("offline")) {
        const bool on = args.isEmpty() || args.first() != QStringLiteral("off");
        QJsonObject p;
        p[QStringLiteral("offline")] = on;
        p[QStringLiteral("latency")] = 0;
        p[QStringLiteral("downloadThroughput")] = -1;
        p[QStringLiteral("uploadThroughput")] = -1;
        const CdpResult r = s.call(QStringLiteral("Network.emulateNetworkConditions"), p);
        if (!r.ok)
            return fail(QStringLiteral("offline failed: %1").arg(r.errorMessage));
        out() << (on ? "offline" : "online") << Qt::endl;
        return Ok;
    }

    if (what == QStringLiteral("headers")) {
        if (args.isEmpty())
            return fail(QStringLiteral("set headers needs a JSON object"), Usage);
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(args.first().toUtf8(), &perr);
        if (!doc.isObject())
            return fail(QStringLiteral("headers must be a JSON object: %1").arg(perr.errorString()),
                        Usage);
        QJsonObject p;
        p[QStringLiteral("headers")] = doc.object();
        const CdpResult r = s.call(QStringLiteral("Network.setExtraHTTPHeaders"), p);
        return r.ok ? Ok : fail(QStringLiteral("headers failed: %1").arg(r.errorMessage));
    }

    if (what == QStringLiteral("media")) {
        const QString scheme = args.isEmpty() ? QStringLiteral("light") : args.first();
        QJsonObject feature;
        feature[QStringLiteral("name")] = QStringLiteral("prefers-color-scheme");
        feature[QStringLiteral("value")] = scheme;
        QJsonObject p;
        p[QStringLiteral("features")] = QJsonArray{feature};
        const CdpResult r = s.call(QStringLiteral("Emulation.setEmulatedMedia"), p);
        if (!r.ok)
            return fail(QStringLiteral("media failed: %1").arg(r.errorMessage));
        out() << scheme << Qt::endl;
        return Ok;
    }

    return fail(QStringLiteral("unknown set target: %1").arg(what), Usage);
}

// ── find ────────────────────────────────────────────────────────────────────

int cmdFind(Session &s, QStringList args, bool json)
{
    if (args.size() < 2)
        return fail(QStringLiteral("find needs a kind and a value — "
                                   "try: anoa find role button"),
                    Usage);
    const QString kind = args.takeFirst();
    const QString needle = args.takeFirst();
    const int nth = takeOption(args, QStringLiteral("--nth"), QStringLiteral("0")).toInt();

    QString e;
    const QJsonValue v = s.evaluate(QStringLiteral("__anoa.find(%1, %2, %3)")
                                        .arg(jsString(kind), jsString(needle))
                                        .arg(nth),
                                    &e);
    const QJsonObject o = v.toObject();
    if (o.contains(QStringLiteral("error")))
        return fail(o.value(QStringLiteral("error")).toString());
    if (v.isNull() || v.isUndefined())
        return fail(e.isEmpty() ? QStringLiteral("find failed") : e);

    if (json) {
        printJson(o);
        return Ok;
    }
    const QJsonArray matches = o.value(QStringLiteral("matches")).toArray();
    for (const QJsonValue &m : matches) {
        const QJsonObject el = m.toObject();
        out() << QStringLiteral("  %1  %2  %3")
                     .arg(el.value(QStringLiteral("ref")).toString(), -5)
                     .arg(el.value(QStringLiteral("role")).toString(), -9)
                     .arg(el.value(QStringLiteral("name")).toString())
              << Qt::endl;
    }
    if (matches.isEmpty()) {
        out() << "no match" << Qt::endl;
        return Failed;
    }
    return Ok;
}

// ── console, errors, network ────────────────────────────────────────────────

int cmdRecorded(Session &s, const QString &verb, QStringList args, bool json)
{
    if (takeFlag(args, QStringLiteral("--clear"))) {
        QString e;
        s.evaluate(QStringLiteral("__anoa.clearHistory()"), &e);
        return e.isEmpty() ? Ok : fail(e);
    }

    const QString level = takeOption(args, QStringLiteral("--level"));
    QString call;
    if (verb == QStringLiteral("console"))
        call = QStringLiteral("__anoa.console(%1)")
                   .arg(level.isEmpty() ? QStringLiteral("null") : jsString(level));
    else if (verb == QStringLiteral("errors"))
        call = QStringLiteral("__anoa.pageErrors()");
    else
        call = QStringLiteral("__anoa.network()");

    QString e;
    const QJsonValue v = s.evaluate(call, &e);
    if (v.isNull() || v.isUndefined())
        return fail(e.isEmpty() ? QStringLiteral("%1 failed").arg(verb) : e);
    const QJsonObject o = v.toObject();

    if (json) {
        printJson(o);
        return Ok;
    }
    const QJsonArray entries = o.value(QStringLiteral("entries")).toArray();
    for (const QJsonValue &en : entries) {
        const QJsonObject x = en.toObject();
        if (verb == QStringLiteral("network")) {
            out() << QStringLiteral("  %1  %2  %3ms  %4")
                         .arg(x.value(QStringLiteral("method")).toString(), -6)
                         .arg(x.value(QStringLiteral("status")).toInt())
                         .arg(x.value(QStringLiteral("ms")).toInt())
                         .arg(x.value(QStringLiteral("url")).toString())
                  << Qt::endl;
        } else if (verb == QStringLiteral("errors")) {
            out() << "  " << x.value(QStringLiteral("text")).toString();
            const QString src = x.value(QStringLiteral("source")).toString();
            if (!src.isEmpty())
                out() << "  (" << src << ":" << x.value(QStringLiteral("line")).toInt() << ")";
            out() << Qt::endl;
        } else {
            out() << QStringLiteral("  %1  %2")
                         .arg(x.value(QStringLiteral("level")).toString(), -5)
                         .arg(x.value(QStringLiteral("text")).toString())
                  << Qt::endl;
        }
    }
    if (entries.isEmpty()) {
        // Say why it might be empty rather than leaving the agent guessing:
        // the recorder only sees what happened after the page last loaded.
        out() << "nothing recorded since the page loaded" << Qt::endl;
    }
    return Ok;
}

// ── mouse ───────────────────────────────────────────────────────────────────

int cmdMouse(Session &s, QStringList args)
{
    if (args.isEmpty())
        return fail(QStringLiteral("mouse takes move, down, up or wheel"), Usage);
    const QString action = args.takeFirst();

    QJsonObject p;
    if (action == QStringLiteral("wheel")) {
        if (args.isEmpty())
            return fail(QStringLiteral("mouse wheel needs a dy"), Usage);
        p[QStringLiteral("type")] = QStringLiteral("mouseWheel");
        p[QStringLiteral("x")] = args.size() > 2 ? args.at(1).toInt() : 10;
        p[QStringLiteral("y")] = args.size() > 2 ? args.at(2).toInt() : 10;
        p[QStringLiteral("deltaY")] = args.at(0).toInt();
        p[QStringLiteral("deltaX")] = 0;
    } else {
        if (args.size() < 2 && action == QStringLiteral("move"))
            return fail(QStringLiteral("mouse move needs x and y"), Usage);
        p[QStringLiteral("x")] = args.size() > 0 ? args.at(0).toInt() : 0;
        p[QStringLiteral("y")] = args.size() > 1 ? args.at(1).toInt() : 0;
        p[QStringLiteral("button")] = QStringLiteral("left");
        if (action == QStringLiteral("move")) {
            p[QStringLiteral("type")] = QStringLiteral("mouseMoved");
        } else if (action == QStringLiteral("down")) {
            p[QStringLiteral("type")] = QStringLiteral("mousePressed");
            p[QStringLiteral("clickCount")] = 1;
            p[QStringLiteral("buttons")] = 1;
        } else if (action == QStringLiteral("up")) {
            p[QStringLiteral("type")] = QStringLiteral("mouseReleased");
            p[QStringLiteral("clickCount")] = 1;
            p[QStringLiteral("buttons")] = 0;
        } else {
            return fail(QStringLiteral("unknown mouse action: %1").arg(action), Usage);
        }
    }
    const CdpResult r = s.call(QStringLiteral("Input.dispatchMouseEvent"), p);
    return r.ok ? Ok : fail(QStringLiteral("mouse failed: %1").arg(r.errorMessage));
}

int cmdStatus(Session &s, const QString &host, int port, bool json)
{
    QString e;
    const QJsonValue v = s.evaluate(QStringLiteral("__anoa.info()"), &e);
    if (v.isNull() || v.isUndefined())
        return fail(e.isEmpty() ? QStringLiteral("status failed") : e);
    if (json) {
        printJson(v);
    } else {
        const QJsonObject o = v.toObject();
        const QJsonObject size = o.value(QStringLiteral("size")).toObject();
        // The address the user gave, not the one the client ended up dialling.
        // Discovery resolves :9222 to the CDP proxy on :9224, and reporting the
        // latter reads as "--port was ignored" to anyone who passed the former.
        out() << "attached  " << host << ":" << port << "  (CDP proxy on "
              << s.client().description() << ")" << Qt::endl
              << "title     " << o.value(QStringLiteral("title")).toString() << Qt::endl
              << "url       " << o.value(QStringLiteral("url")).toString() << Qt::endl
              << "viewport  " << size.value(QStringLiteral("w")).toInt() << "x"
              << size.value(QStringLiteral("h")).toInt() << Qt::endl
              << "state     " << o.value(QStringLiteral("ready")).toString() << Qt::endl;
    }
    return Ok;
}

} // namespace

bool isAgentCommand(const QString &verb)
{
    static const QStringList verbs{
        QStringLiteral("open"),   QStringLiteral("goto"),     QStringLiteral("snapshot"),
        QStringLiteral("click"),  QStringLiteral("fill"),     QStringLiteral("type"),
        QStringLiteral("press"),  QStringLiteral("get"),      QStringLiteral("eval"),
        QStringLiteral("wait"),   QStringLiteral("scroll"),   QStringLiteral("back"),
        QStringLiteral("forward"), QStringLiteral("reload"),  QStringLiteral("screenshot"),
        QStringLiteral("pdf"),    QStringLiteral("status"),   QStringLiteral("skills"),
        QStringLiteral("help"),   QStringLiteral("cookies"),  QStringLiteral("storage"),
        QStringLiteral("set"),    QStringLiteral("find"),     QStringLiteral("console"),
        QStringLiteral("errors"), QStringLiteral("network"),  QStringLiteral("mouse"),
        QStringLiteral("close"),  QStringLiteral("tab"),
        QStringLiteral("upload"), QStringLiteral("exec"), QStringLiteral("downloads"),
    };
    return verbs.contains(verb);
}

int dispatchVerb(Session &session, const QString &verb, QStringList args, bool json,
                 const QString &host, int port);

// `anoa downloads` — what the browser has downloaded, and how it went.
//
// The profile accepts downloads (Qt cancels any nobody accepts, silently) and
// records them, because the signal fires once in that process and the agent
// asking is a different process arriving later.
static int cmdDownloads(Session &s, QStringList args, bool json)
{
    Q_UNUSED(args)
    const CdpResult r = s.call(QStringLiteral("Anoa.getDownloads"));
    if (!r.ok)
        return fail(QStringLiteral("could not read downloads"));
    const QJsonArray list =
        r.result.value(QStringLiteral("downloads")).toArray();
    if (json) {
        out() << QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact))
              << Qt::endl;
        return Ok;
    }
    if (list.isEmpty()) {
        out() << "no downloads" << Qt::endl;
        return Ok;
    }
    for (const QJsonValue &v : list) {
        const QJsonObject o = v.toObject();
        const qint64 got = static_cast<qint64>(o.value(QStringLiteral("received")).toDouble());
        const qint64 tot = static_cast<qint64>(o.value(QStringLiteral("total")).toDouble());
        out() << o.value(QStringLiteral("state")).toString().leftJustified(12)
              << o.value(QStringLiteral("path")).toString();
        if (tot > 0)
            out() << "  (" << got << "/" << tot << " bytes)";
        out() << Qt::endl;
    }
    return Ok;
}

// Split one line of a batch the way a shell would, minus the parts a batch has
// no use for: quotes group, a backslash escapes the next character, and nothing
// else is special. No globbing, no variables, no subshells — a batch file is a
// list of anoa commands, not a program.
static QStringList tokenizeLine(const QString &line, bool *ok)
{
    QStringList out;
    QString cur;
    bool inTok = false, dq = false, sq = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (c == QLatin1Char('\\') && i + 1 < line.size() && !sq) {
            cur += line.at(++i);
            inTok = true;
            continue;
        }
        if (c == QLatin1Char('"') && !sq) { dq = !dq; inTok = true; continue; }
        if (c == QLatin1Char('\'') && !dq) { sq = !sq; inTok = true; continue; }
        if (c.isSpace() && !dq && !sq) {
            if (inTok) { out << cur; cur.clear(); inTok = false; }
            continue;
        }
        cur += c;
        inTok = true;
    }
    if (inTok)
        out << cur;
    if (ok)
        *ok = !dq && !sq;
    return out;
}

// `anoa exec [file]` — run many commands against one attached browser.
//
// Every ordinary command is its own process and its own CDP attach, measured at
// about 130 ms. A twenty-step flow therefore spends some two and a half seconds
// attaching before any page does anything. This pays that once.
int cmdExec(Session &session, QStringList args, bool json,
            const QString &host, int port)
{
    QString source = args.isEmpty() ? QStringLiteral("-") : args.takeFirst();
    QStringList lines;
    if (source == QStringLiteral("-")) {
        QTextStream in(stdin);
        while (!in.atEnd())
            lines << in.readLine();
    } else {
        QFile f(source);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return fail(QStringLiteral("cannot read %1").arg(source));
        QTextStream in(&f);
        while (!in.atEnd())
            lines << in.readLine();
    }

    int lineNo = 0;
    for (const QString &raw : lines) {
        ++lineNo;
        const QString line = raw.trimmed();
        // Blank lines and # comments make a batch file readable, and a batch an
        // agent generates is easier to debug when it can carry notes.
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        bool balanced = true;
        QStringList tokens = tokenizeLine(line, &balanced);
        if (!balanced)
            return fail(QStringLiteral("line %1: unbalanced quote").arg(lineNo), Usage);
        if (tokens.isEmpty())
            continue;

        // The session is attached to one tab, chosen once by `exec --tab`.
        // Honouring a per-line --tab would mean re-attaching, which is the cost
        // this command exists to avoid, so it is refused rather than ignored.
        if (tokens.contains(QStringLiteral("--tab"))) {
            return fail(QStringLiteral("line %1: --tab belongs on `exec` itself, not on a "
                                       "command inside it — one batch drives one tab")
                            .arg(lineNo),
                        Usage);
        }

        const QString verb = tokens.takeFirst();
        if (verb == QStringLiteral("exec"))
            return fail(QStringLiteral("line %1: exec cannot nest").arg(lineNo), Usage);

        const bool lineJson = json || tokens.contains(QStringLiteral("--json"));
        const int rc = dispatchVerb(session, verb, tokens, lineJson, host, port);
        // Fail fast: step five almost always depends on step four, and running
        // the rest against a page that never got there wastes time and produces
        // errors that point at the wrong line.
        if (rc != Ok) {
            err() << "anoa: batch stopped at line " << lineNo << ": " << line << Qt::endl;
            return rc;
        }
    }
    return Ok;
}

int runAgentCommand(const Config &config, const QString &verb, const QStringList &rawArgs)
{
    QStringList args = rawArgs;
    const bool json = takeFlag(args, QStringLiteral("--json"));

    // `skills` never touches the browser: an agent asks for it before it has
    // started one, which is the whole point of a discovery stub.
    if (verb == QStringLiteral("skills"))
        return runSkillsCommand(args);

    // Where to attach. Read from the command's own arguments rather than from
    // Config, because the verb took the rest of the line with it and
    // QCommandLineParser never saw these. Config supplies the defaults.
    const QString host =
        takeOption(args, QStringLiteral("--host"), config.termHost);
    bool portOk = true;
    const int port =
        takeOption(args, QStringLiteral("--port"), QString::number(config.port)).toInt(&portOk);
    const QString token =
        takeOption(args, QStringLiteral("--token"),
                   takeOption(args, QStringLiteral("--auth-token"), config.authToken));
    if (!portOk || port < 1 || port > 65535)
        return fail(QStringLiteral("--port must be 1-65535"), Usage);

    // Which tab. Validated here rather than at discovery so a typo is a usage
    // error the moment it is typed, instead of a network round trip that ends
    // in "no tab tw0".
    const QString tabId = takeOption(args, QStringLiteral("--tab"));
    if (!tabId.isEmpty() && !isValidTabId(tabId) && !isValidTabName(tabId)) {
        return fail(QStringLiteral("--tab takes an id like t1 or a name, not '")
                        + tabId + QLatin1Char('\''),
                    Usage);
    }

    Session session;
    if (!session.attach(host, port, token, 10000, tabId)) {
        // A browser that is running but has no such tab is a different problem
        // from no browser at all, and "start one first" is actively wrong
        // advice for it.
        if (session.tabNotFound()) {
            // Discovery already printed which tabs there are; only the way out
            // is missing.
            err() << "      list them with:  anoa tab list --port " << port << Qt::endl;
            return NoBrowser;
        }
        err() << "anoa: no browser on " << host << ":" << port;
        if (!session.why().isEmpty())
            err() << " (" << session.why() << ")";
        err() << Qt::endl
              << "      start one first:  anoa --headless --port " << port << Qt::endl;
        return NoBrowser;
    }

    if (verb == QStringLiteral("exec"))
        return cmdExec(session, args, json, host, port);

    return dispatchVerb(session, verb, args, json, host, port);
}

// The verb table, split out from runAgentCommand so `exec` can run it many
// times against one attached session. Everything above the split is per-process
// setup — argument parsing, and the CDP attach that costs about 130 ms — and
// that is exactly what a batch is trying not to repeat.
int dispatchVerb(Session &session, const QString &verb, QStringList args, bool json,
                 const QString &host, int port)
{
    if (verb == QStringLiteral("tab"))
        return cmdTab(session, args, json);
    if (verb == QStringLiteral("upload"))
        return cmdUpload(session, args, json);
    if (verb == QStringLiteral("downloads"))
        return cmdDownloads(session, args, json);
    if (verb == QStringLiteral("open") || verb == QStringLiteral("goto"))
        return cmdOpen(session, args, json);
    if (verb == QStringLiteral("snapshot"))
        return cmdSnapshot(session, args, json);
    if (verb == QStringLiteral("click"))
        return cmdClick(session, args, json);
    if (verb == QStringLiteral("fill"))
        return cmdFill(session, args, json);
    if (verb == QStringLiteral("type"))
        return cmdType(session, args);
    if (verb == QStringLiteral("press"))
        return cmdPress(session, args);
    if (verb == QStringLiteral("get"))
        return cmdGet(session, args, json);
    if (verb == QStringLiteral("eval"))
        return cmdEval(session, args, json);
    if (verb == QStringLiteral("wait"))
        return cmdWait(session, args);
    if (verb == QStringLiteral("scroll"))
        return cmdScroll(session, args);
    if (verb == QStringLiteral("back") || verb == QStringLiteral("forward")
        || verb == QStringLiteral("reload"))
        return cmdHistory(session, verb);
    if (verb == QStringLiteral("screenshot"))
        return cmdScreenshot(session, args);
    if (verb == QStringLiteral("pdf"))
        return cmdPdf(session, args);
    if (verb == QStringLiteral("status"))
        return cmdStatus(session, host, port, json);
    if (verb == QStringLiteral("cookies"))
        return cmdCookies(session, args, json);
    if (verb == QStringLiteral("storage"))
        return cmdStorage(session, args, json);
    if (verb == QStringLiteral("set"))
        return cmdSet(session, args);
    if (verb == QStringLiteral("find"))
        return cmdFind(session, args, json);
    if (verb == QStringLiteral("console") || verb == QStringLiteral("errors")
        || verb == QStringLiteral("network"))
        return cmdRecorded(session, verb, args, json);
    if (verb == QStringLiteral("mouse"))
        return cmdMouse(session, args);
    if (verb == QStringLiteral("close")) {
        const CdpResult r = session.call(QStringLiteral("Browser.close"));
        // QtWebEngine does not implement Browser.close; say so plainly rather
        // than reporting a CDP error the user cannot act on.
        if (!r.ok)
            return fail(QStringLiteral("this browser cannot be closed over CDP — "
                                       "stop the process that started it"));
        return Ok;
    }

    return fail(QStringLiteral("unknown command: %1").arg(verb), Usage);
}
