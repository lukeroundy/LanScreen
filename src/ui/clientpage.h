#pragma once

#include "common.h"
#include "discovery.h"

#include <QWidget>

class QComboBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QLabel;
class QCheckBox;

namespace lanscreen {

class ClientSession;
class VideoWidget;

class ClientPage : public QWidget {
    Q_OBJECT
public:
    explicit ClientPage(ClientSession *session, QWidget *parent = nullptr);
    SessionConfig config() const;
    void setConfig(const SessionConfig &cfg);
    VideoWidget *video() const { return video_; }
    void startBrowser();
    void stopBrowser();
    void log(const QString &line);
    QString addressText() const;
    void setAddressText(const QString &s);
    void setImmersive(bool on);

signals:
    void connectRequested();
    void disconnectRequested();
    void fullscreenRequested();

private:
    void refreshList();
    ClientSession *session_ = nullptr;
    DiscoveryBrowser *browser_ = nullptr;
    QListWidget *list_ = nullptr;
    QLineEdit *address_ = nullptr;
    QLineEdit *pin_ = nullptr;
    QComboBox *decoder_ = nullptr;
    QCheckBox *fullscreen_ = nullptr;
    QPushButton *connectBtn_ = nullptr;
    QPushButton *disconnectBtn_ = nullptr;
    QPushButton *fsBtn_ = nullptr;
    QLabel *status_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    VideoWidget *video_ = nullptr;
    QWidget *chrome_ = nullptr;
};

} // namespace lanscreen
