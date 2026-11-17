#include "app/WebSocketTunnel.h"

#include <QSslConfiguration>
#include <QTcpSocket>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>

#include <memory>

WebSocketTunnel::WebSocketTunnel(QNetworkRequest request, QStringList subprotocols, std::function<bool(const QSslCertificate&)> trust,
                                 QObject* parent)
    : QTcpServer(parent), request_(std::move(request)), subprotocols_(std::move(subprotocols)), trust_(std::move(trust))
{
    listen(QHostAddress::LocalHost, 0);
    connect(this, &QTcpServer::newConnection, this, [this] {
        QTcpSocket* tcp = nextPendingConnection();
        close(); // one client
        if (tcp) bridge(tcp);
    });
}

void WebSocketTunnel::bridge(QTcpSocket* tcp)
{
    auto* ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, tcp);
    auto waiting = std::make_shared<QByteArray>(); // until connected
    connect(ws, &QWebSocket::sslErrors, ws, [this, ws](const QList<QSslError>&) {
        if (trust_(ws->sslConfiguration().peerCertificate())) ws->ignoreSslErrors();
    });
    connect(ws, &QWebSocket::connected, ws, [ws, waiting] {
        if (!waiting->isEmpty()) ws->sendBinaryMessage(*waiting);
        waiting->clear();
    });
    connect(ws, &QWebSocket::binaryMessageReceived, tcp, [tcp](const QByteArray& data) { tcp->write(data); });
    connect(tcp, &QTcpSocket::readyRead, ws, [tcp, ws, waiting] {
        const QByteArray data = tcp->readAll();
        if (ws->state() == QAbstractSocket::ConnectedState) ws->sendBinaryMessage(data);
        else *waiting += data;
    });
    connect(ws, &QWebSocket::errorOccurred, this, [this, ws] { emit failed(ws->errorString()); });
    connect(ws, &QWebSocket::disconnected, tcp, [tcp] { tcp->disconnectFromHost(); });
    connect(tcp, &QTcpSocket::disconnected, ws, [ws] { ws->close(); });
    connect(tcp, &QTcpSocket::disconnected, tcp, &QObject::deleteLater);
    QWebSocketHandshakeOptions options;
    options.setSubprotocols(subprotocols_);
    ws->open(request_, options);
}
