#include "terminal/TerminalView.h"
#include "app/Icons.h"
#include "terminal/ColorSchemes.h"

#include "app/Look.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
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
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStandardPaths>
#include <QToolButton>
#include <QUrl>

#include <algorithm>
#include <climits>
#include <cstring>
#include <functional>
#include <utility>

using rdc::SessionState;

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
        TerminalView* v = view(user);
        if (v->resizing_ && v->altScreen_) return 1; // libvterm 0.3.3 reads these from the full-screen program's screen
        v->scrollback_.emplace_back(cells, cells + cols);
        if (v->scrollback_.size() > v->maxScrollback_) v->scrollback_.pop_front();
        ++v->pushed_;
        if (v->log_ && !v->resizing_) v->logScrollback(); // a resize may take them back
        return 1;
    }

    static int popLine(int cols, VTermScreenCell* cells, void* user)
    {
        TerminalView* v = view(user);
        auto& sb = v->scrollback_;
        if (sb.empty()) return 0;
        Line& line = sb.back();
        size_t used = line.size();
        while (used > 0 && !line[used - 1].chars[0]) --used;
        size_t from = 0; // a line wider than the screen now gives back its last screen-wide part and keeps the rest
        if (used > size_t(cols)) from = (used - 1) / size_t(cols) * size_t(cols);
        while (from > 0 && line[from].chars[0] == uint32_t(-1)) --from; // a wide character stays whole
        VTermScreenCell blank{};
        blank.width = 1;
        if (!line.empty()) blank.fg = line.back().fg, blank.bg = line.back().bg;
        for (int i = 0; i < cols; ++i) cells[i] = from + size_t(i) < used ? line[from + size_t(i)] : blank;
        if (from) {
            line.resize(from);
        } else {
            sb.pop_back();
            --v->pushed_;
        }
        return 1;
    }

    static int clearScrollback(void* user)
    {
        view(user)->scrollback_.clear();
        return 1;
    }

    static int osc(int command, VTermStringFragment frag, void* user)
    {
        if (command != 7) return 0;
        TerminalView* v = view(user);
        if (frag.initial) v->oscPart_.clear();
        v->oscPart_.append(frag.str, frag.len);
        if (frag.final) { // file://host/path
            const QUrl url(QString::fromUtf8(v->oscPart_));
            if (url.isLocalFile() || url.scheme() == QLatin1String("file")) emit v->directoryChanged(url.path());
        }
        return 1;
    }

    // OSC 52: a program over SSH copies to this machine's clipboard
    static int setClipboard(VTermSelectionMask mask, VTermStringFragment frag, void* user)
    {
        TerminalView* v = view(user);
        constexpr size_t limit = 16 << 20;
        if (frag.initial) v->clipPart_.clear();
        if (frag.len && v->clipPart_.size() <= limit) v->clipPart_.append(frag.str, frag.len);
        if (!frag.final) return 1;
        const QString text = v->clipPart_.size() > limit ? QString() : QString::fromUtf8(v->clipPart_); // too big: dropped, not cut short
        v->clipPart_.clear();
        if (text.isEmpty()) return 1; // bad base64 arrives as empty: the clipboard stays
        if (mask & ~VTERM_SELECTION_PRIMARY) QApplication::clipboard()->setText(text);
        if ((mask & VTERM_SELECTION_PRIMARY) && QApplication::clipboard()->supportsSelection())
            QApplication::clipboard()->setText(text, QClipboard::Selection);
        return 1;
    }

    static int queryClipboard(VTermSelectionMask, void*) { return 1; } // never shows the clipboard to the remote side

    static void output(const char* s, size_t len, void* user) { view(user)->send(s, len); }
};

