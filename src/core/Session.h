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
    // Blocking; false if declined or the view is closing.
    // `always`: non-null offers "Always trust".
    virtual bool askTrust(const std::string& title, const std::string& details, bool* always = nullptr) = 0;
    virtual bool askCredentials(const std::string& prompt, Credentials& creds) = 0;
};

struct DisplayListener : SessionListener {
    virtual void onResize(int width, int height) = 0;
    virtual void onFrameUpdated(const Rect& dirty) = 0;
    virtual void onClipboardText(std::string text) = 0;
    virtual void onClipboardUris(std::string uriList) = 0; // text/uri-list of local paths
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
    virtual void pullClipboard() {} // hosts that can't announce a change: asked when the user leaves the view
    // copies local files to the remote side (SPICE: into the guest via its agent); `done` gets an error or nothing, on the UI thread
    virtual void relaunchAtSize(int, int) {} // Moonlight: quits the app on the host and starts it again at this size
    virtual bool canSendFiles() const { return false; }
    virtual void sendFiles(const std::vector<std::string>&, std::function<void(std::string)> done) { done("Not supported."); }
    virtual void syncLocks(uint8_t /*LockKey bits*/) {}
    // this computer's USB devices the remote side can take (SPICE); `done` gets an error or nothing, on the UI thread
    struct UsbDevice {
        std::string id, name;
        bool redirected = false;
    };
    virtual bool canRedirectUsb() const { return false; }
    virtual std::vector<UsbDevice> usbDevices() { return {}; }
    virtual void redirectUsb(const std::string&, bool, std::function<void(std::string)> done) { done("Not supported"); }
    virtual void resizeDesktop(int width, int height) = 0; // no-op if the protocol can't
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
};

} // namespace rdc
