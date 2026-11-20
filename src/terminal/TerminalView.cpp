#include "terminal/TerminalView.h"

#include "app/Look.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QImage>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QToolButton>

#include <algorithm>
#include <climits>
#include <cstring>
#include <functional>

using rdc::SessionState;

static const QColor defaultFg(0xd0, 0xd0, 0xd0), defaultBg(0x1e, 0x1e, 0x1e);

static VTermModifier vtermModifiers(Qt::KeyboardModifiers m)
{
    int mod = VTERM_MOD_NONE;
    if (m & Qt::ShiftModifier) mod |= VTERM_MOD_SHIFT;
    if (m & Qt::AltModifier) mod |= VTERM_MOD_ALT;
    if (m & Qt::ControlModifier) mod |= VTERM_MOD_CTRL;
    return VTermModifier(mod);
}

static int vtermButton(Qt::MouseButton b)
{
    return b == Qt::LeftButton ? 1 : b == Qt::MiddleButton ? 2 : b == Qt::RightButton ? 3 : 0;
}

// each QChar's source cell, plus one for the end
static QString lineText(const std::vector<VTermScreenCell>& cells, std::vector<int>& cols)
{
    QString text;
    cols.clear();
    for (int col = 0; col < int(cells.size()); ++col) {
        char32_t ch = cells[size_t(col)].chars[0];
        if (ch == char32_t(-1)) continue; // right half of a wide character
        const QString s = QString::fromUcs4(ch ? &ch : U" ", 1);
        text += s;
        cols.insert(cols.end(), size_t(s.size()), col);
    }
    cols.push_back(int(cells.size()));
    return text;
}

struct TerminalView::Callbacks {
    static TerminalView* view(void* user) { return static_cast<TerminalView*>(user); }

    static int damage(VTermRect, void* user)
    {
        view(user)->viewport()->update();
        return 1;
    }

    static int moveCursor(VTermPos pos, VTermPos, int visible, void* user)
    {
        view(user)->cursor_ = pos;
        view(user)->cursorVisible_ = visible;
        view(user)->viewport()->update();
        return 1;
    }

    static int setTermProp(VTermProp prop, VTermValue* value, void* user)
    {
        TerminalView* v = view(user);
        switch (prop) {
        case VTERM_PROP_CURSORVISIBLE: v->cursorVisible_ = value->boolean; break;
        case VTERM_PROP_ALTSCREEN: v->altScreen_ = value->boolean; break;
        case VTERM_PROP_MOUSE:
            v->mouseMode_ = value->number;
            v->viewport()->setMouseTracking(value->number == VTERM_PROP_MOUSE_MOVE);
            v->viewport()->setCursor(value->number == VTERM_PROP_MOUSE_NONE ? Qt::IBeamCursor : Qt::ArrowCursor);
            break;
        case VTERM_PROP_TITLE:
            if (value->string.initial) v->titlePart_.clear();
            v->titlePart_.append(value->string.str, value->string.len);
            if (value->string.final) emit v->titleChanged(QString::fromUtf8(v->titlePart_));
            break;
        default: break;
        }
        return 1;
    }

    static int bell(void* user)
    {
        emit view(user)->bell();
        return 1;
    }

    static int pushLine(int cols, const VTermScreenCell* cells, void* user)
    {
        auto& sb = view(user)->scrollback_;
        sb.emplace_back(cells, cells + cols);
        if (sb.size() > view(user)->maxScrollback_) sb.pop_front();
        return 1;
    }

    static int popLine(int cols, VTermScreenCell* cells, void* user)
    {
        auto& sb = view(user)->scrollback_;
        if (sb.empty()) return 0;
        const Line& line = sb.back();
        for (int i = 0; i < cols; ++i) {
            if (i < int(line.size())) {
                cells[i] = line[size_t(i)];
            } else {
                cells[i] = VTermScreenCell{};
                cells[i].width = 1;
                cells[i].fg = line.empty() ? VTermColor{} : line.back().fg;
                cells[i].bg = line.empty() ? VTermColor{} : line.back().bg;
            }
        }
        sb.pop_back();
        return 1;
    }

