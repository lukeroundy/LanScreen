#pragma once

#include <QHostAddress>
#include <QList>
#include <QString>

namespace lanscreen {

struct NetInterface {
    QString name;
    QHostAddress address;
    QHostAddress broadcast;
    bool isLoopback = false;
    bool isPrivate = false;
};

QList<NetInterface> listIpv4Interfaces();
QHostAddress firstLanAddress();
bool isPrivateAddress(const QHostAddress &addr);
QString localHostname();
QString srtPassphraseFromPin(const QString &pin);

} // namespace lanscreen
