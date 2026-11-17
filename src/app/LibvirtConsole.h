#pragma once

#include "core/Session.h"

#include <string>

typedef struct _virConnect* virConnectPtr;
typedef struct _virStream* virStreamPtr;

// A VM's serial console (virDomainOpenConsole), driven by libvirt's events on the UI thread.
class LibvirtConsoleSession : public rdc::TerminalSession {
public:
    LibvirtConsoleSession(rdc::TerminalListener& listener, virConnectPtr connection, std::string uuid);
    ~LibvirtConsoleSession() override;

    void connect() override;
    void disconnect() override;
    void write(std::span<const std::byte> data) override;
    void resize(int, int) override {}

private:
    static void onEvent(virStreamPtr stream, int events, void* self);
    void flush();
    void end(rdc::SessionState state, const std::string& error = {});

    rdc::TerminalListener& listener_;
    virConnectPtr connection_;
    std::string uuid_;
    virStreamPtr stream_ = nullptr;
    std::string pending_; // typed while the guest wasn't reading
};
