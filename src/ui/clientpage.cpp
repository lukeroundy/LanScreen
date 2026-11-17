#include "clientpage.h"

#include "clientsession.h"
#include "videowidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace lanscreen {

ClientPage::ClientPage(ClientSession *session, QWidget *parent)
    : QWidget(parent)
    , session_(session)
{
    browser_ = new DiscoveryBrowser(this);
    auto *root = new QVBoxLayout(this);

    auto *top = new QHBoxLayout;
    list_ = new QListWidget;
    list_->setMinimumHeight(80);
    auto *side = new QVBoxLayout;
    side->addWidget(new QLabel(QStringLiteral("Hosts on the LAN (or type an address)")));
    side->addWidget(list_, 1);
    auto *addrRow = new QHBoxLayout;
    address_ = new QLineEdit;
    address_->setPlaceholderText(QStringLiteral("192.168.1.10  or  192.168.1.10:47510"));
    pin_ = new QLineEdit;
    pin_->setPlaceholderText(QStringLiteral("PIN"));
    pin_->setMaximumWidth(120);
    addrRow->addWidget(address_, 1);
    addrRow->addWidget(pin_);
    side->addLayout(addrRow);
    decoder_ = new QComboBox;
    decoder_->addItem(QStringLiteral("Decoder: Auto"), QStringLiteral("auto"));
    decoder_->addItem(QStringLiteral("Decoder: Hardware if available"), QStringLiteral("hw"));
    decoder_->addItem(QStringLiteral("Decoder: Software"), QStringLiteral("sw"));
    side->addWidget(decoder_);
    fullscreen_ = new QCheckBox(QStringLiteral("Start the stream in fullscreen"));
    side->addWidget(fullscreen_);
    auto *btns = new QHBoxLayout;
    connectBtn_ = new QPushButton(QStringLiteral("Connect"));
    disconnectBtn_ = new QPushButton(QStringLiteral("Disconnect"));
    fsBtn_ = new QPushButton(QStringLiteral("Fullscreen"));
    disconnectBtn_->setEnabled(false);
    btns->addWidget(connectBtn_);
    btns->addWidget(disconnectBtn_);
    btns->addWidget(fsBtn_);
    btns->addStretch();
    side->addLayout(btns);
    top->addLayout(side, 1);
    chrome_ = new QWidget(this);
    auto *chromeLay = new QVBoxLayout(chrome_);
    chromeLay->setContentsMargins(0, 0, 0, 0);
    chromeLay->addLayout(top);
    root->addWidget(chrome_);

    video_ = new VideoWidget(this);
    video_->setMinimumHeight(240);
    root->addWidget(video_, 2);

    status_ = new QLabel(QStringLiteral("Idle — looking for hosts on the LAN…"));
    status_->setWordWrap(true);
    root->addWidget(status_);

    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(400);
    log_->setMaximumHeight(140);
    root->addWidget(log_);

    connect(connectBtn_, &QPushButton::clicked, this, &ClientPage::connectRequested);
    connect(disconnectBtn_, &QPushButton::clicked, this, &ClientPage::disconnectRequested);
    connect(fsBtn_, &QPushButton::clicked, this, &ClientPage::fullscreenRequested);
    connect(list_, &QListWidget::itemSelectionChanged, this, [this]() {
        const auto *item = list_->currentItem();
        if (!item)
            return;
        address_->setText(item->data(Qt::UserRole).toString());
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
        emit connectRequested();
    });
    connect(browser_, &DiscoveryBrowser::hostsChanged, this, &ClientPage::refreshList);
    connect(session_, &ClientSession::statusChanged, status_, &QLabel::setText);
    connect(session_, &ClientSession::logMessage, this, &ClientPage::log);
    connect(session_, &ClientSession::connected, this, [this]() {
        connectBtn_->setEnabled(false);
        disconnectBtn_->setEnabled(true);
        video_->setCaptureEnabled(false);
        video_->setStreamSize(session_->streamSize().width(), session_->streamSize().height());
        if (!session_->usingAppsink())
            video_->setPlaceholder(QStringLiteral("The stream is in a separate window."));
        else
            video_->setPlaceholder({});
    });
    connect(session_, &ClientSession::disconnected, this, [this]() {
        connectBtn_->setEnabled(true);
        disconnectBtn_->setEnabled(false);
        video_->setCaptureEnabled(false);
        video_->setPlaceholder({});
        video_->clearFrame();
    });
    connect(session_, &ClientSession::failed, this, [this](const QString &e) {
        connectBtn_->setEnabled(true);
        disconnectBtn_->setEnabled(false);
        log(QStringLiteral("Error: ") + e);
    });
    connect(video_, &VideoWidget::inputEvent, session_, &ClientSession::sendInput);
    connect(session_, &ClientSession::frameReady, video_, &VideoWidget::setFrame);
}

void ClientPage::startBrowser()
{
    browser_->start();
}

void ClientPage::stopBrowser()
{
    browser_->stop();
}

void ClientPage::refreshList()
{
    const QString current = address_->text();
    list_->clear();
    for (const HostBeacon &h : browser_->hosts()) {
        const QString label = QStringLiteral("%1  —  %2  (%3:%4  %5 %6)%7")
                                  .arg(h.sessionName, h.hostname, h.address.toString())
                                  .arg(h.controlPort)
                                  .arg(transportName(h.transport), codecName(h.codec),
                                       h.pinRequired ? QStringLiteral("  PIN") : QString());
        auto *item = new QListWidgetItem(label, list_);
        const QString addr = h.address.toString() + QLatin1Char(':') + QString::number(h.controlPort);
        item->setData(Qt::UserRole, addr);
        if (addr == current)
            list_->setCurrentItem(item);
    }
}

void ClientPage::log(const QString &line)
{
    log_->appendPlainText(line);
}

QString ClientPage::addressText() const
{
    return address_->text().trimmed();
}

void ClientPage::setAddressText(const QString &s)
{
    address_->setText(s);
}

SessionConfig ClientPage::config() const
{
    SessionConfig c;
    c.pin = pin_->text().trimmed();
    c.decoder = decoderFamilyFromName(decoder_->currentData().toString());
    c.fullscreen = fullscreen_->isChecked();
    const QString raw = address_->text().trimmed();
    if (raw.contains(QLatin1Char(':'))) {
        const int idx = raw.lastIndexOf(QLatin1Char(':'));
        c.controlPort = static_cast<quint16>(raw.mid(idx + 1).toUInt());
    }
    return c;
}

void ClientPage::setConfig(const SessionConfig &c)
{
    pin_->setText(c.pin);
    const int d = decoder_->findData(decoderFamilyName(c.decoder));
    if (d >= 0)
        decoder_->setCurrentIndex(d);
    fullscreen_->setChecked(c.fullscreen);
}

void ClientPage::setImmersive(bool on)
{
    if (chrome_)
        chrome_->setVisible(!on);
    if (status_)
        status_->setVisible(!on);
    if (log_)
        log_->setVisible(!on);
    if (on)
        video_->setFocus(Qt::OtherFocusReason);
}

} // namespace lanscreen