static const VTermStateFallbacks fallbacks = {nullptr, nullptr, TerminalView::Callbacks::osc, nullptr, nullptr, nullptr, nullptr};
static const VTermSelectionCallbacks clipboardCallbacks = {TerminalView::Callbacks::setClipboard, TerminalView::Callbacks::queryClipboard};

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
    viewport()->setMouseTracking(true); // links under Ctrl
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    vterm_set_utf8(vt_, 1);
    vterm_output_set_callback(vt_, Callbacks::output, this);
    vterm_screen_set_callbacks(screen_, &screenCallbacks, this);
    vterm_screen_set_unrecognised_fallbacks(screen_, &fallbacks, this);
    vterm_state_set_selection_callbacks(vterm_obtain_state(vt_), &clipboardCallbacks, this, nullptr, 4096); // without a buffer libvterm decodes nothing
    setColorScheme({});
    vterm_screen_enable_altscreen(screen_, 1); // keeps full-screen apps out of scrollback
    vterm_screen_enable_reflow(screen_, true); // a narrower window or a bigger font wraps lines instead of cutting them
    vterm_screen_reset(screen_, 1);
}

TerminalView::~TerminalView()
{
    prompts_.close();
    if (session_) session_->disconnect();
    stopLog();
    vterm_free(vt_);
}

void TerminalView::configure(const QString& fontFamily, int fontSize, int scrollbackLines)
{
    family_ = fontFamily;
    size_ = fontSize;
    maxScrollback_ = size_t(std::max(0, scrollbackLines));
    applyFont();
}

void TerminalView::applyFont()
{
    font_ = look::systemFont(QFontDatabase::FixedFont);
    if (!family_.isEmpty()) font_.setFamily(family_);
    if (size_ > 0) font_.setPointSize(size_);
    if (font_.pointSize() > 0) font_.setPointSize(font_.pointSize() + zoom_);
    font_.setStyleHint(QFont::Monospace);
    const QFontMetrics fm(font_);
    cellW_ = std::max(1, fm.horizontalAdvance(QLatin1Char('M')));
    cellH_ = std::max(1, fm.height());
    ascent_ = fm.ascent();
    resizeTerminal();
    viewport()->update();
}

void TerminalView::zoom(int steps)
{
    const int base = font_.pointSize() - zoom_;
    zoom_ = steps && base > 0 ? std::clamp(zoom_ + steps, 6 - base, 48 - base) : 0;
    applyFont();
}

void TerminalView::setColorScheme(std::string_view key)
{
    const ColorScheme& s = colorScheme(key);
    auto rgb = [](uint32_t c) {
        VTermColor v;
        vterm_color_rgb(&v, uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c));
        return v;
    };
    VTermState* state = vterm_obtain_state(vt_);
    for (int i = 0; i < 16; ++i) {
        const VTermColor c = rgb(s.ansi[i]);
        vterm_state_set_palette_color(state, i, &c);
    }
    const VTermColor fg = rgb(s.fg), bg = rgb(s.bg);
    vterm_screen_set_default_colors(screen_, &fg, &bg);
    fg_ = QColor::fromRgb(QRgb(s.fg));
    bg_ = QColor::fromRgb(QRgb(s.bg));
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
    QMetaObject::invokeMethod(this, [this, state, asked = asked_] {
        if (state == SessionState::Connected) connected_ = true, typed_ = asked;
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
        prompts::errorIn(this, tr("Connection error"), text, tr("&Reconnect"), [this](bool again) {
            if (again) emit reconnectRequested(); // Close keeps the tab
        });
    }, Qt::QueuedConnection);
}

bool TerminalView::askTrust(const std::string& title, const std::string& details, bool*)
{
    return prompts::trust(prompts_, this, QString::fromStdString(title), QString::fromStdString(details));
}

bool TerminalView::askCredentials(const std::string& prompt, rdc::Credentials& creds)
{
    const bool ok = prompts::credentials(prompts_, this, QString::fromStdString(prompt), creds);
    if (ok) typedPassword_ = creds.password, asked_ = creds;
    return ok;
}

std::string TerminalView::takeTypedPassword() { return std::exchange(typedPassword_, {}); }

void TerminalView::setSidePanel(QWidget* panel)
{
    delete side_;
    side_ = panel;
    if (side_) {
        side_->setParent(this);
        if (!sideEdge_) {
            sideEdge_ = new QWidget(this);
            sideEdge_->setCursor(Qt::SplitHCursor);
            sideEdge_->installEventFilter(this);
        }
        side_->show();
    }
    placeSidePanel();
}

void TerminalView::setSidePanelShown(bool on)
{
    if (!side_) return;
    side_->setVisible(on);
    placeSidePanel();
    if (!on && side_->isAncestorOf(QApplication::focusWidget())) setFocus();
}

