#include "browser/browser_window.h"

#include <QAction>
#include <QClipboard>
#include <QColor>
#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPoint>
#include <QResizeEvent>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineHistory>
#include <QWebEnginePage>

#include "browser/anoa_browser.h"
#include "common/url_input.h"

namespace {

// The toolbar in one place. Colours are the flat greys of the macOS reference:
// a light chrome strip, mid-grey glyphs, and a white pill for the address that
// gains a blue ring only while it has focus.
const char kToolbarStyle[] = R"(
QWidget#anoaToolbar {
    background: #ECECEC;
    border-bottom: 1px solid #D6D6D6;
}
QToolButton {
    border: none;
    background: transparent;
    color: #6E6E6E;
    padding: 0 6px;
}
QToolButton:hover  { color: #303030; }
QToolButton:pressed { color: #101010; }
QToolButton:disabled { color: #C2C2C2; }
QToolButton::menu-indicator { image: none; }
QLineEdit {
    background: #FFFFFF;
    border: 1px solid #DCDCDC;
    border-radius: 14px;
    padding: 4px 10px;
    color: #202020;
    selection-background-color: #B4D5FE;
}
QLineEdit:focus { border: 1px solid #4A90D9; }
QWidget#anoaTabStrip {
    background: #E2E2E2;
    border-bottom: 1px solid #D0D0D0;
}
/* One tab is one widget carrying the background, with the label and the close
   inside it. They used to be two siblings in a flat row: the close was
   transparent, so on the active tab the label lit up and the x beside it did
   not, and their paddings differed enough that the accessibility tree reported
   108x21 next to 39x15 — a shorter, misaligned x floating between tabs rather
   than belonging to one. */
QWidget#anoaTab {
    background: #D8D8D8;
    border-right: 1px solid #C8C8C8;
}
QWidget#anoaTab[active="true"] { background: #F6F6F6; }

QToolButton#anoaTabLabel {
    background: transparent;
    border: none;
    color: #4A4A4A;
    padding: 4px 4px 4px 10px;
    text-align: left;
}
QWidget#anoaTab[active="true"] QToolButton#anoaTabLabel { color: #101010; }

/* Narrow, full height, and quiet until you are on it. 39px of x was most of the
   reason it read as its own button. */
QToolButton#anoaTabClose {
    background: transparent;
    border: none;
    border-radius: 3px;
    color: #9A9A9A;
    padding: 0;
    margin: 4px 6px 4px 2px;
    min-width: 16px;
    max-width: 16px;
}
QToolButton#anoaTabClose:hover { background: #C0C0C0; color: #101010; }
QWidget#anoaTab[active="true"] QToolButton#anoaTabClose:hover { background: #DADADA; }

QToolButton#anoaTabNew {
    background: transparent;
    border: none;
    color: #7A7A7A;
    padding: 0 10px;
}
QToolButton#anoaTabNew:hover { color: #101010; }
)";

} // namespace

// Renders one character into a pixmap so it can sit inside the address field.
// QLineEdit::addAction takes a QIcon and nothing else, and shipping icon files
// for six glyphs would put binary assets in a repository that has none.
QIcon BrowserWindow::glyphIcon(const QString &glyph, int pointSize)
{
    QFont font;
    font.setPointSize(pointSize);
    const QFontMetrics fm(font);
    const int side = qMax(fm.height(), fm.horizontalAdvance(glyph)) + 2;

    QPixmap pixmap(side, side);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);
    painter.setPen(QColor(0x8A, 0x8A, 0x8A));
    painter.drawText(pixmap.rect(), Qt::AlignCenter, glyph);
    painter.end();
    return QIcon(pixmap);
}

QToolButton *BrowserWindow::makeGlyphButton(const QString &glyph, const QString &tip, int pointSize)
{
    auto *button = new QToolButton(this);
    button->setText(glyph);
    button->setToolTip(tip);
    button->setCursor(Qt::ArrowCursor);
    button->setAutoRaise(true);
    QFont font = button->font();
    font.setPointSize(pointSize);
    button->setFont(font);
    return button;
}

