#include "vnc/VncSession.h"

#include <rfb/rfbclient.h>

#include "core/Net.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rdc {

namespace {

char clientDataTag; // LibVNCClient client-data key

// per-thread log tail: shows the server's reason on connect failure
thread_local std::string lastError, lastLog;

void capture(std::string& into, const char* format, va_list args)
{
    char buffer[512];
    vsnprintf(buffer, sizeof buffer, format, args);
    into = buffer;
    while (!into.empty() && (into.back() == '\n' || into.back() == '\r')) into.pop_back();
}

const bool debugLog = getenv("PORTAL_MUX_DEBUG") != nullptr;

void logInfo(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    capture(lastLog, format, args);
    va_end(args);
    if (debugLog) fprintf(stderr, "vnc: %s\n", lastLog.c_str());
}

void logError(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    capture(lastError, format, args);
    va_end(args);
    fprintf(stderr, "vnc: %s\n", lastError.c_str());
}

std::string latin1ToUtf8(const char* s, int len)
{
    std::string out;
    for (int i = 0; i < len; ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += char(c);
        } else {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// classic cut text is Latin-1 only
std::string utf8ToLatin1(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (n == 1)
            out += c < 0x80 ? char(c) : '?';
        else if (n == 2 && i + 1 < s.size() && (c & 0x1F) <= 0x03)
            out += char(((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F));
        else
            out += '?';
        i += n;
    }
    return out;
}

// SendExtDesktopSize garbles the screen entry and TigerVNC then disconnects; copy it as-is
bool sendDesktopSize(rfbClient* c, uint16_t width, uint16_t height)
{
    rfbSetDesktopSizeMsg msg{};
    msg.type = rfbSetDesktopSize;
    msg.width = htons(width);
    msg.height = htons(height);
    msg.numberOfScreens = 1;
    rfbExtDesktopScreen screen = c->screen;
    screen.width = htons(width);
    screen.height = htons(height);
    if (!WriteToRFBServer(c, reinterpret_cast<const char*>(&msg), sz_rfbSetDesktopSizeMsg) ||
        !WriteToRFBServer(c, reinterpret_cast<const char*>(&screen), sz_rfbExtDesktopScreen))
        return false;
    // full update request before the flag, else both sides wait
    SendFramebufferUpdateRequest(c, 0, 0, width, height, FALSE);
    c->requestedResize = TRUE;
    return true;
}

bool isLoopback(const std::string& host)
{
    return host == "localhost" || host == "::1" || host.rfind("127.", 0) == 0;
}

} // namespace

struct VncSession::Callbacks {
    static VncSession* self(rfbClient* c) { return static_cast<VncSession*>(rfbClientGetClientData(c, &clientDataTag)); }

    static rfbBool mallocFrameBuffer(rfbClient* c)
    {
        VncSession* s = self(c);
        s->pixels_.assign(size_t(c->width) * c->height * 4, 0);
        c->frameBuffer = s->pixels_.data();
        s->framebuffer_.resize(c->width, c->height);
        s->listener_.onResize(c->width, c->height);
        return TRUE;
    }

    static void gotUpdate(rfbClient* c, int x, int y, int w, int h)
    {
        VncSession* s = self(c);
        const Rect r{x, y, w, h};
        s->framebuffer_.update(r, c->frameBuffer, c->width * 4);
        s->listener_.onFrameUpdated(r);
    }

    static char* getPassword(rfbClient* c)
    {
        VncSession* s = self(c);
        if (!s->connection_.password.empty() && !s->passwordUsed_) {
            s->passwordUsed_ = true;
            return strdup(s->connection_.password.c_str());
        }
        Credentials creds;
        creds.needsUser = false;
        if (!s->listener_.askCredentials("VNC password", creds)) {
            s->cancelled_ = true;
            return nullptr;
        }
        return strdup(creds.password.c_str());
    }

    static rfbCredential* getCredential(rfbClient* c, int type)
    {
        VncSession* s = self(c);
        auto* cred = static_cast<rfbCredential*>(calloc(1, sizeof(rfbCredential)));
        if (!cred || type == rfbCredentialTypeX509) return cred; // system CA store, no client certificate
        if (type != rfbCredentialTypeUser) {
            free(cred);
            return nullptr;
        }
        // VeNCrypt Plain, ARD, MS-Logon
        Credentials creds;
        creds.username = s->connection_.username;
        creds.password = s->connection_.password;
        if (creds.password.empty() || s->passwordUsed_) {
            if (!s->listener_.askCredentials("Log in to the VNC server", creds)) {
                s->cancelled_ = true;
                free(cred);
                return nullptr;
            }
        }
        s->passwordUsed_ = true;
        cred->userCredential.username = strdup(creds.username.c_str());
        cred->userCredential.password = strdup(creds.password.c_str());
        return cred;
    }

    static void gotCursorShape(rfbClient* c, int xhot, int yhot, int w, int h, int bytesPerPixel)
    {
        if (!c->rcSource || bytesPerPixel != 4 || w <= 0 || h <= 0) return;
        std::vector<uint8_t> image(size_t(w) * h * 4);
        for (size_t i = 0; i < size_t(w) * h; ++i) {
            std::memcpy(&image[i * 4], c->rcSource + i * 4, 3);
            image[i * 4 + 3] = c->rcMask && c->rcMask[i] ? 255 : 0;
        }
        self(c)->listener_.onCursor(std::move(image), w, h, xhot, yhot);
    }

    static void deliverClipboard(rfbClient* c, std::string text)
    {
        VncSession* s = self(c);
        s->lastClipboard_ = text; // so the UI's echo is not sent back
        s->listener_.onClipboardText(std::move(text));
    }

    static void gotCutText(rfbClient* c, const char* text, int len) { deliverClipboard(c, latin1ToUtf8(text, len)); }

    static void gotCutTextUtf8(rfbClient* c, const char* text, int len)
    {
        deliverClipboard(c, std::string(text, strnlen(text, size_t(len))));
    }
};

VncSession::VncSession(DisplayListener& listener, Connection connection)
    : listener_(listener), connection_(std::move(connection))
{
}

void VncSession::connect()
{
    if (!worker_.joinable()) worker_ = std::jthread([this](std::stop_token st) { run(st); });
}

void VncSession::disconnect()
{
    if (!worker_.joinable()) return;
    worker_.request_stop();
    {
        std::lock_guard lock(clientMutex_);
        if (client_ && client_->sock != RFB_INVALID_SOCKET) shutdown(client_->sock, SHUT_RDWR);
    }
    worker_.join();
}

void VncSession::sendKey(const KeyEvent& e)
{
    if (e.keysym == 0) return;
    tasks_.post([this, e] {
        if (connected_) SendKeyEvent(client_, e.keysym, e.pressed);
    });
}

void VncSession::sendPointer(const PointerEvent& e)
{
    tasks_.post([this, e] {
        if (!connected_) return;
        buttons_ = e.buttons;
        SendPointerEvent(client_, e.x, e.y, buttons_);
        // wheel = button 4 up / 5 down per notch
        for (wheelRemainder_ += e.wheelDelta; std::abs(wheelRemainder_) >= 120;) {
            const int button = wheelRemainder_ > 0 ? 8 : 16;
            SendPointerEvent(client_, e.x, e.y, buttons_ | button);
            SendPointerEvent(client_, e.x, e.y, buttons_);
            wheelRemainder_ -= wheelRemainder_ > 0 ? 120 : -120;
        }
    });
}

void VncSession::sendClipboardText(const std::string& text)
{
    tasks_.post([this, text] {
        if (!connected_ || text == lastClipboard_) return;
        lastClipboard_ = text;
        std::string utf8 = text;
        if (!SendClientCutTextUTF8(client_, utf8.data(), int(utf8.size()))) {
            std::string latin1 = utf8ToLatin1(text);
            SendClientCutText(client_, latin1.data(), int(latin1.size()));
        }
    });
}

void VncSession::resizeDesktop(int width, int height)
{
    tasks_.post([this, width, height] {
        wantWidth_ = width;
        wantHeight_ = height;
        applyResize();
    });
}

void VncSession::applyResize()
{
    rfbClient* c = client_;
    // width stays 0 until ExtendedDesktopSize; send each size once
    if (!connected_ || wantWidth_ <= 0 || c->screen.width == 0 || c->requestedResize) return;
    if ((c->width == wantWidth_ && c->height == wantHeight_) || (sentWidth_ == wantWidth_ && sentHeight_ == wantHeight_))
        return;
    sentWidth_ = wantWidth_;
    sentHeight_ = wantHeight_;
    sendDesktopSize(c, uint16_t(wantWidth_), uint16_t(wantHeight_));
}

void VncSession::run(std::stop_token stop)
{
    listener_.onStateChanged(SessionState::Connecting);
    static std::once_flag logOnce;
    std::call_once(logOnce, [] {
        rfbClientLog = logInfo;
        rfbClientErr = logError;
    });

    rfbClient* c = rfbGetClient(8, 3, 4);
    if (!c) {
        listener_.onError("Out of memory.");
        listener_.onStateChanged(SessionState::Error);
        return;
    }
    c->format.redShift = 16;
    c->format.greenShift = 8;
    c->format.blueShift = 0;
    c->canHandleNewFBSize = TRUE;
    encodings_ = connection_.option("encodings");
    c->appData.encodingsString = encodings_.c_str();
    c->appData.useRemoteCursor = connection_.flag("remoteCursor");
    c->appData.shareDesktop = connection_.flag("shared");
    c->appData.enableJPEG = TRUE;
    c->appData.qualityLevel = connection_.number("jpegQuality");
    c->appData.compressLevel = connection_.number("compression");
    c->MallocFrameBuffer = Callbacks::mallocFrameBuffer;
    c->GotFrameBufferUpdate = Callbacks::gotUpdate;
    c->GetPassword = Callbacks::getPassword;
    c->GetCredential = Callbacks::getCredential;
    c->GotCursorShape = Callbacks::gotCursorShape;
    c->GotXCutText = Callbacks::gotCutText;
    c->GotXCutTextUTF8 = Callbacks::gotCutTextUtf8;
    c->serverHost = strdup(connection_.host.c_str());
    c->serverPort = connection_.effectivePort();
    // shortcut: handshake can't be interrupted, closing a tab on a dead host waits up to 10 s
    c->connectTimeout = unsigned(std::max(1, connection_.number("timeout")));
    c->readTimeout = c->connectTimeout;
    rfbClientSetClientData(c, &clientDataTag, this);

    if (!rfbInitClient(c, nullptr, nullptr)) {
        if (!stop.stop_requested() && !cancelled_) {
            const std::string& why = !lastError.empty() ? lastError : lastLog;
            listener_.onError(why.empty() ? "Could not connect to the VNC server." : "Could not connect: " + why);
            listener_.onStateChanged(SessionState::Error);
        } else {
            listener_.onStateChanged(SessionState::Disconnected);
        }
        return;
    }
    {
        std::lock_guard lock(clientMutex_);
        client_ = c;
    }

    bool failedEarly = false;
    const bool encrypted = c->tlsSession != nullptr;
    if (!encrypted && connection_.option("security") == "encrypted" && !isLoopback(connection_.host)) {
        listener_.onError("The VNC server did not offer an encrypted connection (VeNCrypt/TLS), and this "
                          "connection only allows encrypted ones.");
        failedEarly = true;
    }
    bool ok = !failedEarly && (encrypted || isLoopback(connection_.host) ||
              listener_.askTrust("This VNC connection is not encrypted. Anyone on the network path can see the screen "
                                 "and what you type. Connect anyway?",
                                 "Use an SSH tunnel (Edit connection > SSH tunnel) or a server with VeNCrypt/TLS."));
    if (ok) {
        connected_ = true;
        listener_.onStateChanged(SessionState::Connected);
        tasks_.drain();
    }
    bool failed = false;
    while (ok && !stop.stop_requested()) {
        // shortcut: 10 ms input poll, use a wake pipe if idle wakeups matter
        // buffered messages count too: WaitForMessage only checks the socket
        const int ready = c->buffered > 0 ? 1 : WaitForMessage(c, 10000);
        if (ready < 0 || (ready > 0 && !HandleRFBServerMessage(c))) {
            failed = true;
            break;
        }
        tasks_.drain();
        applyResize();
    }
    connected_ = false;
    {
        std::lock_guard lock(clientMutex_);
        client_ = nullptr;
    }
    c->frameBuffer = nullptr;
    rfbClientCleanup(c);

    if (failedEarly) {
        listener_.onStateChanged(SessionState::Error);
    } else if (failed && !stop.stop_requested()) {
        listener_.onError(lastError.empty() ? "The VNC connection was closed." : lastError);
        listener_.onStateChanged(SessionState::Error);
    } else {
        listener_.onStateChanged(SessionState::Disconnected);
    }
}

} // namespace rdc
