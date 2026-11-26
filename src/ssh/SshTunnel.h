#pragma once

#include "core/Session.h"
#include "ssh/SshConnection.h"

#include <functional>
#include <memory>
#include <thread>

namespace rdc {

class SshTunnel {
public:
    SshTunnel(SessionListener& listener, SshConnection::Params jump, std::string targetHost, int targetPort);
    ~SshTunnel() { stop(); }

    // stable port: RDP's certificate store keys on host:port
    int listen(int preferredPort);

    // callbacks run on the worker; empty message = cancelled
    void start(std::function<void()> onReady, std::function<void(std::string)> onFailed);
    void stop();

    static SshConnection::Params parse(const std::string& spec);

private:
    void run(std::stop_token stop, std::function<void()> onReady, std::function<void(std::string)> onFailed);

    SessionListener& listener_;
    SshConnection::Params jump_;
    std::string targetHost_;
    int targetPort_;
    int listenFd_ = -1, localPort_ = 0;
    std::unique_ptr<SshConnection> ssh_;
    std::jthread worker_;
};

class TunneledDisplaySession : public DisplaySession {
public:
    TunneledDisplaySession(DisplayListener& listener, std::unique_ptr<SshTunnel> tunnel,
                           std::unique_ptr<DisplaySession> inner)
        : listener_(listener), tunnel_(std::move(tunnel)), inner_(std::move(inner))
    {
    }
    ~TunneledDisplaySession() override { disconnect(); }

    void connect() override;
    void disconnect() override
    {
        tunnel_->stop(); // joins first: no concurrent inner_->connect()
        inner_->disconnect();
    }
    void sendKey(const KeyEvent& e) override { inner_->sendKey(e); }
    void sendPointer(const PointerEvent& e) override { inner_->sendPointer(e); }
    void sendClipboardText(const std::string& t) override { inner_->sendClipboardText(t); }
    void sendClipboardUris(const std::string& u) override { inner_->sendClipboardUris(u); }
    bool offerClipboardFiles() override { return inner_->offerClipboardFiles(); }
    void relayClipboardFiles() override { inner_->relayClipboardFiles(); }
    bool takesClipboardImages() const override { return inner_->takesClipboardImages(); }
    void sendClipboardImage(const std::string& b, const std::string& t) override { inner_->sendClipboardImage(b, t); }
    void syncLocks(uint8_t locks) override { inner_->syncLocks(locks); }
    bool canRedirectUsb() const override { return inner_->canRedirectUsb(); }
    std::vector<UsbDevice> usbDevices() override { return inner_->usbDevices(); }
    void redirectUsb(const std::string& id, bool on, std::function<void(std::string)> done) override { inner_->redirectUsb(id, on, std::move(done)); }
    bool canSendFiles() const override { return inner_->canSendFiles(); }
    void sendFiles(const std::vector<std::string>& p, std::function<void(std::string)> done) override { inner_->sendFiles(p, std::move(done)); }
    void resizeDesktop(int w, int h) override { inner_->resizeDesktop(w, h); }
    void setPixelRatio(double r) override { inner_->setPixelRatio(r); }
    void sendGamepad(const GamepadState& g) override { inner_->sendGamepad(g); }
    Framebuffer& framebuffer() override { return inner_->framebuffer(); }

private:
    DisplayListener& listener_;
    std::unique_ptr<SshTunnel> tunnel_;
    std::unique_ptr<DisplaySession> inner_;
};

} // namespace rdc
