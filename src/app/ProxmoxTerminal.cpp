#include "app/ProxmoxTerminal.h"

#include "app/Proxmox.h"

#include <QSslConfiguration>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>

using rdc::SessionState;

ProxmoxTerminal::ProxmoxTerminal(rdc::TerminalListener& listener, QNetworkRequest request, QByteArray login,
                                 std::function<bool(const QSslCertificate&)> trust)
    : listener_(listener), request_(std::move(request)), login_(std::move(login)), trust_(std::move(trust))
{
    ping_.setInterval(30000); // as xterm.js: keeps pveproxy from closing an idle console
    QObject::connect(&ping_, &QTimer::timeout, [this] {
        if (socket_ && ready_) socket_->sendBinaryMessage("2");
    });
}

ProxmoxTerminal::~ProxmoxTerminal() { disconnect(); }

void ProxmoxTerminal::connect()
{
    disconnect();
    ready_ = false;
    closing_ = false;
    listener_.onStateChanged(SessionState::Connecting);
    socket_ = new QWebSocket;
    QObject::connect(socket_, &QWebSocket::sslErrors, socket_, [this](const QList<QSslError>&) {
        if (trust_(socket_->sslConfiguration().peerCertificate())) socket_->ignoreSslErrors();
    });
    QObject::connect(socket_, &QWebSocket::connected, socket_, [this] { socket_->sendBinaryMessage(login_); });
    QObject::connect(socket_, &QWebSocket::binaryMessageReceived, socket_, [this](const QByteArray& data) { received(data); });
    QObject::connect(socket_, &QWebSocket::textMessageReceived, socket_, [this](const QString& text) { received(text.toUtf8()); });
    QObject::connect(socket_, &QWebSocket::disconnected, socket_, [this] {
        const QString reason = socket_->closeReason().isEmpty() ? socket_->errorString() : socket_->closeReason();
        if (!ready_) end(SessionState::Error, "No console: " + reason.toStdString());
        else if (closing_ && socket_->closeCode() == QWebSocketProtocol::CloseCodeNormal) end(SessionState::Disconnected); // closes the tab
        else end(SessionState::Error, "The console was cut off: " + reason.toStdString());
    });
    QObject::connect(socket_, &QWebSocket::errorOccurred, socket_, [this] { // a failed handshake sends no disconnected()
        if (!ready_) end(SessionState::Error, "No console: " + socket_->errorString().toStdString());
    });
    QObject::connect(socket_, &QWebSocket::aboutToClose, socket_, [this] { closing_ = true; }); // a close frame came: closeCode() is its
    QWebSocketHandshakeOptions options;
    options.setSubprotocols({QStringLiteral("binary")}); // required by pveproxy
    socket_->open(request_, options);
}

void ProxmoxTerminal::received(const QByteArray& data)
{
    if (ready_) return listener_.onData(data.toStdString());
    if (!data.startsWith("OK")) return end(SessionState::Error, "Proxmox refused the console ticket.");
    ready_ = true;
    listener_.onStateChanged(SessionState::Connected);
    socket_->sendBinaryMessage(proxmox::termResize(cols_, rows_));
    if (!pending_.isEmpty()) socket_->sendBinaryMessage(proxmox::termInput(pending_));
    pending_.clear();
    ping_.start();
    if (data.size() > 2) listener_.onData(data.mid(2).toStdString());
}

void ProxmoxTerminal::write(std::span<const std::byte> data)
{
    const QByteArray bytes(reinterpret_cast<const char*>(data.data()), qsizetype(data.size()));
    if (socket_ && ready_) socket_->sendBinaryMessage(proxmox::termInput(bytes));
    else pending_ += bytes;
}

void ProxmoxTerminal::resize(int cols, int rows)
{
    cols_ = cols;
    rows_ = rows;
    if (socket_ && ready_) socket_->sendBinaryMessage(proxmox::termResize(cols, rows));
}

void ProxmoxTerminal::end(SessionState state, const std::string& error)
{
    disconnect();
    if (!error.empty()) listener_.onError(error);
    listener_.onStateChanged(state);
}

void ProxmoxTerminal::disconnect()
{
    ping_.stop();
    if (!socket_) return;
    QObject::disconnect(socket_, nullptr, nullptr, nullptr); // no listener calls after this
    socket_->close();
    socket_->deleteLater();
    socket_ = nullptr;
}