    static int clearScrollback(void* user)
    {
        view(user)->scrollback_.clear();
        return 1;
    }

    static void output(const char* s, size_t len, void* user) { view(user)->send(s, len); }
};

static const VTermScreenCallbacks screenCallbacks = {
    TerminalView::Callbacks::damage,
    nullptr,
    TerminalView::Callbacks::moveCursor,
    TerminalView::Callbacks::setTermProp,
    TerminalView::Callbacks::bell,
    nullptr,
    TerminalView::Callbacks::pushLine,
    TerminalView::Callbacks::popLine,
    TerminalView::Callbacks::clearScrollback,
};

TerminalView::TerminalView(QWidget* parent)
    : QAbstractScrollArea(parent), vt_(vterm_new(rows_, cols_)), screen_(vterm_obtain_screen(vt_))
{
    configure(QString(), 0, int(maxScrollback_));

    setFocusPolicy(Qt::StrongFocus);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    vterm_set_utf8(vt_, 1);
    vterm_output_set_callback(vt_, Callbacks::output, this);
    vterm_screen_set_callbacks(screen_, &screenCallbacks, this);
    VTermColor fg, bg;
    vterm_color_rgb(&fg, uint8_t(defaultFg.red()), uint8_t(defaultFg.green()), uint8_t(defaultFg.blue()));
    vterm_color_rgb(&bg, uint8_t(defaultBg.red()), uint8_t(defaultBg.green()), uint8_t(defaultBg.blue()));
    vterm_screen_set_default_colors(screen_, &fg, &bg);
    vterm_screen_enable_altscreen(screen_, 1); // keeps full-screen apps out of scrollback
    vterm_screen_reset(screen_, 1);
}

TerminalView::~TerminalView()
{
    prompts_.close();
    if (session_) session_->disconnect();
    vterm_free(vt_);
}

void TerminalView::configure(const QString& fontFamily, int fontSize, int scrollbackLines)
{
    font_ = look::systemFont(QFontDatabase::FixedFont);
    if (!fontFamily.isEmpty()) font_.setFamily(fontFamily);
    if (fontSize > 0) font_.setPointSize(fontSize);
    font_.setStyleHint(QFont::Monospace);
    const QFontMetrics fm(font_);
    cellW_ = std::max(1, fm.horizontalAdvance(QLatin1Char('M')));
    cellH_ = std::max(1, fm.height());
    ascent_ = fm.ascent();
    maxScrollback_ = size_t(std::max(0, scrollbackLines));
    resizeTerminal();
    viewport()->update();
}

void TerminalView::start(std::unique_ptr<rdc::TerminalSession> session)
{
    session_ = std::move(session);
    session_->resize(cols_, rows_);
    session_->connect();
}

void TerminalView::onStateChanged(SessionState state)
{
    QMetaObject::invokeMethod(this, [this, state] {
        if (state == SessionState::Connected) connected_ = true;
        live_ = state == SessionState::Connecting || state == SessionState::Connected;
        if (state == SessionState::Disconnected && connected_) {
            emit stateChanged(state);
            emit closeRequested(); // errors keep the tab open
            return;
        }
        if (state == SessionState::Disconnected || state == SessionState::Error) {
            // mouse reporting off so clicks select text again
            static const char note[] = "\x1b[?1000l\r\n\x1b[2m[disconnected]\x1b[0m\r\n";
            feed(note, sizeof note - 1);
        }
        emit stateChanged(state);
    }, Qt::QueuedConnection);
}

void TerminalView::onError(std::string message)
{
    QMetaObject::invokeMethod(this, [this, text = QString::fromStdString(message)] {
        QPointer<TerminalView> alive(this);
        QMessageBox box(QMessageBox::Warning, tr("Connection error"), text, QMessageBox::Close, this);
        const QAbstractButton* again = box.addButton(tr("&Reconnect"), QMessageBox::AcceptRole);
        box.setDefaultButton(QMessageBox::Close);
        box.setOption(QMessageBox::Option::DontUseNativeDialog); // KDE's dialog: Enter presses the first button
        box.exec();
        if (alive && box.clickedButton() == again) emit reconnectRequested(); // Close keeps the tab
    }, Qt::QueuedConnection);
}

