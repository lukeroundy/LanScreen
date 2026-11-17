#include "netutil.h"

#include <QHostInfo>
#include <QNetworkInterface>

namespace lanscreen {

bool isPrivateAddress(const QHostAddress &addr)
{
    const QHostAddress v4 = QHostAddress(addr.toIPv4Address());
    if (v4.isLoopback())
        return false;
    if (v4.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8))
        return true;
    if (v4.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12))
        return true;
    if (v4.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16))
        return true;
    if (v4.isInSubnet(QHostAddress(QStringLiteral("169.254.0.0")), 16))
        return true; // link-local, still "local"
    return false;
}

QList<NetInterface> listIpv4Interfaces()
{
    QList<NetInterface> out;
    const auto ifaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : ifaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp))
            continue;
        if (iface.flags() & QNetworkInterface::IsPointToPoint)
            continue;
        for (const QNetworkAddressEntry &e : iface.addressEntries()) {
            const QHostAddress ip = e.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            NetInterface n;
            n.name = iface.name();
            n.address = ip;
            n.broadcast = e.broadcast();
            n.isLoopback = bool(iface.flags() & QNetworkInterface::IsLoopBack) || ip.isLoopback();
            n.isPrivate = isPrivateAddress(ip);
            out.append(n);
        }
    }
    return out;
}

QHostAddress firstLanAddress()
{
    const auto list = listIpv4Interfaces();
    for (const NetInterface &n : list) {
        if (!n.isLoopback && n.isPrivate)
            return n.address;
    }
    for (const NetInterface &n : list) {
        if (!n.isLoopback)
            return n.address;
    }
    return QHostAddress::LocalHost;
}

QString localHostname()
{
    QString h = QHostInfo::localHostName();
    if (h.isEmpty())
        h = QStringLiteral("lanscreen-host");
    return h;
}

QString srtPassphraseFromPin(const QString &pin)
{
    if (pin.isEmpty())
        return {};
    // SRT passphrases must be at least 10 characters.
    QString p = QStringLiteral("lanscreen-") + pin;
    while (p.size() < 10)
        p.append(QLatin1Char('0'));
    return p;
}

} // namespace lanscreen
