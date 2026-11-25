#include "app/RemoteView.h"
#include "app/Icons.h"

#include "app/KeyboardGrab.h"
#include "app/Keymap.h"
#include "app/LockKeys.h"

#include <QBuffer>
#include <QClipboard>
#include <QDropEvent>
#include <QFileInfo>
#include <QPointer>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMimeData>
#include <QUrl>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>

#include <cstring>
#include <utility>

static const QString remoteMark = QStringLiteral("application/x-portal-mux-remote"); // ours: not offered back

using rdc::KeyEvent;
using rdc::PointerEvent;
using rdc::Rect;
using rdc::SessionState;

RemoteView::RemoteView(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAcceptDrops(true);
    resizeTimer_.setSingleShot(true);
    resizeTimer_.setInterval(300);
    connect(&resizeTimer_, &QTimer::timeout, this, &RemoteView::requestDesktopSize);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &RemoteView::localClipboardChanged);
    fpsTimer_.setInterval(1000);
    connect(&fpsTimer_, &QTimer::timeout, this, [this] {
        const bool changed = frames_ != fps_;
        fps_ = std::exchange(frames_, 0);
        if (changed) emit infoChanged();
    });
}

RemoteView::~RemoteView()
{
    prompts_.close(); // a worker blocked on a dialog gets "declined"
    if (keyboardGrabbed_) keyboardGrab::set(this, false);
    if (session_) session_->disconnect();
}

void RemoteView::start(std::unique_ptr<rdc::DisplaySession> session, Options options)
{
    session_ = std::move(session);
    options_ = options;
    unscaledMode_ = options.mode == Options::Mode::Scale ? Options::Mode::None : options.mode;
    if (parentWidget()) parentWidget()->installEventFilter(this);
    applyMode();
    applyCursor();
    fpsTimer_.start();
    localClipboardChanged();
    // real geometry first, for the initial size
    QTimer::singleShot(0, this, [this] {
        if (options_.sendInitialSize) requestDesktopSize();
        session_->connect();
    });
}

void RemoteView::setViewOnly(bool on)
{
    if (on) releaseKeys();
    options_.viewOnly = on;
    updateGamepadTarget();
    updateKeyboardGrab();
    emit infoChanged();
}

void RemoteView::setClipboardSharing(bool on)
{
    options_.clipboard = on;
    localClipboardChanged();
}

void RemoteView::setLocalCursor(bool on)
{
    options_.localCursor = on;
    applyCursor();
}

void RemoteView::setGrabKeys(bool on)
{
    options_.grabKeys = on;
    updateKeyboardGrab();
}

void RemoteView::setScaled(bool on)
{
    options_.mode = on ? Options::Mode::Scale : unscaledMode_;
    applyMode();
    if (options_.followsTab()) requestDesktopSize();
}

RemoteView::DisplayInfo RemoteView::displayInfo() const
{
    DisplayInfo d{.fps = fps_, .viewOnly = options_.viewOnly, .grabbed = keyboardGrabbed_};
    if (image_.isNull()) return d;
    d.size = image_.size();
    const qreal logical = image_.width() / image_.devicePixelRatio();
    d.percent = logical > 0 ? qRound(100 * imageRect().width() / logical) : 100;
    return d;
}

void RemoteView::syncLocks()
{
    if (!session_ || options_.viewOnly) return;
    if (const auto locks = localLockKeys()) session_->syncLocks(*locks);
}

void RemoteView::sendKeys(const std::vector<KeyEvent>& keys)
{
    if (!session_ || options_.viewOnly) return;
    for (KeyEvent k : keys) {
        k.pressed = true;
        session_->sendKey(k);
    }
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        KeyEvent k = *it;
        k.pressed = false;
        session_->sendKey(k);
    }
}

void RemoteView::updateOverlay(SessionState state)
{
    if (state == SessionState::Connected) wasConnected_ = true;
    const bool ended = state == SessionState::Disconnected, connecting = state == SessionState::Connecting;
    dimmed_ = ended || (connecting && wasConnected_);
    update();
    if ((ended || connecting) && !overlay_ && parentWidget()) {
        overlay_ = new QPushButton(parentWidget()); // viewport child: stays in view
        connect(overlay_, &QPushButton::clicked, this, &RemoteView::reconnectRequested);
    }
    if (!overlay_) return;
    overlay_->setIcon(ended ? icons::lucide("refresh-cw") : QIcon());
    overlay_->setText(ended ? tr("Session ended — &Reconnect") : wasConnected_ ? tr("Reconnecting…") : tr("Connecting…"));
    overlay_->setEnabled(ended);
    overlay_->setVisible(ended || connecting); // a host that doesn't answer: not an empty tab for 15 s
    placeOverlay();
}

