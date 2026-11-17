#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QWindow>

class QDBusPendingCallWatcher;

namespace lanscreen {

// xdg-desktop-portal ScreenCast helper. On success you get a PipeWire fd
// and a node id to feed into pipewiresrc.
class PortalCapture : public QObject {
    Q_OBJECT
public:
    explicit PortalCapture(QObject *parent = nullptr);
    ~PortalCapture() override;

    // preferVirtual asks the portal for a VIRTUAL monitor first (some
    // compositors will actually create one). Falls back to a real monitor.
    void start(QWindow *parentWindow = nullptr, bool preferVirtual = false);
    void cancel();

    int fd() const { return fd_; }
    QString nodePath() const { return nodePath_; }
    bool isReady() const { return fd_ >= 0; }

signals:
    void ready(int fd, const QString &nodePath);
    void failed(const QString &reason);

private slots:
    void onResponse(uint code, const QVariantMap &results);
    void onOpenRemoteFinished(QDBusPendingCallWatcher *watcher);

private:
    enum class Step { Idle, CreateSession, SelectSources, Start, OpenRemote };

    void call(const QString &method, const QVariantList &args, const QString &handleToken);
    void createSession();
    void selectSources();
    void startStream();
    void openRemote();
    void fail(const QString &why);
    void listenFor(const QString &handleToken);

    int fd_ = -1;
    QString nodePath_;
    QString sessionHandle_;
    QString parentWindowHint_;
    QString requestPath_;
    bool preferVirtual_ = false;
    bool triedVirtual_ = false;
    Step step_ = Step::Idle;
};

} // namespace lanscreen