bool TerminalView::askTrust(const std::string& title, const std::string& details, bool*)
{
    return prompts_.ask([this, t = QString::fromStdString(title), d = QString::fromStdString(details)] {
        return prompts::trust(this, t, d);
    });
}

bool TerminalView::askCredentials(const std::string& prompt, rdc::Credentials& creds)
{
    return prompts_.ask([this, p = QString::fromStdString(prompt), &creds] { return prompts::credentials(this, p, creds); });
}

void TerminalView::onData(std::string bytes)
{
    QMetaObject::invokeMethod(this, [this, bytes = std::move(bytes)] { feed(bytes.data(), bytes.size()); },
                              Qt::QueuedConnection);
}

void TerminalView::feed(const char* data, size_t len)
{
    const bool atBottom = verticalScrollBar()->value() == verticalScrollBar()->maximum();
    vterm_input_write(vt_, data, len);
    updateScrollBar(atBottom);
    viewport()->update();
}

void TerminalView::send(const char* data, size_t len)
{
    if (session_) session_->write(std::as_bytes(std::span(data, len)));
}

void TerminalView::paste(const QString& text)
{
    if (text.isEmpty()) return;
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\r")).replace(QLatin1Char('\n'), QLatin1Char('\r'));
    const QByteArray utf8 = t.toUtf8();
    vterm_keyboard_start_paste(vt_);
    send(utf8.constData(), size_t(utf8.size()));
    vterm_keyboard_end_paste(vt_);
}

bool TerminalView::clipboardImage() const
{
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    return session_ && session_->canUpload() && mime && mime->hasImage() && !mime->hasText();
}

// an image goes to a file on the server and its path is pasted, as a dragged-in file (Claude Code takes it as an image)
void TerminalView::pasteClipboard()
{
    if (!clipboardImage()) return paste(QApplication::clipboard()->text());
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    QByteArray png = mime->data(QStringLiteral("image/png"));
    if (png.isEmpty()) {
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        qvariant_cast<QImage>(mime->imageData()).save(&buffer, "PNG");
    }
    session_->upload(png.toStdString(), ".png", [this](std::string path, std::string error) {
        QMetaObject::invokeMethod(this, [this, path = QString::fromStdString(path), error = QString::fromStdString(error)] {
            if (error.isEmpty()) return paste(path);
            const QByteArray note = "\r\n\x1b[2m[" + tr("The image could not be copied to the server: %1").arg(error).toUtf8() + "]\x1b[0m\r\n";
            feed(note.constData(), size_t(note.size()));
        }, Qt::QueuedConnection);
    });
}

void TerminalView::updateScrollBar(bool stickToBottom)
{
    QScrollBar* bar = verticalScrollBar();
    bar->setRange(0, int(scrollback_.size()));
    bar->setPageStep(rows_);
    if (stickToBottom) bar->setValue(bar->maximum());
}

void TerminalView::resizeTerminal()
{
    const int cols = std::max(2, viewport()->width() / cellW_);
    const int rows = std::max(1, viewport()->height() / cellH_);
    if (cols == cols_ && rows == rows_) return;
    cols_ = cols;
    rows_ = rows;
    vterm_set_size(vt_, rows_, cols_);
    if (session_) session_->resize(cols_, rows_);
    updateScrollBar(true);
    emit gridChanged();
}

void TerminalView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    resizeTerminal();
    placeFindBar();
}

void TerminalView::scrollContentsBy(int, int) { viewport()->update(); }

TerminalView::Line TerminalView::lineAt(int line) const
{
    if (line < int(scrollback_.size())) return scrollback_[size_t(line)];
    Line cells(static_cast<size_t>(cols_));
    const int row = line - int(scrollback_.size());
    for (int col = 0; col < cols_; ++col) vterm_screen_get_cell(screen_, VTermPos{row, col}, &cells[size_t(col)]);
    return cells;
}

