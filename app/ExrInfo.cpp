#include "ExrInfo.h"

#include "ExrBackend.h"   // thumbnails/ is on the include path

QStringList ExrInfo::layers(const QString &path) const
{
    return ufb::listExrLayers(path);
}
