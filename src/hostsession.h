#pragma once

#include "common.h"
#include "probe.h"

#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QWindow>

namespace lanscreen {

class DiscoveryAnnouncer;
class ControlServer;
class GstPipelineRunner;
class InputInjector;
class PortalCapture;
class KdeVirtualOutput;

class HostSession : public QObject {
    Q_OBJECT
public:
    explicit HostSession(QObject *parent = nullptr);
    ~HostSession() override;

    void setProbe(const ProbeResult &p) { probe_ = p; }
    const ProbeResult &probe() const { return probe_; }

    bool start(const SessionConfig &cfg, QString *error);
    // For portal capture: begin the portal request, then start the pipeline
    // once the fd is ready. start() itself kicks this off when needed.
    void stop();
    bool isRunning() const { return running_; }
    SessionConfig config() const { return cfg_; }
    QString status() const { return status_; }
    QString pipelineLaunch() const { return launch_; }
    QString encoderUsed() const { return encoderUsed_; }
    QHostAddress listenAddress() const { return listen_; }
    bool clientConnected() const { return clientOk_; }

    void setParentWindow(QWindow *w) { parentWindow_ = w; }

signals:
    void statusChanged(const QString &s);
    void logMessage(const QString &s);
    void clientChanged(bool connected);
    void started();
    void stopped();
    void failed(const QString &error);

private:
    bool startNetworking(QString *error);
    bool startPipeline(int pwFd, const QString &pwPath, QString *error);
    void setStatus(const QString &s);
    void onAuthenticated();
    void configureSinkForClient();

    SessionConfig cfg_;
    ProbeResult probe_;
    DiscoveryAnnouncer *announce_ = nullptr;
    ControlServer *control_ = nullptr;
    GstPipelineRunner *pipe_ = nullptr;
    InputInjector *input_ = nullptr;
    PortalCapture *portal_ = nullptr;
    KdeVirtualOutput *virt_ = nullptr;
    QWindow *parentWindow_ = nullptr;
    QHostAddress listen_;
    QString status_;
    QString launch_;
    QString encoderUsed_;
    bool running_ = false;
    bool clientOk_ = false;
    bool waitingPortal_ = false;
};

} // namespace lanscreen
