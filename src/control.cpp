#include "control.h"

#include <cstring>
#include "netutil.h"

#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtEndian>

namespace lanscreen {

static QByteArray frame(const QJsonObject &obj)
{
    const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QByteArray out;
    const quint32 n = qToBigEndian(quint32(payload.size()));
    out.append(reinterpret_cast<const char *>(&n), 4);
    out.append(payload);
    return out;
}

JsonLineConnection::JsonLineConnection(QTcpSocket *sock, QObject *parent)
    : QObject(parent)
    , sock_(sock)
{
    sock_->setParent(this);
    connect(sock_, &QTcpSocket::readyRead, this, &JsonLineConnection::onReady);
    connect(sock_, &QTcpSocket::disconnected, this, &JsonLineConnection::disconnected);
}

QHostAddress JsonLineConnection::peerAddress() const
{
    return sock_ ? sock_->peerAddress() : QHostAddress();
}

void JsonLineConnection::send(const QJsonObject &obj)
{
    if (!sock_)
        return;
    sock_->write(frame(obj));
}

void JsonLineConnection::onReady()
{
    buf_.append(sock_->readAll());
    while (buf_.size() >= 4) {
        quint32 n = 0;
        memcpy(&n, buf_.constData(), 4);
        n = qFromBigEndian(n);
        if (n > 1 * 1024 * 1024) {
            sock_->disconnectFromHost();
            return;
        }
        if (buf_.size() < int(4 + n))
            return;
        const QByteArray payload = buf_.mid(4, int(n));
        buf_.remove(0, int(4 + n));
        const QJsonDocument doc = QJsonDocument::fromJson(payload);
        if (doc.isObject())
            emit message(doc.object());
    }
}

ControlServer::ControlServer(QObject *parent)
    : QObject(parent)
{
    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection, this, &ControlServer::onIncoming);
}

bool ControlServer::start(const SessionConfig &cfg, const QHostAddress &bind, QString *error)
{
    stop();
    cfg_ = cfg;
    failures_ = 0;
    const QHostAddress addr = bind.isNull() ? QHostAddress::AnyIPv4 : bind;
    if (!server_->listen(addr, cfg.controlPort)) {
        if (error)
            *error = server_->errorString();
        return false;
    }
    port_ = server_->serverPort();
    return true;
}

void ControlServer::stop()
{
    dropClient();
    server_->close();
    port_ = 0;
}

void ControlServer::send(const QJsonObject &obj)
{
    if (conn_)
        conn_->send(obj);
}

QHostAddress ControlServer::clientAddress() const
{
    return conn_ ? conn_->peerAddress() : QHostAddress();
}

void ControlServer::onIncoming()
{
    QTcpSocket *sock = server_->nextPendingConnection();
    if (!sock)
        return;
    if (conn_) {
        // One client in v1. Politely reject extras.
        JsonLineConnection tmp(sock, nullptr);
        tmp.send(QJsonObject{{QStringLiteral("type"), QStringLiteral("busy")},
                             {QStringLiteral("reason"), QStringLiteral("host already has a client")}});
        sock->disconnectFromHost();
        tmp.setParent(sock);
        sock->deleteLater();
        return;
    }
    conn_ = new JsonLineConnection(sock, this);
    authed_ = false;
    connect(conn_, &JsonLineConnection::message, this, &ControlServer::onMessage);
    connect(conn_, &JsonLineConnection::disconnected, this, &ControlServer::dropClient);
    emit clientConnected(sock->peerAddress());
    emit logMessage(QStringLiteral("Client connected from ") + sock->peerAddress().toString());
    conn_->send(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("hello")},
        {QStringLiteral("app"), QStringLiteral("lanscreen")},
        {QStringLiteral("pinRequired"), !cfg_.pin.isEmpty()},
        {QStringLiteral("session"), cfg_.sessionName},
    });
}

