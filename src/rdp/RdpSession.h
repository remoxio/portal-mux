#pragma once

#include "core/Connection.h"
#include "core/Session.h"
#include "core/TaskQueue.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

typedef struct rdp_context rdpContext;
typedef struct s_cliprdr_client_context CliprdrClientContext;
typedef struct s_disp_client_context DispClientContext;
typedef struct cliprdr_file_context CliprdrFileContext;
typedef struct s_wClipboard wClipboard;

namespace rdc {

// RDP via FreeRDP 3.
class RdpSession : public DisplaySession {
public:
    // FreeRDP certificate store
    RdpSession(DisplayListener& listener, Connection connection, std::string configDir);
    ~RdpSession() override;

    void connect() override;
    void disconnect() override;
    void sendKey(const KeyEvent& event) override;
    void syncLocks(uint8_t locks) override;
    void sendPointer(const PointerEvent& event) override;
    void sendClipboardText(const std::string& text) override;
    void sendClipboardUris(const std::string& uriList) override;
    void resizeDesktop(int width, int height) override;
    void setPixelRatio(double ratio) override;
    Framebuffer& framebuffer() override { return framebuffer_; }

    struct Callbacks;

private:
    void run(std::stop_token stop);
    bool reconnect(std::stop_token stop);
    bool applySettings();
    void post(std::function<void()> task);
    void handlePointer(const PointerEvent& e);
    void announceClipboard();
    void sendLayout();
    void checkFrozen();
    std::string label() const { return connection_.name.empty() ? connection_.host : connection_.name; }

    DisplayListener& listener_;
    Connection connection_;
    std::string configDir_;
    Framebuffer framebuffer_;
    TaskQueue tasks_;
    rdpContext* context_ = nullptr;
    void* wakeEvent_ = nullptr;
    std::jthread worker_;

    bool connected_ = false;
    bool cancelled_ = false;
    bool alwaysTrust_ = false;
    PointerEvent lastPointer_;
    std::atomic<std::chrono::steady_clock::rep> lastUpdate_{0}; // set by the paint thread
    std::chrono::steady_clock::time_point lastInput_;            // presses and wheel, the worker's
    std::atomic<bool> frozen_ = false;                           // logged: no update since input
    int wantWidth_ = 1024, wantHeight_ = 768;
    double pixelRatio_ = 1;
    int sentScale_ = 0;
    DispClientContext* disp_ = nullptr;
    bool dispReady_ = false;

    std::mutex clipMutex_;
    CliprdrClientContext* cliprdr_ = nullptr;
    std::string localClipboard_;
    std::string localUris_;      // takes precedence over text
    CliprdrFileContext* fileContext_ = nullptr;
    wClipboard* clipboard_ = nullptr;
    uint32_t fileGroupId_ = 0, uriListId_ = 0;
    uint32_t serverFileGroupId_ = 0;            // 0 if not offered
    uint32_t pendingFormat_ = 0;
};

} // namespace rdc
