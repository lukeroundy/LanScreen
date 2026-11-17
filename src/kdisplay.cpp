#include "kdisplay.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPoint>
#include <QProcess>
#include <QRandomGenerator>
#include <QRect>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QTcpServer>
#include <QThread>
#include <QHostAddress>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

namespace lanscreen {

static QString findTool(const char *name)
{
    return QStandardPaths::findExecutable(QString::fromLatin1(name));
}

KdeEnvironment detectKdeEnvironment()
{
    KdeEnvironment e;
    e.desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    if (e.desktop.isEmpty())
        e.desktop = qEnvironmentVariable("XDG_SESSION_DESKTOP");
    e.sessionType = qEnvironmentVariable("XDG_SESSION_TYPE");
    const QString blob = (e.desktop + QLatin1Char(' ') + qEnvironmentVariable("DESKTOP_SESSION")
                          + QLatin1Char(' ') + qEnvironmentVariable("KDE_FULL_SESSION"))
                             .toLower();
    e.kde = blob.contains(QLatin1String("kde")) || blob.contains(QLatin1String("plasma"));
    e.wayland = !qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty()
        || e.sessionType.compare(QLatin1String("wayland"), Qt::CaseInsensitive) == 0;
    if (QDBusConnection::sessionBus().isConnected()) {
        if (auto *iface = QDBusConnection::sessionBus().interface())
            e.kwinOnBus = iface->isServiceRegistered(QStringLiteral("org.kde.KWin"));
    }
    if (e.kwinOnBus)
        e.kde = true;
    e.krfbPath = findTool("krfb-virtualmonitor");
    e.hasKrfbVirtualMonitor = !e.krfbPath.isEmpty();
    e.kscreenDoctorPath = findTool("kscreen-doctor");
    e.hasKscreenDoctor = !e.kscreenDoctorPath.isEmpty();
    return e;
}

QString KdeEnvironment::summary() const
{
    return QStringLiteral("desktop=%1  session=%2  kde=%3  wayland=%4  kwin=%5  krfb-virtualmonitor=%6  kscreen-doctor=%7")
        .arg(desktop.isEmpty() ? QStringLiteral("(unknown)") : desktop,
             sessionType.isEmpty() ? QStringLiteral("(unknown)") : sessionType,
             kde ? QStringLiteral("yes") : QStringLiteral("no"),
             wayland ? QStringLiteral("yes") : QStringLiteral("no"),
             kwinOnBus ? QStringLiteral("yes") : QStringLiteral("no"),
             hasKrfbVirtualMonitor ? krfbPath : QStringLiteral("not found"),
             hasKscreenDoctor ? kscreenDoctorPath : QStringLiteral("not found"));
}

bool KdeEnvironment::canCreateVirtualOutput() const
{
    return kde && wayland && hasKrfbVirtualMonitor;
}

QString KdeEnvironment::unavailableReason() const
{
    if (!kde && !kwinOnBus)
        return QStringLiteral("This session is not KDE Plasma (no KWin on the session bus).");
    if (!wayland)
        return QStringLiteral("Plasma is not running on Wayland. krfb-virtualmonitor (the official way to add a KWin virtual monitor) only works in a Plasma Wayland session, not on X11. Use a dummy HDMI/DP dongle, pick an existing output, or a test pattern, or log into Plasma on Wayland.");
    if (!hasKrfbVirtualMonitor)
        return QStringLiteral("krfb-virtualmonitor was not found on PATH. Install the krfb package (Arch: pacman -S krfb; Debian/Ubuntu: apt install krfb). kscreen-doctor (kscreen package) is also recommended so the new output can be positioned and its mode set.");
    return QStringLiteral("KDE virtual display is not available.");
}

static QRect jsonRect(const QJsonObject &o)
{
    if (o.contains(QStringLiteral("pos")) || o.contains(QStringLiteral("size"))) {
        const QJsonObject pos = o.value(QStringLiteral("pos")).toObject();
        const QJsonObject size = o.value(QStringLiteral("size")).toObject();
        return QRect(pos.value(QStringLiteral("x")).toInt(), pos.value(QStringLiteral("y")).toInt(),
                     size.value(QStringLiteral("width")).toInt(), size.value(QStringLiteral("height")).toInt());
    }
    if (o.contains(QStringLiteral("geometry"))) {
        const QJsonObject g = o.value(QStringLiteral("geometry")).toObject();
        return QRect(g.value(QStringLiteral("x")).toInt(), g.value(QStringLiteral("y")).toInt(),
                     g.value(QStringLiteral("width")).toInt(), g.value(QStringLiteral("height")).toInt());
    }
    return {};
}

static QList<DisplayOutput> parseKscreenJson(const QByteArray &json)
{
    QList<DisplayOutput> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject())
        return out;
    const QJsonArray arr = doc.object().value(QStringLiteral("outputs")).toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        DisplayOutput d;
        d.name = o.value(QStringLiteral("name")).toString();
        d.id = d.name;
        if (d.id.isEmpty() && o.contains(QStringLiteral("id")))
            d.id = QString::number(o.value(QStringLiteral("id")).toInt());
        d.geom = jsonRect(o);
        d.connected = o.value(QStringLiteral("connected")).toBool(true);
        const bool enabled = o.value(QStringLiteral("enabled")).toBool(true);
        d.source = QStringLiteral("kscreen");
        d.extra = enabled ? QStringLiteral("enabled") : QStringLiteral("disabled");
        if (o.contains(QStringLiteral("priority")))
            d.extra += QStringLiteral(" priority ") + QString::number(o.value(QStringLiteral("priority")).toInt());
        out.append(d);
    }
    return out;
}

