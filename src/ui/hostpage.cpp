#include "hostpage.h"

#include "hostsession.h"
#include "netutil.h"
#include "outputs.h"
#include "probe.h"
#include "kdisplay.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSize>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

namespace lanscreen {

HostPage::HostPage(HostSession *session, QWidget *parent)
    : QWidget(parent)
    , session_(session)
{
    auto *settings = new QWidget;
    auto *root = new QVBoxLayout(settings);
    root->setSizeConstraint(QLayout::SetMinimumSize);

    auto *sess = new QGroupBox(QStringLiteral("Session"), this);
    auto *sf = new QFormLayout(sess);
    name_ = new QLineEdit(QStringLiteral("LanScreen"));
    pin_ = new QLineEdit;
    pin_->setPlaceholderText(QStringLiteral("optional — leave blank for no PIN"));
    pin_->setMaxLength(16);
    bind_ = new QComboBox;
    bind_->setEditable(false);
    controlPort_ = new QSpinBox;
    controlPort_->setRange(1, 65535);
    controlPort_->setValue(kDefaultControlPort);
    mediaPort_ = new QSpinBox;
    mediaPort_->setRange(1, 65535);
    mediaPort_->setValue(kDefaultMediaPort);
    sf->addRow(QStringLiteral("Name"), name_);
    sf->addRow(QStringLiteral("PIN"), pin_);
    sf->addRow(QStringLiteral("Advertise / bind"), bind_);
    auto *ports = new QWidget;
    auto *pl = new QHBoxLayout(ports);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->addWidget(new QLabel(QStringLiteral("Control")));
    pl->addWidget(controlPort_);
    pl->addWidget(new QLabel(QStringLiteral("Media")));
    pl->addWidget(mediaPort_);
    pl->addStretch();
    sf->addRow(QStringLiteral("Ports"), ports);
    root->addWidget(sess);

    auto *cap = new QGroupBox(QStringLiteral("Capture"), this);
    auto *cf = new QFormLayout(cap);
    capture_ = new QComboBox;
    capture_->addItem(QStringLiteral("This machine's screen (X11 / primary)"),
                      QStringLiteral("screen"));
    capture_->addItem(QStringLiteral("PipeWire portal (Wayland, pick a monitor)"),
                      QStringLiteral("portal"));
    capture_->addItem(QStringLiteral("A specific output"), QStringLiteral("output"));
    capture_->addItem(QStringLiteral("Test pattern (no real display needed)"),
                      QStringLiteral("test"));
    output_ = new QComboBox;
    cf->addRow(QStringLiteral("Source"), capture_);
    cf->addRow(QStringLiteral("Output"), output_);
    root->addWidget(cap);

    auto *virt = new QGroupBox(QStringLiteral("KDE virtual monitor"), this);
    auto *vfl = new QFormLayout(virt);
    virtEnable_ = new QCheckBox(QStringLiteral("Create a virtual monitor when sharing starts"));
    virtHint_ = new QLabel;
    virtHint_->setWordWrap(true);
    virtRes_ = new QComboBox;
    virtRes_->addItem(QStringLiteral("1920×1080"), QSize(1920, 1080));
    virtRes_->addItem(QStringLiteral("1280×720"), QSize(1280, 720));
    virtRes_->addItem(QStringLiteral("1280×800"), QSize(1280, 800));
    virtRes_->addItem(QStringLiteral("1920×1200"), QSize(1920, 1200));
    virtRes_->addItem(QStringLiteral("2560×1440"), QSize(2560, 1440));
    virtRes_->addItem(QStringLiteral("2560×1600"), QSize(2560, 1600));
    virtRes_->addItem(QStringLiteral("3840×2160"), QSize(3840, 2160));
    virtRes_->addItem(QStringLiteral("Custom"), QSize(-1, -1));
    virtWidth_ = new QSpinBox;
    virtWidth_->setRange(640, 7680);
    virtWidth_->setValue(1920);
    virtHeight_ = new QSpinBox;
    virtHeight_->setRange(360, 4320);
    virtHeight_->setValue(1080);
    virtRefresh_ = new QSpinBox;
    virtRefresh_->setRange(30, 240);
    virtRefresh_->setValue(60);
    virtRefresh_->setSuffix(QStringLiteral(" Hz"));
    auto *vrRow = new QWidget;
    auto *vrl = new QHBoxLayout(vrRow);
    vrl->setContentsMargins(0, 0, 0, 0);
    vrl->addWidget(virtRes_, 1);
    vrl->addWidget(virtWidth_);
    vrl->addWidget(new QLabel(QStringLiteral("×")));
    vrl->addWidget(virtHeight_);
    connect(virtRes_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        const QSize s = virtRes_->currentData().toSize();
        if (s.width() > 0) {
            virtWidth_->setValue(s.width());
            virtHeight_->setValue(s.height());
        }
    });
    vfl->addRow(virtEnable_);
    vfl->addRow(QStringLiteral("Resolution"), vrRow);
    vfl->addRow(QStringLiteral("Refresh rate"), virtRefresh_);
    vfl->addRow(virtHint_);
    root->addWidget(virt);

