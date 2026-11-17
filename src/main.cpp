#include "clientsession.h"
#include "common.h"
#include "hostsession.h"
#include "outputs.h"
#include "kdisplay.h"
#include "probe.h"
#include "selftest.h"
#include "ui/mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QHostAddress>
#include <QScopedPointer>
#include <QTimer>
#include <cstdio>

#include <gst/gst.h>

using namespace lanscreen;

static SessionConfig configFromParser(const QCommandLineParser &p, SessionConfig c)
{
    if (p.isSet(QStringLiteral("encoder")))
        c.encoder = encoderFamilyFromName(p.value(QStringLiteral("encoder")));
    if (p.isSet(QStringLiteral("decoder")))
        c.decoder = decoderFamilyFromName(p.value(QStringLiteral("decoder")));
    if (p.isSet(QStringLiteral("codec")))
        c.codec = codecFromName(p.value(QStringLiteral("codec")));
    if (p.isSet(QStringLiteral("bitrate")))
        c.bitrateKbps = p.value(QStringLiteral("bitrate")).toInt();
    if (p.isSet(QStringLiteral("rate-control")))
        c.rateControl = rateControlFromName(p.value(QStringLiteral("rate-control")));
    if (p.isSet(QStringLiteral("fps")))
        c.fps = p.value(QStringLiteral("fps")).toInt();
    if (p.isSet(QStringLiteral("width")))
        c.width = p.value(QStringLiteral("width")).toInt();
    if (p.isSet(QStringLiteral("height")))
        c.height = p.value(QStringLiteral("height")).toInt();
    if (p.isSet(QStringLiteral("preset")))
        c.preset = presetFromName(p.value(QStringLiteral("preset")));
    if (p.isSet(QStringLiteral("keyint")))
        c.keyint = p.value(QStringLiteral("keyint")).toInt();
    if (p.isSet(QStringLiteral("transport")))
        c.transport = transportFromName(p.value(QStringLiteral("transport")));
    if (p.isSet(QStringLiteral("bind")))
        c.bindAddress = p.value(QStringLiteral("bind"));
    if (p.isSet(QStringLiteral("port")))
        c.controlPort = static_cast<quint16>(p.value(QStringLiteral("port")).toUInt());
    if (p.isSet(QStringLiteral("media-port")))
        c.mediaPort = static_cast<quint16>(p.value(QStringLiteral("media-port")).toUInt());
    else if (p.isSet(QStringLiteral("port")))
        c.mediaPort = static_cast<quint16>(c.controlPort + 1);
    if (p.isSet(QStringLiteral("pin")))
        c.pin = p.value(QStringLiteral("pin"));
    if (p.isSet(QStringLiteral("source"))) {
        const QString s = p.value(QStringLiteral("source"));
        c.capture = captureFromName(s);
        if (s.startsWith(QLatin1String("output=")))
            c.outputId = s.mid(QStringLiteral("output=").size());
    }
    if (p.isSet(QStringLiteral("name")))
        c.sessionName = p.value(QStringLiteral("name"));
    if (p.isSet(QStringLiteral("srt-latency")))
        c.srtLatencyMs = p.value(QStringLiteral("srt-latency")).toInt();
    if (p.isSet(QStringLiteral("fec")))
        c.fec = true;
    if (p.isSet(QStringLiteral("fullscreen")))
        c.fullscreen = true;
    if (p.isSet(QStringLiteral("virtual-display")))
        c.virtualDisplay = 1;
    if (p.isSet(QStringLiteral("no-virtual-display")))
        c.virtualDisplay = 0;
    if (p.isSet(QStringLiteral("virtual-res"))) {
        const QString r = p.value(QStringLiteral("virtual-res")).toLower();
        const auto parts = r.split(QLatin1Char('x'));
        if (parts.size() == 2) {
            c.virtualWidth = parts[0].toInt();
            c.virtualHeight = parts[1].toInt();
        }
    }
    if (p.isSet(QStringLiteral("virtual-hz")))
        c.virtualRefresh = p.value(QStringLiteral("virtual-hz")).toInt();
    return c;
}

