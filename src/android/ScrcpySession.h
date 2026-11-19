#pragma once

#include "android/Adb.h"
#include "core/Connection.h"
#include "core/Session.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rdc {

// Android via scrcpy's server: video on the worker, audio, control and output on their own threads
class ScrcpySession : public DisplaySession {
public:
    ScrcpySession(DisplayListener& listener, Connection connection, std::string serverPath);
    ~ScrcpySession() override { disconnect(); }

    void connect() override;
    void disconnect() override;
    void sendKey(const KeyEvent& event) override;
    void sendPointer(const PointerEvent& event) override;
    void sendClipboardText(const std::string& text) override;
    void resizeDesktop(int, int) override {}
    Framebuffer& framebuffer() override { return framebuffer_; }

private:
    void run(std::stop_token stop);
    bool start(std::stop_token stop, std::string* error);
    void readVideo();
    void readAudio();
    void readControl();
    void readShell();
    void send(const std::vector<uint8_t>& message);
    void sendHidKey(const KeyEvent& event);
    void sendHidReport();
    std::string serverOutput();

    DisplayListener& listener_;
    Connection connection_;
    std::string serverPath_;
    Framebuffer framebuffer_;
    std::jthread worker_;
    std::thread audioThread_, controlThread_, shellThread_;

    std::mutex socketsMutex_; // shut down from the UI thread
    adb::Socket shell_, video_, audio_, control_;
    std::mutex sendMutex_;
    std::atomic<bool> controlReady_{false};
    std::atomic<int> width_{0}, height_{0}; // shown video size: input coordinates refer to it
    std::atomic<bool> serverExited_{false};
    uint32_t videoCodec_ = 0;

    std::mutex outputMutex_;
    std::string output_;

    const bool hid_;
    std::atomic<uint8_t> deviceLeds_{0};

    uint8_t buttons_ = 0;
    uint32_t metaState_ = 0;
    bool hidKeys_[0x66] = {};
    uint8_t hidMods_ = 0;
    uint64_t clipboardSequence_ = 0;
};

} // namespace rdc