QList<DisplayOutput> listKscreenOutputs()
{
    const KdeEnvironment env = detectKdeEnvironment();
    if (!env.hasKscreenDoctor)
        return {};
    QProcess p;
    p.start(env.kscreenDoctorPath, {QStringLiteral("-j")});
    if (!p.waitForFinished(4000) || p.exitCode() != 0)
        return {};
    return parseKscreenJson(p.readAllStandardOutput());
}

static int pickFreePort()
{
    QTcpServer srv;
    for (int port = 5910; port < 5990; ++port) {
        if (srv.listen(QHostAddress::LocalHost, static_cast<quint16>(port))) {
            srv.close();
            return port;
        }
    }
    if (srv.listen(QHostAddress::LocalHost, 0)) {
        const int port = srv.serverPort();
        srv.close();
        return port;
    }
    return 5910;
}

static QString randomPassword()
{
    static const char kChars[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    QString s;
    s.resize(16);
    for (int i = 0; i < 16; ++i)
        s[i] = QLatin1Char(kChars[QRandomGenerator::global()->bounded(int(sizeof(kChars) - 1))]);
    return s;
}

KdeVirtualOutput::KdeVirtualOutput(QObject *parent)
    : QObject(parent)
{
}

KdeVirtualOutput::~KdeVirtualOutput()
{
    destroy();
}

bool KdeVirtualOutput::isActive() const
{
    return proc_ && proc_->state() != QProcess::NotRunning;
}

void KdeVirtualOutput::destroy()
{
    if (!proc_)
        return;
    emit logMessage(QStringLiteral("Removing the KDE virtual monitor (stopping krfb-virtualmonitor)…"));
    proc_->terminate();
    if (!proc_->waitForFinished(3000))
        proc_->kill();
    proc_->waitForFinished(1000);
    proc_->deleteLater();
    proc_ = nullptr;
    outputName_.clear();
    geometry_ = {};
}

bool KdeVirtualOutput::runKscreenDoctor(const QStringList &args, QString *out, QString *err)
{
    const KdeEnvironment env = detectKdeEnvironment();
    if (!env.hasKscreenDoctor)
        return false;
    QProcess p;
    p.start(env.kscreenDoctorPath, args);
    if (!p.waitForFinished(5000)) {
        p.kill();
        if (err)
            *err = QStringLiteral("kscreen-doctor timed out");
        return false;
    }
    if (out)
        *out = QString::fromUtf8(p.readAllStandardOutput());
    if (err)
        *err = QString::fromUtf8(p.readAllStandardError());
    return p.exitCode() == 0;
}

DisplayOutput KdeVirtualOutput::findOurOutput() const
{
    const QString want = outputName_.isEmpty() ? (QStringLiteral("Virtual-") + name_) : outputName_;
    auto match = [&](const DisplayOutput &o) {
        return o.name.compare(want, Qt::CaseInsensitive) == 0
            || o.id.compare(want, Qt::CaseInsensitive) == 0
            || o.name.endsWith(QLatin1Char('-') + name_, Qt::CaseInsensitive)
            || (o.name.contains(QLatin1String("Virtual"), Qt::CaseInsensitive)
                && o.name.contains(name_, Qt::CaseInsensitive));
    };
    for (const DisplayOutput &o : listKscreenOutputs()) {
        if (match(o))
            return o;
    }
    for (const DisplayOutput &o : listOutputs()) {
        if (match(o))
            return o;
    }
    if (qApp) {
        for (QScreen *s : QGuiApplication::screens()) {
            DisplayOutput o;
            o.name = s->name();
            o.id = s->name();
            o.geom = s->geometry();
            o.source = QStringLiteral("qt");
            if (match(o))
                return o;
        }
    }
    return {};
}

static void briefWait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

static bool isOurVirtualName(const QString &outputName, const QString &name)
{
    return outputName.contains(name, Qt::CaseInsensitive)
        && outputName.contains(QLatin1String("Virtual"), Qt::CaseInsensitive);
}

static bool touchesOtherOutput(const QRect &geom, const QString &ourName, const QString &name)
{
    if (!geom.isValid() || geom.width() <= 0)
        return false;
    const QRect padded = geom.adjusted(-1, -1, 1, 1);
    for (const DisplayOutput &o : listKscreenOutputs()) {
        if (o.name.compare(ourName, Qt::CaseInsensitive) == 0)
            continue;
        if (isOurVirtualName(o.name, name))
            continue;
        if (o.geom.isValid() && o.geom.width() > 0 && padded.intersects(o.geom))
            return true;
    }
    return false;
}

QPoint KdeVirtualOutput::parkedPosition() const
{
    // Leave a large gap so the pointer cannot walk from the main screen onto
    // the virtual monitor during game look-around. KWin's display settings
    // warn that gaps are unsupported, but kscreen-doctor will still apply them
    // and the cursor then stays on the current screen.
    int right = 0;
    int top = 0;
    bool any = false;
    auto consider = [&](const QRect &g) {
        if (!g.isValid() || g.width() <= 0)
            return;
        if (geometry_.isValid() && g == geometry_)
            return;
        any = true;
        right = qMax(right, g.x() + g.width());
        top = qMin(top, g.y());
    };
    for (const DisplayOutput &o : listKscreenOutputs()) {
        if (o.name.compare(outputName_, Qt::CaseInsensitive) == 0)
            continue;
        if (isOurVirtualName(o.name, name_))
            continue;
        consider(o.geom);
    }
    if (!any && qApp) {
        for (QScreen *s : QGuiApplication::screens()) {
            if (s->name().contains(name_, Qt::CaseInsensitive))
                continue;
            consider(s->geometry());
        }
    }
    const int gap = 20000;
    if (!any)
        return QPoint(gap, 0);
    return QPoint(right + gap, top);
}

bool KdeVirtualOutput::applyParkedPosition(int *xOut, int *yOut, QString *err)
{
    const QPoint pos = parkedPosition();
    const int x = pos.x();
    const int y = pos.y();
    if (xOut)
        *xOut = x;
    if (yOut)
        *yOut = y;
    const QString posArg = QStringLiteral("output.%1.position.%2,%3").arg(outputName_).arg(x).arg(y);
    QString out;
    return runKscreenDoctor({posArg}, &out, err);
}

bool KdeVirtualOutput::waitForOutput(int timeoutMs, QString *error)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (proc_ && proc_->state() == QProcess::NotRunning) {
            const QString err = QString::fromUtf8(proc_->readAllStandardError()) + QString::fromUtf8(proc_->readAllStandardOutput());
            if (error)
                *error = QStringLiteral("krfb-virtualmonitor exited early. ") + err.trimmed();
            return false;
        }
        const DisplayOutput o = findOurOutput();
        if (!o.name.isEmpty() && o.geom.width() > 0) {
            outputName_ = o.name;
            geometry_ = o.geom;
            return true;
        }
        QEventLoop loop;
        QTimer::singleShot(200, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (error)
        *error = QStringLiteral("Timed out waiting for KWin to publish the virtual output. Is this a Plasma Wayland session, and is krfb allowed to talk to KWin?");
    return false;
}

bool KdeVirtualOutput::applyLayout(int width, int height, int refreshHz)
{
    if (outputName_.isEmpty())
        return false;
    QString out, err;
    int x = 0;
    int y = 0;

    if (refreshHz <= 0)
        refreshHz = 60;
    // addCustomMode is available on Plasma 6.6+ (refresh is millihertz).
    // krfb-virtualmonitor itself has no --refresh flag and defaults to 60 Hz.
    // Do mode changes first: they can reset position, so we park afterwards.
    const QString add = QStringLiteral("output.%1.addCustomMode.%2.%3.%4.full")
                            .arg(outputName_)
                            .arg(width)
                            .arg(height)
                            .arg(refreshHz * 1000);
    if (runKscreenDoctor({add}, &out, &err)) {
        const QString mode = QStringLiteral("output.%1.mode.%2x%3@%4")
                                 .arg(outputName_)
                                 .arg(width)
                                 .arg(height)
                                 .arg(refreshHz);
        if (runKscreenDoctor({mode}, &out, &err)) {
            emit logMessage(QStringLiteral("Set %1 to %2x%3@%4.")
                                .arg(outputName_)
                                .arg(width)
                                .arg(height)
                                .arg(refreshHz));
        } else {
            emit logMessage(QStringLiteral("Added a custom mode but could not switch to it: %1").arg(err.trimmed()));
        }
    } else if (refreshHz != 60) {
        emit logMessage(QStringLiteral("Could not add a custom %1 Hz mode (needs Plasma 6.6+ kscreen-doctor addCustomMode). The virtual monitor will stay at the default 60 Hz. %2")
                            .arg(refreshHz)
                            .arg(err.trimmed()));
    }

    bool parked = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt > 0)
            briefWait(400);
        if (!applyParkedPosition(&x, &y, &err)) {
            emit logMessage(QStringLiteral("kscreen-doctor could not move %1: %2").arg(outputName_, err.trimmed()));
            break;
        }
        briefWait(250);
        const DisplayOutput now = findOurOutput();
        if (!now.name.isEmpty() && now.geom.width() > 0)
            geometry_ = now.geom;
        else
            geometry_ = QRect(x, y, width, height);
        if (!touchesOtherOutput(geometry_, outputName_, name_)) {
            parked = true;
            break;
        }
        emit logMessage(QStringLiteral("KWin moved %1 back next to the main screen (%2,%3). Trying again.")
                            .arg(outputName_)
                            .arg(geometry_.x())
                            .arg(geometry_.y()));
    }

    if (parked) {
        emit logMessage(QStringLiteral("Parked %1 at %2,%3, far off the desktop so the pointer cannot wander onto it from a game.")
                            .arg(outputName_)
                            .arg(geometry_.x())
                            .arg(geometry_.y()));
    } else {
        emit logMessage(QStringLiteral("The virtual monitor is still at %1,%2 next to the main screen. The pointer may still be able to leave a game.")
                            .arg(geometry_.x())
                            .arg(geometry_.y()));
    }

    desktop_ = geometry_;
    if (qApp) {
        QRect u;
        for (QScreen *s : QGuiApplication::screens())
            u = u.united(s->geometry());
        if (u.isValid())
            desktop_ = u.united(geometry_);
    }
    for (const DisplayOutput &d : listKscreenOutputs())
        desktop_ = desktop_.united(d.geom);
    return true;
}

