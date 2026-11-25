#pragma once

#include "app/AudioPlayer.h"
#include "app/Gamepads.h"
#include "app/Prompts.h"
#include "core/Session.h"

#include <QCursor>
#include <QImage>
#include <QKeySequence>
#include <QPointer>
#include <QUrl>
#include <QTimer>
#include <QWidget>

class QPushButton;

#include <atomic>
#include <map>
#include <memory>
#include <optional>

// Shows a session's framebuffer at 1:1 and forwards input.
class RemoteView : public QWidget, public rdc::DisplayListener {
    Q_OBJECT

public:
    struct Options {
        enum class Mode { Resize, Scale, None } mode = Mode::Resize;
        bool sendInitialSize = true;
        QSize desktopSize;
        bool fitRemote = false;
        bool followsTab() const { return mode == Mode::Resize || (mode == Mode::Scale && fitRemote); }
        bool viewOnly = false;
        bool clipboard = true;
        bool localCursor = true;
        bool grabKeys = false;
    };

    explicit RemoteView(QWidget* parent = nullptr);
    ~RemoteView() override;

    void start(std::unique_ptr<rdc::DisplaySession> session, Options options);
    void setGamepads(Gamepads* gamepads);
    void setWindowShortcuts(QList<QKeySequence> keys) { windowShortcuts_ = std::move(keys); }

    const Options& options() const { return options_; }
    QImage screenshot() const { return image_; }
    const QImage& image() const { return image_; }
    void pointerAt(QPoint remote, Qt::MouseButtons buttons, int wheelDelta = 0); // in the remote's pixels
    void setViewOnly(bool on);
    void setClipboardSharing(bool on);
    void relaunchAtWindowSize();
    void setLocalCursor(bool on);
    void setGrabKeys(bool on);
    void setScaled(bool on);
    // for combos the local desktop grabs (Ctrl+Alt+Del)
    void sendKeys(const std::vector<rdc::KeyEvent>& keys);
    struct DisplayInfo {
        QSize size;
        int percent = 100;
        int fps = 0;
        bool viewOnly = false, grabbed = false;
    };
    DisplayInfo displayInfo() const;
    rdc::DisplaySession* session() const { return session_.get(); }

    // worker thread
    void onStateChanged(rdc::SessionState state) override;
    bool live() const { return live_; }
    void onError(std::string message) override;
    bool askTrust(const std::string& title, const std::string& details, bool* always) override;
    bool askCredentials(const std::string& prompt, rdc::Credentials& creds) override;
    rdc::Credentials typedCredentials() const { return typed_; } // answered at a prompt, once that connected
    void onResize(int width, int height) override;
    void onFrameUpdated(const rdc::Rect& dirty) override;
    void onClipboardText(std::string text) override;
    void onClipboardUris(std::string uriList) override;
    void onClipboardImage(std::string bmp) override;
    void onCursor(std::vector<uint8_t> bgra, int width, int height, int hotX, int hotY) override;
    void onAudio(const int16_t* samples, size_t frames, int sampleRate, int channels) override;
    void onGamepadRumble(int index, uint16_t lowFrequency, uint16_t highFrequency) override;
    void onGamepadTriggerRumble(int index, uint16_t left, uint16_t right) override;

signals:
    void stateChanged(rdc::SessionState state);
    void closeRequested();
    void alwaysTrustChosen();
    void reconnectRequested();
    void infoChanged();
    void imageUpdated(QRect dirty); // in the remote's pixels
    void message(const QString& text, int timeoutMs);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override { sendPointer(event); }
    void mouseReleaseEvent(QMouseEvent* event) override { sendPointer(event); }
    void mouseMoveEvent(QMouseEvent* event) override { sendPointer(event); }
    void wheelEvent(QWheelEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override { sendKey(event, true); }
    void keyReleaseEvent(QKeyEvent* event) override { sendKey(event, false); }
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool focusNextPrevChild(bool) override { return false; } // Tab goes to the remote side

private:
    void drainFrame();
    void localClipboardChanged();
    QRectF imageRect() const;
    void applyImageSize();
    void requestDesktopSize();
    void sendPointer(QMouseEvent* event);
    void sendKey(QKeyEvent* event, bool pressed);
    void updateGamepadTarget(bool resend = false);
    void updateKeyboardGrab();
    void releaseKeys();
    void syncLocks(); // remote lock keys follow this computer's
    void applyCursor();
    void applyMode();
    void updateOverlay(rdc::SessionState state);
    void placeOverlay();

    std::unique_ptr<rdc::DisplaySession> session_;
    PromptBridge prompts_{this, [this] { emit closeRequested(); }};
    rdc::Credentials asked_; // the worker's
    rdc::Credentials typed_;
    AudioPlayer audio_{this};
    QImage image_;
    QTimer resizeTimer_;
    Options options_;
    QString lastRemoteClipboard_; // our own text; not echoed back
    QList<QUrl> lastRemoteUrls_; // our own files; never offered back
    std::atomic<bool> framePending_{false}; // coalesces worker notifications
    std::map<uint32_t, rdc::KeyEvent> pressedKeys_;
    QPointer<Gamepads> gamepads_;
    bool gamepadTarget_ = false;
    QList<QKeySequence> windowShortcuts_;
    bool keyboardGrabbed_ = false;
    std::optional<QCursor> remoteCursor_;
    Options::Mode unscaledMode_ = Options::Mode::None;
    bool wasConnected_ = false;
    bool live_ = false;
    QTimer fpsTimer_;
    int frames_ = 0, fps_ = 0;
    bool dimmed_ = false;
    QPushButton* overlay_ = nullptr;
};
