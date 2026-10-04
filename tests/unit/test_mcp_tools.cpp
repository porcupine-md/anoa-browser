#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QtTest/QtTest>

// Qt6::Core only — the table is data and a pure mapping function, which is why
// it can be tested without a browser, a window, or WebEngine.
#include "mcp/mcp_tools.h"

class TestMcpTools : public QObject
{
    Q_OBJECT

private slots:
    void everyToolHasANameAndADescription();
    void namesAreUniqueAndNamespaced();
    void everySchemaIsAnObjectSchema();
    void everyDeclaredKeyExistsInItsSchema();
    void enumsAreNonEmptyStringLists();
    void argvKeepsPositionalOrder();
    void argvStopsAtAMissingPositional();
    void argvRefusesAMissingRequiredArgument();
    void argvFlattensAnArrayArgument();
    void argvEmitsBooleanFlagsAsPresence();
    void argvEmitsValueFlagsWithTheirValue();
    void argvPrependsFixedArguments();
};

void TestMcpTools::everyToolHasANameAndADescription()
{
    QVERIFY(!mcpTools().isEmpty());
    for (const McpTool &t : mcpTools()) {
        QVERIFY2(!t.name.isEmpty(), "a tool has no name");
        QVERIFY2(!t.verb.isEmpty(), qPrintable(t.name + " has no verb"));
        // The description is what a model reads to choose a tool; an empty one
        // makes the tool unusable without making it look broken.
        QVERIFY2(t.description.size() > 20, qPrintable(t.name + " has a stub description"));
    }
}

void TestMcpTools::namesAreUniqueAndNamespaced()
{
    QSet<QString> seen;
    for (const McpTool &t : mcpTools()) {
        QVERIFY2(!seen.contains(t.name), qPrintable("duplicate tool name: " + t.name));
        seen.insert(t.name);
        // A client may hold several servers at once, so the prefix is what
        // keeps anoa's tools distinguishable from everyone else's.
        QVERIFY2(t.name.startsWith(QStringLiteral("browser_")),
                 qPrintable(t.name + " is not namespaced"));
    }
}

void TestMcpTools::everySchemaIsAnObjectSchema()
{
    for (const McpTool &t : mcpTools()) {
        QCOMPARE(t.inputSchema.value(QStringLiteral("type")).toString(),
                 QStringLiteral("object"));
        QVERIFY2(t.inputSchema.contains(QStringLiteral("properties")),
                 qPrintable(t.name + " declares no properties object"));
    }
}

// The mechanical check that earns its keep across thirty hand-written tables:
// a key named in `positional`, `flags` or `required` but absent from the schema
// would be silently ignored at call time, and the tool would look like it
// worked while dropping an argument.
void TestMcpTools::everyDeclaredKeyExistsInItsSchema()
{
    for (const McpTool &t : mcpTools()) {
        const QJsonObject props =
            t.inputSchema.value(QStringLiteral("properties")).toObject();

        for (const QString &key : t.positional) {
            QVERIFY2(props.contains(key),
                     qPrintable(t.name + ": positional '" + key + "' is not in the schema"));
        }
        for (const auto &flag : t.flags) {
            QVERIFY2(props.contains(flag.first),
                     qPrintable(t.name + ": flag '" + flag.first + "' is not in the schema"));
            QVERIFY2(flag.second.startsWith(QStringLiteral("-")),
                     qPrintable(t.name + ": flag '" + flag.first + "' maps to something "
                                "that is not an option"));
        }
        const QJsonArray required =
            t.inputSchema.value(QStringLiteral("required")).toArray();
        for (const QJsonValue &r : required) {
            QVERIFY2(props.contains(r.toString()),
                     qPrintable(t.name + ": required '" + r.toString()
                                + "' is not in the schema"));
        }
    }
}

void TestMcpTools::enumsAreNonEmptyStringLists()
{
    for (const McpTool &t : mcpTools()) {
        const QJsonObject props =
            t.inputSchema.value(QStringLiteral("properties")).toObject();
        for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
            const QJsonObject p = it.value().toObject();
            if (!p.contains(QStringLiteral("enum")))
                continue;
            const QJsonArray values = p.value(QStringLiteral("enum")).toArray();
            QVERIFY2(!values.isEmpty(),
                     qPrintable(t.name + "." + it.key() + " has an empty enum"));
            for (const QJsonValue &v : values) {
                QVERIFY2(v.isString() && !v.toString().isEmpty(),
                         qPrintable(t.name + "." + it.key() + " has a non-string enum value"));
            }
        }
    }
}

void TestMcpTools::argvKeepsPositionalOrder()
{
    const McpTool *fill = mcpToolNamed(QStringLiteral("browser_fill"));
    QVERIFY(fill);

    QJsonObject args;
    args[QStringLiteral("text")] = QStringLiteral("hello");  // deliberately first
    args[QStringLiteral("target")] = QStringLiteral("@e2");

    QStringList argv;
    QString error;
    QVERIFY2(mcpArgvFor(*fill, args, &argv, &error), qPrintable(error));
    // The table's order, not the JSON object's — a JSON object has no order to
    // rely on, which is the whole reason `positional` exists.
    QCOMPARE(argv, (QStringList{QStringLiteral("@e2"), QStringLiteral("hello")}));
}

