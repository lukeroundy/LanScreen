#include "pipeline.h"

#include "netutil.h"

#include <QDebug>
#include <QImage>
#include <cstring>
#include <QRgb>
#include <QMetaObject>
#include <QTimer>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/videooverlay.h>

namespace lanscreen {

static int clampFps(int fps)
{
    if (fps < 1)
        return 30;
    if (fps > 240)
        return 240;
    return fps;
}

static int clampBitrate(int kbps)
{
    if (kbps < 100)
        return 100;
    if (kbps > 200000)
        return 200000;
    return kbps;
}

static QString escapeLaunch(const QString &s)
{
    // gst_parse_launch treats raw '&' as a pipeline branch, so keep uris out
    // of the launch string and set them as properties later.
    return s;
}

static QString encoderCapsHint(const CodecElement &enc)
{
    // videobox cannot handle NV12, so pad in a packed format first, then
    // convert. Feeding NV12 to alignbox left the pipeline not-linked and the
    // host reported "failed to start streaming."
    // Hardware encoders want NV12. videobox pads to a 32-pixel multiple with
    // black so VAAPI/NVENC don't leave a green strip.
    if (enc.family != QLatin1String("software"))
        return QStringLiteral(
            " ! videoconvert ! video/x-raw,format=BGRx ! videobox name=alignbox fill=black "
            "! videoconvert ! video/x-raw,format=NV12");
    return QStringLiteral(
        " ! videoconvert ! video/x-raw,format=BGRx ! videobox name=alignbox fill=black ! videoconvert");
}

static QString rateControlPropertyFor(const QString &element, RateControl rc, int cq)
{
    Q_UNUSED(cq);
    const QString e = element;
    if (e.contains(QLatin1String("x264"))) {
        if (rc == RateControl::Cqp)
            return QStringLiteral(" pass=quant");
        // x264enc's "pass=cbr" is the live mode we want for both CBR and VBR-ish.
        return QStringLiteral(" pass=cbr");
    }
    if (e.contains(QLatin1String("nv"))) {
        if (rc == RateControl::Cqp)
            return QStringLiteral(" rc-mode=constqp");
        if (rc == RateControl::Vbr)
            return QStringLiteral(" rc-mode=vbr");
        return QStringLiteral(" rc-mode=cbr");
    }
    if (e.startsWith(QLatin1String("va")) || e.contains(QLatin1String("vaapi"))) {
        if (rc == RateControl::Cqp)
            return QStringLiteral(" rate-control=cqp");
        if (rc == RateControl::Vbr)
            return QStringLiteral(" rate-control=vbr");
        return QStringLiteral(" rate-control=cbr");
    }
    return {};
}

static QString softwareOrHwPreset(const CodecElement &enc, Preset preset)
{
    if (enc.element.contains(QLatin1String("x264"))) {
        switch (preset) {
        case Preset::UltraLowLatency:
            return QStringLiteral(" tune=zerolatency sliced-threads=true speed-preset=ultrafast");
        case Preset::LowLatency:
            return QStringLiteral(" tune=zerolatency sliced-threads=true speed-preset=veryfast");
        case Preset::Quality:
            return QStringLiteral(" tune=zerolatency speed-preset=fast");
        }
    }
    if (enc.element.contains(QLatin1String("x265"))) {
        switch (preset) {
        case Preset::UltraLowLatency:
            return QStringLiteral(" tune=zerolatency speed-preset=ultrafast");
        case Preset::LowLatency:
            return QStringLiteral(" tune=zerolatency speed-preset=superfast");
        case Preset::Quality:
            return QStringLiteral(" tune=zerolatency speed-preset=fast");
        }
    }
    if (enc.element.contains(QLatin1String("svtav1"))) {
        switch (preset) {
        case Preset::UltraLowLatency:
            return QStringLiteral(" preset=12");
        case Preset::LowLatency:
            return QStringLiteral(" preset=10");
        case Preset::Quality:
            return QStringLiteral(" preset=8");
        }
    }
    if (enc.family == QLatin1String("nvenc")) {
        switch (preset) {
        case Preset::UltraLowLatency:
            return QStringLiteral(" preset=low-latency-hp zerolatency=true bframes=0");
        case Preset::LowLatency:
            return QStringLiteral(" preset=low-latency-hq zerolatency=true bframes=0");
        case Preset::Quality:
            return QStringLiteral(" preset=hq bframes=0");
        }
    }
    if (enc.family == QLatin1String("vaapi"))
        return {};
    return {};
}

static QString sourceLaunch(const SessionConfig &cfg, int pipewireFd, const QString &pipewirePath,
                            QStringList *warnings)
{
    const int fps = clampFps(cfg.fps);
    const int w = cfg.width > 0 ? cfg.width : 1920;
    const int h = cfg.height > 0 ? cfg.height : 1080;

    if (cfg.capture == CaptureSource::TestPattern || (!elementExists("ximagesrc") && !elementExists("pipewiresrc"))) {
        if (cfg.capture != CaptureSource::TestPattern && warnings)
            warnings->append(QStringLiteral("No capture source available, using a test pattern."));
        return QStringLiteral(
                   "videotestsrc name=src is-live=true pattern=smpte "
                   "! video/x-raw,width=%1,height=%2,framerate=%3/1")
            .arg(w)
            .arg(h)
            .arg(fps);
    }

    if ((cfg.capture == CaptureSource::Portal || (!qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty()
                                                  && cfg.capture != CaptureSource::Screen
                                                  && cfg.capture != CaptureSource::Output))
        && elementExists("pipewiresrc") && pipewireFd >= 0) {
        QString s = QStringLiteral("pipewiresrc name=src do-timestamp=true keepalive-time=%1 fd=%2").arg(1000 / fps).arg(pipewireFd);
        if (!pipewirePath.isEmpty())
            s += QStringLiteral(" path=%1").arg(pipewirePath);
        // KWin screencast offers framerate=0/1 (damage-driven). Forcing
        // video/x-raw,framerate=30/1 makes pipewiresrc advertise 30/1, then
        // the first 0/1 buffer arrives not-linked and the host fails to start.
        // keepalive-time repeats the last frame so a still display still
        // feeds the encoder; videorate only caps the maximum rate.
        s += QStringLiteral(" ! videorate max-rate=%1 drop-only=true").arg(fps);
        if (cfg.width > 0 && cfg.height > 0)
            s += QStringLiteral(" ! videoscale ! video/x-raw,width=%1,height=%2").arg(cfg.width).arg(cfg.height);
        return s;
    }

    if (elementExists("ximagesrc")) {
        QString s = QStringLiteral("ximagesrc name=src use-damage=false show-pointer=true");
        if (!qEnvironmentVariable("DISPLAY").isEmpty())
            s += QStringLiteral(" display-name=%1").arg(qEnvironmentVariable("DISPLAY"));
        if (cfg.cropW > 0 && cfg.cropH > 0) {
            s += QStringLiteral(" startx=%1 starty=%2 endx=%3 endy=%4")
                     .arg(cfg.cropX)
                     .arg(cfg.cropY)
                     .arg(cfg.cropX + cfg.cropW - 1)
                     .arg(cfg.cropY + cfg.cropH - 1);
        }
        s += QStringLiteral(" ! videorate max-rate=%1 drop-only=true").arg(fps);
        if (cfg.width > 0 && cfg.height > 0)
            s += QStringLiteral(" ! videoscale ! video/x-raw,width=%1,height=%2").arg(cfg.width).arg(cfg.height);
        return s;
    }

    if (elementExists("pipewiresrc") && pipewireFd >= 0) {
        QString s = QStringLiteral("pipewiresrc name=src do-timestamp=true keepalive-time=%1 fd=%2").arg(1000 / fps).arg(pipewireFd);
        if (!pipewirePath.isEmpty())
            s += QStringLiteral(" path=%1").arg(pipewirePath);
        s += QStringLiteral(" ! videorate max-rate=%1 drop-only=true").arg(fps);
        if (cfg.width > 0 && cfg.height > 0)
            s += QStringLiteral(" ! videoscale ! video/x-raw,width=%1,height=%2").arg(cfg.width).arg(cfg.height);
        return s;
    }

    if (warnings)
        warnings->append(QStringLiteral("Falling back to a test pattern (no ximagesrc / PipeWire fd)."));
    return QStringLiteral(
               "videotestsrc name=src is-live=true pattern=smpte "
               "! video/x-raw,width=%1,height=%2,framerate=%3/1")
        .arg(w)
        .arg(h)
        .arg(fps);
}

static QString encoderLaunch(const CodecElement &enc, const SessionConfig &cfg)
{
    const int kbps = clampBitrate(cfg.bitrateKbps);
    const int keyint = cfg.keyint > 0 ? cfg.keyint : cfg.fps;
    QString line = enc.element + QStringLiteral(" name=enc");
    line += softwareOrHwPreset(enc, cfg.preset);
    line += rateControlPropertyFor(enc.element, cfg.rateControl, cfg.cqLevel);

    if (enc.element.contains(QLatin1String("x264"))) {
        line += QStringLiteral(" bitrate=%1 key-int-max=%2 bframes=0 byte-stream=true").arg(kbps).arg(keyint);
        if (cfg.rateControl == RateControl::Cqp)
            line += QStringLiteral(" quantizer=%1").arg(cfg.cqLevel);
        int vbv = 300;
        if (cfg.preset == Preset::UltraLowLatency)
            vbv = 80;
        else if (cfg.preset == Preset::LowLatency)
            vbv = 120;
        line += QStringLiteral(" vbv-buf-capacity=%1").arg(vbv);
    } else if (enc.element.contains(QLatin1String("x265"))) {
        line += QStringLiteral(" bitrate=%1 key-int-max=%2").arg(kbps).arg(keyint);
        if (cfg.rateControl == RateControl::Cqp)
            line += QStringLiteral(" qp=%1").arg(cfg.cqLevel);
        line += QStringLiteral(" option-string=bframes=0:repeat-headers=1");
    } else if (enc.element.contains(QLatin1String("svtav1"))) {
        line += QStringLiteral(" target-bitrate=%1 intra-period-length=%2").arg(kbps).arg(keyint);
        if (cfg.rateControl == RateControl::Cqp)
            line += QStringLiteral(" cqp=%1").arg(cfg.cqLevel);
        if (cfg.rateControl == RateControl::Vbr)
            line += QStringLiteral(" max-bitrate=%1").arg(int(kbps * 1.4));
    } else if (enc.element.contains(QLatin1String("av1enc")) && enc.family == QLatin1String("software")) {
        line += QStringLiteral(" target-bitrate=%1").arg(kbps * 1000);
    } else if (enc.family == QLatin1String("nvenc")) {
        line += QStringLiteral(" bitrate=%1 gop-size=%2").arg(kbps).arg(keyint);
        if (cfg.rateControl == RateControl::Cqp)
            line += QStringLiteral(" qp-const=%1").arg(cfg.cqLevel);
    } else if (enc.family == QLatin1String("vaapi")) {
        // Current vah264enc (gst-va) and vaapih264enc both take kbps
        // (range 0-2048000). Sending bps made 8000 kbps look like 8000000
        // and GStreamer dropped the property as out of range.
        line += QStringLiteral(" bitrate=%1").arg(kbps);
        // keyframe property names vary; applyEncoderSettings fills gaps.
        line += QStringLiteral(" key-int-max=%1").arg(keyint);
        if (cfg.rateControl == RateControl::Cqp)
            line += QStringLiteral(" qpi=%1 qpp=%1 qpb=%1").arg(cfg.cqLevel);
    } else if (enc.family == QLatin1String("v4l2")) {
        // video_bitrate is bits/sec. video_bitrate_mode: 0=VBR, 1=CBR, 2=CQ.
        int mode = 1;
        if (cfg.rateControl == RateControl::Vbr)
            mode = 0;
        else if (cfg.rateControl == RateControl::Cqp)
            mode = 2;
        line += QStringLiteral(
                    " extra-controls=\"controls,video_bitrate=%1,video_bitrate_mode=%2,video_gop_size=%3\"")
                    .arg(kbps * 1000)
                    .arg(mode)
                    .arg(keyint);
    }
    return line;
}

static QString parserAndPay(Codec codec, Transport transport, bool fec, int fecPercent)
{
    QString parse;
    QString rtpPay;
    QString encoding;
    switch (codec) {
    case Codec::H264:
        parse = QStringLiteral("h264parse config-interval=-1");
        rtpPay = QStringLiteral("rtph264pay name=pay pt=96 config-interval=1");
        encoding = QStringLiteral("H264");
        break;
    case Codec::Hevc:
        parse = QStringLiteral("h265parse config-interval=-1");
        rtpPay = QStringLiteral("rtph265pay name=pay pt=96 config-interval=1");
        encoding = QStringLiteral("H265");
        break;
    case Codec::Av1:
        parse = QStringLiteral("av1parse");
        rtpPay = QStringLiteral("rtpav1pay name=pay pt=96");
        encoding = QStringLiteral("AV1");
        break;
    }

    if (transport == Transport::UdpRtp) {
        QString s = parse + QStringLiteral(" ! ") + rtpPay;
        if (fec && elementExists("rtpulpfecenc")) {
            s += QStringLiteral(" ! rtpulpfecenc percentage=%1 pt=122").arg(fecPercent);
        }
        s += QStringLiteral(" ! udpsink name=sink sync=false async=false");
        Q_UNUSED(encoding);
        return s;
    }

    // SRT and TCP: mux into a container. AV1 in MPEG-TS is unreliable, use Matroska.
    if (codec == Codec::Av1) {
        return parse + QStringLiteral(" ! matroskamux name=mux streamable=true ! ")
             + (transport == Transport::Tcp ? QStringLiteral("tcpserversink name=sink sync=false")
                                            : QStringLiteral("srtsink name=sink wait-for-connection=false"));
    }
    return parse + QStringLiteral(" ! mpegtsmux name=mux alignment=0 ! ")
         + (transport == Transport::Tcp ? QStringLiteral("tcpserversink name=sink sync=false")
                                        : QStringLiteral("srtsink name=sink wait-for-connection=false"));
}

PipelinePlan planHostPipeline(const SessionConfig &cfg, const ProbeResult &probe,
                              const QString &bindOrClientIp, int pipewireFd,
                              const QString &pipewirePath)
{
    PipelinePlan plan;
    const CodecElement *enc = pickEncoder(probe, cfg.encoder, cfg.codec);
    if (!enc) {
        plan.warnings.append(QStringLiteral("No encoder available for the requested codec/family."));
        // Last-ditch: try software H.264 even if the user asked for something else.
        enc = pickEncoder(probe, EncoderFamily::Software, Codec::H264);
        if (!enc)
            return plan;
        plan.warnings.append(QStringLiteral("Fell back to software H.264."));
    }

    SessionConfig local = cfg;
    if (enc->codec == QLatin1String("h264"))
        local.codec = Codec::H264;
    else if (enc->codec == QLatin1String("hevc"))
        local.codec = Codec::Hevc;
    else
        local.codec = Codec::Av1;

    plan.encoderElement = enc->element;
    const QString src = sourceLaunch(local, pipewireFd, pipewirePath, &plan.warnings);
    const QString mid = encoderCapsHint(*enc);
    const QString encLine = encoderLaunch(*enc, local);
    const QString pay = parserAndPay(local.codec, local.transport, local.fec, local.fecPercent);

    plan.launch = src + mid + QStringLiteral(" ! ") + encLine + QStringLiteral(" ! ") + pay;
    plan.description = QStringLiteral("%1  %2 @ %3 kbps  %4x%5@%6  %7")
                           .arg(enc->displayName, rateControlName(local.rateControl))
                           .arg(clampBitrate(local.bitrateKbps))
                           .arg(local.width > 0 ? local.width : 0)
                           .arg(local.height > 0 ? local.height : 0)
                           .arg(clampFps(local.fps))
                           .arg(transportName(local.transport));
    Q_UNUSED(bindOrClientIp);
    Q_UNUSED(escapeLaunch);
    return plan;
}

static QString pickSink(bool useAppsink, bool fullscreen)
{
    if (useAppsink)
        // A capsfilter in the chain, not caps= on appsink: commas in an unquoted
        // property value break gst_parse_launch and the sink ends up getting I420,
        // which we previously dropped — a black window.
        // Quote caps as a capsfilter property so commas aren't treated as
        // gst_parse_launch separators. Bare "caps" in the chain must stay unquoted;
        // quoted capsfilter *properties* are what the parser accepts.
        return QStringLiteral(
            "videocrop name=crop ! capsfilter caps=\"video/x-raw,format=BGRx\" ! appsink name=sink emit-signals=false sync=false max-buffers=2 drop=true");
    // Prefer a GStreamer-owned toplevel (not a Qt wl_subsurface — that crashes).
    // waylandsink first: vulkansink on this laptop either tried to embed into the
    // Qt window (black) or, with GST_VULKAN_WINDOW=wayland, never mapped a window.
    if (elementExists("waylandsink") && !qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty()) {
        QString s = QStringLiteral("videocrop name=crop ! waylandsink name=sink sync=false qos=false");
        // Exclusive xdg-shell fullscreen hides KDE panels and the LanScreen UI.
        if (fullscreen)
            s += QStringLiteral(" fullscreen=true");
        return s;
    }
    if (elementExists("vulkansink") && elementExists("vulkanupload")) {
        QString s = QStringLiteral("videocrop name=crop ! vulkanupload");
        if (elementExists("vulkancolorconvert"))
            s += QStringLiteral(" ! vulkancolorconvert");
        s += QStringLiteral(" ! vulkansink name=sink sync=false qos=false force-aspect-ratio=true");
        return s;
    }
    if (elementExists("glimagesink"))
        return QStringLiteral("videocrop name=crop ! glimagesink name=sink sync=false qos=false force-aspect-ratio=true");
    if (elementExists("ximagesink"))
        return QStringLiteral("videocrop name=crop ! ximagesink name=sink sync=false force-aspect-ratio=true");
    return QStringLiteral("videocrop name=crop ! autovideosink name=sink sync=false");
}

PipelinePlan planClientPipeline(const SessionConfig &cfg, const ProbeResult &probe,
                                const QString &hostIp, bool useAppsink)
{
    PipelinePlan plan;
    const CodecElement *dec = pickDecoder(probe, cfg.decoder, cfg.codec);
    if (!dec) {
        plan.warnings.append(QStringLiteral("No decoder available for the requested codec."));
        dec = pickDecoder(probe, DecoderFamily::Software, Codec::H264);
        if (!dec)
            return plan;
    }
    plan.decoderElement = dec->element;
    QString decChain = dec->element;
    if (dec->family == QLatin1String("software") && dec->element.startsWith(QLatin1String("avdec_")))
        decChain += QStringLiteral(" thread-type=2 max-threads=4 qos=false");
    if (dec->family == QLatin1String("v4l2")) {
        decChain += QStringLiteral(" qos=false");
        if (useAppsink) {
            // mmap / system-memory output so videoconvert can feed the CPU appsink.
            // DMA-buf frames previously showed up black in QImage.
            decChain += QStringLiteral(" capture-io-mode=mmap");
            if (elementExists("v4l2convert"))
                decChain += QStringLiteral(" ! v4l2convert");
            decChain += QStringLiteral(" ! capsfilter caps=\"video/x-raw\"");
        }
    }
    const QString sink = pickSink(useAppsink, cfg.fullscreen);

    QString parse;
    QString rtpDepay;
    switch (cfg.codec) {
    case Codec::H264:
        parse = QStringLiteral("h264parse config-interval=-1");
        if (dec->family == QLatin1String("v4l2"))
            parse += QStringLiteral(" ! capsfilter caps=\"video/x-h264,stream-format=byte-stream,alignment=au\"");
        rtpDepay = QStringLiteral("rtph264depay");
        break;
    case Codec::Hevc:
        parse = QStringLiteral("h265parse config-interval=-1");
        if (dec->family == QLatin1String("v4l2"))
            parse += QStringLiteral(" ! capsfilter caps=\"video/x-h265,stream-format=byte-stream,alignment=au\"");
        rtpDepay = QStringLiteral("rtph265depay");
        break;
    case Codec::Av1:
        parse = QStringLiteral("av1parse");
        rtpDepay = QStringLiteral("rtpav1depay");
        break;
    }

    if (cfg.transport == Transport::UdpRtp) {
        QString caps;
        if (cfg.codec == Codec::H264)
            caps = QStringLiteral("application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96");
        else if (cfg.codec == Codec::Hevc)
            caps = QStringLiteral("application/x-rtp,media=video,clock-rate=90000,encoding-name=H265,payload=96");
        else
            caps = QStringLiteral("application/x-rtp,media=video,clock-rate=90000,encoding-name=AV1,payload=96");
        plan.launch = QStringLiteral(
                          "udpsrc name=src port=%1 caps=\"%2\" ! rtpjitterbuffer latency=%3 ! %4 ! %5 ! %6 ! videoconvert n-threads=4 ! %7")
                          .arg(cfg.mediaPort)
                          .arg(caps)
                          .arg(qMax(20, cfg.srtLatencyMs))
                          .arg(rtpDepay, parse, decChain, sink);
    } else if (cfg.transport == Transport::Tcp) {
        const QString demux = (cfg.codec == Codec::Av1) ? QStringLiteral("matroskademux")
                                                        : QStringLiteral("tsdemux");
        plan.launch = QStringLiteral(
                          "tcpclientsrc name=src host=%1 port=%2 ! queue ! %3 ! %4 ! %5 ! videoconvert n-threads=4 ! %6")
                          .arg(hostIp)
                          .arg(cfg.mediaPort)
                          .arg(demux, parse, decChain, sink);
    } else {
        const QString demux = (cfg.codec == Codec::Av1) ? QStringLiteral("matroskademux")
                                                        : QStringLiteral("tsdemux");
        plan.launch = QStringLiteral("srtsrc name=src ! queue ! %1 ! %2 ! %3 ! videoconvert n-threads=4 ! %4")
                          .arg(demux, parse, decChain, sink);
    }
    plan.description = QStringLiteral("%1  %2  from %3")
                           .arg(dec->displayName, transportName(cfg.transport), hostIp);
    return plan;
}

static bool elementHasProperty(GstElement *el, const char *name)
{
    if (!el)
        return false;
    GObjectClass *klass = G_OBJECT_GET_CLASS(el);
    return g_object_class_find_property(klass, name) != nullptr;
}

void applyEncoderSettings(GstPipelineRunner &runner, const CodecElement &enc, const SessionConfig &cfg)
{
    GstElement *el = runner.element("enc");
    if (!el)
        return;
    auto setIntIf = [&](const char *name, int value) {
        if (elementHasProperty(el, name))
            runner.setInt("enc", name, value);
    };
    auto setBoolIf = [&](const char *name, bool value) {
        if (elementHasProperty(el, name))
            runner.setBool("enc", name, value);
    };
    if (enc.family == QLatin1String("vaapi")) {
        const bool ultra = cfg.preset == Preset::UltraLowLatency;
        const bool low = cfg.preset == Preset::LowLatency;
        setIntIf("b-frames", 0);
        setIntIf("bframes", 0);
        setIntIf("max-bframes", 0);
        setBoolIf("aud", true);
        if (ultra || low) {
            setIntIf("target-usage", ultra ? 7 : 6);
            setIntIf("quality-level", 1); // gstreamer-vaapi: 1 is fastest
            setIntIf("ref-frames", 1);
            setIntIf("refs", 1);
            setIntIf("num-slices", 4);
            const int cpb = ultra ? 250 : 400;
            setIntIf("cpb-size", cpb);
        }
    }
    gst_object_unref(el);
}

GstPipelineRunner::GstPipelineRunner(QObject *parent)
    : QObject(parent)
{
    timer_ = new QTimer(this);
    timer_->setInterval(50);
    connect(timer_, &QTimer::timeout, this, &GstPipelineRunner::pollBus);
}

GstPipelineRunner::~GstPipelineRunner()
{
    stop();
}

bool GstPipelineRunner::prepare(const QString &launch, QString *error)
{
    stop();
    GError *gerr = nullptr;
    pipeline_ = gst_parse_launch(launch.toUtf8().constData(), &gerr);
    if (!pipeline_) {
        const QString msg = gerr ? QString::fromUtf8(gerr->message) : QStringLiteral("gst_parse_launch failed");
        if (gerr)
            g_error_free(gerr);
        if (error)
            *error = msg + QStringLiteral("\nPipeline: ") + launch;
        return false;
    }
    if (gerr) {
        const QString msg = QString::fromUtf8(gerr->message);
        const bool fatal = msg.contains(QLatin1String("syntax"), Qt::CaseInsensitive)
            || msg.contains(QLatin1String("could not link"), Qt::CaseInsensitive)
            || msg.contains(QLatin1String("no element"), Qt::CaseInsensitive)
            || msg.contains(QLatin1String("can't handle caps"), Qt::CaseInsensitive)
            || msg.contains(QLatin1String("cannot handle caps"), Qt::CaseInsensitive);
        g_error_free(gerr);
        if (fatal) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            if (error)
                *error = msg + QStringLiteral("\nPipeline: ") + launch;
            return false;
        }
        emit infoMessage(msg);
    }
    // Stay in NULL so callers can set SRT uri / PipeWire fd before the socket
    // or device is opened. play() takes the pipeline to PLAYING.
    running_ = true;
    loggedFirstFrame_ = false;
    attachAppsink();
    timer_->start();
    emit stateChanged(QStringLiteral("ready"));
    return true;
}

