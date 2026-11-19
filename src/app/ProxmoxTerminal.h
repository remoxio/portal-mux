#pragma once

#include "core/Session.h"

#include <QByteArray>
#include <QNetworkRequest>
#include <QTimer>

#include <functional>

class QSslCertificate;
class QWebSocket;

// A container's console: termproxy behind the vncwebsocket, as xterm.js. UI thread.
class ProxmoxTerminal : public rdc::TerminalSession {
public:
    ProxmoxTerminal(rdc::TerminalListener& listener, QNetworkRequest request, QByteArray login,
                    std::function<bool(const QSslCertificate&)> trust);
    ~ProxmoxTerminal() override;

    void connect() override;
    void disconnect() override;
    void write(std::span<const std::byte> data) override;
    void resize(int cols, int rows) override;

private:
    void received(const QByteArray& data);
    void end(rdc::SessionState state, const std::string& error = {});

    rdc::TerminalListener& listener_;
    QNetworkRequest request_;
    QByteArray login_; // "user:ticket\n"
    std::function<bool(const QSslCertificate&)> trust_;
    QWebSocket* socket_ = nullptr;
    bool ready_ = false; // termproxy said OK
    QByteArray pending_; // typed before that
    bool closing_ = false; // Proxmox closed it, not a broken connection
    int cols_ = 80, rows_ = 24;
    QTimer ping_;
};
