#pragma once

#include "core/Session.h"

#include <QByteArray>
#include <QUrl>

class QWebSocket;

// An Azure VM's serial console, as `az serial-console`: the token answers the first message, then text both ways. UI thread.
class AzureSerial : public rdc::TerminalSession {
public:
    AzureSerial(rdc::TerminalListener& listener, QUrl url, QString token);
    ~AzureSerial() override;

    void connect() override;
    void disconnect() override;
    void write(std::span<const std::byte> data) override;
    void resize(int, int) override {} // a serial line has no size

private:
    void received(const QByteArray& data);
    void end(rdc::SessionState state, const std::string& error = {});

    rdc::TerminalListener& listener_;
    QUrl url_;
    QString token_;
    QWebSocket* socket_ = nullptr;
    bool ready_ = false; // the token went
    QByteArray pending_; // typed before that
};