bool GstPipelineRunner::play(QString *error)
{
    if (!pipeline_) {
        if (error)
            *error = QStringLiteral("No pipeline to play");
        return false;
    }
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        if (error)
            *error = QStringLiteral("Pipeline failed to start (set_state PLAYING).");
        return false;
    }
    timer_->setInterval(8);
    timer_->start();
    emit stateChanged(QStringLiteral("playing"));
    return true;
}

bool GstPipelineRunner::start(const QString &launch, QString *error)
{
    if (!prepare(launch, error))
        return false;
    if (!play(error)) {
        stop();
        return false;
    }
    return true;
}

void GstPipelineRunner::stop()
{
    timer_->stop();
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    if (running_) {
        running_ = false;
        emit stateChanged(QStringLiteral("stopped"));
    }
}

GstElement *GstPipelineRunner::element(const char *name) const
{
    if (!pipeline_ || !GST_IS_BIN(pipeline_))
        return nullptr;
    return gst_bin_get_by_name(GST_BIN(pipeline_), name);
}

bool GstPipelineRunner::setString(const char *elem, const char *prop, const QString &value)
{
    GstElement *el = element(elem);
    if (!el)
        return false;
    if (!elementHasProperty(el, prop)) {
        gst_object_unref(el);
        return false;
    }
    g_object_set(el, prop, value.toUtf8().constData(), nullptr);
    gst_object_unref(el);
    return true;
}