BrowserWindow::BrowserWindow(AnoaBrowser *view, const Config &config, QWidget *parent)
    : QWidget(parent)
    , m_view(view)
{
    setWindowTitle(QStringLiteral("anoa"));

    m_back = makeGlyphButton(QStringLiteral("‹"), // SINGLE LEFT-POINTING QUOTATION MARK
                             QStringLiteral("Back (Alt+Left)"), 26);
    m_forward = makeGlyphButton(QStringLiteral("›"), // SINGLE RIGHT-POINTING QUOTATION MARK
                                QStringLiteral("Forward (Alt+Right)"), 26);
    auto *reload = makeGlyphButton(QStringLiteral("⟳"), // CLOCKWISE GAPPED CIRCLE ARROW
                                   QStringLiteral("Reload (Ctrl+R)"), 20);

    m_urlEdit = new QLineEdit(this);
    m_urlEdit->setPlaceholderText(QString());
    m_urlEdit->setFrame(false);
    // The magnifier sits inside the pill on the left, and the bookmark star
    // inside it on the right, exactly as in the reference. addAction() is what
    // puts them *within* the field's rounded rect rather than beside it.
    m_urlEdit->addAction(glyphIcon(QStringLiteral("⌕"), 15), // TELEPHONE RECORDER (magnifier)
                         QLineEdit::LeadingPosition);
    m_star = m_urlEdit->addAction(glyphIcon(QStringLiteral("☆"), 16), // WHITE STAR
                                  QLineEdit::TrailingPosition);
    m_star->setToolTip(QStringLiteral("Bookmark this page"));
    connect(m_star, &QAction::triggered, this, &BrowserWindow::onBookmark);

    m_menuButton = makeGlyphButton(QStringLiteral("☰"), // TRIGRAM FOR HEAVEN (hamburger)
                                   QStringLiteral("Menu"), 17);
    m_menu = new QMenu(this);
    m_menuButton->setMenu(m_menu);
    m_menuButton->setPopupMode(QToolButton::InstantPopup);
    rebuildMenu();

    // A real widget rather than a bare layout, so the strip can carry its own
    // background and bottom rule. A QHBoxLayout has nothing to paint.
    m_toolbar = new QWidget(this);
    QWidget *toolbar = m_toolbar;
    toolbar->setObjectName(QStringLiteral("anoaToolbar"));
    auto *bar = new QHBoxLayout(toolbar);
    bar->setContentsMargins(8, 6, 8, 6);
    bar->setSpacing(2);
    bar->addWidget(m_back);
    bar->addWidget(m_forward);
    bar->addWidget(reload);
    bar->addSpacing(6);
    bar->addWidget(m_urlEdit, 1);
    bar->addSpacing(4);
    bar->addWidget(m_menuButton);
    setStyleSheet(QString::fromLatin1(kToolbarStyle));

    // The strip is a sibling of the container, exactly like the toolbar and for
    // exactly the same reason: anything inside the container is in every
    // screenshot and moves every click target with it.
    m_tabStrip = new QWidget(this);
    m_tabStrip->setObjectName(QStringLiteral("anoaTabStrip"));
    m_tabStripLayout = new QHBoxLayout(m_tabStrip);
    m_tabStripLayout->setContentsMargins(0, 0, 0, 0);
    m_tabStripLayout->setSpacing(0);
    m_tabStrip->hide(); // one tab looks exactly as it does today

    connect(m_view, &AnoaBrowser::tabCreated, this,
            [this](const QString &) { rebuildTabStrip(); });
    connect(m_view, &AnoaBrowser::tabClosed, this,
            [this](const QString &) { rebuildTabStrip(); });
    connect(m_view, &AnoaBrowser::tabActivated, this,
            [this](const QString &) { rebuildTabStrip(); });
    connect(m_view, &AnoaBrowser::activeTitleChanged, this,
            [this](const QString &) { rebuildTabStrip(); });

    // The toolbar is deliberately NOT in the layout. Only the tab container is,
    // and the toolbar is positioned by hand across the top with the layout's top
    // margin reserving the space for it. It stays a sibling of the container for
    // the same reason it is not inside the view: anything within the container
    // appears in every screenshot and shifts the coordinate space clicks are
    // measured in.
    //
    // That indirection is what makes auto-hide possible without touching the
    // view's geometry: revealing the bar over the page must not resize the
    // page. HttpServer reports the view's size as the viewport that
    // /render/click coordinates are measured in, so a toolbar that pushed the
    // view down and up again on every hover would move every click target with
    // it and reflow the page twice a second.
    m_root = new QVBoxLayout(this);
    m_root->setContentsMargins(0, 0, 0, 0);
    m_root->setSpacing(0);
    m_root->addWidget(m_view, 1);
    toolbar->raise();

    connect(m_back, &QToolButton::clicked, m_view, &AnoaBrowser::back);
    connect(m_forward, &QToolButton::clicked, m_view, &AnoaBrowser::forward);
    connect(reload, &QToolButton::clicked, m_view, &AnoaBrowser::reload);
    connect(m_urlEdit, &QLineEdit::returnPressed, this, &BrowserWindow::onUrlEntered);
    // The container's active* signals, not one view's: which view is showing
    // can change under us, and the address field has to follow whichever tab
    // is active rather than the one that happened to exist at startup.
    connect(m_view, &AnoaBrowser::activeUrlChanged, this, &BrowserWindow::onUrlChanged);
    connect(m_view, &AnoaBrowser::activeLoadFinished, this,
            [this](bool) { refreshHistoryButtons(); });
    // Switching tabs changes no page, so nothing above fires for it.
    connect(m_view, &AnoaBrowser::tabActivated, this,
            [this](const QString &) { refreshHistoryButtons(); });

    refreshHistoryButtons();

    // The view keeps the size the config asked for; the window is whatever that
    // plus the toolbar comes to. Sizing the window to config.width/height
    // instead would quietly hand the page a shorter viewport than requested.
    // Polls the pointer instead of filtering mouse events. Once the toolbar is
    // hidden the whole window is the web view, and WebEngine delivers pointer
    // events inside its own render widget rather than up the parent chain, so
    // an event filter here would simply never see the pointer reach the top
    // edge. Polling is immune to that, and at 16 Hz it costs nothing.
    m_pointerTimer = new QTimer(this);
    m_pointerTimer->setInterval(60);
    connect(m_pointerTimer, &QTimer::timeout, this, &BrowserWindow::pollPointer);

    m_view->setMinimumSize(config.width, config.height);
    resize(config.width, config.height + toolbar->sizeHint().height());
    layoutToolbar();
}

void BrowserWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutToolbar();
}

void BrowserWindow::layoutToolbar()
{
    if (!m_toolbar)
        return;
    const int barHeight = m_toolbar->sizeHint().height();
    m_toolbar->setGeometry(0, 0, width(), barHeight);

    // The strip sits under the toolbar and shares its fate: both overlay the
    // page when auto-hide is on, and both are reserved for when it is off.
    // isHidden(), not isVisible(). isVisible() is false until the show event
    // has actually been delivered, and rebuildTabStrip() calls setVisible()
    // and then this function in the same turn — so the strip was measured
    // before it counted as visible and laid out at zero height. Every tab
    // button then inherited that: reported by the accessibility layer as
    // 108x0, 170x0, 85x0. Present, addressable, and impossible to click.
    //
    // isHidden() reflects the explicit show/hide state immediately, which is
    // the question being asked here.
    const bool stripVisible = m_tabStrip && !m_tabStrip->isHidden();
    const int stripHeight = stripVisible ? m_tabStrip->sizeHint().height() : 0;
    if (m_tabStrip)
        m_tabStrip->setGeometry(0, barHeight, width(), stripHeight);

    // Overlaying costs the view nothing; docked, the margin is what keeps the
    // page out from under the bar.
    m_root->setContentsMargins(0, m_autoHide ? 0 : barHeight + stripHeight, 0, 0);
    m_toolbar->raise();
    if (m_tabStrip)
        m_tabStrip->raise();
}

