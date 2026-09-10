#include "MediaInfo.h"

#include <QImageReader>

bool MediaInfo::isAnimated(const QString &path) const
{
    if (path.isEmpty()) return false;
    QImageReader r(path);
    if (!r.canRead() || !r.supportsAnimation()) return false;
    // imageCount() is -1 for handlers that cannot say; treat that as
    // static rather than sending an unknown into the video player.
    return r.imageCount() > 1;
}
