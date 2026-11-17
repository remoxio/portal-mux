#include "app/LibvirtConsole.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

using rdc::SessionState;

static constexpr int readEvents = VIR_STREAM_EVENT_READABLE | VIR_STREAM_EVENT_ERROR | VIR_STREAM_EVENT_HANGUP;

LibvirtConsoleSession::LibvirtConsoleSession(rdc::TerminalListener& listener, virConnectPtr connection, std::string uuid)
    : listener_(listener), connection_(connection), uuid_(std::move(uuid))
{
    virConnectRef(connection_);
}

LibvirtConsoleSession::~LibvirtConsoleSession()
{
    disconnect();
    virConnectClose(connection_);
}

void LibvirtConsoleSession::connect()
{
    listener_.onStateChanged(SessionState::Connecting);
    virDomainPtr domain = virDomainLookupByUUIDString(connection_, uuid_.c_str());
    if (!domain) return end(SessionState::Error, "The virtual machine is gone.");
    stream_ = virStreamNew(connection_, VIR_STREAM_NONBLOCK);
    const int rc = stream_ ? virDomainOpenConsole(domain, nullptr, stream_, 0) : -1;
    virDomainFree(domain);
    if (rc < 0) return end(SessionState::Error, std::string("No serial console: ") + virGetLastErrorMessage());
    virStreamEventAddCallback(stream_, readEvents, onEvent, this, nullptr);
    listener_.onStateChanged(SessionState::Connected);
}

void LibvirtConsoleSession::onEvent(virStreamPtr, int events, void* self)
{
    auto* s = static_cast<LibvirtConsoleSession*>(self);
    if (events & VIR_STREAM_EVENT_WRITABLE) s->flush();
    if (events & VIR_STREAM_EVENT_READABLE) {
        char buffer[16384];
        for (;;) {
            const int n = virStreamRecv(s->stream_, buffer, sizeof buffer);
            if (n == -2) break; // drained
            if (n <= 0) return s->end(n == 0 ? SessionState::Disconnected : SessionState::Error,
                                      n == 0 ? std::string() : virGetLastErrorMessage());
            s->listener_.onData(std::string(buffer, size_t(n)));
        }
    }
    if (s->stream_ && (events & (VIR_STREAM_EVENT_ERROR | VIR_STREAM_EVENT_HANGUP))) s->end(SessionState::Disconnected); // VM stopped
}

void LibvirtConsoleSession::write(std::span<const std::byte> data)
{
    pending_.append(reinterpret_cast<const char*>(data.data()), data.size());
    flush();
}

void LibvirtConsoleSession::flush()
{
    while (stream_ && !pending_.empty()) {
        const int n = virStreamSend(stream_, pending_.data(), pending_.size());
        if (n == -2) break; // the guest isn't reading: wait for writable
        if (n < 0) return end(SessionState::Error, virGetLastErrorMessage());
        pending_.erase(0, size_t(n));
    }
    if (stream_) virStreamEventUpdateCallback(stream_, readEvents | (pending_.empty() ? 0 : VIR_STREAM_EVENT_WRITABLE));
}

void LibvirtConsoleSession::end(SessionState state, const std::string& error)
{
    disconnect();
    if (!error.empty()) listener_.onError(error);
    listener_.onStateChanged(state);
}

void LibvirtConsoleSession::disconnect()
{
    if (!stream_) return;
    virStreamEventRemoveCallback(stream_);
    virStreamAbort(stream_);
    virStreamFree(stream_);
    stream_ = nullptr;
}
