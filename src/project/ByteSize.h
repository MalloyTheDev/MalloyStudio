#pragma once

// Shared human-readable byte formatter for the registry value types
// (ClipInfo, MediaInfo, RecordingInfo), which all present file sizes the
// same way in list rows.

#include <QString>
#include <QtGlobal>

inline QString formatByteSize(qint64 bytes) {
    const double mb = bytes / (1024.0 * 1024.0);
    if (mb >= 1024.0) return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
    if (mb >= 1.0)    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
}