void RemoteView::placeOverlay()
{
    if (!overlay_ || overlay_->isHidden()) return;
    overlay_->adjustSize();
    overlay_->move((parentWidget()->width() - overlay_->width()) / 2, (parentWidget()->height() - overlay_->height()) / 2);
    const QObjectList& siblings = parentWidget()->children(); // right above the view: under the tab's prompts
    for (qsizetype i = siblings.indexOf(this) + 1; i < siblings.size(); ++i)
        if (siblings[i] != overlay_ && siblings[i]->isWidgetType()) return overlay_->stackUnder(static_cast<QWidget*>(siblings[i]));
    overlay_->raise();
}

void RemoteView::applyMode()
{
    const bool scale = options_.mode == Options::Mode::Scale;
    if (auto* scroll = qobject_cast<QScrollArea*>(parentWidget() ? parentWidget()->parentWidget() : nullptr))
        scroll->setWidgetResizable(scale);
    if (scale) {
        setMinimumSize(0, 0);
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    }
    if (!image_.isNull()) applyImageSize();
    update();
    emit infoChanged();
}

void RemoteView::applyCursor()
{
    if (!options_.localCursor)
        setCursor(Qt::BlankCursor); // the remote draws its own
    else if (remoteCursor_)
        setCursor(*remoteCursor_);
    else
        unsetCursor();
}

void RemoteView::updateKeyboardGrab()
{
    const bool grab = session_ && options_.grabKeys && !options_.viewOnly && hasFocus() && isActiveWindow();
    if (grab == keyboardGrabbed_) return;
    keyboardGrabbed_ = grab;
    keyboardGrab::set(this, grab);
    emit infoChanged();
}

void RemoteView::localClipboardChanged()
{
    if (!session_ || !options_.clipboard) return;
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || mime->hasFormat(remoteMark)) return;
    if (mime->hasUrls() && mime->urls() == lastRemoteUrls_) return;
    QStringList files;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile()) files << QString::fromUtf8(url.toEncoded());
    if (!files.isEmpty()) {
        session_->sendClipboardUris((files.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toStdString());
        return;
    }
    const QString text = mime->text();
    if (mime->hasImage() && session_->takesClipboardImages()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        QImage flat(image.size(), QImage::Format_RGB32); // Windows makes few-colour bitmaps black and white
        flat.fill(Qt::white);
        QPainter(&flat).drawImage(0, 0, image);
        QByteArray bmp;
        QBuffer buffer(&bmp);
        buffer.open(QIODevice::WriteOnly);
        if (!image.isNull() && flat.save(&buffer, "BMP"))
            return session_->sendClipboardImage(bmp.toStdString(), text.toStdString());
    }
    if (!text.isEmpty() && text != lastRemoteClipboard_) session_->sendClipboardText(text.toStdString());
}

// ---- DisplayListener (worker thread) ----

void RemoteView::onStateChanged(SessionState state)
{
    QMetaObject::invokeMethod(this, [this, state, asked = asked_] {
        if (state == SessionState::Connected) {
            typed_ = asked;
            updateGamepadTarget(true); // session drops controller input until now
            syncLocks();
        }
        live_ = state == SessionState::Connecting || state == SessionState::Connected;
        updateOverlay(state);
        emit stateChanged(state);
    }, Qt::QueuedConnection);
}

void RemoteView::onError(std::string message)
{
    QMetaObject::invokeMethod(this, [this, text = QString::fromStdString(message)] {
        prompts::errorIn(this, tr("Connection error"), text, tr("&Reconnect"), [this](bool again) {
            if (again) emit reconnectRequested();
            else emit closeRequested();
        });
    }, Qt::QueuedConnection);
}

bool RemoteView::askTrust(const std::string& title, const std::string& details, bool* always)
{
    const bool ok = prompts::trust(prompts_, this, QString::fromStdString(title), QString::fromStdString(details), always);
    if (ok && always && *always) QMetaObject::invokeMethod(this, [this] { emit alwaysTrustChosen(); }, Qt::QueuedConnection);
    return ok;
}

bool RemoteView::askCredentials(const std::string& prompt, rdc::Credentials& creds)
{
    const bool ok = prompts::credentials(prompts_, this, QString::fromStdString(prompt), creds);
    if (ok) asked_ = creds;
    return ok;
}

void RemoteView::onAudio(const int16_t* samples, size_t frames, int sampleRate, int channels)
{
    QByteArray pcm(reinterpret_cast<const char*>(samples), qsizetype(frames * channels * sizeof(int16_t)));
    QMetaObject::invokeMethod(this, [this, pcm, sampleRate, channels] { audio_.play(pcm, sampleRate, channels); },
                              Qt::QueuedConnection);
}

