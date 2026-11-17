#pragma once

#include "core/Connection.h"
#include "core/Session.h"

#include <vector>

typedef struct _SpiceSession SpiceSession;
typedef struct _SpiceMainChannel SpiceMainChannel;
typedef struct _SpiceInputsChannel SpiceInputsChannel;
typedef struct _SpiceUsbDeviceManager SpiceUsbDeviceManager;
typedef struct _GObject GObject;

namespace rdc {

// Runs on the GLib main-context thread: spice-glib emits signals there, a worker would race.
// Clipboard and resize need spice-vdagent in the guest.
class SpiceDisplaySession : public DisplaySession {
public:
    SpiceDisplaySession(DisplayListener& listener, Connection connection);
    ~SpiceDisplaySession() override { disconnect(); }

    void connect() override;
    void disconnect() override;
    void sendKey(const KeyEvent& event) override;
    void syncLocks(uint8_t locks) override;
    void sendPointer(const PointerEvent& event) override;
    void sendClipboardText(const std::string& text) override;
    void resizeDesktop(int width, int height) override;
    bool canRedirectUsb() const override { return usb_ && usbChannels_ > 0; }
    std::vector<UsbDevice> usbDevices() override;
    void redirectUsb(const std::string& id, bool on, std::function<void(std::string)> done) override;
    bool canSendFiles() const override { return agentConnected(); }
    void sendFiles(const std::vector<std::string>& paths, std::function<void(std::string)> done) override;
    Framebuffer& framebuffer() override { return framebuffer_; }

    struct Callbacks;

private:
    bool agentConnected() const;

    DisplayListener& listener_;
    Connection connection_;
    Framebuffer framebuffer_;
    SpiceSession* session_ = nullptr;
    SpiceMainChannel* main_ = nullptr;
    SpiceInputsChannel* inputs_ = nullptr;
    SpiceUsbDeviceManager* usb_ = nullptr; // the session's
    int usbChannels_ = 0;                   // the VM's USB redirectors
    std::vector<GObject*> connected_;
    const uint8_t* primary_ = nullptr;
    int stride_ = 0, wantWidth_ = 0, wantHeight_ = 0, authTries_ = 0, wheelRemainder_ = 0;
    unsigned reconnectSource_ = 0;
    PointerEvent lastPointer_;
    bool failed_ = false;
    std::string localClipboard_;
};

} // namespace rdc