static void addOptions(QCommandLineParser *p)
{
    p->setApplicationDescription(
        QStringLiteral("Use a second Linux PC as an extended monitor over the LAN."));
    p->addHelpOption();
    p->addVersionOption();
    p->addOption({QStringLiteral("host"), QStringLiteral("Start as the host (share a screen).")});
    p->addOption({QStringLiteral("client"), QStringLiteral("Start as the client (use as a monitor).")});
    p->addOption({QStringLiteral("headless"), QStringLiteral("No GUI. Use with --host or --client.")});
    p->addOption({{QStringLiteral("connect"), QStringLiteral("c")},
                  QStringLiteral("Client: host IP or host:port."),
                  QStringLiteral("addr")});
    p->addOption({QStringLiteral("pin"), QStringLiteral("Optional PIN / passphrase."), QStringLiteral("pin")});
    p->addOption({QStringLiteral("name"), QStringLiteral("Session name advertised on the LAN."),
                  QStringLiteral("name")});
    p->addOption({QStringLiteral("encoder"),
                  QStringLiteral("auto, nvenc, vaapi, v4l2, or software."),
                  QStringLiteral("enc"),
                  QStringLiteral("auto")});
    p->addOption({QStringLiteral("decoder"), QStringLiteral("auto, hw, or sw."), QStringLiteral("dec"),
                  QStringLiteral("auto")});
    p->addOption({QStringLiteral("codec"), QStringLiteral("h264, hevc, or av1."), QStringLiteral("codec"),
                  QStringLiteral("h264")});
    p->addOption({QStringLiteral("bitrate"), QStringLiteral("Target bitrate in kbps."), QStringLiteral("kbps"),
                  QStringLiteral("8000")});
    p->addOption({QStringLiteral("rate-control"), QStringLiteral("cbr, vbr, or cqp."), QStringLiteral("rc"),
                  QStringLiteral("cbr")});
    p->addOption({QStringLiteral("fps"), QStringLiteral("Framerate."), QStringLiteral("n"), QStringLiteral("60")});
    p->addOption({QStringLiteral("width"), QStringLiteral("Width (0 = native)."), QStringLiteral("px")});
    p->addOption({QStringLiteral("height"), QStringLiteral("Height (0 = native)."), QStringLiteral("px")});
    p->addOption({QStringLiteral("preset"),
                  QStringLiteral("ultra-low-latency, low-latency, or quality."),
                  QStringLiteral("p"),
                  QStringLiteral("low-latency")});
    p->addOption({QStringLiteral("keyint"), QStringLiteral("Keyframe interval in frames."),
                  QStringLiteral("n")});
    p->addOption({QStringLiteral("transport"), QStringLiteral("srt, udp, or tcp."), QStringLiteral("t"),
                  QStringLiteral("srt")});
    p->addOption({QStringLiteral("srt-latency"), QStringLiteral("SRT latency in milliseconds."),
                  QStringLiteral("ms"), QStringLiteral("60")});
    p->addOption({QStringLiteral("fec"), QStringLiteral("Enable experimental RTP FEC (UDP only).")});
    p->addOption({QStringLiteral("bind"), QStringLiteral("Address to advertise / bind."), QStringLiteral("addr")});
    p->addOption({QStringLiteral("port"), QStringLiteral("Control port (media defaults to port+1)."),
                  QStringLiteral("n")});
    p->addOption({QStringLiteral("media-port"), QStringLiteral("Media port."), QStringLiteral("n")});
    p->addOption({QStringLiteral("source"),
                  QStringLiteral("test, screen, portal, or output=NAME."),
                  QStringLiteral("src")});
    p->addOption({QStringLiteral("virtual-display"),
                  QStringLiteral("Create a KDE/KWin virtual monitor (default on when Plasma Wayland is detected).")});
    p->addOption({QStringLiteral("no-virtual-display"),
                  QStringLiteral("Do not create a virtual monitor.")});
    p->addOption({QStringLiteral("virtual-res"),
                  QStringLiteral("Virtual monitor size, e.g. 1920x1080."),
                  QStringLiteral("WxH")});
    p->addOption({QStringLiteral("virtual-hz"),
                  QStringLiteral("Virtual monitor refresh rate (Hz). Needs Plasma 6.6+ if not 60."),
                  QStringLiteral("hz")});
    p->addOption({QStringLiteral("fullscreen"), QStringLiteral("Client: go fullscreen.")});
    p->addOption({QStringLiteral("probe"), QStringLiteral("Print available encoders/decoders and exit.")});
    p->addOption({QStringLiteral("list-outputs"), QStringLiteral("Print displays and virtual-display notes.")});
    p->addOption({QStringLiteral("selftest"),
                  QStringLiteral("Loopback test: software H.264 over SRT, then exit.")});
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);

    // Peek at flags before deciding QApplication vs QCoreApplication.
    bool wantGui = true;
    bool forceGuiApp = false;
    for (int i = 1; i < argc; ++i) {
        const QByteArray a = argv[i];
        if (a == "--list-outputs")
            forceGuiApp = true; // so Qt can see screens
        if (a == "--headless" || a == "--probe" || a == "--selftest"
            || a == "-h" || a == "--help" || a == "-v" || a == "--version")
            wantGui = false;
    }
    if (forceGuiApp)
        wantGui = true;

    QScopedPointer<QCoreApplication> app;
    if (wantGui)
        app.reset(new QApplication(argc, argv));
    else
        app.reset(new QCoreApplication(argc, argv));

    QCoreApplication::setApplicationName(QStringLiteral("lanscreen"));
    QCoreApplication::setApplicationVersion(QStringLiteral(LANSCREEN_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("lanscreen"));

    QCommandLineParser parser;
    addOptions(&parser);
    parser.process(*app);

    if (parser.isSet(QStringLiteral("probe"))) {
        const ProbeResult r = probeCodecs();
        std::fputs(formatProbeReport(r).toUtf8().constData(), stdout);
        return 0;
    }
    if (parser.isSet(QStringLiteral("list-outputs"))) {
        std::fputs(virtualDisplayReport().toUtf8().constData(), stdout);
        return 0;
    }
    if (parser.isSet(QStringLiteral("selftest")))
        return runSelfTest();

    SessionConfig cfg = configFromParser(parser, SessionConfig{});

    if (parser.isSet(QStringLiteral("headless"))) {
        if (parser.isSet(QStringLiteral("host"))) {
            auto *host = new HostSession(app.data());
            QObject::connect(host, &HostSession::logMessage, [](const QString &s) {
                std::fprintf(stderr, "%s\n", s.toUtf8().constData());
            });
            QObject::connect(host, &HostSession::failed, [app = app.data()](const QString &e) {
                std::fprintf(stderr, "error: %s\n", e.toUtf8().constData());
                app->quit();
            });
            QString err;
            if (!host->start(cfg, &err)) {
                std::fprintf(stderr, "error: %s\n", err.toUtf8().constData());
                return 1;
            }
            QObject::connect(app.data(), &QCoreApplication::aboutToQuit, host, &HostSession::stop);
            return app->exec();
        }
        if (parser.isSet(QStringLiteral("client"))) {
            const QString addr = parser.value(QStringLiteral("connect"));
            if (addr.isEmpty()) {
                std::fprintf(stderr, "error: --client --headless needs --connect HOST\n");
                return 2;
            }
            QString host = addr;
            if (addr.contains(QLatin1Char(':'))) {
                const int i = addr.lastIndexOf(QLatin1Char(':'));
                host = addr.left(i);
                cfg.controlPort = static_cast<quint16>(addr.mid(i + 1).toUInt());
            }
            auto *client = new ClientSession(app.data());
            client->setUseAppsink(true);
            QObject::connect(client, &ClientSession::logMessage, [](const QString &s) {
                std::fprintf(stderr, "%s\n", s.toUtf8().constData());
            });
            QObject::connect(client, &ClientSession::failed, [app = app.data()](const QString &e) {
                std::fprintf(stderr, "error: %s\n", e.toUtf8().constData());
                QTimer::singleShot(50, app, &QCoreApplication::quit);
            });
            client->connectToHost(QHostAddress(host), cfg);
            QObject::connect(app.data(), &QCoreApplication::aboutToQuit, client,
                             &ClientSession::disconnectFromHost);
            return app->exec();
        }
        std::fprintf(stderr, "error: --headless requires --host or --client\n");
        return 2;
    }

    auto *gui = qobject_cast<QApplication *>(app.data());
    if (!gui) {
        std::fprintf(stderr, "error: GUI requested but QApplication was not created\n");
        return 2;
    }
    MainWindow w;
    if (parser.isSet(QStringLiteral("host")))
        w.applyCliHost(cfg);
    if (parser.isSet(QStringLiteral("client"))) {
        const QString addr = parser.value(QStringLiteral("connect"));
        w.applyCliClient(addr, cfg);
    }
    w.show();
    return app->exec();
}