void TerminalView::placeSidePanel()
{
    const bool shown = sidePanelShown();
    const int width = std::clamp(sideWidth_, 160, std::max(160, this->width() / 2));
    setViewportMargins(shown ? width + 5 : 0, 0, 0, 0); // 5: the edge to drag
    if (sideEdge_) sideEdge_->setVisible(shown);
    if (shown) {
        const QRect r = contentsRect();
        side_->setGeometry(r.left(), r.top(), width, r.height());
        sideEdge_->setGeometry(r.left() + width, r.top(), 5, r.height());
    }
    resizeTerminal();
}

bool TerminalView::focusNextPrevChild(bool next)
{
    if (side_ && side_->isAncestorOf(QApplication::focusWidget())) return QAbstractScrollArea::focusNextPrevChild(next);
    return false;
}

void TerminalView::onData(std::string bytes)
{
    QMetaObject::invokeMethod(this, [this, bytes = std::move(bytes)] { feed(bytes.data(), bytes.size()); },
                              Qt::QueuedConnection);
}

void TerminalView::feed(const char* data, size_t len)
{
    const bool atBottom = verticalScrollBar()->value() == verticalScrollBar()->maximum();
    std::string_view rest(data, len);
    for (size_t at; log_ && (at = rest.find("\x1b[2J")) != std::string_view::npos; rest.remove_prefix(at + 4)) {
        vterm_input_write(vt_, rest.data(), at);
        if (!altScreen_) logScreen(); // before the clear erases it
        vterm_input_write(vt_, rest.data() + at, 4);
    }
    vterm_input_write(vt_, rest.data(), rest.size());
    if (!altScreen_) checkPrompt();
    updateScrollBar(atBottom);
    viewport()->update();
}

// Debian's prompt without a title or OSC 7 still tells the folder
void TerminalView::checkPrompt()
{
    const int line = int(scrollback_.size()) + cursor_.row;
    const Text t = joined(lineStart(line), line);
    const auto end = std::find_if(t.at.begin(), t.at.end(), [&](Pos p) { return p.line == line && p.col >= cursor_.col; });
    static const QRegularExpression prompt(QStringLiteral("^([^@\\s]+@[^:\\s]+:(?:~|/).*?)[$#] ?$"));
    const QRegularExpressionMatch m = prompt.match(t.text.left(end - t.at.begin()));
    const long long at = pushed_ + cursor_.row;
    if (!m.hasMatch() || (at == promptLine_ && m.captured(1) == prompt_)) return;
    promptLine_ = at;
    prompt_ = m.captured(1);
    emit promptShown(prompt_);
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
    if (!clipboardImage()) {
        const QString text = QApplication::clipboard()->text();
        paste(text);
        emit pasted(text);
        return;
    }
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
    resizing_ = true;
    vterm_set_size(vt_, rows_, cols_);
    resizing_ = false;
    if (session_) session_->resize(cols_, rows_);
    updateScrollBar(true);
    emit gridChanged();
}

void TerminalView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    placeSidePanel();
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

const TerminalView::Line& TerminalView::cellsAt(int line, Line& buffer) const
{
    return line < int(scrollback_.size()) ? scrollback_[size_t(line)] : (buffer = lineAt(line));
}

bool TerminalView::wraps(int line) const
{
    const int next = line + 1 - int(scrollback_.size()); // its screen row
    if (next >= rows_) return false;
    if (next >= 0) return vterm_state_get_lineinfo(vterm_obtain_state(vt_), next)->continuation;
    // shortcut: the scrollback keeps no wrap marks, so a full line counts as wrapped
    const Line& cells = scrollback_[size_t(line)];
    return !cells.empty() && cells.back().chars[0] && cells.back().chars[0] != ' ';
}

int TerminalView::lineStart(int line) const
{
    while (line > 0 && wraps(line - 1)) --line;
    return line;
}

int TerminalView::lineEnd(int line) const
{
    while (line < int(scrollback_.size()) + rows_ - 1 && wraps(line)) ++line;
    return line;
}

