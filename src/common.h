#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QSettings>

namespace lanscreen {

inline constexpr const char *kAppName = "LanScreen";
inline constexpr quint16 kDiscoveryPort = 47500;
inline constexpr quint16 kDefaultControlPort = 47510;
inline constexpr quint16 kDefaultMediaPort = 47511;
inline constexpr int kDiscoveryMagic = 0x4C534352; // "LSCR"
inline constexpr quint8 kProtocolVersion = 1;

enum class Role { Host, Client };

enum class Transport { Srt, UdpRtp, Tcp };

enum class Codec { H264, Hevc, Av1 };

enum class EncoderFamily { Auto, Nvenc, Vaapi, V4l2, Software };

enum class DecoderFamily { Auto, Hardware, Software };

enum class RateControl { Cbr, Vbr, Cqp };

enum class Preset { UltraLowLatency, LowLatency, Quality };

enum class CaptureSource { TestPattern, Screen, Portal, Output };

struct SessionConfig {
    QString sessionName = QStringLiteral("LanScreen");
    QString pin; // empty = no PIN
    QString bindAddress; // empty = all / auto LAN
    quint16 controlPort = kDefaultControlPort;
    quint16 mediaPort = kDefaultMediaPort;

    CaptureSource capture = CaptureSource::Screen;
    QString outputId; // drm/xrandr/qt screen id
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0; // 0 = full

    // Virtual extended display (KDE / KWin). -1 = auto (on when Plasma
    // Wayland + krfb-virtualmonitor are detected), 0 = off, 1 = on.
    int virtualDisplay = -1;
    int virtualWidth = 1920;
    int virtualHeight = 1080;
    int virtualRefresh = 60;
    QString virtualName = QStringLiteral("lanscreen");

    EncoderFamily encoder = EncoderFamily::Auto;
    DecoderFamily decoder = DecoderFamily::Auto;
    Codec codec = Codec::H264;
    int bitrateKbps = 8000;
    RateControl rateControl = RateControl::Cbr;
    int fps = 60;
    int width = 1920;  // 0 = native / source
    int height = 1080;
    Preset preset = Preset::LowLatency;
    int keyint = 60; // frames
    int cqLevel = 23; // used when rateControl == Cqp
    Transport transport = Transport::Srt;
    int srtLatencyMs = 60;
    bool fec = false;
    int fecPercent = 20;

    bool fullscreen = true;

