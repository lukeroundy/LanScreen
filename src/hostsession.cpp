#include "hostsession.h"

#include "control.h"
#include "discovery.h"
#include "inputinject.h"
#include "netutil.h"
#include "outputs.h"
#include "pipeline.h"
#include "portal.h"
#include "kdisplay.h"

#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

namespace lanscreen {

HostSession::HostSession(QObject *parent)
    : QObject(parent)
{
    announce_ = new DiscoveryAnnouncer(this);
    control_ = new ControlServer(this);
    pipe_ = new GstPipelineRunner(this);
    input_ = new InputInjector(this);
    portal_ = new PortalCapture(this);
    virt_ = new KdeVirtualOutput(this);
    connect(virt_, &KdeVirtualOutput::logMessage, this, &HostSession::logMessage);

    connect(control_, &ControlServer::logMessage, this, &HostSession::logMessage);
    connect(control_, &ControlServer::authenticated, this, &HostSession::onAuthenticated);
    connect(control_, &ControlServer::clientDisconnected, this, [this]() {
        clientOk_ = false;
        input_->park();
        emit clientChanged(false);
        emit logMessage(QStringLiteral("Client left — pointer returned to the main display."));
    });
    connect(control_, &ControlServer::inputEvent, this, [this](const QJsonObject &obj) {
        input_->handleEvent(obj);
    });
    connect(pipe_, &GstPipelineRunner::errorMessage, this, [this](const QString &m) {
        emit logMessage(QStringLiteral("GStreamer: ") + m);
        emit failed(m);
    });
    connect(pipe_, &GstPipelineRunner::pictureSize, this, [this](int w, int h) {
        if (w <= 0 || h <= 0)
            return;
        if (cfg_.width != w || cfg_.height != h)
            emit logMessage(QStringLiteral("Capture size %1x%2").arg(w).arg(h));
        cfg_.width = w;
        cfg_.height = h;
        control_->setStreamSize(w, h);
    });
    connect(pipe_, &GstPipelineRunner::infoMessage, this, [this](const QString &m) {
        emit logMessage(m);
    });
    connect(portal_, &PortalCapture::ready, this, [this](int fd, const QString &path) {
        waitingPortal_ = false;
        QString err;
        if (!startPipeline(fd, path, &err)) {
            emit failed(err);
            stop();
        } else {
            setStatus(QStringLiteral("Sharing (portal capture) — waiting for a client"));
            emit logMessage(QStringLiteral("Capturing KDE screen share (node %1).").arg(path));
            emit started();
        }
    });
    connect(portal_, &PortalCapture::failed, this, [this](const QString &why) {
        waitingPortal_ = false;
        emit logMessage(QStringLiteral("Portal: ") + why);
        // Fall back to X11 or test pattern so the host still starts.
        emit logMessage(QStringLiteral("Falling back to X11 / test-pattern capture."));
        SessionConfig c = cfg_;
        if (c.capture == CaptureSource::Portal)
            c.capture = CaptureSource::Screen;
        cfg_ = c;
        QString err;
        if (!startPipeline(-1, {}, &err)) {
            emit failed(err);
            stop();
        } else {
            setStatus(QStringLiteral("Sharing — waiting for a client"));
            emit started();
        }
    });
}

HostSession::~HostSession()
{
    stop();
}

void HostSession::setStatus(const QString &s)
{
    status_ = s;
    emit statusChanged(s);
}

bool HostSession::start(const SessionConfig &cfg, QString *error)
{
    stop();
    cfg_ = cfg;
    if (probe_.encoders.isEmpty())
        probe_ = probeCodecs();

    const bool wantVirtual = cfg_.capture != CaptureSource::TestPattern
        && (cfg_.virtualDisplay == 1
            || (cfg_.virtualDisplay != 0 && detectKdeEnvironment().canCreateVirtualOutput()));

    if (wantVirtual) {
        const int vw = cfg_.virtualWidth > 0 ? cfg_.virtualWidth
                                             : (cfg_.width > 0 ? cfg_.width : 1920);
        const int vh = cfg_.virtualHeight > 0 ? cfg_.virtualHeight
                                             : (cfg_.height > 0 ? cfg_.height : 1080);
        const int hz = cfg_.virtualRefresh > 0 ? cfg_.virtualRefresh : 60;
        QString verr;
        if (!virt_->create(cfg_.virtualName.isEmpty() ? QStringLiteral("lanscreen")
                                                      : cfg_.virtualName,
                           vw, vh, hz, &verr)) {
            if (error)
                *error = verr + QStringLiteral("\n\nExisting fallbacks still work: pick an output, plug a dummy HDMI/DP dongle, or use a test pattern.");
            emit logMessage(verr);
            stop();
            return false;
        }
        const QRect g = virt_->geometry();
        cfg_.outputId = virt_->outputName();
        cfg_.cropX = g.x();
        cfg_.cropY = g.y();
        cfg_.cropW = g.width();
        cfg_.cropH = g.height();
        if (cfg_.width <= 0)
            cfg_.width = g.width();
        if (cfg_.height <= 0)
            cfg_.height = g.height();
        // Capture the new virtual output, not the primary screen.
        if (cfg_.capture == CaptureSource::Screen)
            cfg_.capture = qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty()
                               ? CaptureSource::Output
                               : CaptureSource::Portal;
        emit logMessage(QStringLiteral("Using virtual output %1 as the extended screen.")
                            .arg(virt_->outputName()));
    }

    // If the user picked a Qt screen / output, crop ximagesrc to it.
    if ((cfg_.capture == CaptureSource::Screen || cfg_.capture == CaptureSource::Output)
        && !cfg_.outputId.isEmpty()) {
        for (const DisplayOutput &o : listOutputs()) {
            if (o.id == cfg_.outputId || o.name == cfg_.outputId) {
                if (o.geom.isValid() && o.geom.width() > 0) {
                    cfg_.cropX = o.geom.x();
                    cfg_.cropY = o.geom.y();
                    cfg_.cropW = o.geom.width();
                    cfg_.cropH = o.geom.height();
                    if (cfg_.width <= 0)
                        cfg_.width = o.geom.width();
                    if (cfg_.height <= 0)
                        cfg_.height = o.geom.height();
                }
                break;
            }
        }
    } else if (cfg_.capture == CaptureSource::Screen && qApp) {
        if (QScreen *s = QGuiApplication::primaryScreen()) {
            const QRect g = s->geometry();
            cfg_.cropX = g.x();
            cfg_.cropY = g.y();
            cfg_.cropW = g.width();
            cfg_.cropH = g.height();
            if (cfg_.width <= 0)
                cfg_.width = g.width();
            if (cfg_.height <= 0)
                cfg_.height = g.height();
        }
    }

    QString err;
    if (!startNetworking(&err)) {
        if (error)
            *error = err;
        stop();
        return false;
    }

    QString injErr;
    const int iw = cfg_.cropW > 0 ? cfg_.cropW : (cfg_.width > 0 ? cfg_.width : 1920);
    const int ih = cfg_.cropH > 0 ? cfg_.cropH : (cfg_.height > 0 ? cfg_.height : 1080);
    int deskW = 0, deskH = 0;
    if (virt_ && virt_->isActive() && virt_->desktopBounds().isValid()) {
        deskW = virt_->desktopBounds().width();
        deskH = virt_->desktopBounds().height();
        // Re-read geometry in case kscreen-doctor moved the output after uinput prep.
        const QRect g = virt_->geometry();
        if (g.width() > 0) {
            cfg_.cropX = g.x();
            cfg_.cropY = g.y();
            cfg_.cropW = g.width();
            cfg_.cropH = g.height();
        }
    }
    input_->setDisplayRect(cfg_.cropX, cfg_.cropY,
                           cfg_.cropW > 0 ? cfg_.cropW : iw,
                           cfg_.cropH > 0 ? cfg_.cropH : ih,
                           deskW, deskH);
    if (qApp) {
        QScreen *parkScreen = QGuiApplication::primaryScreen();
        for (QScreen *s : QGuiApplication::screens()) {
            const QString n = s->name();
            if (n.startsWith(QLatin1String("Virtual-"), Qt::CaseInsensitive)
                || n.contains(QLatin1String("xdp"), Qt::CaseInsensitive))
                continue;
            parkScreen = s;
            if (s == QGuiApplication::primaryScreen())
                break;
        }
        if (parkScreen) {
            const QPoint c = parkScreen->geometry().center();
            input_->setParkPosition(c.x(), c.y());
        }
    }
    // Display-only: do not create a uinput keyboard/mouse. A virtual input
    // device shares key state with the physical keyboard, so a stuck modifier
    // (for example Alt) can fire desktop shortcuts like search.
    Q_UNUSED(injErr);
    emit logMessage(QStringLiteral("Client keyboard and mouse are off — display only."));

    const bool wantPortal = (cfg_.capture == CaptureSource::Portal)
        || (!qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty()
            && cfg_.capture != CaptureSource::TestPattern && elementExists("pipewiresrc"));

    if (wantPortal && cfg_.capture != CaptureSource::TestPattern) {
        running_ = true;
        waitingPortal_ = true;
        setStatus(QStringLiteral("Waiting for screen-share permission…"));
        if (virt_ && virt_->isActive()) {
            emit logMessage(QStringLiteral(
                "If a share dialog appears, pick the LanScreen virtual monitor (%1) — not your main screen.")
                                .arg(virt_->outputName()));
        }
        // We already created the virtual output; do not ask the portal for a
        // second virtual monitor. Just capture the one that now exists.
        portal_->start(parentWindow_, false);
        return true;
    }

    if (!startPipeline(-1, {}, &err)) {
        if (error)
            *error = err;
        stop();
        return false;
    }
    running_ = true;
    setStatus(QStringLiteral("Sharing — waiting for a client on %1:%2")
                  .arg(listen_.toString())
                  .arg(cfg_.controlPort));
    emit started();
    return true;
}

bool HostSession::startNetworking(QString *error)
{
    if (!cfg_.bindAddress.isEmpty())
        listen_ = QHostAddress(cfg_.bindAddress);
    else
        listen_ = firstLanAddress();

    // Listen on all IPv4 addresses so localhost self-tests still work, but we
    // advertise the LAN address in discovery / the UI.
    QHostAddress bind = QHostAddress::AnyIPv4;
    if (!cfg_.bindAddress.isEmpty() && cfg_.bindAddress != QLatin1String("0.0.0.0")
        && cfg_.bindAddress != QLatin1String("auto"))
        bind = QHostAddress(cfg_.bindAddress);

    QString err;
    if (!control_->start(cfg_, bind, &err)) {
        if (error)
            *error = QStringLiteral("Control port: ") + err;
        return false;
    }
    cfg_.controlPort = control_->port();
    announce_->start(cfg_, bind);
    emit logMessage(QStringLiteral("Listening on %1  (control %2, media %3, %4)")
                        .arg(listen_.toString())
                        .arg(cfg_.controlPort)
                        .arg(cfg_.mediaPort)
                        .arg(transportName(cfg_.transport)));
    return true;
}

bool HostSession::startPipeline(int pwFd, const QString &pwPath, QString *error)
{
    const PipelinePlan plan = planHostPipeline(cfg_, probe_, listen_.toString(), pwFd, pwPath);
    if (plan.launch.isEmpty()) {
        if (error)
            *error = plan.warnings.join(QLatin1Char('\n'));
        return false;
    }
    for (const QString &w : plan.warnings)
        emit logMessage(w);
    launch_ = plan.launch;
    encoderUsed_ = plan.encoderElement;
    emit logMessage(QStringLiteral("Pipeline: ") + launch_);
    emit logMessage(QStringLiteral("Encoder: ") + plan.description);

    QString err;
    if (!pipe_->prepare(launch_, &err)) {
        if (error)
            *error = err;
        return false;
    }
    if (const CodecElement *enc = pickEncoder(probe_, cfg_.encoder, cfg_.codec))
        applyEncoderSettings(*pipe_, *enc, cfg_);
    pipe_->enableEncodeAlign(32);

    // Transport endpoints that must be set as properties (avoid '&' in launch).
    if (cfg_.transport == Transport::Srt) {
        const QString ip = (cfg_.bindAddress.isEmpty() || cfg_.bindAddress == QLatin1String("auto"))
                               ? QStringLiteral("0.0.0.0")
                               : cfg_.bindAddress;
        QString uri = QStringLiteral("srt://%1:%2?mode=listener&latency=%3&transtype=live")
                          .arg(ip)
                          .arg(cfg_.mediaPort)
                          .arg(cfg_.srtLatencyMs);
        const QString pass = srtPassphraseFromPin(cfg_.pin);
        if (!pass.isEmpty())
            uri += QStringLiteral("&passphrase=%1").arg(pass);
        pipe_->setString("sink", "uri", uri);
        pipe_->setInt("sink", "latency", cfg_.srtLatencyMs);
        pipe_->setInt("sink", "mode", 2); // listener
        pipe_->setBool("sink", "wait-for-connection", false);
    } else if (cfg_.transport == Transport::Tcp) {
        pipe_->setInt("sink", "port", cfg_.mediaPort);
        pipe_->setString("sink", "host", QStringLiteral("0.0.0.0"));
    } else {
        // UDP: bind locally; host is filled in once a client authenticates.
        pipe_->setInt("sink", "port", cfg_.mediaPort);
        pipe_->setInt("sink", "bind-port", cfg_.mediaPort);
        pipe_->setString("sink", "host", QStringLiteral("127.0.0.1"));
    }
    if (!pipe_->play(&err)) {
        if (error)
            *error = err;
        return false;
    }
    running_ = true;
    return true;
}

void HostSession::configureSinkForClient()
{
    if (cfg_.transport != Transport::UdpRtp)
        return;
    const QHostAddress peer = control_->clientAddress();
    if (peer.isNull())
        return;
    pipe_->setString("sink", "host", peer.toString());
    emit logMessage(QStringLiteral("UDP target set to ") + peer.toString() + QLatin1Char(':')
                    + QString::number(cfg_.mediaPort));
}

void HostSession::onAuthenticated()
{
    clientOk_ = true;
    configureSinkForClient();
    setStatus(QStringLiteral("Client connected from %1  (%2)")
                  .arg(control_->clientAddress().toString(), encoderUsed_));
    emit clientChanged(true);
}

void HostSession::stop()
{
    waitingPortal_ = false;
    portal_->cancel();
    pipe_->stop();
    input_->stop();
    if (virt_)
        virt_->destroy();
    announce_->stop();
    control_->stop();
    const bool was = running_;
    running_ = false;
    clientOk_ = false;
    if (was) {
        setStatus(QStringLiteral("Stopped"));
        emit stopped();
    }
}

} // namespace lanscreen
