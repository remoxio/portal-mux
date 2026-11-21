#include "app/AzureSerial.h"

#include <QWebSocket>

using rdc::SessionState;

AzureSerial::AzureSerial(rdc::TerminalListener& listener, QUrl url, QString token)
    : listener_(listener), url_(std::move(url)), token_(std::move(token))
{
}

AzureSerial::~AzureSerial() { disconnect(); }

void AzureSerial::connect()
{
    disconnect();
    ready_ = false;
    listener_.onStateChanged(SessionState::Connecting);
    socket_ = new QWebSocket;
    QObject::connect(socket_, &QWebSocket::binaryMessageReceived, socket_, [this](const QByteArray& data) { received(data); });
    QObject::connect(socket_, &QWebSocket::textMessageReceived, socket_, [this](const QString& text) { received(text.toUtf8()); });
    QObject::connect(socket_, &QWebSocket::disconnected, socket_, [this] {
        const QString reason = socket_->closeReason().isEmpty() ? socket_->errorString() : socket_->closeReason();
        end(SessionState::Error, (ready_ ? "The serial console was cut off" : "No serial console (is the VM running?)") +
                                     (reason.isEmpty() ? std::string() : ": " + reason.toStdString()));
    });
    QObject::connect(socket_, &QWebSocket::errorOccurred, socket_, [this] { // a failed handshake sends no disconnected()
        if (!ready_) end(SessionState::Error, "No serial console: " + socket_->errorString().toStdString());
    });
    socket_->open(url_);
}

void AzureSerial::received(const QByteArray& data)
{
    if (ready_) return listener_.onData(data.toStdString());
    socket_->sendTextMessage(token_); // what the first message asks for; it is shown by nobody
    token_.clear();
    ready_ = true;
    listener_.onStateChanged(SessionState::Connected);
    if (!pending_.isEmpty()) socket_->sendTextMessage(QString::fromUtf8(pending_));
    pending_.clear();
}

void AzureSerial::write(std::span<const std::byte> data)
{
    const QByteArray bytes(reinterpret_cast<const char*>(data.data()), qsizetype(data.size()));
    if (socket_ && ready_) socket_->sendTextMessage(QString::fromUtf8(bytes));
    else pending_ += bytes;
}

void AzureSerial::end(SessionState state, const std::string& error)
{
    disconnect();
    if (!error.empty()) listener_.onError(error);
    listener_.onStateChanged(state);
}

void AzureSerial::disconnect()
{
    if (!socket_) return;
    QObject::disconnect(socket_, nullptr, nullptr, nullptr); // no listener calls after this
    socket_->close();
    socket_->deleteLater();
    socket_ = nullptr;
}