QColor TerminalView::color(VTermColor c) const
{
    vterm_screen_convert_color_to_rgb(screen_, &c);
    return QColor(c.rgb.red, c.rgb.green, c.rgb.blue);
}

void TerminalView::paintEvent(QPaintEvent*)
{
    // shortcut: draws every cell each repaint, group runs by attribute if slow
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), defaultBg);
    const int first = verticalScrollBar()->value();
    const int cursorLine = int(scrollback_.size()) + cursor_.row;
    for (int row = 0; row < rows_; ++row) {
        const int line = first + row;
        const Line cells = lineAt(line);
        const int y = row * cellH_;
        for (int col = 0; col < int(cells.size());) {
            const VTermScreenCell& cell = cells[size_t(col)];
            const int width = std::max<int>(1, cell.width);
            QColor fg = color(cell.fg), bg = color(cell.bg);
            const bool cursorHere = line == cursorLine && col == cursor_.col && cursorVisible_ && hasFocus();
            if (bool(cell.attrs.reverse) != (selected(line, col) != cursorHere)) std::swap(fg, bg);
            const QRect box(col * cellW_, y, width * cellW_, cellH_);
            if (bg != defaultBg) p.fillRect(box, bg);
            if (cell.chars[0] != 0 && cell.chars[0] != uint32_t(-1)) {
                int n = 0;
                while (n < VTERM_MAX_CHARS_PER_CELL && cell.chars[n]) ++n;
                QFont f = font_;
                f.setBold(cell.attrs.bold);
                f.setItalic(cell.attrs.italic);
                f.setUnderline(cell.attrs.underline);
                f.setStrikeOut(cell.attrs.strike);
                p.setFont(f);
                p.setPen(fg);
                p.drawText(QPoint(box.x(), y + ascent_), QString::fromUcs4(reinterpret_cast<const char32_t*>(cell.chars), n));
            }
            col += width;
        }
    }
    if (!hasFocus() && cursorVisible_ && cursorLine >= first && cursorLine < first + rows_) {
        p.setPen(defaultFg);
        p.drawRect(cursor_.col * cellW_, (cursorLine - first) * cellH_, cellW_ - 1, cellH_ - 1);
    }
}

TerminalView::Pos TerminalView::posAt(QPoint point) const
{
    const int row = std::clamp(point.y() / cellH_, 0, rows_ - 1);
    return Pos{verticalScrollBar()->value() + row, std::clamp(point.x() / cellW_, 0, cols_)};
}

bool TerminalView::selected(int line, int col) const
{
    if (selStart_.line < 0 || selStart_ == selEnd_) return false;
    const auto [a, b] = std::minmax(selStart_, selEnd_);
    const Pos p{line, col};
    return a <= p && p < b;
}

QString TerminalView::selectedText() const
{
    if (selStart_.line < 0 || selStart_ == selEnd_) return {};
    const auto [a, b] = std::minmax(selStart_, selEnd_);
    QStringList lines;
    for (int line = a.line; line <= b.line; ++line) {
        const Line cells = lineAt(line);
        const int from = line == a.line ? a.col : 0;
        const int to = line == b.line ? b.col : int(cells.size());
        QString text;
        for (int col = from; col < to && col < int(cells.size()); ++col) {
            const uint32_t ch = cells[size_t(col)].chars[0];
            if (ch == uint32_t(-1)) continue; // right half of a wide character
            text += ch ? QString::fromUcs4(reinterpret_cast<const char32_t*>(&ch), 1) : QStringLiteral(" ");
        }
        while (text.endsWith(QLatin1Char(' '))) text.chop(1);
        lines << text;
    }
    return lines.join(QLatin1Char('\n'));
}

void TerminalView::mouseToProgram(QPointF point, Qt::KeyboardModifiers m)
{
    // callers scroll to the bottom on clicks
    vterm_mouse_move(vt_, std::clamp(int(point.y()) / cellH_, 0, rows_ - 1), std::clamp(int(point.x()) / cellW_, 0, cols_ - 1),
                     vtermModifiers(m));
}