    static SessionConfig fromSettings(QSettings &s);
    void save(QSettings &s) const;
};

inline QString transportName(Transport t)
{
    switch (t) {
    case Transport::Srt: return QStringLiteral("srt");
    case Transport::UdpRtp: return QStringLiteral("udp");
    case Transport::Tcp: return QStringLiteral("tcp");
    }
    return QStringLiteral("srt");
}

inline Transport transportFromName(const QString &n)
{
    const QString s = n.toLower();
    if (s == QLatin1String("udp") || s == QLatin1String("rtp") || s == QLatin1String("udp-rtp"))
        return Transport::UdpRtp;
    if (s == QLatin1String("tcp"))
        return Transport::Tcp;
    return Transport::Srt;
}

inline QString codecName(Codec c)
{
    switch (c) {
    case Codec::H264: return QStringLiteral("h264");
    case Codec::Hevc: return QStringLiteral("hevc");
    case Codec::Av1: return QStringLiteral("av1");
    }
    return QStringLiteral("h264");
}

inline Codec codecFromName(const QString &n)
{
    const QString s = n.toLower();
    if (s == QLatin1String("hevc") || s == QLatin1String("h265") || s == QLatin1String("h.265"))
        return Codec::Hevc;
    if (s == QLatin1String("av1"))
        return Codec::Av1;
    return Codec::H264;
}

inline QString encoderFamilyName(EncoderFamily f)
{
    switch (f) {
    case EncoderFamily::Auto: return QStringLiteral("auto");
    case EncoderFamily::Nvenc: return QStringLiteral("nvenc");
    case EncoderFamily::Vaapi: return QStringLiteral("vaapi");
    case EncoderFamily::V4l2: return QStringLiteral("v4l2");
    case EncoderFamily::Software: return QStringLiteral("software");
    }
    return QStringLiteral("auto");
}

inline EncoderFamily encoderFamilyFromName(const QString &n)
{
    const QString s = n.toLower();
    if (s == QLatin1String("nvenc") || s == QLatin1String("nvidia"))
        return EncoderFamily::Nvenc;
    if (s == QLatin1String("vaapi") || s == QLatin1String("mesa"))
        return EncoderFamily::Vaapi;
    if (s == QLatin1String("v4l2") || s == QLatin1String("venus") || s == QLatin1String("adreno")
        || s == QLatin1String("snapdragon"))
        return EncoderFamily::V4l2;
    if (s == QLatin1String("software") || s == QLatin1String("sw") || s == QLatin1String("x264"))
        return EncoderFamily::Software;
    return EncoderFamily::Auto;
}

inline QString rateControlName(RateControl r)
{
    switch (r) {
    case RateControl::Cbr: return QStringLiteral("cbr");
    case RateControl::Vbr: return QStringLiteral("vbr");
    case RateControl::Cqp: return QStringLiteral("cqp");
    }
    return QStringLiteral("cbr");
}

inline RateControl rateControlFromName(const QString &n)
{
    const QString s = n.toLower();
    if (s == QLatin1String("vbr"))
        return RateControl::Vbr;
    if (s == QLatin1String("cqp") || s == QLatin1String("qp") || s == QLatin1String("constqp"))
        return RateControl::Cqp;
    return RateControl::Cbr;
}

inline QString presetName(Preset p)
{
    switch (p) {
    case Preset::UltraLowLatency: return QStringLiteral("ultra-low-latency");
    case Preset::LowLatency: return QStringLiteral("low-latency");
    case Preset::Quality: return QStringLiteral("quality");
    }
    return QStringLiteral("low-latency");
}

inline Preset presetFromName(const QString &n)
{
    const QString s = n.toLower();
    if (s == QLatin1String("ultra-low-latency") || s == QLatin1String("ull")
        || s == QLatin1String("ultra"))
        return Preset::UltraLowLatency;
    if (s == QLatin1String("quality") || s == QLatin1String("hq"))
        return Preset::Quality;
    return Preset::LowLatency;
}

inline QString captureName(CaptureSource c)
{
    switch (c) {
    case CaptureSource::TestPattern: return QStringLiteral("test");
    case CaptureSource::Screen: return QStringLiteral("screen");
    case CaptureSource::Portal: return QStringLiteral("portal");
    case CaptureSource::Output: return QStringLiteral("output");
    }
    return QStringLiteral("screen");
}

inline CaptureSource captureFromName(const QString &n)
{
    const QString s = n.toLower();
    if (s == QLatin1String("test") || s == QLatin1String("test-pattern")
        || s == QLatin1String("pattern"))
        return CaptureSource::TestPattern;
    if (s == QLatin1String("portal") || s == QLatin1String("pipewire")
        || s == QLatin1String("wayland"))
        return CaptureSource::Portal;
    if (s.startsWith(QLatin1String("output")))
        return CaptureSource::Output;
    return CaptureSource::Screen;
}

inline QString decoderFamilyName(DecoderFamily f)
{
    switch (f) {
    case DecoderFamily::Auto: return QStringLiteral("auto");
    case DecoderFamily::Hardware: return QStringLiteral("hw");
    case DecoderFamily::Software: return QStringLiteral("sw");
    }
    return QStringLiteral("auto");
}

inline DecoderFamily decoderFamilyFromName(const QString &n)
{
    const QString s = n.toLower();
    if (s == QLatin1String("hw") || s == QLatin1String("hardware"))
        return DecoderFamily::Hardware;
    if (s == QLatin1String("sw") || s == QLatin1String("software"))
        return DecoderFamily::Software;
    return DecoderFamily::Auto;
}

} // namespace lanscreen