void BrowserWindow::rebuildTabStrip()
{
    if (!m_tabStrip || !m_view)
        return;

    // Torn down and rebuilt rather than patched: one button per tab is cheap,
    // and keeping a second model of which tabs exist is how the two drift.
    while (QLayoutItem *item = m_tabStripLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            // Out of the widget tree now, not whenever the event loop gets to
            // it. deleteLater alone leaves the old button a child of the strip:
            // gone from the layout, so it keeps whatever geometry it last had,
            // but still there to be hit-tested and still carrying its old
            // handler. Rebuilding stacks them up — three closes all reading
            // "Close t1" at one position — and a click meant for a tab lands on
            // a leftover that closes the first one instead. deleteLater is
            // still what frees it, because this can run from inside the very
            // button's own clicked() handler.
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete item;
    }

    const QStringList ids = m_view->tabIds();
    const QString active = m_view->activeTabId();

    for (const QString &id : ids) {
        // The tab is the container; the label and the close live inside it, so
        // the active background covers both and they read as one thing.
        auto *tab = new QWidget(m_tabStrip);
        tab->setObjectName(QStringLiteral("anoaTab"));
        tab->setProperty("active", id == active);
        auto *tabLayout = new QHBoxLayout(tab);
        tabLayout->setContentsMargins(0, 0, 0, 0);
        tabLayout->setSpacing(0);

        auto *button = new QToolButton(tab);
        button->setObjectName(QStringLiteral("anoaTabLabel"));
        QString label = m_view->titleFor(id);
        if (label.isEmpty())
            label = m_view->urlFor(id);
        if (label.isEmpty())
            label = id;
        if (label.size() > 24)
            label = label.left(23) + QStringLiteral("\xE2\x80\xA6"); // HORIZONTAL ELLIPSIS
        button->setText(label);
        button->setToolTip(id + QStringLiteral("  ") + m_view->urlFor(id));
        button->setCursor(Qt::ArrowCursor);
        connect(button, &QToolButton::clicked, this, [this, id]() { m_view->selectTab(id); });
        tabLayout->addWidget(button);
        m_tabStripLayout->addWidget(tab);

        // No close button on the last tab: the registry would refuse it, and
        // offering a control that cannot work is worse than not offering it.
        if (ids.size() > 1) {
            auto *close = new QToolButton(tab);
            close->setObjectName(QStringLiteral("anoaTabClose"));
            close->setText(QStringLiteral("\xC3\x97")); // MULTIPLICATION SIGN
            close->setToolTip(QStringLiteral("Close ") + id);
            close->setCursor(Qt::ArrowCursor);
            // accessibleName as well as the tooltip: without it every close
            // button reports as its neighbour, which makes the strip impossible
            // to check from the outside and a screen reader announce the wrong
            // tab.
            close->setAccessibleName(QStringLiteral("Close ") + id);
            connect(close, &QToolButton::clicked, this, [this, id]() { m_view->closeTab(id); });
            tabLayout->addWidget(close);
        }
    }

    auto *add = new QToolButton(m_tabStrip);
    add->setObjectName(QStringLiteral("anoaTabNew"));
    add->setText(QStringLiteral("+"));
    add->setToolTip(QStringLiteral("New tab"));
    add->setCursor(Qt::ArrowCursor);
    connect(add, &QToolButton::clicked, this, [this]() { m_view->newTab(); });
    m_tabStripLayout->addWidget(add);
    m_tabStripLayout->addStretch(1);

    // Hidden at one tab, so today's window is untouched. Auto-hide owns
    // visibility when it is on: the strip appears with the toolbar, not apart
    // from it.
    const bool want = ids.size() > 1;
    m_tabStrip->setVisible(want && (!m_autoHide || m_toolbar->isVisible()));
    // The buttons were added a moment ago and the layout has not recalculated,
    // so sizeHint() still answers for the strip as it was — zero. layoutToolbar
    // reads that sizeHint to decide the strip's height, which is how every tab
    // button ended up 108x0: laid out, addressable, and impossible to click.
    m_tabStripLayout->activate();
    layoutToolbar();

    // And again once the event loop has turned. sizeHint() during a rebuild is
    // not dependable: the buttons were created a moment ago and have not been
    // polished, so the strip answers 0x0 as often as it answers its real
    // height — measured going 21, then 0 again the next time a tab was added.
    // Laying out twice costs nothing and is the difference between a strip you
    // can click and one that is a zero-height line.
    QTimer::singleShot(0, this, &BrowserWindow::layoutToolbar);

    // The menu carries "Close tab", whose enabled state depends on how many
    // tabs there are. This function is the one funnel every tab change already
    // goes through, so the menu is refreshed here rather than from four call
    // sites that would each have to remember.
    rebuildMenu();
}

