#include "portal.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusArgument>
#include <QDBusUnixFileDescriptor>
#include <QUuid>
#include <unistd.h>

namespace lanscreen {

static const QString kService = QStringLiteral("org.freedesktop.portal.Desktop");
static const QString kPath = QStringLiteral("/org/freedesktop/portal/desktop");
static const QString kIface = QStringLiteral("org.freedesktop.portal.ScreenCast");
static const QString kRequest = QStringLiteral("org.freedesktop.portal.Request");

static QString makeToken()
{
    return QLatin1Char('t') + QUuid::createUuid().toString(QUuid::Id128);
}

static QString requestPathFor(const QString &handleToken)
{
    QString unique = QDBusConnection::sessionBus().baseService(); // e.g. :1.42
    if (unique.startsWith(QLatin1Char(':')))
        unique = unique.mid(1);
    unique.replace(QLatin1Char('.'), QLatin1Char('_'));
    return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(unique, handleToken);
}


static QString firstStreamNodeId(const QVariant &streamsVar)
{
    // The Start reply is a(ua{sv}). QDBusArgument's read methods are const;
    // a non-const copy is treated as a write object and can spin forever.
    if (streamsVar.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = qvariant_cast<QDBusArgument>(streamsVar);
        if (arg.currentType() == QDBusArgument::ArrayType) {
            arg.beginArray();
            int seen = 0;
            while (!arg.atEnd() && seen++ < 32) {
                if (arg.currentType() != QDBusArgument::StructureType)
                    break;
                arg.beginStructure();
                quint32 nodeId = 0;
                QVariantMap props;
                arg >> nodeId >> props;
                arg.endStructure();
                if (nodeId) {
                    arg.endArray();
                    return QString::number(nodeId);
                }
            }
            arg.endArray();
        }
    }

    const QVariantList list = streamsVar.toList();
    for (const QVariant &item : list) {
        if (item.canConvert<QVariantList>()) {
            const QVariantList rec = item.toList();
            if (!rec.isEmpty()) {
                bool ok = false;
                const quint32 id = rec.first().toUInt(&ok);
                if (ok && id)
                    return QString::number(id);
            }
        }
        bool ok = false;
        const quint32 id = item.toUInt(&ok);
        if (ok && id)
            return QString::number(id);
    }
    return {};
}

PortalCapture::PortalCapture(QObject *parent)
    : QObject(parent)
{
}

PortalCapture::~PortalCapture()
{
    cancel();
}

void PortalCapture::cancel()
{
    if (!requestPath_.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(kService, requestPath_, kRequest,
                                                 QStringLiteral("Response"), this,
                                                 SLOT(onResponse(uint,QVariantMap)));
        requestPath_.clear();
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    nodePath_.clear();
    sessionHandle_.clear();
    step_ = Step::Idle;
}

void PortalCapture::fail(const QString &why)
{
    cancel();
    emit failed(why);
}

void PortalCapture::start(QWindow *parentWindow, bool preferVirtual)
{
    cancel();
    preferVirtual_ = preferVirtual;
    triedVirtual_ = false;
    parentWindowHint_.clear();
    if (parentWindow) {
        const QString wid = QString::number(static_cast<qulonglong>(parentWindow->winId()), 16);
        if (!qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty())
            parentWindowHint_ = QStringLiteral("wayland:") + wid;
        else
            parentWindowHint_ = QStringLiteral("x11:") + wid;
    }
    if (!QDBusConnection::sessionBus().isConnected()) {
        emit failed(QStringLiteral("No session D-Bus. The desktop portal isn't available."));
        return;
    }
    createSession();
}

void PortalCapture::listenFor(const QString &handleToken)
{
    if (!requestPath_.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(kService, requestPath_, kRequest,
                                                 QStringLiteral("Response"), this,
                                                 SLOT(onResponse(uint,QVariantMap)));
    }
    requestPath_ = requestPathFor(handleToken);
    // Subscribe before the method returns to avoid missing a fast Response.
    QDBusConnection::sessionBus().connect(kService, requestPath_, kRequest,
                                          QStringLiteral("Response"), this,
                                          SLOT(onResponse(uint,QVariantMap)));
}

void PortalCapture::call(const QString &method, const QVariantList &args, const QString &handleToken)
{
    listenFor(handleToken);
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kIface, method);
    msg.setArguments(args);
    QDBusPendingCall pcall = QDBusConnection::sessionBus().asyncCall(msg);
    auto *w = new QDBusPendingCallWatcher(pcall, this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<QDBusObjectPath> reply = *w;
        if (reply.isError())
            fail(reply.error().message());
    });
}