void RemoteView::onResize(int, int)
{
}

void RemoteView::onFrameUpdated(const Rect&)
{
    if (!framePending_.exchange(true))
        QMetaObject::invokeMethod(this, [this] { drainFrame(); }, Qt::QueuedConnection);
}

void RemoteView::onGamepadRumble(int index, uint16_t lowFrequency, uint16_t highFrequency)
{
    QMetaObject::invokeMethod(this, [this, index, lowFrequency, highFrequency] {
        if (gamepads_ && gamepadTarget_) gamepads_->rumble(index, lowFrequency, highFrequency);
    }, Qt::QueuedConnection);
}

void RemoteView::onGamepadTriggerRumble(int index, uint16_t left, uint16_t right)
{
    QMetaObject::invokeMethod(this, [this, index, left, right] {
        if (gamepads_ && gamepadTarget_) gamepads_->rumbleTriggers(index, left, right);
    }, Qt::QueuedConnection);
}

void RemoteView::setGamepads(Gamepads* gamepads)
{
    gamepads_ = gamepads;
    connect(gamepads, &Gamepads::changed, this, [this](const rdc::GamepadState& state) {
        if (session_ && (gamepadTarget_ || !state.present)) session_->sendGamepad(state); // unplugging always reaches the host
    });
    updateGamepadTarget();
}

void RemoteView::updateGamepadTarget(bool resend)
{
    if (!gamepads_ || !session_) return;
    const bool target = isVisible() && isActiveWindow() && !options_.viewOnly;
    if (target == gamepadTarget_ && !resend) return;
    const bool was = gamepadTarget_;
    gamepadTarget_ = target;
    if (options_.viewOnly && !was) return;
    for (rdc::GamepadState state : gamepads_->states()) {
        if (!target) {
            state.buttons = 0;
            state.leftX = state.leftY = state.rightX = state.rightY = 0;
            state.leftTrigger = state.rightTrigger = 0;
            gamepads_->rumble(state.index, 0, 0);
            gamepads_->rumbleTriggers(state.index, 0, 0);
        }
        session_->sendGamepad(state);
    }
}

void RemoteView::onClipboardText(std::string text)
{
    QMetaObject::invokeMethod(this, [this, text = QString::fromStdString(text)] {
        if (!options_.clipboard) return;
        lastRemoteClipboard_ = text;
        QGuiApplication::clipboard()->setText(text);
    }, Qt::QueuedConnection);
}

void RemoteView::onClipboardImage(std::string bmp)
{
    QMetaObject::invokeMethod(this, [this, bmp = QByteArray::fromStdString(bmp)] {
        if (!options_.clipboard) return;
        const QImage image = QImage::fromData(bmp, "BMP");
        if (image.isNull()) return;
        auto* mime = new QMimeData;
        mime->setImageData(image);
        mime->setData(remoteMark, {});
        QGuiApplication::clipboard()->setMimeData(mime);
    }, Qt::QueuedConnection);
}

void RemoteView::onClipboardUris(std::string uriList)
{
    QMetaObject::invokeMethod(this, [this, uriList = QString::fromStdString(uriList)] {
        if (!options_.clipboard) return;
        QList<QUrl> urls;
        QStringList lines;
        for (const QString& line : uriList.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts))
            if (!line.startsWith(QLatin1Char('#'))) {
                urls << QUrl::fromEncoded(line.toUtf8());
                lines << line;
            }
        if (urls.isEmpty()) return;
        auto* mime = new QMimeData;
        mime->setUrls(urls);
        mime->setData(QStringLiteral("x-special/gnome-copied-files"), ("copy\n" + lines.join(QLatin1Char('\n'))).toUtf8());
        lastRemoteUrls_ = urls;
        QGuiApplication::clipboard()->setMimeData(mime);
    }, Qt::QueuedConnection);
}

void RemoteView::onCursor(std::vector<uint8_t> bgra, int width, int height, int hotX, int hotY)
{
    QMetaObject::invokeMethod(this, [this, bgra = std::move(bgra), width, height, hotX, hotY] {
        if (bgra.empty()) {
            remoteCursor_.reset();
        } else {
            // BGRA == ARGB32 little-endian; copy() detaches
            QImage image(bgra.data(), width, height, width * 4, QImage::Format_ARGB32);
            QPixmap pixmap = QPixmap::fromImage(image.copy());
            pixmap.setDevicePixelRatio(devicePixelRatioF());
            remoteCursor_ = QCursor(pixmap, int(hotX / devicePixelRatioF()), int(hotY / devicePixelRatioF()));
        }
        applyCursor();
    }, Qt::QueuedConnection);
}

