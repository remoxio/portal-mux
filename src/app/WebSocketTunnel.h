#pragma once

#include <QNetworkRequest>
#include <QStringList>
#include <QTcpServer>

#include <functional>

class QSslCertificate;

// One local TCP connection carried over a websocket (LibVNCClient → Proxmox's vncwebsocket).
class WebSocketTunnel : public QTcpServer {
    Q_OBJECT

public:
    WebSocketTunnel(QNetworkRequest request, QStringList subprotocols, std::function<bool(const QSslCertificate&)> trust,
                    QObject* parent = nullptr);

signals:
    void failed(const QString& message);

private:
    void bridge(QTcpSocket* tcp);

    QNetworkRequest request_;
    QStringList subprotocols_;
    std::function<bool(const QSslCertificate&)> trust_;
};