bool GstPipelineRunner::setInt(const char *elem, const char *prop, int value)
{
    GstElement *el = element(elem);
    if (!el)
        return false;
    if (!elementHasProperty(el, prop)) {
        gst_object_unref(el);
        return false;
    }
    g_object_set(el, prop, value, nullptr);
    gst_object_unref(el);
    return true;
}

bool GstPipelineRunner::setBool(const char *elem, const char *prop, bool value)
{
    GstElement *el = element(elem);
    if (!el)
        return false;
    if (!elementHasProperty(el, prop)) {
        gst_object_unref(el);
        return false;
    }
    g_object_set(el, prop, value ? TRUE : FALSE, nullptr);
    gst_object_unref(el);
    return true;
}

void GstPipelineRunner::setOverlayHandle(quintptr wid)
{
    GstElement *sink = element("sink");
    if (!sink)
        return;
    if (GST_IS_VIDEO_OVERLAY(sink)) {
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), static_cast<guintptr>(wid));
    } else {
        // autovideosink / glimagesink may be a bin
        GstIterator *it = gst_bin_iterate_recurse(GST_BIN(pipeline_));
        GValue item = G_VALUE_INIT;
        while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
            GstElement *e = GST_ELEMENT(g_value_get_object(&item));
            if (e && GST_IS_VIDEO_OVERLAY(e)) {
                gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(e), static_cast<guintptr>(wid));
            }
            g_value_reset(&item);
        }
        g_value_unset(&item);
        gst_iterator_free(it);
    }
    gst_object_unref(sink);
}

