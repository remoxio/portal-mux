#pragma once

#include "core/Connection.h"
#include "core/Session.h"
#include "core/TaskQueue.h"

#include <mutex>
#include <thread>
#include <vector>

typedef struct _rfbClient rfbClient;

namespace rdc {

class VncSession : public DisplaySession {
public:
    VncSession(DisplayListener& listener, Connection connection);
    ~VncSession() override { disconnect(); }

    void connect() override;
    void disconnect() override;
    void sendKey(const KeyEvent& event) override;
    void sendPointer(const PointerEvent& event) override;
    void sendClipboardText(const std::string& text) override;
    void resizeDesktop(int width, int height) override;
    Framebuffer& framebuffer() override { return framebuffer_; }

    struct Callbacks;

private:
    void run(std::stop_token stop);
    void applyResize();

    DisplayListener& listener_;
    Connection connection_;
    Framebuffer framebuffer_;
    TaskQueue tasks_;
    std::jthread worker_;

    std::vector<uint8_t> pixels_;
    bool connected_ = false, cancelled_ = false, passwordUsed_ = false;
    int wantWidth_ = 0, wantHeight_ = 0, sentWidth_ = 0, sentHeight_ = 0, wheelRemainder_ = 0;
    uint8_t buttons_ = 0;
    std::string lastClipboard_;
    std::string encodings_; // outlives the client: LibVNCClient keeps the pointer

    std::mutex clientMutex_; // guards client_ against disconnect() from the UI thread
    rfbClient* client_ = nullptr;
};

} // namespace rdc
