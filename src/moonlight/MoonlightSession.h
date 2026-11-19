#pragma once

#include "core/Connection.h"
#include "core/Session.h"
#include "media/AudioDecoder.h"
#include "media/VideoDecoder.h"
#include "moonlight/GameStream.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace rdc {

// Moonlight game streaming; one at a time (moonlight-common-c's global state)
class MoonlightSession : public DisplaySession {
public:
    MoonlightSession(DisplayListener& listener, Connection connection, std::string identityDir);
    ~MoonlightSession() override;

    void connect() override;
    void disconnect() override;
    void sendKey(const KeyEvent& event) override;
    void sendPointer(const PointerEvent& event) override;
    void sendClipboardText(const std::string& text) override; // Apollo hosts only
    void pullClipboard() override;
    void resizeDesktop(int width, int height) override; // before connecting: window size, for "window" resolution
    void relaunchAtSize(int width, int height) override;
    void sendGamepad(const GamepadState& state) override;
    Framebuffer& framebuffer() override { return framebuffer_; }

    struct Callbacks;

private:
    void run(std::stop_token stop);
    bool prepare(std::string* error);
    bool launch(std::string* error, bool relaunch);
    bool startStream(std::stop_token stop, std::string* error);
    void syncClipboard();

    DisplayListener& listener_;
    Connection connection_;
    std::string identityDir_;
    Framebuffer framebuffer_;
    std::jthread worker_;
    gamestream::Cancel cancel_;

    gamestream::Identity identity_;
    gamestream::Host host_;
    gamestream::ServerInfo info_;
    gamestream::App app_;
    std::string rtspUrl_;
    struct Config;
    std::unique_ptr<Config> config_;

    std::mutex mutex_;
    std::condition_variable ended_;
    bool terminated_ = false;
    int terminationError_ = 0;
    std::string stageError_;
    std::optional<std::string> clipboardOut_; // queued for the worker, like clipboardPull_
    bool clipboardPull_ = false;
    bool relaunch_ = false;
    std::string hostClipboard_; // as last sent or read; worker only
    std::atomic<bool> starting_{false}, streaming_{false};
    std::atomic<int> wantWidth_{0}, wantHeight_{0}, width_{0}, height_{0};

    VideoDecoder video_;
    AudioDecoder audio_;
    std::vector<uint8_t> frame_;
    std::vector<int16_t> pcm_;

    uint8_t buttons_ = 0, modifiers_ = 0;
    int lastX_ = -1, lastY_ = -1;
    uint16_t gamepadMask_ = 0;
};

} // namespace rdc
