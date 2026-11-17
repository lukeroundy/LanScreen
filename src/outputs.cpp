#include "outputs.h"
#include "kdisplay.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>

#include <unistd.h>

namespace lanscreen {

static QString readFileTrim(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll()).trimmed();
}

static bool moduleLoaded(const QString &name)
{
    QFile f(QStringLiteral("/proc/modules"));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = f.readAll();
    return data.contains('\n' + name.toLatin1() + ' ') || data.startsWith(name.toLatin1() + ' ');
}

static QList<DisplayOutput> listDrm()
{
    QList<DisplayOutput> out;
    const QDir drm(QStringLiteral("/sys/class/drm"));
    for (const QString &name : drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!name.contains(QLatin1Char('-')))
            continue; // skip card0, renderD128
        const QString base = drm.filePath(name);
        DisplayOutput o;
        o.id = name;
        o.name = name;
        o.source = QStringLiteral("drm");
        const QString status = readFileTrim(base + QStringLiteral("/status"));
        o.connected = (status == QLatin1String("connected") || status.isEmpty());
        o.extra = status;
        const QString modes = readFileTrim(base + QStringLiteral("/modes"));
        if (!modes.isEmpty()) {
            const QString first = modes.split(QLatin1Char('\n')).first();
            const QStringList wh = first.split(QLatin1Char('x'));
            if (wh.size() >= 2)
                o.geom = QRect(0, 0, wh[0].toInt(), wh[1].split(QLatin1Char('p')).first().toInt());
            o.extra += QStringLiteral("  mode ") + first;
        }
        out.append(o);
    }
    return out;
}

static QList<DisplayOutput> listQtScreens()
{
    QList<DisplayOutput> out;
    if (!qApp)
        return out;
    const auto screens = QGuiApplication::screens();
    int i = 0;
    for (QScreen *s : screens) {
        DisplayOutput o;
        o.id = QStringLiteral("screen-%1").arg(i++);
        o.name = s->name().isEmpty() ? o.id : s->name();
        o.geom = s->geometry();
        o.connected = true;
        o.source = QStringLiteral("qt");
        o.extra = QStringLiteral("%1x%2+%3+%4  %5 dpr")
                      .arg(o.geom.width())
                      .arg(o.geom.height())
                      .arg(o.geom.x())
                      .arg(o.geom.y())
                      .arg(s->devicePixelRatio());
        out.append(o);
    }
    return out;
}

QList<DisplayOutput> listOutputs()
{
    QList<DisplayOutput> out = listQtScreens();
    // Add DRM connectors that Qt didn't already name.
    QStringList known;
    for (const DisplayOutput &o : out)
        known.append(o.name);
    for (const DisplayOutput &o : listDrm()) {
        bool exists = false;
        for (const QString &k : known) {
            if (o.name.contains(k) || k.contains(o.name.section(QLatin1Char('-'), 1))) {
                exists = true;
                break;
            }
        }
        if (!exists)
            out.append(o);
    }
    return out;
}

VirtualDisplayStatus probeVirtualDisplay()
{
    VirtualDisplayStatus s;
    s.evdiLoaded = moduleLoaded(QStringLiteral("evdi"));
    s.vkmsLoaded = moduleLoaded(QStringLiteral("vkms"));
    const QDir dri(QStringLiteral("/sys/class/drm"));
    for (const QString &name : dri.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString uevent = readFileTrim(dri.filePath(name) + QStringLiteral("/device/uevent"));
        if (uevent.contains(QLatin1String("evdi")) || name.contains(QLatin1String("evdi")))
            s.evdiDevice = true;
    }
    auto has = [](const QString &bin) {
        return !QStandardPaths::findExecutable(bin).isEmpty();
    };
    if (has(QStringLiteral("kscreen-doctor")))
        s.toolsFound.append(QStringLiteral("kscreen-doctor (KDE)"));
    if (has(QStringLiteral("gdctl")))
        s.toolsFound.append(QStringLiteral("gdctl (GNOME)"));
    if (has(QStringLiteral("xrandr")))
        s.toolsFound.append(QStringLiteral("xrandr"));
    if (has(QStringLiteral("swaymsg")))
        s.toolsFound.append(QStringLiteral("swaymsg"));
    if (has(QStringLiteral("wlr-randr")))
        s.toolsFound.append(QStringLiteral("wlr-randr"));

    QStringList lines;
    const KdeEnvironment ken = detectKdeEnvironment();
    lines << ken.summary();
    if (ken.canCreateVirtualOutput())
        lines << QStringLiteral("This is a Plasma Wayland session with krfb-virtualmonitor. LanScreen can create a KWin virtual monitor automatically.");
    else
        lines << ken.unavailableReason();
    lines << QStringLiteral("");
    if (s.evdiLoaded)
        lines << QStringLiteral("The evdi kernel module is loaded. A DisplayLink-style virtual output may already exist.");
    else
        lines << QStringLiteral("evdi is not loaded. You can install it if you want a software virtual output, but a dummy HDMI/DP dongle is simpler.");
    if (s.vkmsLoaded)
        lines << QStringLiteral("vkms is loaded (a kernel virtual KMS device).");
    if (!s.toolsFound.isEmpty())
        lines << QStringLiteral("Found display tools: ") + s.toolsFound.join(QStringLiteral(", "));
    lines << QStringLiteral("On KDE Plasma Wayland, LanScreen can create a virtual monitor automatically with krfb-virtualmonitor.");
    lines << QStringLiteral("Otherwise:");
    lines << QStringLiteral("  1. Plug a cheap HDMI/DisplayPort dummy dongle, then pick that output as the capture source.");
    lines << QStringLiteral("  2. Pick an existing output in the Host tab.");
    lines << QStringLiteral("  3. For a first test, use the built-in test pattern (no extra display needed).");
    s.advice = lines.join(QLatin1Char('\n'));
    return s;
}

QString virtualDisplayReport()
{
    QString r;
    const VirtualDisplayStatus v = probeVirtualDisplay();
    r += v.advice + QLatin1Char('\n') + QLatin1Char('\n');
    r += QStringLiteral("Detected outputs\n----------------\n");
    for (const DisplayOutput &o : listOutputs()) {
        r += QStringLiteral("  %1  %2  %3x%4+%5+%6  %7  %8\n")
                 .arg(o.id, o.name)
                 .arg(o.geom.width())
                 .arg(o.geom.height())
                 .arg(o.geom.x())
                 .arg(o.geom.y())
                 .arg(o.source, o.extra);
    }
    return r;
}

} // namespace lanscreen
