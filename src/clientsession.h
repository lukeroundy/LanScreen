#pragma once

#include "common.h"
#include "discovery.h"
#include "probe.h"

#include <QHostAddress>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QSize>
#include <QString>

namespace lanscreen {

class ControlClient;
class GstPipelineRunner;

class ClientSession : public QObject {
    Q_OBJECT
public:
    explicit ClientSession(QObject *parent = nullptr);
    ~ClientSession() override;

    void setProbe(const ProbeResult &p) { probe_ = p; }
    void setVideoHandle(quintptr wid) { videoHandle_ = wid; }
    void setUseAppsink(bool v) { useAppsink_ = v; }
    bool usingAppsink() const { return useAppsink_; }

    void connectToHost(const QHostAddress &host, const SessionConfig &cfg);
    void disconnectFromHost();
    bool isConnected() const { return connected_; }
    QString status() const { return status_; }
    QString pipelineLaunch() const { return launch_; }
    SessionConfig config() const { return cfg_; }
    QSize streamSize() const { return QSize(streamW_, streamH_); }

    void sendInput(const QJsonObject &ev);
    bool setStreamFullscreen(bool on);
    bool streamFullscreen() const { return streamFullscreen_; }

signals:
    void statusChanged(const QString &s);
    void logMessage(const QString &s);
    void connected();
    void disconnected();
    void failed(const QString &error);
    void frameReady(const QImage &frame);

private:
    void onWelcome(const QJsonObject &obj);
    void startMedia(const QString &host);
    void setStatus(const QString &s);

    ProbeResult probe_;
    SessionConfig cfg_;
    ControlClient *control_ = nullptr;
    GstPipelineRunner *pipe_ = nullptr;
    quintptr videoHandle_ = 0;
    bool useAppsink_ = true;
    bool connected_ = false;
    bool dropping_ = false;
    bool triedSoftwareFallback_ = false;
    bool streamFullscreen_ = false;
    QString status_;
    QString launch_;
    QHostAddress host_;
    int streamW_ = 0, streamH_ = 0;
};

} // namespace lanscreen