void PortalCapture::createSession()
{
    step_ = Step::CreateSession;
    const QString handle = makeToken();
    const QString session = makeToken();
    QVariantMap opts;
    opts.insert(QStringLiteral("handle_token"), handle);
    opts.insert(QStringLiteral("session_handle_token"), session);
    call(QStringLiteral("CreateSession"), QVariantList{opts}, handle);
}

void PortalCapture::selectSources()
{
    step_ = Step::SelectSources;
    const QString handle = makeToken();
    QVariantMap opts;
    opts.insert(QStringLiteral("handle_token"), handle);
    // 1 = MONITOR, 2 = WINDOW, 4 = VIRTUAL
    uint types = 1u;
    if (preferVirtual_ && !triedVirtual_) {
        types = 4u;
        triedVirtual_ = true;
    }
    opts.insert(QStringLiteral("types"), types);
    opts.insert(QStringLiteral("multiple"), false);
    opts.insert(QStringLiteral("cursor_mode"), 2u); // embedded
    QDBusObjectPath session(sessionHandle_);
    call(QStringLiteral("SelectSources"), QVariantList{QVariant::fromValue(session), opts}, handle);
}

void PortalCapture::startStream()
{
    step_ = Step::Start;
    const QString handle = makeToken();
    QVariantMap opts;
    opts.insert(QStringLiteral("handle_token"), handle);
    QDBusObjectPath session(sessionHandle_);
    call(QStringLiteral("Start"),
         QVariantList{QVariant::fromValue(session), parentWindowHint_, opts}, handle);
}

void PortalCapture::openRemote()
{
    step_ = Step::OpenRemote;
    QVariantMap opts;
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kIface,
                                                      QStringLiteral("OpenPipeWireRemote"));
    msg << QVariant::fromValue(QDBusObjectPath(sessionHandle_)) << opts;
    QDBusPendingCall pcall = QDBusConnection::sessionBus().asyncCall(msg);
    auto *w = new QDBusPendingCallWatcher(pcall, this);
    connect(w, &QDBusPendingCallWatcher::finished, this, &PortalCapture::onOpenRemoteFinished);
}

void PortalCapture::onOpenRemoteFinished(QDBusPendingCallWatcher *watcher)
{
    watcher->deleteLater();
    QDBusPendingReply<QDBusUnixFileDescriptor> reply = *watcher;
    if (reply.isError()) {
        fail(QStringLiteral("OpenPipeWireRemote failed: ") + reply.error().message());
        return;
    }
    const QDBusUnixFileDescriptor ufd = reply.value();
    if (!ufd.isValid()) {
        fail(QStringLiteral("Portal returned an invalid PipeWire fd."));
        return;
    }
    fd_ = ::dup(ufd.fileDescriptor());
    if (fd_ < 0) {
        fail(QStringLiteral("Could not dup the PipeWire fd."));
        return;
    }
    emit ready(fd_, nodePath_);
}

void PortalCapture::onResponse(uint code, const QVariantMap &results)
{
    if (!requestPath_.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(kService, requestPath_, kRequest,
                                                 QStringLiteral("Response"), this,
                                                 SLOT(onResponse(uint,QVariantMap)));
        requestPath_.clear();
    }
    if (code != 0) {
        if (step_ == Step::SelectSources && preferVirtual_) {
            // Virtual monitor wasn't accepted. Try a real one.
            preferVirtual_ = false;
            selectSources();
            return;
        }
        fail(code == 1 ? QStringLiteral("Screen share was cancelled.")
                       : QStringLiteral("Portal request failed (code %1).").arg(code));
        return;
    }
    if (step_ == Step::CreateSession) {
        sessionHandle_ = results.value(QStringLiteral("session_handle")).toString();
        if (sessionHandle_.isEmpty()) {
            fail(QStringLiteral("Portal session handle missing."));
            return;
        }
        selectSources();
        return;
    }
    if (step_ == Step::SelectSources) {
        startStream();
        return;
    }
    if (step_ == Step::Start) {
        // streams is a(ua{sv}): (PipeWire node id, props). pipewiresrc needs
        // that node id as path=. With an empty path it autoselects the default
        // Video/Source, which can be a capture card instead of the KDE share.
        nodePath_ = firstStreamNodeId(results.value(QStringLiteral("streams")));
        if (nodePath_.isEmpty()) {
            fail(QStringLiteral("Portal did not return a KDE screen-share node. Not falling back to a camera or capture card."));
            return;
        }
        openRemote();
    }
}

} // namespace lanscreen