    const KdeEnvironment ken = detectKdeEnvironment();
    virtEnable_->setChecked(ken.canCreateVirtualOutput());
    if (ken.canCreateVirtualOutput()) {
        virtHint_->setText(QStringLiteral(
            "This session is Plasma on Wayland. When you start sharing, LanScreen will ask KWin "
            "to add a virtual monitor (via krfb-virtualmonitor), capture that output, and remove "
            "it when you stop. Refresh rates other than 60 Hz need Plasma 6.6+ (kscreen-doctor addCustomMode)."));
    } else {
        virtHint_->setText(ken.unavailableReason() + QLatin1Char(' ') + ken.summary());
    }

    auto *vid = new QGroupBox(QStringLiteral("Encoder"), this);
    auto *vf = new QFormLayout(vid);
    encoder_ = new QComboBox;
    encoder_->addItem(QStringLiteral("Auto (probe at start)"), QStringLiteral("auto"));
    encoder_->addItem(QStringLiteral("NVIDIA NVENC"), QStringLiteral("nvenc"));
    encoder_->addItem(QStringLiteral("Mesa / Intel VAAPI"), QStringLiteral("vaapi"));
    encoder_->addItem(QStringLiteral("V4L2 Venus (Snapdragon / Adreno)"), QStringLiteral("v4l2"));
    encoder_->addItem(QStringLiteral("Software (x264 / x265 / SVT-AV1)"), QStringLiteral("software"));
    codec_ = new QComboBox;
    codec_->addItem(QStringLiteral("H.264 (most compatible)"), QStringLiteral("h264"));
    codec_->addItem(QStringLiteral("HEVC"), QStringLiteral("hevc"));
    codec_->addItem(QStringLiteral("AV1"), QStringLiteral("av1"));
    rc_ = new QComboBox;
    rc_->addItem(QStringLiteral("CBR — constant bitrate"), QStringLiteral("cbr"));
    rc_->addItem(QStringLiteral("VBR — variable bitrate"), QStringLiteral("vbr"));
    rc_->addItem(QStringLiteral("CQP — constant quantizer"), QStringLiteral("cqp"));
    auto *brRow = new QWidget;
    auto *brl = new QHBoxLayout(brRow);
    brl->setContentsMargins(0, 0, 0, 0);
    bitrate_ = new QSlider(Qt::Horizontal);
    bitrate_->setRange(500, 50000);
    bitrate_->setValue(8000);
    bitrate_->setSingleStep(100);
    bitrate_->setPageStep(1000);
    bitrateSpin_ = new QSpinBox;
    bitrateSpin_->setRange(100, 200000);
    bitrateSpin_->setSuffix(QStringLiteral(" kbps"));
    bitrateSpin_->setValue(8000);
    connect(bitrate_, &QSlider::valueChanged, bitrateSpin_, &QSpinBox::setValue);
    connect(bitrateSpin_, qOverload<int>(&QSpinBox::valueChanged), bitrate_, &QSlider::setValue);
    brl->addWidget(bitrate_, 1);
    brl->addWidget(bitrateSpin_);
    fps_ = new QSpinBox;
    fps_->setRange(1, 240);
    fps_->setValue(60);
    fps_->setSuffix(QStringLiteral(" fps"));
    res_ = new QComboBox;
    res_->addItem(QStringLiteral("1920×1080"), QSize(1920, 1080));
    res_->addItem(QStringLiteral("1280×720"), QSize(1280, 720));
    res_->addItem(QStringLiteral("2560×1440"), QSize(2560, 1440));
    res_->addItem(QStringLiteral("3840×2160"), QSize(3840, 2160));
    res_->addItem(QStringLiteral("Native / source size (0×0)"), QSize(0, 0));
    res_->addItem(QStringLiteral("Custom"), QSize(-1, -1));
    width_ = new QSpinBox;
    width_->setRange(0, 7680);
    width_->setValue(1920);
    height_ = new QSpinBox;
    height_->setRange(0, 4320);
    height_->setValue(1080);
    auto *resRow = new QWidget;
    auto *rl = new QHBoxLayout(resRow);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(res_, 1);
    rl->addWidget(width_);
    rl->addWidget(new QLabel(QStringLiteral("×")));
    rl->addWidget(height_);
    connect(res_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        const QSize s = res_->currentData().toSize();
        if (s.width() >= 0) {
            width_->setValue(s.width());
            height_->setValue(s.height());
        }
    });
    preset_ = new QComboBox;
    preset_->addItem(QStringLiteral("Ultra low latency"), QStringLiteral("ultra-low-latency"));
    preset_->addItem(QStringLiteral("Low latency (recommended)"), QStringLiteral("low-latency"));
    preset_->addItem(QStringLiteral("Quality"), QStringLiteral("quality"));
    preset_->setCurrentIndex(1);
    keyint_ = new QSpinBox;
    keyint_->setRange(0, 1000);
    keyint_->setValue(60);
    keyint_->setSpecialValueText(QStringLiteral("auto"));
    cq_ = new QSpinBox;
    cq_->setRange(0, 51);
    cq_->setValue(23);
    transport_ = new QComboBox;
    transport_->addItem(QStringLiteral("SRT (recommended, low-latency + recovery)"),
                        QStringLiteral("srt"));
    transport_->addItem(QStringLiteral("RTP / UDP"), QStringLiteral("udp"));
    transport_->addItem(QStringLiteral("TCP (fallback)"), QStringLiteral("tcp"));
    srtLatency_ = new QSpinBox;
    srtLatency_->setRange(20, 2000);
    srtLatency_->setValue(60);
    srtLatency_->setSuffix(QStringLiteral(" ms"));
    fec_ = new QCheckBox(QStringLiteral("UDP FEC (experimental, RTP only)"));
    encoderHint_ = new QLabel;
    encoderHint_->setWordWrap(true);
    vf->addRow(QStringLiteral("Encoder"), encoder_);
    vf->addRow(QStringLiteral("Codec"), codec_);
    vf->addRow(QStringLiteral("Bitrate"), brRow);
    vf->addRow(QStringLiteral("Rate control"), rc_);
    vf->addRow(QStringLiteral("CQP level"), cq_);
    vf->addRow(QStringLiteral("Framerate"), fps_);
    vf->addRow(QStringLiteral("Resolution"), resRow);
    vf->addRow(QStringLiteral("Preset"), preset_);
    vf->addRow(QStringLiteral("Keyframe interval"), keyint_);
    vf->addRow(QStringLiteral("Transport"), transport_);
    vf->addRow(QStringLiteral("SRT latency"), srtLatency_);
    vf->addRow(QString(), fec_);
    vf->addRow(encoderHint_);
    root->addWidget(vid);

    auto *scroll = new QScrollArea;
    scroll->setWidget(settings);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto *outer = new QVBoxLayout(this);
    outer->addWidget(scroll, 3);

    auto *btnRow = new QHBoxLayout;
    startBtn_ = new QPushButton(QStringLiteral("Start sharing"));
    stopBtn_ = new QPushButton(QStringLiteral("Stop"));
    stopBtn_->setEnabled(false);
    startBtn_->setDefault(true);
    btnRow->addWidget(startBtn_);
    btnRow->addWidget(stopBtn_);
    btnRow->addStretch();
    outer->addLayout(btnRow);

    status_ = new QLabel(QStringLiteral("Idle"));
    status_->setWordWrap(true);
    outer->addWidget(status_);

    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(500);
    log_->setPlaceholderText(QStringLiteral("Log"));
    log_->setMinimumHeight(120);
    outer->addWidget(log_, 1);

    connect(startBtn_, &QPushButton::clicked, this, &HostPage::startRequested);
    connect(stopBtn_, &QPushButton::clicked, this, &HostPage::stopRequested);
    connect(session_, &HostSession::statusChanged, status_, &QLabel::setText);
    connect(session_, &HostSession::logMessage, this, &HostPage::log);
    connect(session_, &HostSession::started, this, [this]() {
        startBtn_->setEnabled(false);
        stopBtn_->setEnabled(true);
    });
    connect(session_, &HostSession::stopped, this, [this]() {
        startBtn_->setEnabled(true);
        stopBtn_->setEnabled(false);
    });
    connect(session_, &HostSession::failed, this, [this](const QString &e) {
        startBtn_->setEnabled(true);
        stopBtn_->setEnabled(false);
        log(QStringLiteral("Error: ") + e);
    });

    // Bind list
    bind_->addItem(QStringLiteral("Auto (LAN, listen on all interfaces)"), QStringLiteral("auto"));
    for (const NetInterface &n : listIpv4Interfaces()) {
        const QString label = QStringLiteral("%1  —  %2%3")
                                  .arg(n.address.toString(), n.name,
                                       n.isLoopback ? QStringLiteral(" (loopback)")
                                                    : (n.isPrivate ? QString() : QStringLiteral(" (not a private LAN)")));
        bind_->addItem(label, n.address.toString());
    }

    refreshOutputs();
}

