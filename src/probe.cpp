#include "probe.h"

#include <gst/gst.h>

namespace lanscreen {

bool elementExists(const char *name)
{
    GstElementFactory *f = gst_element_factory_find(name);
    if (!f)
        return false;
    gst_object_unref(f);
    return true;
}

bool elementReachesReady(const char *name, QString *detail)
{
    GstElement *el = gst_element_factory_make(name, nullptr);
    if (!el) {
        if (detail)
            *detail = QStringLiteral("factory not present (no plugin or no hardware)");
        return false;
    }
    GstStateChangeReturn ret = gst_element_set_state(el, GST_STATE_READY);
    const bool ok = (ret != GST_STATE_CHANGE_FAILURE);
    if (!ok && detail) {
        *detail = QStringLiteral("element failed to reach READY (missing device or driver?)");
    } else if (ok && detail) {
        *detail = QStringLiteral("ready");
    }
    gst_element_set_state(el, GST_STATE_NULL);
    gst_object_unref(el);
    return ok;
}

static CodecElement makeEl(const char *element, const char *family, const char *codec,
                           const char *display)
{
    CodecElement e;
    e.element = QString::fromLatin1(element);
    e.family = QString::fromLatin1(family);
    e.codec = QString::fromLatin1(codec);
    e.displayName = QString::fromLatin1(display);
    return e;
}

ProbeResult probeCodecs()
{
    ProbeResult r;

    // Candidate encoders. Multiple names per family because plugin versions differ.
    const CodecElement encCandidates[] = {
        makeEl("nvh264enc", "nvenc", "h264", "NVIDIA NVENC H.264"),
        makeEl("nvcudah264enc", "nvenc", "h264", "NVIDIA CUDA NVENC H.264"),
        makeEl("nvv4l2h264enc", "nvenc", "h264", "NVIDIA V4L2 NVENC H.264 (Jetson)"),
        makeEl("nvh265enc", "nvenc", "hevc", "NVIDIA NVENC HEVC"),
        makeEl("nvcudah265enc", "nvenc", "hevc", "NVIDIA CUDA NVENC HEVC"),
        makeEl("nvv4l2h265enc", "nvenc", "hevc", "NVIDIA V4L2 NVENC HEVC (Jetson)"),
        makeEl("nvav1enc", "nvenc", "av1", "NVIDIA NVENC AV1"),

        makeEl("vah264enc", "vaapi", "h264", "VA-API H.264 (gst-va)"),
        makeEl("vaapih264enc", "vaapi", "h264", "VA-API H.264 (gstreamer-vaapi)"),
        makeEl("vah265enc", "vaapi", "hevc", "VA-API HEVC (gst-va)"),
        makeEl("vaapih265enc", "vaapi", "hevc", "VA-API HEVC (gstreamer-vaapi)"),
        makeEl("vaav1enc", "vaapi", "av1", "VA-API AV1 (gst-va)"),
        makeEl("vaapiav1enc", "vaapi", "av1", "VA-API AV1 (gstreamer-vaapi)"),

        makeEl("v4l2h264enc", "v4l2", "h264", "V4L2 H.264 (Venus / Adreno)"),
        makeEl("v4l2h265enc", "v4l2", "hevc", "V4L2 HEVC (Venus / Adreno)"),

        makeEl("x264enc", "software", "h264", "x264 (software)"),
        makeEl("openh264enc", "software", "h264", "OpenH264 (software)"),
        makeEl("x265enc", "software", "hevc", "x265 (software)"),
        makeEl("svtav1enc", "software", "av1", "SVT-AV1 (software)"),
        makeEl("av1enc", "software", "av1", "libaom AV1 (software)"),
    };

    const CodecElement decCandidates[] = {
        makeEl("nvh264dec", "nvenc", "h264", "NVIDIA NVDEC H.264"),
        makeEl("nvh265dec", "nvenc", "hevc", "NVIDIA NVDEC HEVC"),
        makeEl("nvav1dec", "nvenc", "av1", "NVIDIA NVDEC AV1"),
        makeEl("vah264dec", "vaapi", "h264", "VA-API H.264 decoder (gst-va)"),
        makeEl("vaapih264dec", "vaapi", "h264", "VA-API H.264 decoder"),
        makeEl("vah265dec", "vaapi", "hevc", "VA-API HEVC decoder (gst-va)"),
        makeEl("vaapih265dec", "vaapi", "hevc", "VA-API HEVC decoder"),
        makeEl("vaav1dec", "vaapi", "av1", "VA-API AV1 decoder"),
        makeEl("v4l2h264dec", "v4l2", "h264", "V4L2 H.264 decoder"),
        makeEl("v4l2h265dec", "v4l2", "hevc", "V4L2 HEVC decoder"),
        makeEl("avdec_h264", "software", "h264", "libav H.264 decoder"),
        makeEl("openh264dec", "software", "h264", "OpenH264 decoder"),
        makeEl("avdec_h265", "software", "hevc", "libav HEVC decoder"),
        makeEl("dav1ddec", "software", "av1", "dav1d AV1 decoder"),
        makeEl("avdec_av1", "software", "av1", "libav AV1 decoder"),
        makeEl("av1dec", "software", "av1", "libaom AV1 decoder"),
    };

    auto probeList = [](const CodecElement *cands, size_t n, QList<CodecElement> *out) {
        for (size_t i = 0; i < n; ++i) {
            CodecElement e = cands[i];
            if (e.family == QLatin1String("software")) {
                // Factory presence is enough. Putting svtav1enc into READY prints a banner.
                if (elementExists(e.element.toLatin1().constData())) {
                    e.ready = true;
                    e.detail = QStringLiteral("ready");
                } else {
                    e.ready = false;
                    e.detail = QStringLiteral("factory not present (no plugin or no hardware)");
                }
            } else {
                e.ready = elementReachesReady(e.element.toLatin1().constData(), &e.detail);
            }
            out->append(e);
        }
    };

    probeList(encCandidates, sizeof(encCandidates) / sizeof(encCandidates[0]), &r.encoders);
    probeList(decCandidates, sizeof(decCandidates) / sizeof(decCandidates[0]), &r.decoders);

    if (!elementExists("srtsink") || !elementExists("srtsrc"))
        r.notes.append(QStringLiteral("SRT elements missing (need gst-plugins-bad / gst-plugin-rsrt). UDP and TCP still work."));
    if (!elementExists("ximagesrc"))
        r.notes.append(QStringLiteral("ximagesrc missing — X11 capture unavailable."));
    if (!elementExists("pipewiresrc"))
        r.notes.append(QStringLiteral("pipewiresrc missing — Wayland portal capture unavailable."));
    if (!elementExists("x264enc"))
        r.notes.append(QStringLiteral("x264enc missing — install gst-plugins-ugly for the software H.264 fallback."));

    return r;
}

const CodecElement *pickEncoder(const ProbeResult &probe, EncoderFamily family, Codec codec)
{
    const QString wantCodec = codecName(codec);
    auto tryFamily = [&](const char *fam) -> const CodecElement * {
        for (const CodecElement &e : probe.encoders) {
            if (e.ready && e.family == QLatin1String(fam) && e.codec == wantCodec)
                return &e;
        }
        return nullptr;
    };

    if (family == EncoderFamily::Nvenc)
        return tryFamily("nvenc");
    if (family == EncoderFamily::Vaapi)
        return tryFamily("vaapi");
    if (family == EncoderFamily::V4l2)
        return tryFamily("v4l2");
    if (family == EncoderFamily::Software)
        return tryFamily("software");

    // Auto: hardware first, in a sensible order, then software.
    if (const CodecElement *e = tryFamily("nvenc"))
        return e;
    if (const CodecElement *e = tryFamily("vaapi"))
        return e;
    if (const CodecElement *e = tryFamily("v4l2"))
        return e;
    return tryFamily("software");
}

const CodecElement *pickDecoder(const ProbeResult &probe, DecoderFamily family, Codec codec)
{
    const QString wantCodec = codecName(codec);
    auto tryFamily = [&](const char *fam) -> const CodecElement * {
        for (const CodecElement &e : probe.decoders) {
            if (e.ready && e.family == QLatin1String(fam) && e.codec == wantCodec)
                return &e;
        }
        return nullptr;
    };
    auto tryAnyHw = [&]() -> const CodecElement * {
        if (const CodecElement *e = tryFamily("nvenc"))
            return e;
        if (const CodecElement *e = tryFamily("vaapi"))
            return e;
        if (const CodecElement *e = tryFamily("v4l2"))
            return e;
        return nullptr;
    };

    if (family == DecoderFamily::Hardware)
        return tryAnyHw();
    if (family == DecoderFamily::Software)
        return tryFamily("software");
    // Auto: prefer NVDEC/VAAPI, then software. Do not auto-pick V4L2/Iris;
    // live SRT streams come out green with "Too old frames, bug in decoder."
    if (const CodecElement *e = tryFamily("nvenc"))
        return e;
    if (const CodecElement *e = tryFamily("vaapi"))
        return e;
    if (const CodecElement *e = tryFamily("software"))
        return e;
    return tryFamily("v4l2");
}

QString formatProbeReport(const ProbeResult &probe)
{
    QString out;
    out += QStringLiteral("Encoders\n========\n");
    for (const CodecElement &e : probe.encoders) {
        out += QStringLiteral("  [%1] %-16s  %2  (%3/%4)  %5\n")
                   .arg(e.ready ? QStringLiteral("ready") : QStringLiteral("  -- "))
                   .arg(e.displayName, e.family, e.codec, e.detail);
        // The %-16s above won't format QString that way; rewrite simply:
    }
    // The previous loop used mixed formatting incorrectly. Rebuild cleanly.
    out.clear();
    auto dump = [&](const char *title, const QList<CodecElement> &list) {
        out += QString::fromLatin1(title) + QLatin1Char('\n');
        for (const CodecElement &e : list) {
            out += QStringLiteral("  %1  %2  (%3, %4)  %5\n")
                       .arg(e.ready ? QStringLiteral("[ready]") : QStringLiteral("[ -- ]"),
                            e.element,
                            e.family,
                            e.codec,
                            e.detail);
        }
        out += QLatin1Char('\n');
    };
    dump("Encoders", probe.encoders);
    dump("Decoders", probe.decoders);
    if (!probe.notes.isEmpty()) {
        out += QStringLiteral("Notes\n=====\n");
        for (const QString &n : probe.notes)
            out += QStringLiteral("  - ") + n + QLatin1Char('\n');
    }
    return out;
}

} // namespace lanscreen
