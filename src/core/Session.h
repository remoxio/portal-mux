#pragma once

#include "core/Framebuffer.h"
#include "core/Types.h"

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace rdc {

struct Credentials {
    std::string username, password, domain;
    bool needsUser = true, needsDomain = false;
};

// worker thread; marshal to the UI thread
struct SessionListener {
    virtual ~SessionListener() = default;
    virtual void onStateChanged(SessionState state) = 0;
    virtual void onError(std::string message) = 0;
    // Blocking; false if declined or closing; `always` offers "Always trust"
    virtual bool askTrust(const std::string& title, const std::string& details, bool* always = nullptr) = 0;
    virtual bool askCredentials(const std::string& prompt, Credentials& creds) = 0;
};

struct DisplayListener : SessionListener {
    virtual void onResize(int width, int height) = 0;
    virtual void onFrameUpdated(const Rect& dirty) = 0;
    virtual void onClipboardText(std::string text) = 0;
    virtual void onClipboardUris(std::string uriList) = 0; // text/uri-list of local paths
    virtual void onClipboardFiles() {} // remote files without local paths: DisplaySession::offerClipboardFiles
    virtual void onClipboardImage(std::string /*bmp file*/) {}
    // BGRA, width*height*4 bytes; empty = hide
    virtual void onCursor(std::vector<uint8_t> bgra, int width, int height, int hotX, int hotY) = 0;
    // interleaved s16 PCM
    virtual void onAudio(const int16_t*, size_t /*frames*/, int /*sampleRate*/, int /*channels*/) {}
    // 0..0xFFFF per motor, until the next call
    virtual void onGamepadRumble(int /*index*/, uint16_t /*lowFrequency*/, uint16_t /*highFrequency*/) {}
    virtual void onGamepadTriggerRumble(int /*index*/, uint16_t /*left*/, uint16_t /*right*/) {}
};

struct TerminalListener : SessionListener {
    virtual void onData(std::string bytes) = 0;
};

// UI thread only; must not block on the network
class DisplaySession {
public:
    virtual ~DisplaySession() = default;
    virtual void connect() = 0;
    virtual void disconnect() = 0; // joins the worker; no listener calls after
    virtual void sendKey(const KeyEvent& event) = 0;
    virtual void sendPointer(const PointerEvent& event) = 0;
    virtual void sendClipboardText(const std::string& text) = 0;
    virtual void sendClipboardUris(const std::string&) {}
    virtual bool offerClipboardFiles() { return false; } // on the pasteboard, fetched on paste (macOS); false: not offered
    virtual void relayClipboardFiles() {} // another session's offered files to this one's server, fetched from that one on paste
    // a BMP file, offered with the text beside it
    virtual bool takesClipboardImages() const { return false; }
    virtual void sendClipboardImage(const std::string& /*bmp*/, const std::string& text) { if (!text.empty()) sendClipboardText(text); }
    virtual void pullClipboard() {} // hosts that can't announce a change: asked when the user leaves the view
    // local files to the remote side (SPICE: via its agent); `done`: an error or empty, UI thread
    virtual void relaunchAtSize(int, int) {} // Moonlight: quits the app on the host and starts it again at this size
    virtual bool canSendFiles() const { return false; }
    virtual void sendFiles(const std::vector<std::string>&, std::function<void(std::string)> done) { done("Not supported."); }
    virtual void syncLocks(uint8_t /*LockKey bits*/) {}
    // local USB devices the remote side can take (SPICE); `done` as above
    struct UsbDevice {
        std::string id, name;
        bool redirected = false;
    };
    virtual bool canRedirectUsb() const { return false; }
    virtual std::vector<UsbDevice> usbDevices() { return {}; }
    virtual void redirectUsb(const std::string&, bool, std::function<void(std::string)> done) { done("Not supported"); }
    virtual void resizeDesktop(int width, int height) = 0; // no-op if the protocol can't
    virtual void setPixelRatio(double) {}                  // RDP: the remote's scale is multiplied by it, as this display's
    // full state per change; a new index = plugged in
    virtual void sendGamepad(const GamepadState&) {}
    virtual Framebuffer& framebuffer() = 0;
};

class TerminalSession {
public:
    virtual ~TerminalSession() = default;
    virtual void connect() = 0;
    virtual void disconnect() = 0;
    virtual void write(std::span<const std::byte> data) = 0;
    virtual void resize(int cols, int rows) = 0;
    // a file in a private folder on the remote side, removed when the session ends; done(path, error) on the worker
    virtual bool canUpload() const { return false; }
    virtual void upload(std::string /*data*/, std::string /*extension*/, std::function<void(std::string, std::string)> done)
    {
        done({}, "Not supported.");
    }
};

} // namespace rdc
