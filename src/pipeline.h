#pragma once

#include "common.h"
#include "probe.h"

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>
#include <functional>
#include <QTimer>

typedef struct _GstElement GstElement;
typedef struct _GstPipeline GstPipeline;
typedef struct _GstBus GstBus;

namespace lanscreen {

struct PipelinePlan {
    QString launch;          // gst_parse_launch string (uri properties may be set later)
    QString encoderElement;  // factory actually used
    QString decoderElement;
    QString description;     // human summary
    QStringList warnings;
};

class GstPipelineRunner : public QObject {
    Q_OBJECT
public:
    explicit GstPipelineRunner(QObject *parent = nullptr);
    ~GstPipelineRunner() override;

    bool start(const QString &launch, QString *error);
    // Parse and go to PAUSED so properties can be set before PLAYING.
    bool prepare(const QString &launch, QString *error);
    bool play(QString *error);
    void stop();
    bool isRunning() const { return running_; }

    GstElement *pipeline() const { return pipeline_; }
    GstElement *element(const char *name) const;

    // Set a string property on a named element after parse.
    bool setString(const char *elem, const char *prop, const QString &value);
    bool setInt(const char *elem, const char *prop, int value);
    bool setBool(const char *elem, const char *prop, bool value);

    void setOverlayHandle(quintptr wid);

    // Crop decoded frames to the host's announced picture size (drops H.264
    // macroblock padding, which otherwise shows up as a green bar).
    void setActiveVideoSize(int width, int height);

    // Pad the encoder input to a macroblock multiple (videobox name=alignbox).
    void enableEncodeAlign(int multiple);
    // Crop the decoder output to activeW_/activeH_ (videocrop name=crop).
    void enableDecodeCrop();

signals:
    void errorMessage(const QString &msg);
    void stateChanged(const QString &state);
    void infoMessage(const QString &msg);
    void frameReady(const QImage &frame);
    // Unpadded capture size, once the host align box sees input caps.
    void pictureSize(int width, int height);

private:
    void pollBus();
    void handleMessage(void *msg);
    void attachAppsink();
    void pullPendingFrames();
    QImage imageFromSample(void *sample);
    void onAlignCaps(void *pad);
    void onCropCaps(void *pad);
    static void alignCapsThunk(void *pad, void *pspec, void *self);
    static void cropCapsThunk(void *pad, void *pspec, void *self);

    GstElement *pipeline_ = nullptr;
    bool running_ = false;
    bool loggedFirstFrame_ = false;
    int activeW_ = 0;
    int activeH_ = 0;
    int alignMultiple_ = 0;
    QTimer *timer_ = nullptr;
};

PipelinePlan planHostPipeline(const SessionConfig &cfg, const ProbeResult &probe,
                              const QString &bindOrClientIp, int pipewireFd,
                              const QString &pipewirePath);
PipelinePlan planClientPipeline(const SessionConfig &cfg, const ProbeResult &probe,
                                const QString &hostIp, bool useAppsink);

// Apply encoder-specific properties that are awkward in a launch string
// (units differ across elements).
void applyEncoderSettings(GstPipelineRunner &runner, const CodecElement &enc,
                          const SessionConfig &cfg);

} // namespace lanscreen