void TerminalView::mousePressEvent(QMouseEvent* event)
{
    if (reportsMouse(event->modifiers())) {
        if (const int button = vtermButton(event->button())) {
            verticalScrollBar()->setValue(verticalScrollBar()->maximum());
            mouseToProgram(event->position(), event->modifiers());
            vterm_mouse_button(vt_, button, true, vtermModifiers(event->modifiers()));
        }
        return;
    }
    if (event->button() == Qt::MiddleButton) {
        paste(QApplication::clipboard()->text(QClipboard::Selection));
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    const Pos pos = posAt(event->position().toPoint());
    const bool third = lineClick_.isValid() && lineClick_.elapsed() < QApplication::doubleClickInterval();
    lineClick_.invalidate();
    selStart_ = third ? Pos{pos.line, 0} : pos;
    selEnd_ = third ? Pos{pos.line, cols_} : pos;
    selecting_ = true;
    viewport()->update();
}

void TerminalView::mouseMoveEvent(QMouseEvent* event)
{
    if (selecting_) {
        selEnd_ = posAt(event->position().toPoint());
        viewport()->update();
    } else if (mouseMode_ != VTERM_PROP_MOUSE_NONE) {
        mouseToProgram(event->position(), event->modifiers());
    }
}

void TerminalView::mouseReleaseEvent(QMouseEvent* event)
{
    if (selecting_ && event->button() == Qt::LeftButton) {
        selecting_ = false;
        const QString text = selectedText();
        if (!text.isEmpty() && QApplication::clipboard()->supportsSelection())
            QApplication::clipboard()->setText(text, QClipboard::Selection);
    } else if (!selecting_ && mouseMode_ != VTERM_PROP_MOUSE_NONE) {
        if (const int button = vtermButton(event->button())) { // libvterm skips releases of buttons never pressed
            mouseToProgram(event->position(), event->modifiers());
            vterm_mouse_button(vt_, button, false, vtermModifiers(event->modifiers()));
        }
    }
}

void TerminalView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (reportsMouse(event->modifiers()) || event->button() != Qt::LeftButton) return mousePressEvent(event);
    // word characters as in Konsole
    const Pos pos = posAt(event->position().toPoint());
    const Line cells = lineAt(pos.line);
    auto inWord = [&cells](int col) {
        if (col < 0 || col >= int(cells.size())) return false;
        const char32_t ch = cells[size_t(col)].chars[0];
        return ch == char32_t(-1) || QChar::isLetterOrNumber(ch) || (ch < 0x80 && ch && std::strchr(":@-./_~?&=%+#", int(ch)));
    };
    int from = pos.col, to = pos.col;
    if (inWord(pos.col)) {
        while (inWord(from - 1)) --from;
        while (inWord(to)) ++to;
    }
    selStart_ = {pos.line, from};
    selEnd_ = {pos.line, to};
    selecting_ = true; // the release fills the selection clipboard
    lineClick_.start();
    viewport()->update();
}

void TerminalView::wheelEvent(QWheelEvent* event)
{
    const bool toProgram = reportsMouse(event->modifiers());
    // no-mouse full-screen apps get arrows (Konsole); Shift scrolls
    const bool arrows = !toProgram && altScreen_ && !(event->modifiers() & Qt::ShiftModifier);
    if (!toProgram && !arrows) return QAbstractScrollArea::wheelEvent(event);
    event->accept();
    wheelRest_ += event->angleDelta().y(); // touchpads send small steps
    const int steps = wheelRest_ / 120;
    wheelRest_ -= steps * 120;
    if (!steps) return;
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    if (toProgram) mouseToProgram(event->position(), event->modifiers());
    for (int i = 0; i < std::abs(steps); ++i) {
        if (toProgram) {
            vterm_mouse_button(vt_, steps > 0 ? 4 : 5, true, vtermModifiers(event->modifiers()));
        } else {
            for (int line = 0; line < QApplication::wheelScrollLines(); ++line)
                vterm_keyboard_key(vt_, steps > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
        }
    }
}

void TerminalView::contextMenuEvent(QContextMenuEvent* event)
{
    if (event->reason() == QContextMenuEvent::Mouse && reportsMouse(event->modifiers())) return;
    QMenu menu(this);
    const QString selection = selectedText(), clipboard = QApplication::clipboard()->text();
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), tr("&Copy"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this,
                   [selection] { QApplication::clipboard()->setText(selection); })
        ->setEnabled(!selection.isEmpty());
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-paste")), tr("&Paste"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), this,
                   [this] { pasteClipboard(); })
        ->setEnabled(!clipboard.isEmpty() || clipboardImage());
    menu.addSeparator();
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-find")), tr("&Find…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), this,
                   &TerminalView::openFind);
    menu.exec(event->globalPos());
}