void BrowserWindow::setAutoHide(bool on)
{
    m_autoHide = on;
    if (on) {
        m_toolbar->hide();
        if (m_tabStrip)
            m_tabStrip->hide();
        m_pointerTimer->start();
    } else {
        m_pointerTimer->stop();
        m_toolbar->show();
        if (m_tabStrip && m_view && m_view->tabCount() > 1)
            m_tabStrip->show();
    }
    layoutToolbar();
}

void BrowserWindow::pollPointer()
{
    if (!m_autoHide)
        return;

    const int barHeight = m_toolbar->sizeHint().height();
    const QPoint local = mapFromGlobal(QCursor::pos());
    const bool insideHorizontally = local.x() >= 0 && local.x() < width();

    // Two different thresholds on purpose. Revealing takes a deliberate move
    // into the top few pixels; hiding waits until the pointer is clear of the
    // whole bar. One shared threshold would flicker the bar on and off while
    // the pointer sat on the boundary.
    static constexpr int kRevealZone = 3;
    if (!m_toolbar->isVisible()) {
        if (insideHorizontally && local.y() >= 0 && local.y() <= kRevealZone) {
            m_toolbar->show();
            m_toolbar->raise();
            // The strip comes and goes with the toolbar rather than on its own
            // trigger: two things appearing at the top edge on two different
            // rules would be one surprise too many.
            if (m_tabStrip && m_view && m_view->tabCount() > 1) {
                m_tabStrip->show();
                m_tabStrip->raise();
            }
            layoutToolbar();
        }
        return;
    }

    // Keep it up while it is being used: the pointer is on it, a menu is open,
    // or the address field has focus and is being typed into.
    if (m_urlEdit->hasFocus() || (m_menu && m_menu->isVisible()))
        return;
    const int stripHeight =
        (m_tabStrip && m_tabStrip->isVisible()) ? m_tabStrip->sizeHint().height() : 0;
    if (insideHorizontally && local.y() >= 0 && local.y() < barHeight + stripHeight)
        return;
    m_toolbar->hide();
    if (m_tabStrip)
        m_tabStrip->hide();
}

// Session-only, and deliberately so: there is no bookmark store anywhere in
// this project, and inventing a file format for one is a larger decision than a
// toolbar button should make. The list lives as long as the window does.
void BrowserWindow::onBookmark()
{
    const QUrl current = m_view->url();
    if (current.isEmpty())
        return;
    const QString label = m_view->title().isEmpty() ? current.toString() : m_view->title();
    for (const auto &existing : m_bookmarks) {
        if (existing.second == current)
            return; // already starred; starring twice is not two bookmarks
    }
    m_bookmarks.append({label, current});
    rebuildMenu();
}

