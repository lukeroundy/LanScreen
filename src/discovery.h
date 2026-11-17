#pragma once

#include "common.h"

#include <QHostAddress>
#include <QObject>
#include <QVector>

class QUdpSocket;
class QTimer;

namespace lanscreen {

struct HostBeacon {
    QString sessionName;
    QString hostname;
    QHostAddress address;
    quint16 controlPort = kDefaultControlPort;
    quint16 mediaPort = kDefaultMediaPort;
    Transport transport = Transport::Srt;
    Codec codec = Codec::H264;
    bool pinRequired = false;
    qint64 lastSeenMs = 0;
};

class DiscoveryAnnouncer : public QObject {
    Q_OBJECT
public:
    explicit DiscoveryAnnouncer(QObject *parent = nullptr);
    bool start(const SessionConfig &cfg, const QHostAddress &bind);
    void stop();
    void update(const SessionConfig &cfg);

private:
    void send();
    QUdpSocket *sock_ = nullptr;
    QTimer *timer_ = nullptr;
    SessionConfig cfg_;
    QHostAddress bind_;
};

class DiscoveryBrowser : public QObject {
    Q_OBJECT
public:
    explicit DiscoveryBrowser(QObject *parent = nullptr);
    void start();
    void stop();
    QVector<HostBeacon> hosts() const { return hosts_; }

signals:
    void hostsChanged();

private:
    void readPending();
    void expire();
    QUdpSocket *sock_ = nullptr;
    QTimer *timer_ = nullptr;
    QVector<HostBeacon> hosts_;
};

} // namespace lanscreen