void TerminalView::openFind()
{
    if (!findBar_) {
        auto* bar = new QFrame(this);
        bar->setFrameShape(QFrame::StyledPanel);
        bar->setAutoFillBackground(true);
        bar->setBackgroundRole(QPalette::Window);
        findText_ = new QLineEdit;
        findText_->setPlaceholderText(tr("Find"));
        findText_->setClearButtonEnabled(true);
        findText_->setMinimumWidth(findText_->fontMetrics().averageCharWidth() * 28);
        findText_->installEventFilter(this);
        findStatus_ = new QLabel;
        auto* layout = new QHBoxLayout(bar);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->addWidget(findText_);
        layout->addWidget(findStatus_);
        auto addButton = [this, layout](const char* icon, const QString& tip, std::function<void()> act) {
            auto* b = new QToolButton;
            b->setIcon(QIcon::fromTheme(QLatin1String(icon)));
            b->setToolTip(tip);
            b->setAutoRaise(true);
            connect(b, &QToolButton::clicked, this, act);
            layout->addWidget(b);
        };
        addButton("go-up", tr("Older match (Enter)"), [this] { find(true, false); });
        addButton("go-down", tr("Newer match (Shift+Enter)"), [this] { find(false, false); });
        addButton("window-close", tr("Close (Esc)"), [this] {
            findBar_->hide();
            setFocus();
        });
        connect(findText_, &QLineEdit::textEdited, this, [this] { find(true, true); });
        findBar_ = bar;
    }
    findBar_->show();
    placeFindBar();
    findText_->setFocus();
    findText_->selectAll();
}

void TerminalView::placeFindBar()
{
    if (!findBar_ || findBar_->isHidden()) return;
    findBar_->adjustSize();
    const QRect area = viewport()->geometry();
    findBar_->move(std::max(area.left(), area.right() - findBar_->width() - 8), area.top() + 4);
    findBar_->raise();
}

bool TerminalView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == findText_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            find(!(key->modifiers() & Qt::ShiftModifier), false);
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            findBar_->hide();
            setFocus();
            return true;
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}

// shortcut: a match must fit one screen line, join wrapped lines if long commands need it
void TerminalView::find(bool older, bool restart)
{
    const QString needle = findText_->text();
    findStatus_->clear();
    placeFindBar();
    if (needle.isEmpty()) {
        selStart_ = selEnd_ = Pos{};
        viewport()->update();
        return;
    }
    const int total = int(scrollback_.size()) + rows_;
    const bool fromMatch = !restart && selStart_.line >= 0 && selStart_.line < total;
    const Pos from = fromMatch ? std::min(selStart_, selEnd_) : older ? Pos{total - 1, INT_MAX} : Pos{0, -1};
    std::vector<int> cols;
    Line screenLine;
    for (int i = 0; i <= total; ++i) {
        const int line = older ? ((from.line - i) % total + total) % total : (from.line + i) % total;
        const Line& cells = line < int(scrollback_.size()) ? scrollback_[size_t(line)] : (screenLine = lineAt(line));
        const QString text = lineText(cells, cols);
        qsizetype at = -1;
        if (older) {
            for (at = text.lastIndexOf(needle, -1, Qt::CaseInsensitive); at >= 0 && i == 0 && cols[size_t(at)] >= from.col;)
                at = at > 0 ? text.lastIndexOf(needle, at - 1, Qt::CaseInsensitive) : -1;
        } else {
            for (at = text.indexOf(needle, 0, Qt::CaseInsensitive); at >= 0 && i == 0 && cols[size_t(at)] <= from.col;)
                at = text.indexOf(needle, at + 1, Qt::CaseInsensitive);
        }
        if (at < 0) continue;
        selStart_ = {line, cols[size_t(at)]};
        selEnd_ = {line, cols[size_t(at + needle.size())]};
        selecting_ = false;
        QScrollBar* bar = verticalScrollBar();
        if (line < bar->value() || line >= bar->value() + rows_) bar->setValue(line - rows_ / 2);
        viewport()->update();
        return;
    }
    findStatus_->setText(tr("Not found"));
    placeFindBar();
}

