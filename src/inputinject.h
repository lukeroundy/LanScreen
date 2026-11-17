#pragma once

#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>

namespace lanscreen {

class InputInjector : public QObject {
    Q_OBJECT
public:
    explicit InputInjector(QObject *parent = nullptr);
    ~InputInjector() override;

    // geometry of the captured / virtual display, in host desktop coordinates.
    // desktopW/H is the union of all screens so ABS events can land on an
    // extended output rather than being scaled onto the primary only.
    void setDisplayRect(int x, int y, int w, int h, int desktopW = 0, int desktopH = 0);
    bool start(QString *error);
    // Move the pointer to a real monitor before destroying the virtual tablet,
    // so it is not left on a hidden virtual output.
    void setParkPosition(int x, int y);
    void park();
    void stop();
    void releaseAllKeys();
    bool isActive() const { return fd_ >= 0; }
    void handleEvent(const QJsonObject &obj);
    QString status() const { return status_; }

private:
    bool emitEvent(int type, int code, int value);
    bool sync();
    int fd_ = -1;
    int x_ = 0, y_ = 0, w_ = 1920, h_ = 1080;
    int deskW_ = 0, deskH_ = 0;
    int parkX_ = -1, parkY_ = -1;
    QString status_;
    bool needRestart_ = false;
    QSet<int> pressedKeys_;
};

// Qt key code -> linux evdev KEY_*
int evdevFromQtKey(int qtKey);

} // namespace lanscreen