void TestMcpTools::argvStopsAtAMissingPositional()
{
    const McpTool *get = mcpToolNamed(QStringLiteral("browser_get"));
    QVERIFY(get);

    // `what` and `name` given, `target` missing. Emitting both would shift
    // `name` into `target`'s slot and read the wrong attribute off the wrong
    // element — a wrong answer rather than an error.
    QJsonObject args;
    args[QStringLiteral("what")] = QStringLiteral("attr");
    args[QStringLiteral("name")] = QStringLiteral("href");

    QStringList argv;
    QVERIFY(mcpArgvFor(*get, args, &argv, nullptr));
    QCOMPARE(argv, (QStringList{QStringLiteral("attr")}));
}

void TestMcpTools::argvRefusesAMissingRequiredArgument()
{
    const McpTool *open = mcpToolNamed(QStringLiteral("browser_open"));
    QVERIFY(open);

    QStringList argv;
    QString error;
    QVERIFY(!mcpArgvFor(*open, QJsonObject(), &argv, &error));
    QVERIFY2(error.contains(QStringLiteral("url")), qPrintable(error));
}

void TestMcpTools::argvFlattensAnArrayArgument()
{
    const McpTool *upload = mcpToolNamed(QStringLiteral("browser_upload"));
    QVERIFY(upload);

    QJsonObject args;
    args[QStringLiteral("target")] = QStringLiteral("@e1");
    args[QStringLiteral("files")] = QJsonArray{QStringLiteral("/tmp/a.png"),
                                               QStringLiteral("/tmp/b.png")};

    QStringList argv;
    QString error;
    QVERIFY2(mcpArgvFor(*upload, args, &argv, &error), qPrintable(error));
    QCOMPARE(argv, (QStringList{QStringLiteral("@e1"), QStringLiteral("/tmp/a.png"),
                                QStringLiteral("/tmp/b.png")}));
}

void TestMcpTools::argvEmitsBooleanFlagsAsPresence()
{
    const McpTool *snapshot = mcpToolNamed(QStringLiteral("browser_snapshot"));
    QVERIFY(snapshot);

    QJsonObject on;
    on[QStringLiteral("interactive")] = true;
    QStringList argv;
    QVERIFY(mcpArgvFor(*snapshot, on, &argv, nullptr));
    QVERIFY(argv.contains(QStringLiteral("-i")));

    // False is not "pass the flag with a false value" — it is "leave it out".
    QJsonObject off;
    off[QStringLiteral("interactive")] = false;
    QVERIFY(mcpArgvFor(*snapshot, off, &argv, nullptr));
    QVERIFY(!argv.contains(QStringLiteral("-i")));
}

void TestMcpTools::argvEmitsValueFlagsWithTheirValue()
{
    const McpTool *wait = mcpToolNamed(QStringLiteral("browser_wait"));
    QVERIFY(wait);

    QJsonObject args;
    args[QStringLiteral("selector")] = QStringLiteral(".results");
    args[QStringLiteral("timeout")] = 8000;
    args[QStringLiteral("network_idle")] = true;

    QStringList argv;
    QVERIFY(mcpArgvFor(*wait, args, &argv, nullptr));
    const int sel = argv.indexOf(QStringLiteral("--selector"));
    QVERIFY(sel >= 0);
    QCOMPARE(argv.at(sel + 1), QStringLiteral(".results"));
    const int timeout = argv.indexOf(QStringLiteral("--timeout"));
    QVERIFY(timeout >= 0);
    QCOMPARE(argv.at(timeout + 1), QStringLiteral("8000"));
    QVERIFY(argv.contains(QStringLiteral("--network-idle")));
}

void TestMcpTools::argvPrependsFixedArguments()
{
    // No tool uses `fixed` today — subcommands are enums instead — but the
    // mapping supports it, and an untested branch is one that breaks the first
    // time somebody reaches for it.
    McpTool tool;
    tool.name = QStringLiteral("browser_test");
    tool.verb = QStringLiteral("tab");
    tool.fixed = QStringList{QStringLiteral("new")};
    tool.positional = QStringList{QStringLiteral("url")};
    QJsonObject props;
    props[QStringLiteral("url")] = QJsonObject{{QStringLiteral("type"),
                                                QStringLiteral("string")}};
    tool.inputSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                   {QStringLiteral("properties"), props}};

    QJsonObject args;
    args[QStringLiteral("url")] = QStringLiteral("example.com");

    QStringList argv;
    QVERIFY(mcpArgvFor(tool, args, &argv, nullptr));
    QCOMPARE(argv, (QStringList{QStringLiteral("new"), QStringLiteral("example.com")}));
}

QTEST_MAIN(TestMcpTools)
#include "test_mcp_tools.moc"