bool TerminalView::event(QEvent* event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        event->accept(); // the shell owns Ctrl+W/Q while focused
        return true;
    }
    return QAbstractScrollArea::event(event);
}

void TerminalView::keyPressEvent(QKeyEvent* event)
{
    const Qt::KeyboardModifiers m = event->modifiers();
    if (m == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_C) {
        if (const QString text = selectedText(); !text.isEmpty()) QApplication::clipboard()->setText(text);
        return;
    }
    if ((m == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_V) ||
        (m == Qt::ShiftModifier && event->key() == Qt::Key_Insert)) {
        pasteClipboard();
        return;
    }
    if (m == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_F) {
        openFind();
        return;
    }

    const VTermModifier vmod = vtermModifiers(m);
    const int mod = vmod;

    static const std::pair<int, VTermKey> keys[] = {
        {Qt::Key_Return, VTERM_KEY_ENTER},  {Qt::Key_Enter, VTERM_KEY_ENTER},     {Qt::Key_Tab, VTERM_KEY_TAB},
        {Qt::Key_Backtab, VTERM_KEY_TAB},   {Qt::Key_Backspace, VTERM_KEY_BACKSPACE}, {Qt::Key_Escape, VTERM_KEY_ESCAPE},
        {Qt::Key_Up, VTERM_KEY_UP},         {Qt::Key_Down, VTERM_KEY_DOWN},       {Qt::Key_Left, VTERM_KEY_LEFT},
        {Qt::Key_Right, VTERM_KEY_RIGHT},   {Qt::Key_Insert, VTERM_KEY_INS},      {Qt::Key_Delete, VTERM_KEY_DEL},
        {Qt::Key_Home, VTERM_KEY_HOME},     {Qt::Key_End, VTERM_KEY_END},         {Qt::Key_PageUp, VTERM_KEY_PAGEUP},
        {Qt::Key_PageDown, VTERM_KEY_PAGEDOWN},
    };
    const int key = event->key();
    bool handled = false;
    for (auto [qtKey, vtKey] : keys)
        if (key == qtKey) {
            vterm_keyboard_key(vt_, vtKey, vmod);
            handled = true;
        }
    if (!handled && key >= Qt::Key_F1 && key <= Qt::Key_F12) {
        vterm_keyboard_key(vt_, VTermKey(VTERM_KEY_FUNCTION(key - Qt::Key_F1 + 1)), vmod);
        handled = true;
    }
    if (!handled && (m & Qt::ControlModifier) && key >= Qt::Key_A && key <= Qt::Key_Z) {
        vterm_keyboard_unichar(vt_, uint32_t('a' + (key - Qt::Key_A)), vmod);
        handled = true;
    }
    if (!handled) {
        // Shift is applied; keep Alt for the ESC prefix
        for (char32_t ch : event->text().toUcs4())
            if (ch >= 0x20 || ch == 0x1b) vterm_keyboard_unichar(vt_, uint32_t(ch), VTermModifier(mod & VTERM_MOD_ALT));
    }
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}