void BrowserWindow::rebuildMenu()
{
    m_menu->clear();
    m_menu->addAction(QStringLiteral("Back"), m_view, &AnoaBrowser::back);
    m_menu->addAction(QStringLiteral("Forward"), m_view, &AnoaBrowser::forward);
    m_menu->addAction(QStringLiteral("Reload"), m_view, &AnoaBrowser::reload);
    m_menu->addSeparator();
    // The tab strip hides itself at one tab so a single-tab window stays plain,
    // which left the "+" button reachable only once you already had two tabs —
    // no way to open the second one from the window at all. These two are that
    // way in, and the strip appears by itself the moment there is more than one.
    QAction *newTab = m_menu->addAction(QStringLiteral("New tab"), this, [this]() {
        const QString id = m_view->newTab();
        if (!id.isEmpty())
            m_view->selectTab(id);
    });
    newTab->setShortcut(QKeySequence::AddTab);

    QAction *closeTab = m_menu->addAction(QStringLiteral("Close tab"), this, [this]() {
        const QString id = m_view->activeTabId();
        if (!id.isEmpty())
            m_view->closeTab(id);
    });
    closeTab->setShortcut(QKeySequence::Close);
    // The registry refuses the last tab, and a menu item that cannot work reads
    // as a broken window rather than a deliberate rule.
    closeTab->setEnabled(m_view->tabCount() > 1);

    m_menu->addSeparator();
    m_menu->addAction(QStringLiteral("Copy address"), this, [this]() {
        QGuiApplication::clipboard()->setText(m_view->url().toString());
    });

    m_menu->addSeparator();
    // Rebuilt with the menu, so it has to carry its state rather than assume
    // the fresh action's default.
    m_autoHideAction = m_menu->addAction(QStringLiteral("Auto-hide toolbar"));
    m_autoHideAction->setCheckable(true);
    m_autoHideAction->setChecked(m_autoHide);
    m_autoHideAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H));
    connect(m_autoHideAction, &QAction::toggled, this, &BrowserWindow::setAutoHide);

    m_menu->addSeparator();
    if (m_bookmarks.isEmpty()) {
        QAction *empty = m_menu->addAction(QStringLiteral("No bookmarks"));
        empty->setEnabled(false);
    } else {
        for (const auto &bookmark : m_bookmarks) {
            const QUrl target = bookmark.second;
            m_menu->addAction(bookmark.first, this, [this, target]() { m_view->load(target); });
        }
    }
}

BrowserWindow::~BrowserWindow()
{
    // The container is borrowed, not owned: it lives on main()'s stack and is
    // declared before this window so it outlives it. Releasing it here keeps
    // Qt's parent-child teardown from deleting a stack object. What is released
    // is the whole tab container, so every view inside it goes with it and none
    // is left parented to a window that no longer exists.
    if (m_view) {
        m_view->hide(); // so releasing it does not flash a bare top-level widget
        m_view->setParent(nullptr);
    }
}

void BrowserWindow::onUrlEntered()
{
    const QString url = normalizeUserUrl(m_urlEdit->text());
    if (url.isEmpty())
        return;
    m_view->load(QUrl(url));
    m_view->setFocus();
}

void BrowserWindow::onUrlChanged(const QUrl &url)
{
    // Not while it is being typed into: a redirect landing mid-edit would
    // otherwise replace what the user is still writing.
    if (m_urlEdit->hasFocus())
        return;
    m_urlEdit->setText(url.toString());
}

void BrowserWindow::refreshHistoryButtons()
{
    // The active tab's history. Each tab keeps its own, so these buttons mean
    // "back in what you are looking at", not "back in the first tab opened".
    QWebEnginePage *page = m_view->page();
    const bool canBack = page && page->history()->canGoBack();
    const bool canForward = page && page->history()->canGoForward();
    m_back->setEnabled(canBack);
    m_forward->setEnabled(canForward);
}
