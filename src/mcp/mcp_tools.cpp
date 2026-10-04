#include "mcp/mcp_tools.h"

#include <QJsonArray>

namespace {

// ── schema construction ─────────────────────────────────────────────────────
// Written as helpers rather than thirty raw JSON literals: a literal per tool
// is thirty chances to misplace a brace, and the compiler cannot see any of
// them. These build the same shapes with the types checked.

QJsonObject prop(const QString &type, const QString &description)
{
    QJsonObject o;
    o[QStringLiteral("type")] = type;
    o[QStringLiteral("description")] = description;
    return o;
}

QJsonObject enumProp(const QStringList &values, const QString &description)
{
    QJsonObject o = prop(QStringLiteral("string"), description);
    QJsonArray allowed;
    for (const QString &v : values)
        allowed.append(v);
    o[QStringLiteral("enum")] = allowed;
    return o;
}

QJsonObject arrayProp(const QString &itemType, const QString &description)
{
    QJsonObject items;
    items[QStringLiteral("type")] = itemType;
    QJsonObject o = prop(QStringLiteral("array"), description);
    o[QStringLiteral("items")] = items;
    return o;
}

QJsonObject schema(const QVector<QPair<QString, QJsonObject>> &properties,
                   const QStringList &required = {})
{
    QJsonObject props;
    for (const auto &p : properties)
        props[p.first] = p.second;

    QJsonObject s;
    s[QStringLiteral("type")] = QStringLiteral("object");
    s[QStringLiteral("properties")] = props;
    if (!required.isEmpty()) {
        QJsonArray req;
        for (const QString &r : required)
            req.append(r);
        s[QStringLiteral("required")] = req;
    }
    return s;
}

// Shorthands for the arguments that recur across tools.
QJsonObject targetProp()
{
    return prop(QStringLiteral("string"),
                QStringLiteral("A ref from snapshot (@e2), or any CSS selector."));
}
QJsonObject jsonProp()
{
    return prop(QStringLiteral("boolean"),
                QStringLiteral("Return machine-readable JSON instead of text."));
}

QVector<McpTool> buildTools()
{
    QVector<McpTool> t;

    const auto add = [&t](const QString &name, const QString &verb,
                          const QString &description, const QJsonObject &inputSchema,
                          const QStringList &positional = {},
                          const QStringList &fixed = {},
                          const QVector<QPair<QString, QString>> &flags = {}) {
        t.append(McpTool{name, verb, description, inputSchema, positional, fixed, flags});
    };

    const QVector<QPair<QString, QString>> jsonFlag = {
        {QStringLiteral("json"), QStringLiteral("--json")}};

    // ── navigate ────────────────────────────────────────────────────────────
    add(QStringLiteral("browser_open"), QStringLiteral("open"),
        QStringLiteral("Navigate the tab to a URL and wait for it to load. The scheme is "
                       "optional: example.com works."),
        schema({{QStringLiteral("url"), prop(QStringLiteral("string"),
                                             QStringLiteral("Where to go."))},
                {QStringLiteral("json"), jsonProp()}},
               {QStringLiteral("url")}),
        {QStringLiteral("url")}, {}, jsonFlag);

    add(QStringLiteral("browser_back"), QStringLiteral("back"),
        QStringLiteral("Go back one entry in history."), schema({}));
    add(QStringLiteral("browser_forward"), QStringLiteral("forward"),
        QStringLiteral("Go forward one entry in history."), schema({}));
    add(QStringLiteral("browser_reload"), QStringLiteral("reload"),
        QStringLiteral("Reload the current page."), schema({}));

    add(QStringLiteral("browser_wait"), QStringLiteral("wait"),
        QStringLiteral("Wait for a condition. With no argument, waits for the load event. "
                       "network_idle is the one for a single-page app, where the document "
                       "never reloads so load returns immediately."),
        schema({{QStringLiteral("selector"), prop(QStringLiteral("string"),
                     QStringLiteral("Wait for this CSS selector to appear."))},
                {QStringLiteral("text"), prop(QStringLiteral("string"),
                     QStringLiteral("Wait for this text to appear on the page."))},
                {QStringLiteral("url"), prop(QStringLiteral("string"),
                     QStringLiteral("Wait until the URL contains this."))},
                {QStringLiteral("fn"), prop(QStringLiteral("string"),
                     QStringLiteral("Wait until this JavaScript expression is truthy."))},
                {QStringLiteral("ms"), prop(QStringLiteral("integer"),
                     QStringLiteral("Wait this many milliseconds, unconditionally."))},
                {QStringLiteral("state"), enumProp({QStringLiteral("hidden")},
                     QStringLiteral("Wait for the selector to go away rather than appear."))},
                {QStringLiteral("load"), prop(QStringLiteral("boolean"),
                     QStringLiteral("Wait for the load event."))},
                {QStringLiteral("network_idle"), prop(QStringLiteral("boolean"),
                     QStringLiteral("Wait until no fetch or XHR is outstanding and none "
                                    "finished recently."))},
                {QStringLiteral("download"), prop(QStringLiteral("boolean"),
                     QStringLiteral("Wait for downloads to finish. Returns at once when "
                                    "nothing is downloading."))},
                {QStringLiteral("timeout"), prop(QStringLiteral("integer"),
                     QStringLiteral("Give up after this many milliseconds."))}}),
        {}, {},
        {{QStringLiteral("selector"), QStringLiteral("--selector")},
         {QStringLiteral("text"), QStringLiteral("--text")},
         {QStringLiteral("url"), QStringLiteral("--url")},
         {QStringLiteral("fn"), QStringLiteral("--fn")},
         {QStringLiteral("ms"), QStringLiteral("--ms")},
         {QStringLiteral("state"), QStringLiteral("--state")},
         {QStringLiteral("load"), QStringLiteral("--load")},
         {QStringLiteral("network_idle"), QStringLiteral("--network-idle")},
         {QStringLiteral("download"), QStringLiteral("--download")},
         {QStringLiteral("timeout"), QStringLiteral("--timeout")}});

    // ── inspect ─────────────────────────────────────────────────────────────
    add(QStringLiteral("browser_snapshot"), QStringLiteral("snapshot"),
        QStringLiteral("The page as an agent can act on it: headings, and interactive "
                       "elements each carrying a ref like @e2 that later commands target. "
                       "Refs survive between calls because they live on the DOM node."),
        schema({{QStringLiteral("interactive"), prop(QStringLiteral("boolean"),
                     QStringLiteral("Interactive elements only, without the outline."))},
                {QStringLiteral("json"), jsonProp()}}),
        {}, {},
        {{QStringLiteral("interactive"), QStringLiteral("-i")},
         {QStringLiteral("json"), QStringLiteral("--json")}});

    add(QStringLiteral("browser_find"), QStringLiteral("find"),
        QStringLiteral("Locate elements by role, by visible text, or by selector."),
        schema({{QStringLiteral("by"), enumProp({QStringLiteral("role"),
                                                 QStringLiteral("text"),
                                                 QStringLiteral("selector")},
                     QStringLiteral("How to search."))},
                {QStringLiteral("value"), prop(QStringLiteral("string"),
                     QStringLiteral("The role, text or selector to look for."))},
                {QStringLiteral("nth"), prop(QStringLiteral("integer"),
                     QStringLiteral("Return only the nth match, counting from 0."))},
                {QStringLiteral("json"), jsonProp()}},
               {QStringLiteral("by"), QStringLiteral("value")}),
        {QStringLiteral("by"), QStringLiteral("value")}, {},
        {{QStringLiteral("nth"), QStringLiteral("--nth")},
         {QStringLiteral("json"), QStringLiteral("--json")}});

    add(QStringLiteral("browser_get"), QStringLiteral("get"),
        QStringLiteral("Read something off the page: its visible text, its HTML, an "
                       "input's value, or one attribute."),
        schema({{QStringLiteral("what"), enumProp({QStringLiteral("text"),
                                                   QStringLiteral("html"),
                                                   QStringLiteral("value"),
                                                   QStringLiteral("attr")},
                     QStringLiteral("Which property to read."))},
                {QStringLiteral("target"), targetProp()},
                {QStringLiteral("name"), prop(QStringLiteral("string"),
                     QStringLiteral("The attribute name, when what is attr."))},
                {QStringLiteral("json"), jsonProp()}},
               {QStringLiteral("what")}),
        {QStringLiteral("what"), QStringLiteral("target"), QStringLiteral("name")}, {},
        jsonFlag);

    add(QStringLiteral("browser_eval"), QStringLiteral("eval"),
        QStringLiteral("Run JavaScript in the page and return the result. Promises are "
                       "awaited."),
        schema({{QStringLiteral("expression"), prop(QStringLiteral("string"),
                     QStringLiteral("The JavaScript to evaluate."))}},
               {QStringLiteral("expression")}),
        {QStringLiteral("expression")});

    add(QStringLiteral("browser_status"), QStringLiteral("status"),
        QStringLiteral("Where the browser is: URL, title, and whether it is reachable."),
        schema({{QStringLiteral("json"), jsonProp()}}), {}, {}, jsonFlag);

    // ── interact ────────────────────────────────────────────────────────────
    add(QStringLiteral("browser_click"), QStringLiteral("click"),
        QStringLiteral("Click an element. The click is hit-tested: if something covers the "
                       "target, that is reported rather than clicked through, so a button "
                       "under a consent banner does not silently succeed."),
        schema({{QStringLiteral("target"), targetProp()},
                {QStringLiteral("json"), jsonProp()}},
               {QStringLiteral("target")}),
        {QStringLiteral("target")}, {}, jsonFlag);

    add(QStringLiteral("browser_fill"), QStringLiteral("fill"),
        QStringLiteral("Set an input's value and fire the events a page listens for."),
        schema({{QStringLiteral("target"), targetProp()},
                {QStringLiteral("text"), prop(QStringLiteral("string"),
                     QStringLiteral("What to put in it."))},
                {QStringLiteral("json"), jsonProp()}},
               {QStringLiteral("target"), QStringLiteral("text")}),
        {QStringLiteral("target"), QStringLiteral("text")}, {}, jsonFlag);

    add(QStringLiteral("browser_type"), QStringLiteral("type"),
        QStringLiteral("Type text at the keyboard, key by key, wherever focus is."),
        schema({{QStringLiteral("text"), prop(QStringLiteral("string"),
                     QStringLiteral("The text to type."))}},
               {QStringLiteral("text")}),
        {QStringLiteral("text")});

    add(QStringLiteral("browser_press"), QStringLiteral("press"),
        QStringLiteral("Press one key, such as Enter, Tab, Escape or ArrowDown."),
        schema({{QStringLiteral("key"), prop(QStringLiteral("string"),
                     QStringLiteral("The key name."))}},
               {QStringLiteral("key")}),
        {QStringLiteral("key")});

    add(QStringLiteral("browser_scroll"), QStringLiteral("scroll"),
        QStringLiteral("Scroll the page, or scroll an element into view."),
        schema({{QStringLiteral("target"), targetProp()},
                {QStringLiteral("dy"), prop(QStringLiteral("integer"),
                     QStringLiteral("Angle-delta units; negative scrolls down, roughly 60 "
                                    "page pixels per 120."))}}),
        {QStringLiteral("target")}, {},
        {{QStringLiteral("dy"), QStringLiteral("--dy")}});

    add(QStringLiteral("browser_mouse"), QStringLiteral("mouse"),
        QStringLiteral("Raw mouse input, for drags and hovers that click cannot express."),
        schema({{QStringLiteral("action"), enumProp({QStringLiteral("move"),
                                                     QStringLiteral("down"),
                                                     QStringLiteral("up"),
                                                     QStringLiteral("wheel")},
                     QStringLiteral("Which event to send."))},
                {QStringLiteral("x"), prop(QStringLiteral("integer"),
                     QStringLiteral("Logical pixels from the left."))},
                {QStringLiteral("y"), prop(QStringLiteral("integer"),
                     QStringLiteral("Logical pixels from the top."))}},
               {QStringLiteral("action")}),
        {QStringLiteral("action"), QStringLiteral("x"), QStringLiteral("y")});

    add(QStringLiteral("browser_upload"), QStringLiteral("upload"),
        QStringLiteral("Put files into a file input and fire its change event. A file "
                       "dialog has nobody to answer it, so this arms the input directly."),
        schema({{QStringLiteral("target"), targetProp()},
                {QStringLiteral("files"), arrayProp(QStringLiteral("string"),
                     QStringLiteral("Absolute paths to upload."))},
                {QStringLiteral("json"), jsonProp()}},
               {QStringLiteral("target"), QStringLiteral("files")}),
        {QStringLiteral("target"), QStringLiteral("files")}, {}, jsonFlag);

    // ── capture ─────────────────────────────────────────────────────────────
    add(QStringLiteral("browser_screenshot"), QStringLiteral("screenshot"),
        QStringLiteral("Save a PNG of the page."),
        schema({{QStringLiteral("path"), prop(QStringLiteral("string"),
                     QStringLiteral("Where to write it. Defaults to the working "
                                    "directory."))}}),
        {QStringLiteral("path")});

    add(QStringLiteral("browser_pdf"), QStringLiteral("pdf"),
        QStringLiteral("Print the page to PDF."),
        schema({{QStringLiteral("path"), prop(QStringLiteral("string"),
                     QStringLiteral("Where to write it."))}}),
        {QStringLiteral("path")});

    // ── state ───────────────────────────────────────────────────────────────
    add(QStringLiteral("browser_cookies"), QStringLiteral("cookies"),
        QStringLiteral("Read, set or clear cookies for the current origin."),
        schema({{QStringLiteral("action"), enumProp({QStringLiteral("list"),
                                                     QStringLiteral("set"),
                                                     QStringLiteral("clear")},
                     QStringLiteral("What to do. Defaults to listing."))},
                {QStringLiteral("name"), prop(QStringLiteral("string"),
                     QStringLiteral("Cookie name, when setting."))},
                {QStringLiteral("value"), prop(QStringLiteral("string"),
                     QStringLiteral("Cookie value, when setting."))},
                {QStringLiteral("json"), jsonProp()}}),
        {QStringLiteral("action"), QStringLiteral("name"), QStringLiteral("value")}, {},
        jsonFlag);

    add(QStringLiteral("browser_storage"), QStringLiteral("storage"),
        QStringLiteral("Read or change localStorage or sessionStorage."),
        schema({{QStringLiteral("area"), enumProp({QStringLiteral("local"),
                                                   QStringLiteral("session")},
                     QStringLiteral("Which store."))},
                {QStringLiteral("action"), enumProp({QStringLiteral("get"),
                                                     QStringLiteral("set"),
                                                     QStringLiteral("remove"),
                                                     QStringLiteral("clear")},
                     QStringLiteral("What to do. Defaults to reading it all."))},
                {QStringLiteral("key"), prop(QStringLiteral("string"),
                     QStringLiteral("The key."))},
                {QStringLiteral("value"), prop(QStringLiteral("string"),
                     QStringLiteral("The value, when setting."))},
                {QStringLiteral("json"), jsonProp()}},
               {QStringLiteral("area")}),
        {QStringLiteral("area"), QStringLiteral("action"), QStringLiteral("key"),
         QStringLiteral("value")},
        {}, jsonFlag);

    add(QStringLiteral("browser_set"), QStringLiteral("set"),
        QStringLiteral("Emulate something about the device or the network. An override "
                       "belongs to the tab and outlives this call, so a later screenshot "
                       "sees it — and a tab left offline stays offline."),
        schema({{QStringLiteral("what"), enumProp({QStringLiteral("viewport"),
                                                   QStringLiteral("device"),
                                                   QStringLiteral("geo"),
                                                   QStringLiteral("offline"),
                                                   QStringLiteral("headers"),
                                                   QStringLiteral("media")},
                     QStringLiteral("Which emulation to change."))},
                {QStringLiteral("values"), arrayProp(QStringLiteral("string"),
                     QStringLiteral("Its arguments: viewport takes width and height; "
                                    "device a preset name; geo a latitude and longitude; "
                                    "offline on or off; headers a JSON object; media dark "
                                    "or light."))}},
               {QStringLiteral("what")}),
        {QStringLiteral("what"), QStringLiteral("values")});

    // ── tabs ────────────────────────────────────────────────────────────────
    add(QStringLiteral("browser_tab"), QStringLiteral("tab"),
        QStringLiteral("Open, list, select or close tabs. Parallel work stays in its own "
                       "tab, and a name given at creation works anywhere an id does."),
        schema({{QStringLiteral("action"), enumProp({QStringLiteral("new"),
                                                     QStringLiteral("list"),
                                                     QStringLiteral("select"),
                                                     QStringLiteral("close")},
                     QStringLiteral("What to do."))},
                {QStringLiteral("target"), prop(QStringLiteral("string"),
                     QStringLiteral("A URL for new, or a tab id or name for select and "
                                    "close."))},
                {QStringLiteral("name"), prop(QStringLiteral("string"),
                     QStringLiteral("Name a new tab, so later calls can address it by "
                                    "name."))},
                {QStringLiteral("isolated"), prop(QStringLiteral("boolean"),
                     QStringLiteral("Give the new tab its own cookie jar."))},
                {QStringLiteral("json"), jsonProp()}},
               {QStringLiteral("action")}),
        {QStringLiteral("action"), QStringLiteral("target")}, {},
        {{QStringLiteral("name"), QStringLiteral("--name")},
         {QStringLiteral("isolated"), QStringLiteral("--isolated")},
         {QStringLiteral("json"), QStringLiteral("--json")}});

    // ── debug ───────────────────────────────────────────────────────────────
    const auto recorded = [&](const QString &name, const QString &verb,
                              const QString &description) {
        add(name, verb, description,
            schema({{QStringLiteral("clear"), prop(QStringLiteral("boolean"),
                         QStringLiteral("Forget what has been recorded so far."))},
                    {QStringLiteral("json"), jsonProp()}}),
            {}, {},
            {{QStringLiteral("clear"), QStringLiteral("--clear")},
             {QStringLiteral("json"), QStringLiteral("--json")}});
    };
    recorded(QStringLiteral("browser_console"), QStringLiteral("console"),
             QStringLiteral("What the page logged. Recorded inside the page, so it covers "
                            "what happened before this call."));
    recorded(QStringLiteral("browser_errors"), QStringLiteral("errors"),
             QStringLiteral("Uncaught exceptions and unhandled rejections."));
    recorded(QStringLiteral("browser_network"), QStringLiteral("network"),
             QStringLiteral("Every request the page made — the document, scripts, "
                            "stylesheets and images as well as fetch and XHR. A status of "
                            "'-' means the response disclosed none."));

    add(QStringLiteral("browser_downloads"), QStringLiteral("downloads"),
        QStringLiteral("State, path and bytes for everything the browser downloaded."),
        schema({{QStringLiteral("json"), jsonProp()}}), {}, {}, jsonFlag);

    // ── agents, and the way out ─────────────────────────────────────────────
    add(QStringLiteral("browser_exec"), QStringLiteral("exec"),
        QStringLiteral("Run several anoa commands against one connection, one per line. "
                       "Stops at the first failure and names the line."),
        schema({{QStringLiteral("script"), prop(QStringLiteral("string"),
                     QStringLiteral("The commands, newline separated."))}},
               {QStringLiteral("script")}),
        {}, {}, {});

    add(QStringLiteral("browser_skills"), QStringLiteral("skills"),
        QStringLiteral("The skill documents this binary carries: 'core' is the workflow, "
                       "'commands' the full reference."),
        schema({{QStringLiteral("what"), enumProp({QStringLiteral("list"),
                                                   QStringLiteral("core"),
                                                   QStringLiteral("commands")},
                     QStringLiteral("Which document. Defaults to listing them."))}}),
        {QStringLiteral("what")});

    add(QStringLiteral("browser_close"), QStringLiteral("close"),
        QStringLiteral("Stop the browser. Returns once the process is gone and the port "
                       "is free."),
        schema({}));

    return t;
}

} // namespace

