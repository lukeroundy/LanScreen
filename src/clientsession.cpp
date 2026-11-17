#include "clientsession.h"

#include "control.h"
#include "netutil.h"
#include "pipeline.h"

#include <QJsonObject>
#include <QTimer>

namespace lanscreen {

ClientSession::ClientSession(QObject *parent)
    : QObject(parent)
{
    control_ = new ControlClient(this);
    pipe_ = new GstPipelineRunner(this);
    connect(control_, &ControlClient::logMessage, this, &ClientSession::logMessage);
    connect(control_, &ControlClient::welcome, this, &ClientSession::onWelcome);
    connect(control_, &ControlClient::streamSize, this, [this](int w, int h) {
        if (w > 0)
            streamW_ = w;
        if (h > 0)
            streamH_ = h;
        if (streamW_ > 0 && streamH_ > 0)
            pipe_->setActiveVideoSize(streamW_, streamH_);
    });
    connect(control_, &ControlClient::rejected, this, [this](const QString &why) {
        emit failed(why);
        setStatus(QStringLiteral("Rejected: ") + why);
        disconnectFromHost();
    });
    connect(control_, &ControlClient::disconnected, this, [this]() {
        if (connected_) {
            connected_ = false;
            pipe_->stop();
            setStatus(QStringLiteral("Disconnected"));
            emit disconnected();
        }
    });
    connect(pipe_, &GstPipelineRunner::errorMessage, this, [this](const QString &m) {
        emit logMessage(QStringLiteral("GStreamer: ") + m);
        // Snapdragon V4L2 (and some other HW decoders) reach READY but then fail
        // caps negotiation on a live NVENC/x264 stream. Retry once with libav.
        if (!dropping_ && !triedSoftwareFallback_ && cfg_.decoder != DecoderFamily::Software) {
            triedSoftwareFallback_ = true;
            emit logMessage(QStringLiteral("Hardware decoder failed; retrying with the software decoder."));
            setStatus(QStringLiteral("Retrying with software decoder…"));
            const QString host = host_.toString();
            QTimer::singleShot(0, this, [this, host]() {
                if (dropping_)
                    return;
                pipe_->stop();
                cfg_.decoder = DecoderFamily::Software;
                startMedia(host);
            });
            return;
        }
        emit failed(m);
        // Drop the control link too, so the host can park the cursor instead of
        // leaving it on the virtual display after a failed stream.
        if (!dropping_)
            disconnectFromHost();
    });
    connect(pipe_, &GstPipelineRunner::infoMessage, this, &ClientSession::logMessage);
    connect(pipe_, &GstPipelineRunner::frameReady, this, &ClientSession::frameReady);
}

ClientSession::~ClientSession()
{
    disconnectFromHost();
}

void ClientSession::setStatus(const QString &s)
{
    status_ = s;
    emit statusChanged(s);
}

void ClientSession::connectToHost(const QHostAddress &host, const SessionConfig &cfg)
{
    disconnectFromHost();
    host_ = host;
    cfg_ = cfg;
    triedSoftwareFallback_ = false;
    if (probe_.decoders.isEmpty())
        probe_ = probeCodecs();
    setStatus(QStringLiteral("Connecting to %1:%2…").arg(host.toString()).arg(cfg.controlPort));
    control_->connectTo(host, cfg.controlPort, cfg.pin, localHostname());
}

void ClientSession::disconnectFromHost()
{
    if (dropping_)
        return;
    dropping_ = true;
    pipe_->stop();
    streamFullscreen_ = false;
    control_->disconnectFromHost();
    const bool was = connected_;
    connected_ = false;
    if (was) {
        setStatus(QStringLiteral("Disconnected"));
        emit disconnected();
    }
    dropping_ = false;
}

void ClientSession::onWelcome(const QJsonObject &obj)
{
    const QJsonObject stream = obj.value(QStringLiteral("stream")).toObject();
    const QString proto = stream.value(QStringLiteral("protocol")).toString();
    if (!proto.isEmpty())
        cfg_.transport = transportFromName(proto);
    if (stream.contains(QStringLiteral("port")))
        cfg_.mediaPort = static_cast<quint16>(stream.value(QStringLiteral("port")).toInt());
    if (stream.contains(QStringLiteral("codec")))
        cfg_.codec = codecFromName(stream.value(QStringLiteral("codec")).toString());
    streamW_ = stream.value(QStringLiteral("width")).toInt();
    streamH_ = stream.value(QStringLiteral("height")).toInt();
    if (stream.contains(QStringLiteral("fps")))
        cfg_.fps = stream.value(QStringLiteral("fps")).toInt();
    if (stream.contains(QStringLiteral("srtLatencyMs")))
        cfg_.srtLatencyMs = stream.value(QStringLiteral("srtLatencyMs")).toInt();
    if (stream.contains(QStringLiteral("fec")))
        cfg_.fec = stream.value(QStringLiteral("fec")).toBool();
    const QJsonObject display = obj.value(QStringLiteral("display")).toObject();
    if (display.value(QStringLiteral("w")).toInt() > 0) {
        streamW_ = display.value(QStringLiteral("w")).toInt();
        streamH_ = display.value(QStringLiteral("h")).toInt();
    }
    emit logMessage(QStringLiteral("Welcome: ") + codecName(cfg_.codec) + QLatin1Char(' ')
                    + transportName(cfg_.transport) + QStringLiteral(" port ")
                    + QString::number(cfg_.mediaPort));
    startMedia(host_.toString());
}

void ClientSession::startMedia(const QString &host)
{
    const bool canNative = (elementExists("waylandsink")
                            && !qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty())
        || (elementExists("vulkansink") && elementExists("vulkanupload"));
    if (canNative)
        useAppsink_ = false;
    streamFullscreen_ = false;
    PipelinePlan plan = planClientPipeline(cfg_, probe_, host, useAppsink_);
    if (plan.launch.isEmpty() && !useAppsink_) {
        emit logMessage(QStringLiteral("Stream window unavailable; falling back to the in-app view."));
        useAppsink_ = true;
        plan = planClientPipeline(cfg_, probe_, host, true);
    }
    if (plan.launch.isEmpty()) {
        emit failed(plan.warnings.isEmpty() ? QStringLiteral("Could not build a decode pipeline")
                                            : plan.warnings.join(QLatin1Char('\n')));
        return;
    }
    if (!useAppsink_)
        emit logMessage(QStringLiteral("Opening a stream window."));
    for (const QString &w : plan.warnings)
        emit logMessage(w);
    launch_ = plan.launch;
    emit logMessage(QStringLiteral("Pipeline: ") + launch_);

    QString err;
    if (!pipe_->prepare(launch_, &err)) {
        if (!useAppsink_) {
            emit logMessage(QStringLiteral("Stream window pipeline failed (%1); falling back to the in-app view.").arg(err));
            useAppsink_ = true;
            plan = planClientPipeline(cfg_, probe_, host, true);
            if (plan.launch.isEmpty()) {
                emit failed(err);
                return;
            }
            launch_ = plan.launch;
            emit logMessage(QStringLiteral("Pipeline: ") + launch_);
            err.clear();
            if (!pipe_->prepare(launch_, &err)) {
                emit failed(err);
                return;
            }
        } else {
            emit failed(err);
            return;
        }
    }
    // Do not pass the Qt widget handle to vulkansink/waylandsink. Those sinks
    // need their own toplevel; vulkanwindowxcb has no set_window_handle and
    // embedding into a Qt Wayland surface left a black screen in 0.2.18.
    auto applyMediaProps = [&]() {
        if (streamW_ > 0 && streamH_ > 0)
            pipe_->setActiveVideoSize(streamW_, streamH_);
        else
            pipe_->enableDecodeCrop();
        if (cfg_.transport == Transport::Srt) {
            QString uri = QStringLiteral("srt://%1:%2?mode=caller&latency=%3&transtype=live")
                              .arg(host)
                              .arg(cfg_.mediaPort)
                              .arg(cfg_.srtLatencyMs);
            const QString pass = srtPassphraseFromPin(cfg_.pin);
            if (!pass.isEmpty())
                uri += QStringLiteral("&passphrase=%1").arg(pass);
            pipe_->setString("src", "uri", uri);
            pipe_->setInt("src", "latency", cfg_.srtLatencyMs);
            pipe_->setInt("src", "mode", 1); // caller
        }
    };
    applyMediaProps();
    if (!pipe_->play(&err)) {
        if (!useAppsink_) {
            emit logMessage(QStringLiteral("Stream window failed (%1); falling back to the in-app view.").arg(err));
            useAppsink_ = true;
            plan = planClientPipeline(cfg_, probe_, host, true);
            if (plan.launch.isEmpty()) {
                emit failed(err);
                return;
            }
            launch_ = plan.launch;
            emit logMessage(QStringLiteral("Pipeline: ") + launch_);
            err.clear();
            if (!pipe_->prepare(launch_, &err)) {
                emit failed(err);
                return;
            }
            applyMediaProps();
            if (!pipe_->play(&err)) {
                emit failed(err);
                return;
            }
        } else {
            emit failed(err);
            return;
        }
    }

    streamFullscreen_ = !useAppsink_ && cfg_.fullscreen;
    if (streamFullscreen_)
        pipe_->setBool("sink", "fullscreen", true);

    connected_ = true;
    setStatus(QStringLiteral("Connected to %1  (%2)").arg(host, plan.description));
    emit connected();
}

bool ClientSession::setStreamFullscreen(bool on)
{
    if (!connected_ || useAppsink_ || !pipe_)
        return false;
    if (!pipe_->setBool("sink", "fullscreen", on))
        return false;
    streamFullscreen_ = on;
    return true;
}

void ClientSession::sendInput(const QJsonObject &ev)
{
    if (!connected_)
        return;
    QJsonObject o = ev;
    o.insert(QStringLiteral("type"), QStringLiteral("input"));
    control_->send(o);
}

} // namespace lanscreen