void GstPipelineRunner::setActiveVideoSize(int width, int height)
{
    activeW_ = width;
    activeH_ = height;
    if (pipeline_)
        enableDecodeCrop();
}

void GstPipelineRunner::alignCapsThunk(void *pad, void *, void *self)
{
    static_cast<GstPipelineRunner *>(self)->onAlignCaps(pad);
}

void GstPipelineRunner::cropCapsThunk(void *pad, void *, void *self)
{
    static_cast<GstPipelineRunner *>(self)->onCropCaps(pad);
}

static bool capsSize(void *rawPad, int *w, int *h)
{
    GstPad *pad = static_cast<GstPad *>(rawPad);
    if (!pad)
        return false;
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps)
        return false;
    bool ok = false;
    if (gst_caps_get_size(caps) > 0) {
        GstStructure *st = gst_caps_get_structure(caps, 0);
        ok = gst_structure_get_int(st, "width", w) && gst_structure_get_int(st, "height", h);
    }
    gst_caps_unref(caps);
    return ok && *w > 0 && *h > 0;
}

void GstPipelineRunner::onAlignCaps(void *rawPad)
{
    if (!pipeline_ || alignMultiple_ < 2)
        return;
    int w = 0, h = 0;
    if (!capsSize(rawPad, &w, &h))
        return;
    const int m = alignMultiple_;
    const int aw = (w + m - 1) / m * m;
    const int ah = (h + m - 1) / m * m;
    // videobox: negative bottom/right *adds* a border.
    setInt("alignbox", "left", 0);
    setInt("alignbox", "top", 0);
    setInt("alignbox", "right", -(aw - w));
    setInt("alignbox", "bottom", -(ah - h));
    emit pictureSize(w, h);
}

