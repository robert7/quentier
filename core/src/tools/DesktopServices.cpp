#include "DesktopServices.h"

#if QT_VERSION >= 0x050000
#include <QStandardPaths>
#else
#include <QDesktopServices>
#endif

namespace qute_note {

const QString GetApplicationPersistentStoragePath()
{
#if QT_VERSION >= 0x050000
    return QStandardPaths::writableLocation(QStandardPaths::DataLocation);
#else
    return QDesktopServices::storageLocation(QDesktopServices::DataLocation);
#endif
}

const QString GetTemporaryStoragePath()
{
#if QT_VERSION >= 0x50000
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation);
#else
    return QDesktopServices::storageLocation(QDesktopServices::TempLocation);
#endif
}

} // namespace qute_note
