#pragma once

#include <QObject>
#include <QString>

#include <deque>
#include <vector>

// Whether hosts answer on their port: a TCP connect, a few at once, three seconds each
class Reachability : public QObject {
    Q_OBJECT

public:
    struct Target {
        QString id, host;
        quint16 port;
    };
    using QObject::QObject;
    void check(const std::vector<Target>& targets); // after those already waiting

signals:
    void answered(const QString& id, bool online);

private:
    void next();

    std::deque<Target> queue_;
    int running_ = 0;
};