void GstPipelineRunner::onCropCaps(void *rawPad)
{
    if (!pipeline_)
        return;
    int w = 0, h = 0;
    if (!capsSize(rawPad, &w, &h))
        return;
    int right = 0, bottom = 0;
    if (activeW_ > 0 && activeW_ < w)
        right = w - activeW_;
    if (activeH_ > 0 && activeH_ < h)
        bottom = h - activeH_;
    setInt("crop", "left", 0);
    setInt("crop", "top", 0);
    setInt("crop", "right", right);
    setInt("crop", "bottom", bottom);
}

void GstPipelineRunner::enableEncodeAlign(int multiple)
{
    alignMultiple_ = multiple;
    GstElement *box = element("alignbox");
    if (!box)
        return;
    GstPad *sink = gst_element_get_static_pad(box, "sink");
    if (sink) {
        g_signal_connect(sink, "notify::caps", G_CALLBACK(alignCapsThunk), this);
        onAlignCaps(sink);
        gst_object_unref(sink);
    }
    gst_object_unref(box);
}

void GstPipelineRunner::enableDecodeCrop()
{
    GstElement *crop = element("crop");
    if (!crop)
        return;
    GstPad *sink = gst_element_get_static_pad(crop, "sink");
    if (sink) {
        g_signal_connect(sink, "notify::caps", G_CALLBACK(cropCapsThunk), this);
        onCropCaps(sink);
        gst_object_unref(sink);
    }
    gst_object_unref(crop);
}

