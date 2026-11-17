#pragma once

#include <QImage>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QWidget>

namespace lanscreen {

class VideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget *parent = nullptr);
    quintptr nativeHandle() const;
    void setStreamSize(int w, int h);
    void setPlaceholder(const QString &text) { placeholder_ = text; update(); }
    void setCaptureEnabled(bool on);

public slots:
    void setFrame(const QImage &img);
    void clearFrame();

signals:
    void inputEvent(const QJsonObject &ev);
    void escapePressed();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void sendMove(const QPoint &pos);
    void sendButton(Qt::MouseButton b, bool down);
    void sendKey(QKeyEvent *e, bool down);
    void releaseHeldInput();
    QPoint mapToStream(const QPoint &pos) const;
    QRect letterboxRect() const;

    int streamW_ = 1920;
    int streamH_ = 1080;
    bool capture_ = false;
    QImage frame_;
    QString placeholder_;
    QSet<int> pressedKeys_;
    QSet<int> pressedButtons_;
};

} // namespace lanscreen