TerminalView::Text TerminalView::joined(int first, int last) const
{
    Text t;
    Line buffer;
    int end = 0;
    for (int line = first; line <= last; ++line) {
        const Line& cells = cellsAt(line, buffer);
        for (int col = 0; col < int(cells.size()); ++col) {
            char32_t ch = cells[size_t(col)].chars[0];
            if (ch == char32_t(-1)) continue; // right half of a wide character
            const QString s = QString::fromUcs4(ch ? &ch : U" ", 1);
            t.text += s;
            t.at.insert(t.at.end(), size_t(s.size()), Pos{line, col});
        }
        end = int(cells.size());
    }
    t.at.push_back(Pos{last, end});
    return t;
}

TerminalView::Link TerminalView::linkAt(Pos pos) const
{
    if (pos.line < 0 || pos.line >= int(scrollback_.size()) + rows_) return {};
    const Text t = joined(std::max(lineStart(pos.line), pos.line - 20), std::min(lineEnd(pos.line), pos.line + 20));
    static const QRegularExpression url(QStringLiteral(R"(https?://[^\s<>"'`\x{2500}-\x{257f}]+)"));
    for (auto it = url.globalMatch(t.text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        QString link = m.captured();
        // the sentence's punctuation, and a bracket opened before the link
        while (!link.isEmpty() && (QStringLiteral(".,;:!?'\"").contains(link.back()) ||
                                   (link.back() == QLatin1Char(')') && link.count(QLatin1Char('(')) < link.count(QLatin1Char(')'))) ||
                                   (link.back() == QLatin1Char(']') && link.count(QLatin1Char('[')) < link.count(QLatin1Char(']')))))
            link.chop(1);
        const Pos from = t.at[size_t(m.capturedStart())], to = t.at[size_t(m.capturedStart() + link.size())];
        if (from <= pos && pos < to) return {from, to, link};
    }
    return {};
}

void TerminalView::hoverLink(QPoint point, Qt::KeyboardModifiers m)
{
    const Link link = (m & Qt::ControlModifier) && !selecting_ && viewport()->rect().contains(point) ? linkAt(posAt(point)) : Link{};
    if (link.from != hover_.from || link.to != hover_.to) {
        hover_ = link;
        viewport()->update();
    }
    const Qt::CursorShape shape = !hover_.url.isEmpty()                  ? Qt::PointingHandCursor
                                  : mouseMode_ == VTERM_PROP_MOUSE_NONE ? Qt::IBeamCursor
                                                                         : Qt::ArrowCursor;
    if (viewport()->cursor().shape() != shape) viewport()->setCursor(shape);
}

QColor TerminalView::color(VTermColor c) const
{
    if (VTERM_COLOR_IS_DEFAULT_FG(&c)) return fg_; // scrollback lines keep the scheme they were written in
    if (VTERM_COLOR_IS_DEFAULT_BG(&c)) return bg_;
    vterm_screen_convert_color_to_rgb(screen_, &c);
    return QColor(c.rgb.red, c.rgb.green, c.rgb.blue);
}

void TerminalView::paintEvent(QPaintEvent*)
{
    // shortcut: draws every cell each repaint, group runs by attribute if slow
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), bg_);
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
            if (bg != bg_) p.fillRect(box, bg);
            if (cell.chars[0] != 0 && cell.chars[0] != uint32_t(-1)) {
                int n = 0;
                while (n < VTERM_MAX_CHARS_PER_CELL && cell.chars[n]) ++n;
                QFont f = font_;
                f.setBold(cell.attrs.bold);
                f.setItalic(cell.attrs.italic);
                f.setUnderline(cell.attrs.underline || (hover_.from <= Pos{line, col} && Pos{line, col} < hover_.to));
                f.setStrikeOut(cell.attrs.strike);
                p.setFont(f);
                p.setPen(fg);
                p.drawText(QPoint(box.x(), y + ascent_), QString::fromUcs4(reinterpret_cast<const char32_t*>(cell.chars), n));
            }
            col += width;
        }
    }
    if (!hasFocus() && cursorVisible_ && cursorLine >= first && cursorLine < first + rows_) {
        p.setPen(fg_);
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
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier))
        if (const Link link = linkAt(posAt(event->position().toPoint())); !link.url.isEmpty()) {
            QDesktopServices::openUrl(QUrl(link.url));
            return;
        }
    if (reportsMouse(event->modifiers())) {
        if (const int button = vtermButton(event->button())) {
            verticalScrollBar()->setValue(verticalScrollBar()->maximum());
            mouseToProgram(event->position(), event->modifiers());
            vterm_mouse_button(vt_, button, true, vtermModifiers(event->modifiers()));
        }
        return;
    }
    if (event->button() == Qt::MiddleButton) {
        const QString text = QApplication::clipboard()->text(QClipboard::Selection);
        paste(text);
        emit pasted(text);
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
        mouseToProgram(event->position(), event->modifiers()); // libvterm sends moves only to programs that asked
    }
    hoverLink(event->position().toPoint(), event->modifiers());
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
    if (event->modifiers() == Qt::ControlModifier) {
        event->accept();
        wheelRest_ += event->angleDelta().y();
        const int steps = wheelRest_ / 120;
        wheelRest_ -= steps * 120;
        if (steps) zoom(steps);
        return;
    }
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
    menu.addAction(icons::lucide("copy"), tr("&Copy"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this,
                   [selection] { QApplication::clipboard()->setText(selection); })
        ->setEnabled(!selection.isEmpty());
    menu.addAction(icons::lucide("clipboard-paste"), tr("&Paste"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), this,
                   [this] { pasteClipboard(); })
        ->setEnabled(!clipboard.isEmpty() || clipboardImage());
    menu.addSeparator();
    menu.addAction(icons::lucide("search"), tr("&Find…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), this,
                   &TerminalView::openFind);
    QAction* log = menu.addAction(tr("&Log to File…"), this, [this](bool on) { on ? startLog() : stopLog(); });
    log->setCheckable(true);
    log->setChecked(bool(log_));
    if (log_) log->setToolTip(log_->fileName());
    menu.setToolTipsVisible(true);
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
            b->setIcon(icons::lucide(icon));
            b->setToolTip(tip);
            b->setAutoRaise(true);
            connect(b, &QToolButton::clicked, this, act);
            layout->addWidget(b);
        };
        addButton("arrow-up", tr("Older match (Enter)"), [this] { find(true, false); });
        addButton("arrow-down", tr("Newer match (Shift+Enter)"), [this] { find(false, false); });
        addButton("x", tr("Close (Esc)"), [this] {
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
    if (watched == sideEdge_) {
        if (event->type() == QEvent::MouseMove && (static_cast<QMouseEvent*>(event)->buttons() & Qt::LeftButton)) {
            sideWidth_ = mapFromGlobal(static_cast<QMouseEvent*>(event)->globalPosition().toPoint()).x() - contentsRect().left();
            placeSidePanel();
            return true;
        }
        if (event->type() == QEvent::Paint) { // a line in a separator's shade
            QPainter p(sideEdge_);
            p.fillRect(sideEdge_->rect(), palette().window());
            p.fillRect(QRect(2, 0, 1, sideEdge_->height()), palette().mid());
            return true;
        }
    }
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
    const int start = lineStart(std::min(from.line, total - 1));
    // wrapped lines are searched as one; the first is searched again last, for the matches on the other side of from
    for (int first = start, pass = 0;; ++pass) {
        const int last = lineEnd(first);
        const Text t = joined(first, last);
        qsizetype at = -1;
        if (older) {
            for (at = t.text.lastIndexOf(needle, -1, Qt::CaseInsensitive); at >= 0 && pass == 0 && !(t.at[size_t(at)] < from);)
                at = at > 0 ? t.text.lastIndexOf(needle, at - 1, Qt::CaseInsensitive) : -1;
        } else {
            for (at = t.text.indexOf(needle, 0, Qt::CaseInsensitive); at >= 0 && pass == 0 && !(from < t.at[size_t(at)]);)
                at = t.text.indexOf(needle, at + 1, Qt::CaseInsensitive);
        }
        if (at >= 0) {
            selStart_ = t.at[size_t(at)];
            selEnd_ = t.at[size_t(at + needle.size())];
            selecting_ = false;
            QScrollBar* bar = verticalScrollBar();
            if (selStart_.line < bar->value() || selEnd_.line >= bar->value() + rows_) bar->setValue(selStart_.line - rows_ / 2);
            viewport()->update();
            return;
        }
        if (pass > 0 && first == start) break;
        first = older ? lineStart(first > 0 ? first - 1 : total - 1) : last + 1 < total ? last + 1 : 0;
    }
    findStatus_->setText(tr("Not found"));
    placeFindBar();
}

bool TerminalView::event(QEvent* event)
{
    if (event->type() == QEvent::ShortcutOverride && hasFocus()) { // not the files panel's: it's a child
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
    if ((m & ~(Qt::ShiftModifier | Qt::KeypadModifier)) == Qt::ControlModifier) {
        const int key = event->key();
        if (key == Qt::Key_Plus || key == Qt::Key_Equal || key == Qt::Key_Minus || (key == Qt::Key_0 && !(m & Qt::ShiftModifier)))
            return zoom(key == Qt::Key_Minus ? -1 : key == Qt::Key_0 ? 0 : 1);
    }
    if (event->key() == Qt::Key_Control) hoverLink(viewport()->mapFromGlobal(QCursor::pos()), m | Qt::ControlModifier);
    typeKey(event->key(), m, event->text());
    emit keyTyped(event->key(), m, event->text());
}

void TerminalView::keyReleaseEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Control) hoverLink(viewport()->mapFromGlobal(QCursor::pos()), Qt::NoModifier);
    QAbstractScrollArea::keyReleaseEvent(event);
}

void TerminalView::typeKey(int key, Qt::KeyboardModifiers m, const QString& text)
{
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
        for (char32_t ch : text.toUcs4())
            if (ch >= 0x20 || ch == 0x1b) vterm_keyboard_unichar(vt_, uint32_t(ch), VTermModifier(mod & VTERM_MOD_ALT));
    }
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

// the text as shown, wrapped lines joined: what scrolls into the scrollback, the screen before a clear and at the end
void TerminalView::startLog()
{
    static QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString name = property("address").toString();
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
    const QString suggested = (name.isEmpty() ? QStringLiteral("terminal") : name) +
                              QDateTime::currentDateTime().toString(QStringLiteral("-yyyyMMdd-HHmmss")) + QStringLiteral(".log");
    const QString path = QFileDialog::getSaveFileName(this, tr("Log to File"), dir + QLatin1Char('/') + suggested,
                                                      tr("Log files (*.log *.txt);;All files (*)"));
    if (path.isEmpty()) return;
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Log to File"), tr("%1 could not be written: %2").arg(path, file->errorString()));
        return;
    }
    dir = QFileInfo(path).absolutePath();
    log_ = std::move(file);
    logged_ = pushed_;
}