QImage GstPipelineRunner::imageFromSample(void *rawSample)
{
    GstSample *sample = static_cast<GstSample *>(rawSample);
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstVideoInfo vinfo;
    if (!caps || !buf || !gst_video_info_from_caps(&vinfo, caps))
        return {};

    const GstVideoFormat fmt0 = GST_VIDEO_INFO_FORMAT(&vinfo);
    const bool packedRgb = (fmt0 == GST_VIDEO_FORMAT_BGRx || fmt0 == GST_VIDEO_FORMAT_BGRA
                            || fmt0 == GST_VIDEO_FORMAT_RGBx || fmt0 == GST_VIDEO_FORMAT_RGBA
                            || fmt0 == GST_VIDEO_FORMAT_RGB);
    GstSample *converted = nullptr;
    if (!packedRgb) {
        GstCaps *to = gst_caps_from_string("video/x-raw,format=BGRx");
        GError *gerr = nullptr;
        converted = gst_video_convert_sample(sample, to, GST_SECOND, &gerr);
        gst_caps_unref(to);
        if (!converted) {
            if (!loggedFirstFrame_) {
                loggedFirstFrame_ = true;
                const QString why = gerr ? QString::fromUtf8(gerr->message)
                                         : QStringLiteral("gst_video_convert_sample failed");
                emit infoMessage(QStringLiteral("Could not convert video frame (%1, %2x%3): %4")
                                     .arg(QString::fromUtf8(gst_video_format_to_string(fmt0)))
                                     .arg(GST_VIDEO_INFO_WIDTH(&vinfo))
                                     .arg(GST_VIDEO_INFO_HEIGHT(&vinfo))
                                     .arg(why));
            }
            if (gerr)
                g_error_free(gerr);
            return {};
        }
        caps = gst_sample_get_caps(converted);
        buf = gst_sample_get_buffer(converted);
        if (!caps || !buf || !gst_video_info_from_caps(&vinfo, caps)) {
            gst_sample_unref(converted);
            return {};
        }
        sample = converted;
    }

    GstVideoFrame vframe;
    if (!gst_video_frame_map(&vframe, &vinfo, buf, GST_MAP_READ)) {
        if (converted)
            gst_sample_unref(converted);
        return {};
    }

    const int w = GST_VIDEO_FRAME_WIDTH(&vframe);
    const int h = GST_VIDEO_FRAME_HEIGHT(&vframe);
    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 0);
    const guint8 *data = static_cast<const guint8 *>(GST_VIDEO_FRAME_PLANE_DATA(&vframe, 0));
    int srcX = 0;
    int srcY = 0;
    int outW = w;
    int outH = h;
    if (GstVideoCropMeta *crop = gst_buffer_get_video_crop_meta(buf)) {
        if (crop->width > 0 && crop->height > 0
            && crop->x >= 0 && crop->y >= 0
            && crop->x + crop->width <= w && crop->y + crop->height <= h) {
            srcX = static_cast<int>(crop->x);
            srcY = static_cast<int>(crop->y);
            outW = static_cast<int>(crop->width);
            outH = static_cast<int>(crop->height);
        }
    }
    // Padding is typically on the bottom/right; keep the top-left picture.
    if (activeW_ > 0 && activeW_ < outW)
        outW = activeW_;
    if (activeH_ > 0 && activeH_ < outH)
        outH = activeH_;
    QImage img;
    if (outW > 0 && outH > 0 && stride > 0 && data) {
        const GstVideoFormat fmt = GST_VIDEO_INFO_FORMAT(&vinfo);
        if (fmt == GST_VIDEO_FORMAT_BGRx || fmt == GST_VIDEO_FORMAT_BGRA) {
            // BGRx is already B,G,R,X in memory, same as Format_RGB32 on
            // little-endian. Only force the unused byte to 0xFF so Wayland
            // doesn't treat a 0 alpha as a black/transparent frame.
            img = QImage(outW, outH, QImage::Format_RGB32);
            for (int y = 0; y < outH; ++y) {
                const auto *src = reinterpret_cast<const quint32 *>(
                    data + (srcY + y) * stride + srcX * 4);
                auto *dst = reinterpret_cast<quint32 *>(img.scanLine(y));
                std::memcpy(dst, src, static_cast<size_t>(outW) * 4);
                for (int x = 0; x < outW; ++x)
                    dst[x] |= 0xff000000u;
            }
        } else if (fmt == GST_VIDEO_FORMAT_RGB) {
            img = QImage(data + srcY * stride + srcX * 3, outW, outH, stride, QImage::Format_RGB888).copy();
        } else if (fmt == GST_VIDEO_FORMAT_RGBx) {
            img = QImage(data + srcY * stride + srcX * 4, outW, outH, stride, QImage::Format_RGBX8888).copy();
            img.convertTo(QImage::Format_RGB32);
        } else if (fmt == GST_VIDEO_FORMAT_RGBA) {
            img = QImage(data + srcY * stride + srcX * 4, outW, outH, stride, QImage::Format_RGBA8888).copy();
            img.convertTo(QImage::Format_RGB32);
        }
    }
    gst_video_frame_unmap(&vframe);
    if (converted)
        gst_sample_unref(converted);

    if (!img.isNull()) {
        const int iw = img.width();
        const int ih = img.height();
        auto isPadRow = [&](int y) {
            const auto *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            const int step = qMax(1, iw / 64);
            int pad = 0, n = 0;
            for (int x = 0; x < iw; x += step) {
                const int r = qRed(row[x]);
                const int g = qGreen(row[x]);
                const int b = qBlue(row[x]);
                const bool blackish = r < 24 && g < 24 && b < 24;
                const bool greenish = r < 40 && b < 40 && g > 80 && g < 200;
                if (blackish || greenish)
                    ++pad;
                ++n;
            }
            return n > 0 && pad * 4 >= n * 3;
        };
        int cut = 0;
        const int maxCut = qMin(32, ih / 4);
        for (int y = ih - 1; y >= ih - maxCut; --y) {
            if (!isPadRow(y))
                break;
            ++cut;
        }
        if (cut >= 2)
            img = img.copy(0, 0, iw, ih - cut);
    }

    if (!loggedFirstFrame_) {
        loggedFirstFrame_ = true;
        emit infoMessage(QStringLiteral("First frame: %1 %2x%3%4")
                             .arg(QString::fromUtf8(gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&vinfo))))
                             .arg(outW)
                             .arg(outH)
                             .arg(img.isNull() ? QStringLiteral(" (could not copy)") : QString()));
    }
    return img;
}

