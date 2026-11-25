#include "app/Reachability.h"

#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <utility>

static constexpr int parallel = 8;

void Reachability::check(const std::vector<Target>& targets)
{
    for (const Target& t : targets)
        if (std::none_of(queue_.begin(), queue_.end(), [&](const Target& q) { return q.id == t.id; })) queue_.push_back(t);
    while (running_ < parallel && !queue_.empty()) next();
}

void Reachability::next()
{
    if (queue_.empty()) return;
    const Target t = queue_.front();
    queue_.pop_front();
    ++running_;
    auto* socket = new QTcpSocket(this);
    auto* timeout = new QTimer(socket);
    auto finished = std::make_shared<bool>(false);
    auto done = [this, socket, finished, id = t.id](bool online) {
        if (std::exchange(*finished, true)) return;
        socket->abort();
        socket->deleteLater();
        --running_;
        emit answered(id, online);
        next();
    };
    connect(socket, &QTcpSocket::connected, this, [done] { done(true); });
    connect(socket, &QTcpSocket::errorOccurred, this, [done] { done(false); });
    connect(timeout, &QTimer::timeout, this, [done] { done(false); });
    timeout->setSingleShot(true);
    timeout->start(3000);
    socket->connectToHost(t.host, t.port);
}
