#include "common.h"

namespace lanscreen {

SessionConfig SessionConfig::fromSettings(QSettings &s)
{
    SessionConfig c;
    s.beginGroup(QStringLiteral("session"));
    c.sessionName = s.value(QStringLiteral("name"), c.sessionName).toString();
    c.pin = s.value(QStringLiteral("pin"), c.pin).toString();
    c.bindAddress = s.value(QStringLiteral("bind"), c.bindAddress).toString();
    c.controlPort = static_cast<quint16>(s.value(QStringLiteral("controlPort"), c.controlPort).toUInt());
    c.mediaPort = static_cast<quint16>(s.value(QStringLiteral("mediaPort"), c.mediaPort).toUInt());
    c.capture = captureFromName(s.value(QStringLiteral("capture"), captureName(c.capture)).toString());
    c.outputId = s.value(QStringLiteral("outputId"), c.outputId).toString();
    c.encoder = encoderFamilyFromName(s.value(QStringLiteral("encoder"), encoderFamilyName(c.encoder)).toString());
    c.decoder = decoderFamilyFromName(s.value(QStringLiteral("decoder"), decoderFamilyName(c.decoder)).toString());
    c.codec = codecFromName(s.value(QStringLiteral("codec"), codecName(c.codec)).toString());
    c.bitrateKbps = s.value(QStringLiteral("bitrateKbps"), c.bitrateKbps).toInt();
    c.rateControl = rateControlFromName(s.value(QStringLiteral("rateControl"), rateControlName(c.rateControl)).toString());
    c.fps = s.value(QStringLiteral("fps"), c.fps).toInt();
    c.width = s.value(QStringLiteral("width"), c.width).toInt();
    c.height = s.value(QStringLiteral("height"), c.height).toInt();
    c.preset = presetFromName(s.value(QStringLiteral("preset"), presetName(c.preset)).toString());
    c.keyint = s.value(QStringLiteral("keyint"), c.keyint).toInt();
    c.cqLevel = s.value(QStringLiteral("cqLevel"), c.cqLevel).toInt();
    c.transport = transportFromName(s.value(QStringLiteral("transport"), transportName(c.transport)).toString());
    c.srtLatencyMs = s.value(QStringLiteral("srtLatencyMs"), c.srtLatencyMs).toInt();
    c.fec = s.value(QStringLiteral("fec"), c.fec).toBool();
    c.fecPercent = s.value(QStringLiteral("fecPercent"), c.fecPercent).toInt();
    c.fullscreen = s.value(QStringLiteral("streamFullscreen"), c.fullscreen).toBool();
    c.virtualDisplay = s.value(QStringLiteral("virtualDisplay"), c.virtualDisplay).toInt();
    c.virtualWidth = s.value(QStringLiteral("virtualWidth"), c.virtualWidth).toInt();
    c.virtualHeight = s.value(QStringLiteral("virtualHeight"), c.virtualHeight).toInt();
    c.virtualRefresh = s.value(QStringLiteral("virtualRefresh"), c.virtualRefresh).toInt();
    c.virtualName = s.value(QStringLiteral("virtualName"), c.virtualName).toString();
    s.endGroup();
    return c;
}

void SessionConfig::save(QSettings &s) const
{
    s.beginGroup(QStringLiteral("session"));
    s.setValue(QStringLiteral("name"), sessionName);
    s.setValue(QStringLiteral("pin"), pin);
    s.setValue(QStringLiteral("bind"), bindAddress);
    s.setValue(QStringLiteral("controlPort"), controlPort);
    s.setValue(QStringLiteral("mediaPort"), mediaPort);
    s.setValue(QStringLiteral("capture"), captureName(capture));
    s.setValue(QStringLiteral("outputId"), outputId);
    s.setValue(QStringLiteral("encoder"), encoderFamilyName(encoder));
    s.setValue(QStringLiteral("decoder"), decoderFamilyName(decoder));
    s.setValue(QStringLiteral("codec"), codecName(codec));
    s.setValue(QStringLiteral("bitrateKbps"), bitrateKbps);
    s.setValue(QStringLiteral("rateControl"), rateControlName(rateControl));
    s.setValue(QStringLiteral("fps"), fps);
    s.setValue(QStringLiteral("width"), width);
    s.setValue(QStringLiteral("height"), height);
    s.setValue(QStringLiteral("preset"), presetName(preset));
    s.setValue(QStringLiteral("keyint"), keyint);
    s.setValue(QStringLiteral("cqLevel"), cqLevel);
    s.setValue(QStringLiteral("transport"), transportName(transport));
    s.setValue(QStringLiteral("srtLatencyMs"), srtLatencyMs);
    s.setValue(QStringLiteral("fec"), fec);
    s.setValue(QStringLiteral("fecPercent"), fecPercent);
    s.setValue(QStringLiteral("streamFullscreen"), fullscreen);
    s.setValue(QStringLiteral("virtualDisplay"), virtualDisplay);
    s.setValue(QStringLiteral("virtualWidth"), virtualWidth);
    s.setValue(QStringLiteral("virtualHeight"), virtualHeight);
    s.setValue(QStringLiteral("virtualRefresh"), virtualRefresh);
    s.setValue(QStringLiteral("virtualName"), virtualName);
    s.endGroup();
}

} // namespace lanscreen
