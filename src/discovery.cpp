#include "discovery.h"
#include "netutil.h"

#include <QDateTime>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>
#include <algorithm>
#include <cstring>

namespace lanscreen {

// Packet is 96 bytes, easy to parse in any language.
//  0-3  magic "LSCR"
//  4    version
//  5    flags (bit0 = pin required)
//  6    transport
//  7    codec
//  8-9  control port BE
// 10-11 media port BE
// 12-43 session name UTF-8, NUL padded
// 44-75 hostname UTF-8, NUL padded
// 76-95 reserved

static const int kPacketSize = 96;

static QByteArray packBeacon(const SessionConfig &cfg)
{
    QByteArray b(kPacketSize, '\0');
    memcpy(b.data(), "LSCR", 4);
    b[4] = static_cast<char>(kProtocolVersion);
    b[5] = cfg.pin.isEmpty() ? 0 : 1;
    b[6] = static_cast<char>(static_cast<int>(cfg.transport));
    b[7] = static_cast<char>(static_cast<int>(cfg.codec));
    const quint16 cp = qToBigEndian(cfg.controlPort);
    const quint16 mp = qToBigEndian(cfg.mediaPort);
    memcpy(b.data() + 8, &cp, 2);
    memcpy(b.data() + 10, &mp, 2);
    const QByteArray name = cfg.sessionName.toUtf8();
    memcpy(b.data() + 12, name.constData(), qMin(31, name.size()));
    const QByteArray host = localHostname().toUtf8();
    memcpy(b.data() + 44, host.constData(), qMin(31, host.size()));
    return b;
}

static bool unpackBeacon(const QByteArray &b, const QHostAddress &from, HostBeacon *out)
{
    if (b.size() < kPacketSize)
        return false;
    if (memcmp(b.constData(), "LSCR", 4) != 0)
        return false;
    if (static_cast<quint8>(b[4]) != kProtocolVersion)
        return false;
    out->address = from;
    out->pinRequired = (static_cast<quint8>(b[5]) & 1) != 0;
    out->transport = static_cast<Transport>(static_cast<quint8>(b[6]));
    out->codec = static_cast<Codec>(static_cast<quint8>(b[7]));
    quint16 cp = 0, mp = 0;
    memcpy(&cp, b.constData() + 8, 2);
    memcpy(&mp, b.constData() + 10, 2);
    out->controlPort = qFromBigEndian(cp);
    out->mediaPort = qFromBigEndian(mp);
    out->sessionName = QString::fromUtf8(b.constData() + 12, strnlen(b.constData() + 12, 32));
    out->hostname = QString::fromUtf8(b.constData() + 44, strnlen(b.constData() + 44, 32));
    out->lastSeenMs = QDateTime::currentMSecsSinceEpoch();
    return out->controlPort != 0;
}

DiscoveryAnnouncer::DiscoveryAnnouncer(QObject *parent)
    : QObject(parent)
{
    sock_ = new QUdpSocket(this);
    timer_ = new QTimer(this);
    timer_->setInterval(2000);
    connect(timer_, &QTimer::timeout, this, &DiscoveryAnnouncer::send);
}

bool DiscoveryAnnouncer::start(const SessionConfig &cfg, const QHostAddress &bind)
{
    cfg_ = cfg;
    bind_ = bind;
    sock_->close();
    if (!sock_->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress))
        return false;
    timer_->start();
    send();
    return true;
}

void DiscoveryAnnouncer::stop()
{
    timer_->stop();
    sock_->close();
}

void DiscoveryAnnouncer::update(const SessionConfig &cfg)
{
    cfg_ = cfg;
}

void DiscoveryAnnouncer::send()
{
    const QByteArray pkt = packBeacon(cfg_);
    sock_->writeDatagram(pkt, QHostAddress::Broadcast, kDiscoveryPort);
    for (const NetInterface &n : listIpv4Interfaces()) {
        if (n.isLoopback)
            continue;
        if (!n.broadcast.isNull())
            sock_->writeDatagram(pkt, n.broadcast, kDiscoveryPort);
        // Also hit localhost so a client on the same machine can find us.
        sock_->writeDatagram(pkt, QHostAddress::LocalHost, kDiscoveryPort);
    }
}

DiscoveryBrowser::DiscoveryBrowser(QObject *parent)
    : QObject(parent)
{
    sock_ = new QUdpSocket(this);
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(sock_, &QUdpSocket::readyRead, this, &DiscoveryBrowser::readPending);
    connect(timer_, &QTimer::timeout, this, &DiscoveryBrowser::expire);
}

void DiscoveryBrowser::start()
{
    sock_->close();
    sock_->bind(QHostAddress::AnyIPv4, kDiscoveryPort,
                QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    timer_->start();
}

void DiscoveryBrowser::stop()
{
    timer_->stop();
    sock_->close();
    hosts_.clear();
    emit hostsChanged();
}

void DiscoveryBrowser::readPending()
{
    bool changed = false;
    while (sock_->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(int(sock_->pendingDatagramSize()));
        QHostAddress from;
        quint16 port = 0;
        sock_->readDatagram(data.data(), data.size(), &from, &port);
        HostBeacon b;
        if (!unpackBeacon(data, from, &b))
            continue;
        bool found = false;
        for (HostBeacon &h : hosts_) {
            if (h.address == b.address && h.controlPort == b.controlPort) {
                h = b;
                found = true;
                changed = true;
                break;
            }
        }
        if (!found) {
            hosts_.append(b);
            changed = true;
        }
    }
    if (changed)
        emit hostsChanged();
}

void DiscoveryBrowser::expire()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int before = hosts_.size();
    hosts_.erase(std::remove_if(hosts_.begin(), hosts_.end(),
                                [now](const HostBeacon &h) { return now - h.lastSeenMs > 6000; }),
                 hosts_.end());
    if (hosts_.size() != before)
        emit hostsChanged();
}

} // namespace lanscreen
