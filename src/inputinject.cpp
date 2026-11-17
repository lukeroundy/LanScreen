#include "inputinject.h"

#include <QKeySequence>

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

namespace lanscreen {

InputInjector::InputInjector(QObject *parent)
    : QObject(parent)
{
}

InputInjector::~InputInjector()
{
    stop();
}

void InputInjector::setDisplayRect(int x, int y, int w, int h, int desktopW, int desktopH)
{
    x_ = x;
    y_ = y;
    w_ = w > 0 ? w : 1920;
    h_ = h > 0 ? h : 1080;
    deskW_ = desktopW > 0 ? desktopW : qMax(x_ + w_, w_);
    deskH_ = desktopH > 0 ? desktopH : qMax(y_ + h_, h_);
    if (fd_ >= 0)
        needRestart_ = true;
}

bool InputInjector::start(QString *error)
{
    stop();
    fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd_ < 0) {
        status_ = QStringLiteral("cannot open /dev/uinput (need the udev rule or root)");
        if (error)
            *error = status_;
        return false;
    }

    ioctl(fd_, UI_SET_EVBIT, EV_KEY);
    ioctl(fd_, UI_SET_EVBIT, EV_ABS);
    ioctl(fd_, UI_SET_EVBIT, EV_REL);
    ioctl(fd_, UI_SET_EVBIT, EV_SYN);
    ioctl(fd_, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd_, UI_SET_RELBIT, REL_HWHEEL);
    ioctl(fd_, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd_, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd_, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd_, UI_SET_KEYBIT, BTN_SIDE);
    ioctl(fd_, UI_SET_KEYBIT, BTN_EXTRA);
    for (int k = 0; k < 256; ++k)
        ioctl(fd_, UI_SET_KEYBIT, k);
    // Common extended keys
    const int extras[] = {KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTSHIFT, KEY_RIGHTSHIFT,
                          KEY_LEFTALT,  KEY_RIGHTALT,  KEY_LEFTMETA,  KEY_RIGHTMETA,
                          KEY_PAGEUP,   KEY_PAGEDOWN,  KEY_HOME,      KEY_END,
                          KEY_INSERT,   KEY_DELETE,    KEY_BACKSPACE, KEY_ESC,
                          KEY_UP,       KEY_DOWN,      KEY_LEFT,      KEY_RIGHT};
    for (int k : extras)
        ioctl(fd_, UI_SET_KEYBIT, k);

    ioctl(fd_, UI_SET_ABSBIT, ABS_X);
    ioctl(fd_, UI_SET_ABSBIT, ABS_Y);

    const int absMaxX = qMax(2, (deskW_ > 0 ? deskW_ : (x_ + w_)) - 1);
    const int absMaxY = qMax(2, (deskH_ > 0 ? deskH_ : (y_ + h_)) - 1);
    uinput_abs_setup abs{};
    abs.code = ABS_X;
    abs.absinfo.minimum = 0;
    abs.absinfo.maximum = absMaxX;
    ioctl(fd_, UI_ABS_SETUP, &abs);
    abs.code = ABS_Y;
    abs.absinfo.maximum = absMaxY;
    ioctl(fd_, UI_ABS_SETUP, &abs);

    uinput_setup setup{};
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x5A53; // "ZS"
    setup.id.product = 0x4C53; // "LS"
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "LanScreen Virtual Input");
    if (ioctl(fd_, UI_DEV_SETUP, &setup) < 0 || ioctl(fd_, UI_DEV_CREATE) < 0) {
        status_ = QStringLiteral("uinput device setup failed");
        if (error)
            *error = status_;
        close(fd_);
        fd_ = -1;
        return false;
    }
    // Give udev a moment; not strictly required.
    status_ = QStringLiteral("uinput active");
    return true;
}

void InputInjector::setParkPosition(int x, int y)
{
    parkX_ = x;
    parkY_ = y;
}

void InputInjector::releaseAllKeys()
{
    if (fd_ < 0)
        return;
    const int extras[] = {KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTSHIFT, KEY_RIGHTSHIFT,
                          KEY_LEFTALT,  KEY_RIGHTALT,  KEY_LEFTMETA,  KEY_RIGHTMETA,
                          KEY_SPACE,    KEY_ESC,       KEY_ENTER,
                          BTN_LEFT,     BTN_MIDDLE,    BTN_RIGHT,     BTN_SIDE, BTN_EXTRA};
    QSet<int> codes = pressedKeys_;
    for (int c : extras)
        codes.insert(c);
    for (int c : codes)
        emitEvent(EV_KEY, c, 0);
    pressedKeys_.clear();
    sync();
}

void InputInjector::park()
{
    releaseAllKeys();
    if (fd_ < 0 || parkX_ < 0 || parkY_ < 0)
        return;
    int x = parkX_;
    int y = parkY_;
    if (deskW_ > 0 && x >= deskW_)
        x = deskW_ - 1;
    if (deskH_ > 0 && y >= deskH_)
        y = deskH_ - 1;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    emitEvent(EV_ABS, ABS_X, x);
    emitEvent(EV_ABS, ABS_Y, y);
    sync();
}

void InputInjector::stop()
{
    park();
    if (fd_ >= 0) {
        ioctl(fd_, UI_DEV_DESTROY);
        close(fd_);
        fd_ = -1;
    }
    status_ = QStringLiteral("stopped");
}

bool InputInjector::emitEvent(int type, int code, int value)
{
    if (fd_ < 0)
        return false;
    input_event ev{};
    ev.type = static_cast<__u16>(type);
    ev.code = static_cast<__u16>(code);
    ev.value = value;
    return write(fd_, &ev, sizeof(ev)) == sizeof(ev);
}

