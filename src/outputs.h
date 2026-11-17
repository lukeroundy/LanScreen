#pragma once

#include <QList>
#include <QRect>
#include <QString>

namespace lanscreen {

struct DisplayOutput {
    QString id;
    QString name;
    QRect geom;
    bool connected = true;
    QString source; // "qt", "drm", "xrandr"
    QString extra;
};

struct VirtualDisplayStatus {
    bool evdiLoaded = false;
    bool evdiDevice = false;
    bool vkmsLoaded = false;
    QString advice;
    QStringList toolsFound;
};

QList<DisplayOutput> listOutputs();
VirtualDisplayStatus probeVirtualDisplay();
QString virtualDisplayReport();

} // namespace lanscreen