const QVector<McpTool> &mcpTools()
{
    static const QVector<McpTool> tools = buildTools();
    return tools;
}

const McpTool *mcpToolNamed(const QString &name)
{
    for (const McpTool &t : mcpTools()) {
        if (t.name == name)
            return &t;
    }
    return nullptr;
}

bool mcpArgvFor(const McpTool &tool, const QJsonObject &args, QStringList *argv,
                QString *error)
{
    QStringList out = tool.fixed;

    // Required properties first, so a missing one is reported as itself rather
    // than as whatever the command makes of a short argument list.
    const QJsonArray required =
        tool.inputSchema.value(QStringLiteral("required")).toArray();
    for (const QJsonValue &r : required) {
        const QString key = r.toString();
        if (!args.contains(key) || args.value(key).isNull()) {
            if (error)
                *error = QStringLiteral("missing required argument: %1").arg(key);
            return false;
        }
    }

    // Positionals keep their order, and a gap ends the list: a command reads
    // argv by position, so emitting a later argument without an earlier one
    // would silently shift everything left.
    for (const QString &key : tool.positional) {
        if (!args.contains(key) || args.value(key).isNull())
            break;
        const QJsonValue v = args.value(key);
        if (v.isArray()) {
            const QJsonArray items = v.toArray();
            for (const QJsonValue &item : items)
                out << item.toVariant().toString();
        } else {
            out << v.toVariant().toString();
        }
    }

    for (const auto &flag : tool.flags) {
        if (!args.contains(flag.first) || args.value(flag.first).isNull())
            continue;
        const QJsonValue v = args.value(flag.first);
        if (v.isBool()) {
            // A boolean flag is the flag's presence; false means leave it out.
            if (v.toBool())
                out << flag.second;
        } else {
            out << flag.second << v.toVariant().toString();
        }
    }

    if (argv)
        *argv = out;
    return true;
}