void GstPipelineRunner::pullPendingFrames()
{
    GstElement *sink = element("sink");
    if (!sink)
        return;
    if (!GST_IS_APP_SINK(sink)) {
        gst_object_unref(sink);
        return;
    }
    QImage last;
    for (int i = 0; i < 8; ++i) {
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 0);
        if (!sample)
            break;
        QImage img = imageFromSample(sample);
        gst_sample_unref(sample);
        if (!img.isNull())
            last = img;
    }
    gst_object_unref(sink);
    if (!last.isNull())
        emit frameReady(last);
}

void GstPipelineRunner::attachAppsink()
{
    GstElement *sink = element("sink");
    if (!sink)
        return;
    if (!GST_IS_APP_SINK(sink)) {
        gst_object_unref(sink);
        return;
    }
    gst_app_sink_set_emit_signals(GST_APP_SINK(sink), FALSE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(sink), 2);
    gst_app_sink_set_drop(GST_APP_SINK(sink), TRUE);
    GstCaps *caps = gst_caps_from_string("video/x-raw,format=BGRx");
    if (caps) {
        gst_app_sink_set_caps(GST_APP_SINK(sink), caps);
        gst_caps_unref(caps);
    }
    // Samples are pulled from the Qt timer on the UI thread.
    gst_object_unref(sink);
}

