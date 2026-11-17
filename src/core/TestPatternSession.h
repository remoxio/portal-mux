#pragma once

#include "core/Session.h"

#include <mutex>
#include <thread>
#include <vector>

namespace rdc {

// Fake display backend: animated pattern on a worker thread, left-drag paints dots.
class TestPatternSession : public DisplaySession {
public:
    TestPatternSession(DisplayListener& listener, int width = 1280, int height = 800);
    ~TestPatternSession() override { disconnect(); }

    void connect() override;
    void disconnect() override;
    void sendKey(const KeyEvent&) override {}
    void sendPointer(const PointerEvent& event) override;
    void sendClipboardText(const std::string&) override {}
    void resizeDesktop(int, int) override {}
    Framebuffer& framebuffer() override { return framebuffer_; }

private:
    void run(std::stop_token stop);

    DisplayListener& listener_;
    const int width_, height_;
    Framebuffer framebuffer_;
    std::mutex inputMutex_;
    std::vector<PointerEvent> pointerQueue_;
    std::jthread worker_;
};

} // namespace rdc