bool InputInjector::sync()
{
    return emitEvent(EV_SYN, SYN_REPORT, 0);
}

void InputInjector::handleEvent(const QJsonObject &obj)
{
    if (fd_ < 0)
        return;
    const QString ev = obj.value(QStringLiteral("ev")).toString();
    if (needRestart_) {
        needRestart_ = false;
        QString err;
        start(&err);
        if (fd_ < 0)
            return;
    }
    if (ev == QLatin1String("move")) {
        // Client sends x,y in stream pixels (0..w-1, 0..h-1). Map onto the
        // captured output's position in the desktop so the pointer hits the
        // virtual monitor, not the primary screen.
        int x = obj.value(QStringLiteral("x")).toInt();
        int y = obj.value(QStringLiteral("y")).toInt();
        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        if (x >= w_)
            x = w_ - 1;
        if (y >= h_)
            y = h_ - 1;
        emitEvent(EV_ABS, ABS_X, x_ + x);
        emitEvent(EV_ABS, ABS_Y, y_ + y);
        sync();
        return;
    }
    if (ev == QLatin1String("button")) {
        const int btn = obj.value(QStringLiteral("btn")).toInt(); // 1 left, 2 middle, 3 right
        const bool down = obj.value(QStringLiteral("down")).toBool();
        int code = BTN_LEFT;
        if (btn == 2)
            code = BTN_MIDDLE;
        else if (btn == 3)
            code = BTN_RIGHT;
        else if (btn == 4)
            code = BTN_SIDE;
        else if (btn == 5)
            code = BTN_EXTRA;
        emitEvent(EV_KEY, code, down ? 1 : 0);
        if (down)
            pressedKeys_.insert(code);
        else
            pressedKeys_.remove(code);
        sync();
        return;
    }
    if (ev == QLatin1String("wheel")) {
        const int dy = obj.value(QStringLiteral("dy")).toInt();
        const int dx = obj.value(QStringLiteral("dx")).toInt();
        if (dy)
            emitEvent(EV_REL, REL_WHEEL, dy);
        if (dx)
            emitEvent(EV_REL, REL_HWHEEL, dx);
        sync();
        return;
    }
    if (ev == QLatin1String("key")) {
        int code = obj.value(QStringLiteral("code")).toInt();
        if (code <= 0)
            code = evdevFromQtKey(obj.value(QStringLiteral("qt")).toInt());
        if (code <= 0)
            return;
        const bool down = obj.value(QStringLiteral("down")).toBool();
        emitEvent(EV_KEY, code, down ? 1 : 0);
        if (down)
            pressedKeys_.insert(code);
        else
            pressedKeys_.remove(code);
        sync();
        return;
    }
    if (ev == QLatin1String("release")) {
        releaseAllKeys();
        return;
    }
}

int evdevFromQtKey(int qtKey)
{
    using namespace Qt;
    if (qtKey >= Key_A && qtKey <= Key_Z)
        return KEY_A + (qtKey - Key_A);
    if (qtKey >= Key_0 && qtKey <= Key_9)
        return (qtKey == Key_0) ? KEY_0 : (KEY_1 + (qtKey - Key_1));
    if (qtKey >= Key_F1 && qtKey <= Key_F12)
        return KEY_F1 + (qtKey - Key_F1);
    if (qtKey >= Key_F13 && qtKey <= Key_F24)
        return KEY_F13 + (qtKey - Key_F13);
    switch (qtKey) {
    case Key_Escape: return KEY_ESC;
    case Key_Tab: return KEY_TAB;
    case Key_Backspace: return KEY_BACKSPACE;
    case Key_Return:
    case Key_Enter: return KEY_ENTER;
    case Key_Space: return KEY_SPACE;
    case Key_Minus: return KEY_MINUS;
    case Key_Equal: return KEY_EQUAL;
    case Key_BracketLeft: return KEY_LEFTBRACE;
    case Key_BracketRight: return KEY_RIGHTBRACE;
    case Key_Backslash: return KEY_BACKSLASH;
    case Key_Semicolon: return KEY_SEMICOLON;
    case Key_Apostrophe: return KEY_APOSTROPHE;
    case Key_QuoteLeft: return KEY_GRAVE;
    case Key_Comma: return KEY_COMMA;
    case Key_Period: return KEY_DOT;
    case Key_Slash: return KEY_SLASH;
    case Key_CapsLock: return KEY_CAPSLOCK;
    case Key_NumLock: return KEY_NUMLOCK;
    case Key_ScrollLock: return KEY_SCROLLLOCK;
    case Key_Shift:
        return KEY_LEFTSHIFT;
    case Key_Control: return KEY_LEFTCTRL;
    case Key_Alt: return KEY_LEFTALT;
    case Key_Meta:
    case Key_Super_L: return KEY_LEFTMETA;
    case Key_Super_R: return KEY_RIGHTMETA;
    case Key_AltGr: return KEY_RIGHTALT;
    case Key_Insert: return KEY_INSERT;
    case Key_Delete: return KEY_DELETE;
    case Key_Home: return KEY_HOME;
    case Key_End: return KEY_END;
    case Key_PageUp: return KEY_PAGEUP;
    case Key_PageDown: return KEY_PAGEDOWN;
    case Key_Left: return KEY_LEFT;
    case Key_Right: return KEY_RIGHT;
    case Key_Up: return KEY_UP;
    case Key_Down: return KEY_DOWN;
    case Key_Print: return KEY_PRINT;
    case Key_Pause: return KEY_PAUSE;
    case Key_Menu: return KEY_MENU;
    case Key_Plus: return KEY_EQUAL;
    case Key_Asterisk: return KEY_KPASTERISK;
    case Key_Agrave: return KEY_GRAVE;
    default:
        return 0;
    }
}

} // namespace lanscreen