void TerminalView::stopLog()
{
    if (!log_) return;
    logScreen();
    log_.reset();
}

void TerminalView::logLine(int line)
{
    Line buffer;
    const Line& cells = cellsAt(line, buffer);
    QString text;
    for (const VTermScreenCell& cell : cells)
        if (cell.chars[0] != uint32_t(-1)) {
            int n = 0;
            while (n < VTERM_MAX_CHARS_PER_CELL && cell.chars[n]) ++n;
            text += n ? QString::fromUcs4(reinterpret_cast<const char32_t*>(cell.chars), n) : QStringLiteral(" ");
        }
    if (!wraps(line))
        while (text.endsWith(QLatin1Char(' '))) text.chop(1);
    log_->write(text.toUtf8() + (wraps(line) ? "" : "\n"));
}

void TerminalView::logScrollback()
{
    const long long fresh = std::min<long long>(pushed_ - logged_, (long long)scrollback_.size());
    for (long long i = fresh; i > 0; --i) logLine(int(scrollback_.size() - size_t(i)));
    logged_ = std::max(logged_, pushed_);
    log_->flush();
}

void TerminalView::logScreen()
{
    logScrollback(); // what a resize left there
    const int sb = int(scrollback_.size());
    int last = cursor_.row;
    Line buffer;
    for (int row = rows_ - 1; row > last; --row) {
        const Line& cells = cellsAt(sb + row, buffer);
        if (std::any_of(cells.begin(), cells.end(), [](const VTermScreenCell& c) { return c.chars[0] && c.chars[0] != ' '; })) last = row;
    }
    for (int row = 0; row <= last; ++row) logLine(sb + row);
    if (wraps(sb + last)) log_->write("\n");
    log_->flush();
}