void ControlServer::onMessage(const QJsonObject &obj)
{
    const QString type = obj.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("auth") || type == QLatin1String("hello")) {
        const QString pin = obj.value(QStringLiteral("pin")).toString();
        if (!cfg_.pin.isEmpty() && pin != cfg_.pin) {
            failures_++;
            const QString why = QStringLiteral("bad PIN");
            if (conn_)
                conn_->send(QJsonObject{{QStringLiteral("type"), QStringLiteral("auth_fail")},
                                        {QStringLiteral("reason"), why}});
            emit authFailed(why);
            emit logMessage(QStringLiteral("Auth failed (bad PIN)"));
            if (failures_ >= 5)
                dropClient();
            return;
        }
        authed_ = true;
        failures_ = 0;
        QJsonObject stream{
            {QStringLiteral("protocol"), transportName(cfg_.transport)},
            {QStringLiteral("port"), cfg_.mediaPort},
            {QStringLiteral("codec"), codecName(cfg_.codec)},
            {QStringLiteral("width"), cfg_.width},
            {QStringLiteral("height"), cfg_.height},
            {QStringLiteral("fps"), cfg_.fps},
            {QStringLiteral("bitrate"), cfg_.bitrateKbps},
            {QStringLiteral("fec"), cfg_.fec},
            {QStringLiteral("srtLatencyMs"), cfg_.srtLatencyMs},
        };
        if (conn_) {
            conn_->send(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("welcome")},
                {QStringLiteral("ok"), true},
                {QStringLiteral("stream"), stream},
                {QStringLiteral("display"),
                 QJsonObject{
                     {QStringLiteral("x"), cfg_.cropX},
                     {QStringLiteral("y"), cfg_.cropY},
                     {QStringLiteral("w"), cfg_.cropW > 0 ? cfg_.cropW : cfg_.width},
                     {QStringLiteral("h"), cfg_.cropH > 0 ? cfg_.cropH : cfg_.height},
                 }},
            });
        }
        emit authenticated();
        emit logMessage(QStringLiteral("Client authenticated"));
        return;
    }
    if (!authed_)
        return;
    if (type == QLatin1String("input")) {
        emit inputEvent(obj);
        return;
    }
    if (type == QLatin1String("ping")) {
        if (conn_)
            conn_->send(QJsonObject{{QStringLiteral("type"), QStringLiteral("pong")}});
    }
}

void ControlServer::dropClient()
{
    if (conn_) {
        conn_->deleteLater();
        conn_ = nullptr;
        authed_ = false;
        emit clientDisconnected();
        emit logMessage(QStringLiteral("Client disconnected"));
    }
}

ControlClient::ControlClient(QObject *parent)
    : QObject(parent)
{
    sock_ = new QTcpSocket(this);
}

void ControlClient::connectTo(const QHostAddress &host, quint16 port, const QString &pin,
                              const QString &name)
{
    disconnectFromHost();
    pin_ = pin;
    name_ = name;
    // Always use a fresh socket so a previous failed attempt cannot stack
    // connected/error handlers or leave a half-open SYN-SENT behind.
    if (sock_) {
        sock_->disconnect(this);
        sock_->abort();
        sock_->deleteLater();
    }
    sock_ = new QTcpSocket(this);
    connect(sock_, &QTcpSocket::connected, this, [this]() {
        conn_ = new JsonLineConnection(sock_, this);
        // sock_ parent was this; JsonLineConnection will reparent it. Keep sock_ alive with the conn.
        sock_->setParent(conn_);
        connect(conn_, &JsonLineConnection::message, this, &ControlClient::onMessage);
        connect(conn_, &JsonLineConnection::disconnected, this, [this]() {
            connected_ = false;
            emit disconnected();
        });
        conn_->send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("auth")},
            {QStringLiteral("pin"), pin_},
            {QStringLiteral("name"), name_},
            {QStringLiteral("role"), QStringLiteral("client")},
        });
    });
    connect(sock_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        const QString why = sock_->errorString();
        emit logMessage(QStringLiteral("Control connection error: ") + why);
        connected_ = false;
        emit disconnected();
    });
    sock_->connectToHost(host, port);
}

void ControlClient::disconnectFromHost()
{
    if (conn_) {
        // sock_ is owned by conn_ after connect
        conn_->deleteLater();
        conn_ = nullptr;
        sock_ = new QTcpSocket(this);
    } else if (sock_) {
        sock_->abort();
    }
    connected_ = false;
}

void ControlClient::send(const QJsonObject &obj)
{
    if (conn_)
        conn_->send(obj);
}

void ControlServer::setStreamSize(int width, int height)
{
    if (width > 0)
        cfg_.width = width;
    if (height > 0)
        cfg_.height = height;
    if (width > 0) {
        cfg_.cropW = width;
        cfg_.cropH = height;
    }
    if (authed_ && conn_ && width > 0 && height > 0) {
        conn_->send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("stream_size")},
            {QStringLiteral("width"), width},
            {QStringLiteral("height"), height},
        });
    }
}

void ControlClient::onMessage(const QJsonObject &obj)
{
    const QString type = obj.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("welcome")) {
        connected_ = true;
        emit connected();
        emit welcome(obj);
        return;
    }
    if (type == QLatin1String("stream_size")) {
        emit streamSize(obj.value(QStringLiteral("width")).toInt(),
                        obj.value(QStringLiteral("height")).toInt());
        return;
    }
    if (type == QLatin1String("auth_fail") || type == QLatin1String("busy")) {
        emit rejected(obj.value(QStringLiteral("reason")).toString());
        disconnectFromHost();
        return;
    }
    if (type == QLatin1String("hello")) {
        // Server greeted us first; auth is sent on TCP connect already.
        return;
    }
}

} // namespace lanscreen