void HostPage::refreshOutputs()
{
    output_->clear();
    const auto outs = listOutputs();
    if (outs.isEmpty())
        output_->addItem(QStringLiteral("(no outputs detected)"), QString());
    for (const DisplayOutput &o : outs) {
        const QString label = QStringLiteral("%1  %2×%3  %4")
                                  .arg(o.name)
                                  .arg(o.geom.width())
                                  .arg(o.geom.height())
                                  .arg(o.source);
        output_->addItem(label, o.id);
    }
}

void HostPage::log(const QString &line)
{
    log_->appendPlainText(line);
}

SessionConfig HostPage::config() const
{
    SessionConfig c;
    c.sessionName = name_->text().trimmed().isEmpty() ? QStringLiteral("LanScreen")
                                                      : name_->text().trimmed();
    c.pin = pin_->text().trimmed();
    c.bindAddress = bind_->currentData().toString();
    if (c.bindAddress == QLatin1String("auto"))
        c.bindAddress.clear();
    c.controlPort = static_cast<quint16>(controlPort_->value());
    c.mediaPort = static_cast<quint16>(mediaPort_->value());
    c.capture = captureFromName(capture_->currentData().toString());
    c.outputId = output_->currentData().toString();
    c.encoder = encoderFamilyFromName(encoder_->currentData().toString());
    c.codec = codecFromName(codec_->currentData().toString());
    c.bitrateKbps = bitrateSpin_->value();
    c.rateControl = rateControlFromName(rc_->currentData().toString());
    c.fps = fps_->value();
    c.width = width_->value();
    c.height = height_->value();
    c.preset = presetFromName(preset_->currentData().toString());
    c.keyint = keyint_->value();
    c.cqLevel = cq_->value();
    c.transport = transportFromName(transport_->currentData().toString());
    c.srtLatencyMs = srtLatency_->value();
    c.fec = fec_->isChecked();
    c.virtualDisplay = virtEnable_->isChecked() ? 1 : 0;
    c.virtualWidth = virtWidth_->value();
    c.virtualHeight = virtHeight_->value();
    c.virtualRefresh = virtRefresh_->value();
    return c;
}

