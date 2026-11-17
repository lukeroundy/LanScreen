#include "mainwindow.h"

#include "clientpage.h"
#include "clientsession.h"
#include "hostpage.h"
#include "hostsession.h"
#include "outputs.h"
#include "videowidget.h"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QTabBar>
#include <QHostAddress>
#include <QMessageBox>
#include <QSettings>
#include <QTabWidget>
#include <QTimer>

namespace lanscreen {

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("LanScreen"));
    resize(980, 780);

    host_ = new HostSession(this);
    client_ = new ClientSession(this);
    hostPage_ = new HostPage(host_, this);
    clientPage_ = new ClientPage(client_, this);

    tabs_ = new QTabWidget(this);
    tabs_->addTab(hostPage_, QStringLiteral("Share a screen  (Host)"));
    tabs_->addTab(clientPage_, QStringLiteral("Use as a monitor  (Client)"));
    setCentralWidget(tabs_);

    connect(hostPage_, &HostPage::startRequested, this, &MainWindow::startHost);
    connect(hostPage_, &HostPage::stopRequested, this, &MainWindow::stopHost);
    connect(clientPage_, &ClientPage::connectRequested, this, &MainWindow::startClient);
    connect(clientPage_, &ClientPage::disconnectRequested, this, &MainWindow::stopClient);
    connect(clientPage_, &ClientPage::fullscreenRequested, this, &MainWindow::toggleFullscreen);
    connect(clientPage_->video(), &VideoWidget::escapePressed, this, [this]() {
        if (isFullScreen())
            toggleFullscreen();
    });

    loadSettings();
    clientPage_->startBrowser();
    QTimer::singleShot(0, this, [this]() {
        host_->setParentWindow(windowHandle());
        client_->setVideoHandle(clientPage_->video()->nativeHandle());
        hostPage_->log(virtualDisplayReport());
    });
}

void MainWindow::startHost()
{
    SessionConfig cfg = hostPage_->config();
    QString err;
    if (!host_->start(cfg, &err)) {
        QMessageBox::warning(this, QStringLiteral("Couldn't start host"), err);
        hostPage_->log(QStringLiteral("Error: ") + err);
    }
}

void MainWindow::stopHost()
{
    host_->stop();
}

void MainWindow::startClient()
{
    SessionConfig cfg = clientPage_->config();
    const QString addrText = clientPage_->addressText();
    if (addrText.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("No host"),
                                 QStringLiteral("Pick a discovered host or type an IP address."));
        return;
    }
    QString host = addrText;
    quint16 port = cfg.controlPort ? cfg.controlPort : kDefaultControlPort;
    if (addrText.contains(QLatin1Char(':'))) {
        const int idx = addrText.lastIndexOf(QLatin1Char(':'));
        host = addrText.left(idx);
        port = static_cast<quint16>(addrText.mid(idx + 1).toUInt());
    }
    cfg.controlPort = port;
    client_->setVideoHandle(clientPage_->video()->nativeHandle());
    client_->connectToHost(QHostAddress(host), cfg);
}

void MainWindow::stopClient()
{
    if (isFullScreen())
        showNormal();
    client_->disconnectFromHost();
}

void MainWindow::toggleFullscreen()
{
    // Native stream window (waylandsink): toggle exclusive fullscreen there,
    // not on the Qt app. The old path only fullscreened LanScreen itself.
    if (client_ && client_->isConnected() && !client_->usingAppsink()) {
        const bool next = !client_->streamFullscreen();
        if (client_->setStreamFullscreen(next))
            return;
    }
    if (isFullScreen()) {
        clientPage_->setImmersive(false);
        if (tabs_)
            tabs_->tabBar()->show();
        showNormal();
        return;
    }
    if (tabs_) {
        tabs_->setCurrentWidget(clientPage_);
        tabs_->tabBar()->hide();
    }
    clientPage_->setImmersive(true);
    showFullScreen();
    clientPage_->video()->setFocus();
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape
        && (isFullScreen() || (client_ && client_->isConnected() && client_->streamFullscreen()))) {
        toggleFullscreen();
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_F11) {
        toggleFullscreen();
        e->accept();
        return;
    }
    QMainWindow::keyPressEvent(e);
}

void MainWindow::applyCliHost(const SessionConfig &cfg)
{
    hostPage_->setConfig(cfg);
    QTimer::singleShot(200, this, &MainWindow::startHost);
}

void MainWindow::applyCliClient(const QString &address, const SessionConfig &cfg)
{
    clientPage_->setConfig(cfg);
    clientPage_->setAddressText(address);
    QTimer::singleShot(200, this, &MainWindow::startClient);
}

void MainWindow::loadSettings()
{
    QSettings s(QStringLiteral("lanscreen"), QStringLiteral("lanscreen"));
    const SessionConfig c = SessionConfig::fromSettings(s);
    hostPage_->setConfig(c);
    clientPage_->setConfig(c);
    const QByteArray geo = s.value(QStringLiteral("geometry")).toByteArray();
    if (!geo.isEmpty())
        restoreGeometry(geo);
}

void MainWindow::saveSettings()
{
    QSettings s(QStringLiteral("lanscreen"), QStringLiteral("lanscreen"));
    hostPage_->config().save(s);
    s.setValue(QStringLiteral("geometry"), saveGeometry());
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    saveSettings();
    host_->stop();
    client_->disconnectFromHost();
    QMainWindow::closeEvent(e);
}

} // namespace lanscreen
