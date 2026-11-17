#pragma once

#include "outputs.h"

#include <QObject>
#include <QRect>
#include <QString>

class QProcess;

namespace lanscreen {

struct KdeEnvironment {
    bool kde = false;
    bool wayland = false;
    bool kwinOnBus = false;
    bool hasKrfbVirtualMonitor = false;
    bool hasKscreenDoctor = false;
    QString krfbPath;
    QString kscreenDoctorPath;
    QString desktop;
    QString sessionType;
    QString summary() const;
    // True when we can actually create a Plasma virtual monitor.
    bool canCreateVirtualOutput() const;
    QString unavailableReason() const;
};

KdeEnvironment detectKdeEnvironment();

class KdeVirtualOutput : public QObject {
    Q_OBJECT
public:
    explicit KdeVirtualOutput(QObject *parent = nullptr);
    ~KdeVirtualOutput() override;

    // Create a KWin virtual monitor via krfb-virtualmonitor (the official
    // KDE tool). KWin has no addVirtualOutput D-Bus method. The output is
    // named Virtual-<name> and lives until destroy() kills the process.
    bool create(const QString &name, int width, int height, int refreshHz, QString *error);
    void destroy();
    bool isActive() const;

    QString outputName() const { return outputName_; }
    QRect geometry() const { return geometry_; }
    QRect desktopBounds() const { return desktop_; }
    QString lastLog() const { return log_; }

signals:
    void logMessage(const QString &s);

private:
    bool waitForOutput(int timeoutMs, QString *error);
    bool applyLayout(int width, int height, int refreshHz);
    bool runKscreenDoctor(const QStringList &args, QString *out, QString *err);
    DisplayOutput findOurOutput() const;
    QPoint parkedPosition() const;
    bool applyParkedPosition(int *x, int *y, QString *err);

    QProcess *proc_ = nullptr;
    QString name_;
    QString outputName_;
    QRect geometry_;
    QRect desktop_;
    QString log_;
    int vncPort_ = 0;
};

// Parse `kscreen-doctor -j` / `-o` plus Qt screens. Used by the virtual
// output helper and by --list-outputs.
QList<DisplayOutput> listKscreenOutputs();

} // namespace lanscreen