// ---- UI thread ----

void RemoteView::drainFrame()
{
    framePending_ = false; // reset first: updates during the copy schedule another drain
    if (!session_) return;

    Rect dirty;
    bool resized = false;
    ++frames_;
    session_->framebuffer().consume([&](const uint8_t* px, int w, int h, int stride, Rect d) {
        if (image_.width() != w || image_.height() != h) {
            image_ = QImage(w, h, QImage::Format_RGB32);
            d = {0, 0, w, h};
            resized = true;
        }
        for (int y = d.y; y < d.y + d.height; ++y)
            std::memcpy(image_.scanLine(y) + size_t(d.x) * 4, px + size_t(y) * stride + size_t(d.x) * 4,
                        size_t(d.width) * 4);
        dirty = d;
    });

    if (!dirty.empty()) emit imageUpdated(QRect(dirty.x, dirty.y, dirty.width, dirty.height));
    if (resized) {
        applyImageSize();
    } else if (options_.mode == Options::Mode::Scale) {
        const QRectF r = imageRect();
        const qreal dpr = image_.devicePixelRatio();
        if (!dirty.empty() && qFuzzyCompare(r.width(), image_.width() / dpr) && qFuzzyCompare(r.height(), image_.height() / dpr))
            update(QRectF(r.x() + dirty.x / dpr, r.y() + dirty.y / dpr, dirty.width / dpr, dirty.height / dpr).toAlignedRect());
        else
            update(); // shortcut: scaled repaints everything, map dirty rects if slow
    } else if (!dirty.empty()) {
        const qreal dpr = image_.devicePixelRatio();
        update(QRectF(dirty.x / dpr, dirty.y / dpr, dirty.width / dpr, dirty.height / dpr).toAlignedRect());
    }
}

void RemoteView::applyImageSize()
{
    image_.setDevicePixelRatio(devicePixelRatioF());
    if (options_.mode != Options::Mode::Scale) setFixedSize((QSizeF(image_.size()) / devicePixelRatioF()).toSize());
    update();
    emit infoChanged();
}

void RemoteView::relaunchAtWindowSize()
{
    if (!session_ || !parentWidget()) return;
    const QSize size = parentWidget()->size() * devicePixelRatioF();
    session_->relaunchAtSize(size.width(), size.height());
}

void RemoteView::requestDesktopSize()
{
    if (!session_ || !parentWidget()) return;
    const QSize size = options_.desktopSize.isValid() ? options_.desktopSize : parentWidget()->size() * devicePixelRatioF();
    if (!options_.desktopSize.isValid()) session_->setPixelRatio(devicePixelRatioF()); // in this display's pixels: scaled as it
    if (size.width() > 0 && size.height() > 0) session_->resizeDesktop(size.width(), size.height());
}

bool RemoteView::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::ShortcutOverride:
        // every key goes remote, menu shortcuts too
        if (session_ && !options_.viewOnly &&
            (options_.grabKeys ||
             !windowShortcuts_.contains(QKeySequence(static_cast<QKeyEvent*>(event)->keyCombination())))) {
            event->accept();
            return true;
        }
        break;
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::WindowActivate:
    case QEvent::WindowDeactivate:
        QMetaObject::invokeMethod(this, [this] { updateGamepadTarget(); }, Qt::QueuedConnection); // once visibility settled
        break;
    case QEvent::DevicePixelRatioChange:
        if (!image_.isNull()) applyImageSize();
        if (options_.followsTab()) resizeTimer_.start();
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

bool RemoteView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize && options_.followsTab())
        resizeTimer_.start();
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        placeOverlay();
        emit infoChanged();
    }
    return QWidget::eventFilter(watched, event);
}

