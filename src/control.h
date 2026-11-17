#pragma once

#include "common.h"

#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

class QTcpServer;
class QTcpSocket;

namespace lanscreen {

class JsonLineConnection : public QObject {
    Q_OBJECT
public:
    explicit JsonLineConnection(QTcpSocket *sock, QObject *parent = nullptr);
    void send(const QJsonObject &obj);
    QTcpSocket *socket() const { return sock_; }
    QHostAddress peerAddress() const;

signals:
    void message(const QJsonObject &obj);
    void disconnected();

private:
    void onReady();
    QTcpSocket *sock_ = nullptr;
    QByteArray buf_;
};

class ControlServer : public QObject {
    Q_OBJECT
public:
    explicit ControlServer(QObject *parent = nullptr);
    bool start(const SessionConfig &cfg, const QHostAddress &bind, QString *error);
    void stop();
    void send(const QJsonObject &obj);
    void setStreamSize(int width, int height);
    bool hasClient() const { return bool(conn_); }
    QHostAddress clientAddress() const;
    quint16 port() const { return port_; }

signals:
    void clientConnected(const QHostAddress &addr);
    void clientDisconnected();
    void authenticated();
    void authFailed(const QString &why);
    void inputEvent(const QJsonObject &obj);
    void logMessage(const QString &msg);

private:
    void onIncoming();
    void onMessage(const QJsonObject &obj);
    void dropClient();

    QTcpServer *server_ = nullptr;
    QPointer<JsonLineConnection> conn_;
    SessionConfig cfg_;
    quint16 port_ = 0;
    int failures_ = 0;
    bool authed_ = false;
};

class ControlClient : public QObject {
    Q_OBJECT
public:
    explicit ControlClient(QObject *parent = nullptr);
    void connectTo(const QHostAddress &host, quint16 port, const QString &pin,
                   const QString &name);
    void disconnectFromHost();
    void send(const QJsonObject &obj);
    bool isConnected() const { return connected_; }

signals:
    void connected();
    void disconnected();
    void welcome(const QJsonObject &obj);
    void streamSize(int width, int height);
    void rejected(const QString &why);
    void logMessage(const QString &msg);

private:
    void onMessage(const QJsonObject &obj);

    QTcpSocket *sock_ = nullptr;
    QPointer<JsonLineConnection> conn_;
    QString pin_;
    QString name_;
    bool connected_ = false;
};

} // namespace lanscreen
