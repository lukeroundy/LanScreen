#include "videowidget.h"
#include "inputinject.h"

#include <QEnterEvent>
#include <QFocusEvent>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QWheelEvent>

namespace lanscreen {

VideoWidget::VideoWidget(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(320, 180);
}

QRect VideoWidget::letterboxRect() const
{
    const int ww = width();
    const int wh = height();
    if (ww <= 0 || wh <= 0)
        return {};
    const double srcAr = double(streamW_) / double(streamH_ > 0 ? streamH_ : 1);
    const double dstAr = double(ww) / double(wh);
    int vx = 0, vy = 0, vw = ww, vh = wh;
    if (dstAr > srcAr) {
        vw = int(wh * srcAr);
        vx = (ww - vw) / 2;
    } else if (dstAr < srcAr) {
        vh = int(ww / srcAr);
        vy = (wh - vh) / 2;
    }
    return {vx, vy, vw, vh};
}

void VideoWidget::setFrame(const QImage &img)
{
    if (img.isNull())
        return;
    frame_ = img;
    if (frame_.width() > 0 && frame_.height() > 0) {
        streamW_ = frame_.width();
        streamH_ = frame_.height();
    }
    update();
}

void VideoWidget::clearFrame()
{
    if (frame_.isNull())
        return;
    frame_ = QImage();
    update();
}

void VideoWidget::paintEvent(QPaintEvent *e)
{
    Q_UNUSED(e);
    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), QColor(0x11, 0x11, 0x11));
    if (frame_.isNull()) {
        if (!placeholder_.isEmpty()) {
            p.setPen(QColor(0xcc, 0xcc, 0xcc));
            p.drawText(rect().adjusted(16, 16, -16, -16),
                       Qt::AlignCenter | Qt::TextWordWrap, placeholder_);
        }
        return;
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(letterboxRect(), frame_);
}

quintptr VideoWidget::nativeHandle() const
{
    return static_cast<quintptr>(winId());
}

void VideoWidget::setStreamSize(int w, int h)
{
    if (w > 0)
        streamW_ = w;
    if (h > 0)
        streamH_ = h;
}

void VideoWidget::setCaptureEnabled(bool on)
{
    if (capture_ && !on)
        releaseHeldInput();
    capture_ = on;
    if (on)
        setFocus(Qt::OtherFocusReason);
}

void VideoWidget::releaseHeldInput()
{
    for (int code : pressedKeys_) {
        emit inputEvent(QJsonObject{{QStringLiteral("ev"), QStringLiteral("key")},
                                    {QStringLiteral("code"), code},
                                    {QStringLiteral("down"), false}});
    }
    pressedKeys_.clear();
    for (int btn : pressedButtons_) {
        emit inputEvent(QJsonObject{{QStringLiteral("ev"), QStringLiteral("button")},
                                    {QStringLiteral("btn"), btn},
                                    {QStringLiteral("down"), false}});
    }
    pressedButtons_.clear();
    emit inputEvent(QJsonObject{{QStringLiteral("ev"), QStringLiteral("release")}});
}

QPoint VideoWidget::mapToStream(const QPoint &pos) const
{
    const QRect r = letterboxRect();
    if (!r.isValid())
        return {0, 0};
    const int x = streamW_ * (pos.x() - r.x()) / qMax(1, r.width());
    const int y = streamH_ * (pos.y() - r.y()) / qMax(1, r.height());
    return {qBound(0, x, streamW_ - 1), qBound(0, y, streamH_ - 1)};
}

void VideoWidget::sendMove(const QPoint &pos)
{
    if (!capture_)
        return;
    const QPoint p = mapToStream(pos);
    emit inputEvent(QJsonObject{{QStringLiteral("ev"), QStringLiteral("move")},
                                {QStringLiteral("x"), p.x()},
                                {QStringLiteral("y"), p.y()}});
}

void VideoWidget::sendButton(Qt::MouseButton b, bool down)
{
    if (!capture_)
        return;
    int btn = 1;
    if (b == Qt::MiddleButton)
        btn = 2;
    else if (b == Qt::RightButton)
        btn = 3;
    else if (b == Qt::BackButton)
        btn = 4;
    else if (b == Qt::ForwardButton)
        btn = 5;
    emit inputEvent(QJsonObject{{QStringLiteral("ev"), QStringLiteral("button")},
                                {QStringLiteral("btn"), btn},
                                {QStringLiteral("down"), down}});
    if (down)
        pressedButtons_.insert(btn);
    else
        pressedButtons_.remove(btn);
}

void VideoWidget::sendKey(QKeyEvent *e, bool down)
{
    if (!capture_)
        return;
    if (e->isAutoRepeat())
        return;
    int code = evdevFromQtKey(e->key());
    // On X11, native scan codes are evdev + 8.
    const int sc = int(e->nativeScanCode());
    if (code <= 0 && sc >= 8)
        code = sc - 8;
    if (code <= 0)
        return;
    emit inputEvent(QJsonObject{{QStringLiteral("ev"), QStringLiteral("key")},
                                {QStringLiteral("code"), code},
                                {QStringLiteral("qt"), e->key()},
                                {QStringLiteral("down"), down}});
    if (down)
        pressedKeys_.insert(code);
    else
        pressedKeys_.remove(code);
}

void VideoWidget::mouseMoveEvent(QMouseEvent *e)
{
    sendMove(e->pos());
}

void VideoWidget::mousePressEvent(QMouseEvent *e)
{
    sendMove(e->pos());
    sendButton(e->button(), true);
    setFocus(Qt::MouseFocusReason);
}

void VideoWidget::mouseReleaseEvent(QMouseEvent *e)
{
    sendButton(e->button(), false);
}

void VideoWidget::wheelEvent(QWheelEvent *e)
{
    if (!capture_)
        return;
    const QPoint pd = e->angleDelta();
    const int dy = pd.y() / 120;
    const int dx = pd.x() / 120;
    if (dy || dx) {
        emit inputEvent(QJsonObject{{QStringLiteral("ev"), QStringLiteral("wheel")},
                                    {QStringLiteral("dx"), dx},
                                    {QStringLiteral("dy"), dy}});
    }
}

void VideoWidget::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        emit escapePressed();
        e->accept();
        return;
    }
    sendKey(e, true);
}

void VideoWidget::keyReleaseEvent(QKeyEvent *e)
{
    sendKey(e, false);
}

void VideoWidget::enterEvent(QEnterEvent *e)
{
    Q_UNUSED(e);
    if (capture_)
        setFocus(Qt::OtherFocusReason);
}

void VideoWidget::leaveEvent(QEvent *e)
{
    Q_UNUSED(e);
}

void VideoWidget::focusOutEvent(QFocusEvent *e)
{
    Q_UNUSED(e);
    releaseHeldInput();
}

void VideoWidget::hideEvent(QHideEvent *e)
{
    Q_UNUSED(e);
    releaseHeldInput();
}

void VideoWidget::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
}

} // namespace lanscreen
