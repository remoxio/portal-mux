#pragma once

#include "app/Prompts.h"
#include "core/Session.h"

#include <QAbstractScrollArea>
#include <QPointer>
#include <QElapsedTimer>
#include <QFont>

#include <deque>
#include <memory>
#include <vector>

#include <vterm.h>

class QLabel;
class QLineEdit;

// Terminal widget on libvterm; mouse-aware programs get clicks, Shift+mouse selects.
class TerminalView : public QAbstractScrollArea, public rdc::TerminalListener {
    Q_OBJECT

public:
    explicit TerminalView(QWidget* parent = nullptr);
    ~TerminalView() override;

    // empty family / 0 size = system default
    void configure(const QString& fontFamily, int fontSize, int scrollbackLines);
    void start(std::unique_ptr<rdc::TerminalSession> session);
    QSize grid() const { return {cols_, rows_}; }

    void onStateChanged(rdc::SessionState state) override;
    bool live() const { return live_; }
    void onError(std::string message) override;
    bool askTrust(const std::string& title, const std::string& details, bool* always) override;
    bool askCredentials(const std::string& prompt, rdc::Credentials& creds) override;

    // a widget left of the terminal, as wide as its edge is dragged; null: none
    void setSidePanel(QWidget* panel);
    QWidget* sidePanel() const { return side_; }
    void setSidePanelShown(bool on);
    bool sidePanelShown() const { return side_ && !side_->isHidden(); }
    std::string takeTypedPassword(); // what the login prompt was answered with, once
    void paste(const QString& text);
    void onData(std::string bytes) override;

    struct Callbacks;

signals:
    void stateChanged(rdc::SessionState state);
    void closeRequested(); // the user cancelled a login or trust prompt, or the remote shell exited
    void titleChanged(const QString& title);
    void reconnectRequested();
    void gridChanged();
    void bell();
    void directoryChanged(const QString& path); // the shell's, by OSC 7

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void scrollContentsBy(int, int) override;
    bool focusNextPrevChild(bool next) override; // Tab goes to the shell

private:
    using Line = std::vector<VTermScreenCell>;
    struct Pos {
        int line = -1, col = 0; // 0 = oldest scrollback line
        auto operator<=>(const Pos&) const = default;
    };

    void feed(const char* data, size_t len);
    void send(const char* data, size_t len);
    void pasteClipboard();
    bool clipboardImage() const; // to upload: an image without text, and a session that can
    void resizeTerminal();
    void updateScrollBar(bool stickToBottom);
    Line lineAt(int line) const;
    Pos posAt(QPoint point) const;
    bool selected(int line, int col) const;
    QString selectedText() const;
    QColor color(VTermColor c) const;
    bool reportsMouse(Qt::KeyboardModifiers m) const { return mouseMode_ != VTERM_PROP_MOUSE_NONE && !(m & Qt::ShiftModifier); }
    void mouseToProgram(QPointF point, Qt::KeyboardModifiers m);
    void openFind();
    void find(bool older, bool restart);
    void placeFindBar();
    void placeSidePanel();

    int rows_ = 24, cols_ = 80; // before vt_: used to create it
    VTerm* vt_;
    VTermScreen* screen_;
    std::deque<Line> scrollback_;
    std::unique_ptr<rdc::TerminalSession> session_;
    PromptBridge prompts_{this, [this] { emit closeRequested(); }};
    QFont font_;
    int cellW_ = 8, cellH_ = 16, ascent_ = 12;
    VTermPos cursor_{0, 0};
    bool cursorVisible_ = true;
    bool connected_ = false;
    bool live_ = false;
    Pos selStart_, selEnd_;
    bool selecting_ = false;
    QElapsedTimer lineClick_; // double click; a third click selects the line
    size_t maxScrollback_ = 10000;
    int mouseMode_ = VTERM_PROP_MOUSE_NONE;
    bool altScreen_ = false;
    int wheelRest_ = 0; // eighths of a degree not yet sent as a wheel step
    std::string titlePart_; // the title arrives in pieces
    std::string oscPart_;   // OSC 7 too
    std::string clipPart_;  // OSC 52, decoded
    QPointer<QWidget> side_;
    QWidget* sideEdge_ = nullptr;
    int sideWidth_ = 280;
    std::string typedPassword_; // written by the worker before it reports Connected
    QWidget* findBar_ = nullptr;
    QLineEdit* findText_ = nullptr;
    QLabel* findStatus_ = nullptr;
};