void HostPage::setConfig(const SessionConfig &c)
{
    name_->setText(c.sessionName);
    pin_->setText(c.pin);
    controlPort_->setValue(c.controlPort);
    mediaPort_->setValue(c.mediaPort);
    const int cap = capture_->findData(captureName(c.capture));
    if (cap >= 0)
        capture_->setCurrentIndex(cap);
    const int enc = encoder_->findData(encoderFamilyName(c.encoder));
    if (enc >= 0)
        encoder_->setCurrentIndex(enc);
    const int co = codec_->findData(codecName(c.codec));
    if (co >= 0)
        codec_->setCurrentIndex(co);
    bitrateSpin_->setValue(c.bitrateKbps);
    const int rci = rc_->findData(rateControlName(c.rateControl));
    if (rci >= 0)
        rc_->setCurrentIndex(rci);
    fps_->setValue(c.fps);
    width_->setValue(c.width);
    height_->setValue(c.height);
    const int pr = preset_->findData(presetName(c.preset));
    if (pr >= 0)
        preset_->setCurrentIndex(pr);
    keyint_->setValue(c.keyint);
    cq_->setValue(c.cqLevel);
    const int tr = transport_->findData(transportName(c.transport));
    if (tr >= 0)
        transport_->setCurrentIndex(tr);
    srtLatency_->setValue(c.srtLatencyMs);
    fec_->setChecked(c.fec);
    if (c.virtualDisplay >= 0)
        virtEnable_->setChecked(c.virtualDisplay != 0);
    if (c.virtualWidth > 0)
        virtWidth_->setValue(c.virtualWidth);
    if (c.virtualHeight > 0)
        virtHeight_->setValue(c.virtualHeight);
    if (c.virtualRefresh > 0)
        virtRefresh_->setValue(c.virtualRefresh);
    const QSize vs(c.virtualWidth, c.virtualHeight);
    const int vri = virtRes_->findData(vs);
    virtRes_->setCurrentIndex(vri >= 0 ? vri : virtRes_->count() - 1);
}

} // namespace lanscreen
