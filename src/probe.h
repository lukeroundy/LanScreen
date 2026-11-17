#pragma once

#include "common.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace lanscreen {

struct CodecElement {
    QString element;      // GStreamer factory name
    QString family;       // nvenc, vaapi, v4l2, software
    QString codec;        // h264, hevc, av1
    QString displayName;
    bool ready = false;   // factory exists and element reached READY
    QString detail;       // why it failed, or a short note
};

struct ProbeResult {
    QList<CodecElement> encoders;
    QList<CodecElement> decoders;
    QStringList notes;
};

// Probe GStreamer factories. Hardware plugins often register zero features
// when the GPU/driver is missing, so factory-find is already a decent check.
// We also try to set the element to READY when possible.
ProbeResult probeCodecs();

// Pick the first ready encoder matching family + codec. Auto prefers
// NVENC, then VAAPI, then V4L2, then software.
const CodecElement *pickEncoder(const ProbeResult &probe, EncoderFamily family, Codec codec);

const CodecElement *pickDecoder(const ProbeResult &probe, DecoderFamily family, Codec codec);

QString formatProbeReport(const ProbeResult &probe);

bool elementExists(const char *name);
bool elementReachesReady(const char *name, QString *detail = nullptr);

} // namespace lanscreen
