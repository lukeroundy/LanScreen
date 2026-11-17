#pragma once

#include "common.h"

#include <QWidget>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QSlider;
class QPlainTextEdit;
class QPushButton;
class QLabel;
class QCheckBox;

namespace lanscreen {

class HostSession;

class HostPage : public QWidget {
    Q_OBJECT
public:
    explicit HostPage(HostSession *session, QWidget *parent = nullptr);
    SessionConfig config() const;
    void setConfig(const SessionConfig &cfg);
    void refreshOutputs();
    void log(const QString &line);

signals:
    void startRequested();
    void stopRequested();

private:
    void rebuild();
    HostSession *session_ = nullptr;
    QLineEdit *name_ = nullptr;
    QLineEdit *pin_ = nullptr;
    QComboBox *bind_ = nullptr;
    QSpinBox *controlPort_ = nullptr;
    QSpinBox *mediaPort_ = nullptr;
    QComboBox *capture_ = nullptr;
    QComboBox *output_ = nullptr;
    QCheckBox *virtEnable_ = nullptr;
    QComboBox *virtRes_ = nullptr;
    QSpinBox *virtWidth_ = nullptr;
    QSpinBox *virtHeight_ = nullptr;
    QSpinBox *virtRefresh_ = nullptr;
    QLabel *virtHint_ = nullptr;
    QComboBox *encoder_ = nullptr;
    QComboBox *codec_ = nullptr;
    QComboBox *rc_ = nullptr;
    QSlider *bitrate_ = nullptr;
    QSpinBox *bitrateSpin_ = nullptr;
    QSpinBox *fps_ = nullptr;
    QComboBox *res_ = nullptr;
    QSpinBox *width_ = nullptr;
    QSpinBox *height_ = nullptr;
    QComboBox *preset_ = nullptr;
    QSpinBox *keyint_ = nullptr;
    QSpinBox *cq_ = nullptr;
    QComboBox *transport_ = nullptr;
    QSpinBox *srtLatency_ = nullptr;
    QCheckBox *fec_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *encoderHint_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    QPushButton *startBtn_ = nullptr;
    QPushButton *stopBtn_ = nullptr;
};

} // namespace lanscreen