QRectF RemoteView::imageRect() const
{
    const QSizeF logical = QSizeF(image_.size()) / image_.devicePixelRatio();
    if (options_.mode != Options::Mode::Scale || logical.isEmpty()) return QRectF(QPointF(0, 0), logical);
    const QSizeF fitted = logical.scaled(size(), Qt::KeepAspectRatio);
    return QRectF(QPointF((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
}

void RemoteView::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    if (options_.mode == Options::Mode::Scale) {
        painter.fillRect(rect(), Qt::black);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(imageRect(), image_);
    } else {
        const QRect target = event->rect();
        const qreal dpr = image_.devicePixelRatio();
        painter.drawImage(target, image_, QRectF(target.topLeft() * dpr, QSizeF(target.size()) * dpr));
    }
    if (dimmed_) painter.fillRect(event->rect(), QColor(0, 0, 0, 140));
}

static PointerEvent toPointer(QPointF pos, QRectF r, QSize image, Qt::MouseButtons b)
{
    PointerEvent pe;
    pe.x = r.width() > 0 ? int((pos.x() - r.x()) * image.width() / r.width()) : 0;
    pe.y = r.height() > 0 ? int((pos.y() - r.y()) * image.height() / r.height()) : 0;
    pe.buttons = (b & Qt::LeftButton ? PointerEvent::Left : 0) | (b & Qt::MiddleButton ? PointerEvent::Middle : 0)
               | (b & Qt::RightButton ? PointerEvent::Right : 0);
    return pe;
}

void RemoteView::sendPointer(QMouseEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress) setFocus();
    if (!session_ || options_.viewOnly) return;
    // buttons() is the state after the event
    session_->sendPointer(toPointer(event->position(), imageRect(), image_.size(), event->buttons()));
}

void RemoteView::pointerAt(QPoint remote, Qt::MouseButtons buttons, int wheelDelta)
{
    if (!session_ || options_.viewOnly) return;
    PointerEvent pe = toPointer(remote, QRectF(QPointF(0, 0), QSizeF(image_.size())), image_.size(), buttons);
    pe.wheelDelta = wheelDelta;
    session_->sendPointer(pe);
}

void RemoteView::wheelEvent(QWheelEvent* event)
{
    const QPoint delta = event->angleDelta();
    if (!session_ || options_.viewOnly || delta.isNull()) return;
    PointerEvent pe = toPointer(event->position(), imageRect(), image_.size(), event->buttons());
    pe.wheelDelta = delta.y();   // 120 per notch, positive = away
    pe.hwheelDelta = -delta.x(); // Qt's positive is left
    session_->sendPointer(pe);
}

void RemoteView::sendKey(QKeyEvent* event, bool pressed)
{
    if (!session_ || options_.viewOnly) return;
    if (!pressed && event->isAutoRepeat()) return; // X11 sends release+press on auto-repeat
#ifdef Q_OS_MACOS
    const uint32_t native = event->nativeVirtualKey(); // Qt leaves nativeScanCode() unset there
#else
    const uint32_t native = event->nativeScanCode();
#endif
    KeyEvent ke;
    if (pressed) {
        ke.scancode = xtScancode(event);
        ke.keysym = keysym(event);
        ke.text = event->text().toStdString();
        ke.pressed = true;
        pressedKeys_[native] = ke;
    } else {
        // keysym may differ if a modifier changed
        auto it = pressedKeys_.find(native);
        if (it == pressedKeys_.end()) return;
        ke = it->second;
        ke.pressed = false;
        pressedKeys_.erase(it);
    }
    session_->sendKey(ke);
}

void RemoteView::releaseKeys()
{
    if (session_)
        for (auto& [code, ke] : pressedKeys_) {
            ke.pressed = false;
            session_->sendKey(ke);
        }
    pressedKeys_.clear();
}

static QStringList droppedFiles(const QMimeData* mime) // regular files only: folders can't be sent
{
    QStringList files;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) files << url.toLocalFile();
    return files;
}

void RemoteView::dragEnterEvent(QDragEnterEvent* event)
{
    if (session_ && !options_.viewOnly && session_->canSendFiles() && !droppedFiles(event->mimeData()).isEmpty())
        event->acceptProposedAction();
}

void RemoteView::dropEvent(QDropEvent* event)
{
    const QStringList files = droppedFiles(event->mimeData());
    if (!session_ || files.isEmpty()) return;
    event->acceptProposedAction();
    std::vector<std::string> paths;
    for (const QString& file : files) paths.push_back(file.toStdString());
    emit message(tr("Copying %n file(s) to the remote side…", "", int(files.size())), 0);
    session_->sendFiles(paths, [view = QPointer<RemoteView>(this), n = int(files.size())](std::string error) {
        if (!view) return;
        if (error.empty()) return emit view->message(tr("Copied %n file(s) to the remote side", "", n), 8000);
        emit view->message({}, 0);
        prompts::errorIn(view, tr("Copy Files"), QString::fromStdString(error), {});
    });
}

void RemoteView::focusInEvent(QFocusEvent* event)
{
    updateKeyboardGrab();
    syncLocks();
    QWidget::focusInEvent(event);
}

void RemoteView::focusOutEvent(QFocusEvent* event)
{
    if (session_ && options_.clipboard) session_->pullClipboard();
    releaseKeys();
    updateKeyboardGrab();
    QWidget::focusOutEvent(event);
}