void GstPipelineRunner::pollBus()
{
    if (!pipeline_)
        return;
    pullPendingFrames();
    GstBus *bus = gst_element_get_bus(pipeline_);
    if (!bus)
        return;
    for (;;) {
        GstMessage *msg = gst_bus_pop_filtered(
            bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS
                                             | GST_MESSAGE_STATE_CHANGED | GST_MESSAGE_INFO));
        if (!msg)
            break;
        handleMessage(msg);
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

void GstPipelineRunner::handleMessage(void *raw)
{
    GstMessage *msg = static_cast<GstMessage *>(raw);
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *gerr = nullptr;
        gchar *dbg = nullptr;
        gst_message_parse_error(msg, &gerr, &dbg);
        QString m = gerr ? QString::fromUtf8(gerr->message) : QStringLiteral("GStreamer error");
        if (dbg)
            m += QStringLiteral("\n") + QString::fromUtf8(dbg);
        if (gerr)
            g_error_free(gerr);
        g_free(dbg);
        qWarning().noquote() << "GStreamer:" << m;
        emit errorMessage(m);
        break;
    }
    case GST_MESSAGE_WARNING: {
        GError *gerr = nullptr;
        gchar *dbg = nullptr;
        gst_message_parse_warning(msg, &gerr, &dbg);
        QString m = gerr ? QString::fromUtf8(gerr->message) : QStringLiteral("GStreamer warning");
        if (gerr)
            g_error_free(gerr);
        g_free(dbg);
        qWarning().noquote() << "GStreamer:" << m;
        emit infoMessage(m);
        break;
    }
    case GST_MESSAGE_EOS:
        emit infoMessage(QStringLiteral("End of stream"));
        break;
    default:
        break;
    }
}

} // namespace lanscreen
