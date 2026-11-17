#pragma once

#include "common.h"

#include <QMainWindow>

class QTabWidget;

namespace lanscreen {

class HostSession;
class ClientSession;
class HostPage;
class ClientPage;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    void applyCliHost(const SessionConfig &cfg);
    void applyCliClient(const QString &address, const SessionConfig &cfg);

private:
    void startHost();
    void stopHost();
    void startClient();
    void stopClient();
    void toggleFullscreen();
    void loadSettings();
    void saveSettings();
    void closeEvent(QCloseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

    HostSession *host_ = nullptr;
    ClientSession *client_ = nullptr;
    HostPage *hostPage_ = nullptr;
    ClientPage *clientPage_ = nullptr;
    QTabWidget *tabs_ = nullptr;
};

} // namespace lanscreen
