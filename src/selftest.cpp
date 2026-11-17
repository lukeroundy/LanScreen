#include "selftest.h"

#include "common.h"
#include "pipeline.h"
#include "probe.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

namespace lanscreen {

static int countAppsinkFrames(GstElement *sink, int timeoutMs, int *got)
{
    *got = 0;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND);
        if (sample) {
            ++(*got);
            gst_sample_unref(sample);
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return *got;
}

int runSelfTest()
{
    std::fprintf(stderr, "LanScreen self-test\n");
    ProbeResult probe = probeCodecs();
    const QString report = formatProbeReport(probe);
    std::fprintf(stderr, "%s", report.toUtf8().constData());

    const CodecElement *enc = pickEncoder(probe, EncoderFamily::Software, Codec::H264);
    if (!enc) {
        std::fprintf(stderr, "FAIL: no software H.264 encoder (install gst-plugins-ugly for x264enc)\n");
        return 1;
    }
    const CodecElement *dec = pickDecoder(probe, DecoderFamily::Software, Codec::H264);
    if (!dec) {
        std::fprintf(stderr, "FAIL: no software H.264 decoder (install gst-libav)\n");
        return 1;
    }

    SessionConfig cfg;
    cfg.capture = CaptureSource::TestPattern;
    cfg.encoder = EncoderFamily::Software;
    cfg.decoder = DecoderFamily::Software;
    cfg.codec = Codec::H264;
    cfg.bitrateKbps = 2000;
    cfg.fps = 30;
    cfg.width = 1280;
    cfg.height = 720;
    cfg.keyint = 30;
    cfg.preset = Preset::UltraLowLatency;
    cfg.transport = Transport::Srt;
    cfg.srtLatencyMs = 80;
    cfg.mediaPort = 47551; // avoid colliding with a running session
    cfg.pin.clear();

    const PipelinePlan hostPlan = planHostPipeline(cfg, probe, QStringLiteral("127.0.0.1"), -1, {});
    const PipelinePlan clientPlan = planClientPipeline(cfg, probe, QStringLiteral("127.0.0.1"), true);
    if (hostPlan.launch.isEmpty() || clientPlan.launch.isEmpty()) {
        std::fprintf(stderr, "FAIL: could not build pipelines\n");
        return 1;
    }
    std::fprintf(stderr, "Host pipeline:\n  %s\n", hostPlan.launch.toUtf8().constData());
    std::fprintf(stderr, "Client pipeline:\n  %s\n", clientPlan.launch.toUtf8().constData());

    GstPipelineRunner host;
    GstPipelineRunner client;
    QString err;
    if (!host.prepare(hostPlan.launch, &err)) {
        std::fprintf(stderr, "FAIL: host prepare: %s\n", err.toUtf8().constData());
        return 1;
    }
    const QString uriListen =
        QStringLiteral("srt://127.0.0.1:%1?mode=listener&latency=%2&transtype=live")
            .arg(cfg.mediaPort)
            .arg(cfg.srtLatencyMs);
    const QString uriCall =
        QStringLiteral("srt://127.0.0.1:%1?mode=caller&latency=%2&transtype=live")
            .arg(cfg.mediaPort)
            .arg(cfg.srtLatencyMs);
    host.setString("sink", "uri", uriListen);
    host.setInt("sink", "mode", 2);
    host.setInt("sink", "latency", cfg.srtLatencyMs);
    host.setBool("sink", "wait-for-connection", false);
    if (!host.play(&err)) {
        std::fprintf(stderr, "FAIL: host play: %s\n", err.toUtf8().constData());
        return 1;
    }

    QObject::connect(&host, &GstPipelineRunner::errorMessage, [](const QString &m) {
        std::fprintf(stderr, "host gst: %s\n", m.toUtf8().constData());
    });
    QObject::connect(&client, &GstPipelineRunner::errorMessage, [](const QString &m) {
        std::fprintf(stderr, "client gst: %s\n", m.toUtf8().constData());
    });

    // Give the listener a moment to bind.
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 300)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    if (!client.prepare(clientPlan.launch, &err)) {
        std::fprintf(stderr, "FAIL: client prepare: %s\n", err.toUtf8().constData());
        host.stop();
        return 1;
    }
    client.setString("src", "uri", uriCall);
    client.setInt("src", "mode", 1);
    client.setInt("src", "latency", cfg.srtLatencyMs);
    if (!client.play(&err)) {
        std::fprintf(stderr, "FAIL: client play: %s\n", err.toUtf8().constData());
        host.stop();
        return 1;
    }

    GstElement *sink = client.element("sink");
    if (!sink) {
        std::fprintf(stderr, "FAIL: no appsink named sink\n");
        host.stop();
        client.stop();
        return 1;
    }
    int got = 0;
    countAppsinkFrames(sink, 4000, &got);
    gst_object_unref(sink);
    client.stop();
    host.stop();

    std::fprintf(stderr, "Received %d frames in 4 seconds.\n", got);
    if (got < 10) {
        std::fprintf(stderr, "FAIL: expected at least 10 decoded frames via the software path.\n");
        return 1;
    }
    std::fprintf(stderr, "OK: software encode/decode over SRT loopback works.\n");
    return 0;
}

} // namespace lanscreen