bool KdeVirtualOutput::create(const QString &name, int width, int height, int refreshHz, QString *error)
{
    destroy();
    const KdeEnvironment env = detectKdeEnvironment();
    if (!env.canCreateVirtualOutput()) {
        if (error)
            *error = env.unavailableReason() + QLatin1Char('\n') + env.summary();
        return false;
    }
    name_ = name.isEmpty() ? QStringLiteral("lanscreen") : name;
    outputName_ = QStringLiteral("Virtual-") + name_;
    if (width < 160)
        width = 1920;
    if (height < 120)
        height = 1080;

    vncPort_ = pickFreePort();
    const QString password = randomPassword();

    proc_ = new QProcess(this);
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    // Newer krfb (2026) can omit --password and then will not start VNC.
    // Older builds require both --password and --port. Passing both works on
    // both; the VNC listener is a side effect we do not use. A random
    // password keeps it from being an open desktop share.
    const QStringList args{
        QStringLiteral("--name"),
        name_,
        QStringLiteral("--resolution"),
        QStringLiteral("%1x%2").arg(width).arg(height),
        QStringLiteral("--scale"),
        QStringLiteral("1"),
        QStringLiteral("--password"),
        password,
        QStringLiteral("--port"),
        QString::number(vncPort_),
    };
    emit logMessage(QStringLiteral("Starting krfb-virtualmonitor --name %1 --resolution %2x%3 (KWin virtual output %4)…")
                        .arg(name_)
                        .arg(width)
                        .arg(height)
                        .arg(outputName_));
    proc_->start(env.krfbPath, args);
    if (!proc_->waitForStarted(3000)) {
        if (error)
            *error = QStringLiteral("Failed to start krfb-virtualmonitor: ") + proc_->errorString();
        destroy();
        return false;
    }

    QString waitErr;
    if (!waitForOutput(8000, &waitErr)) {
        if (error)
            *error = waitErr;
        destroy();
        return false;
    }
    emit logMessage(QStringLiteral("KWin published %1 at %2x%3+%4+%5.")
                        .arg(outputName_)
                        .arg(geometry_.width())
                        .arg(geometry_.height())
                        .arg(geometry_.x())
                        .arg(geometry_.y()));
    applyLayout(width, height, refreshHz);
    log_ = QStringLiteral("%1 %2x%3+%4+%5")
               .arg(outputName_)
               .arg(geometry_.width())
               .arg(geometry_.height())
               .arg(geometry_.x())
               .arg(geometry_.y());
    return true;
}

} // namespace lanscreen
