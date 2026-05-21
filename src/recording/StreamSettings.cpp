#include "StreamSettings.h"

#include <QSettings>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>

namespace {
constexpr wchar_t kCredentialName[] = L"MalloyStudio_StreamKey";

QString serviceToString(StreamSettings::Service s) {
    switch (s) {
        case StreamSettings::Service::Twitch:  return QStringLiteral("twitch");
        case StreamSettings::Service::YouTube: return QStringLiteral("youtube");
        case StreamSettings::Service::Custom:  return QStringLiteral("custom");
    }
    return QStringLiteral("twitch");
}

StreamSettings::Service serviceFromString(const QString& s) {
    if (s == QLatin1String("youtube")) return StreamSettings::Service::YouTube;
    if (s == QLatin1String("custom"))  return StreamSettings::Service::Custom;
    return StreamSettings::Service::Twitch;
}

// Read the stream key from the Windows Credential Manager.
// Returns an empty string if the credential isn't set or fails to load.
QString loadStreamKey() {
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(kCredentialName, CRED_TYPE_GENERIC, 0, &cred) || !cred)
        return QString();

    QString key;
    if (cred->CredentialBlob && cred->CredentialBlobSize > 0) {
        // Stored as UTF-16 LE without the trailing NUL.
        key = QString::fromUtf16(
            reinterpret_cast<const char16_t*>(cred->CredentialBlob),
            static_cast<int>(cred->CredentialBlobSize / sizeof(wchar_t)));
    }
    CredFree(cred);
    return key;
}

void saveStreamKey(const QString& key) {
    if (key.isEmpty()) {
        // Clear the credential entirely.
        CredDeleteW(kCredentialName, CRED_TYPE_GENERIC, 0);
        return;
    }

    const std::wstring w = key.toStdWString();
    CREDENTIALW cred = {};
    cred.Type            = CRED_TYPE_GENERIC;
    cred.TargetName      = const_cast<LPWSTR>(kCredentialName);
    cred.CredentialBlobSize = static_cast<DWORD>(w.size() * sizeof(wchar_t));
    cred.CredentialBlob  = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(w.data()));
    cred.Persist         = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName        = const_cast<LPWSTR>(L"MalloyStudio");
    CredWriteW(&cred, 0);
}
} // namespace

QString StreamSettings::templateFor(Service s) {
    switch (s) {
        case Service::Twitch:  return QStringLiteral("rtmp://live.twitch.tv/app/{key}");
        case Service::YouTube: return QStringLiteral("rtmp://a.rtmp.youtube.com/live2/{key}");
        case Service::Custom:  return QStringLiteral("rtmp://your-server.example.com/live/{key}");
    }
    return QString();
}

QString StreamSettings::displayName(Service s) {
    switch (s) {
        case Service::Twitch:  return QStringLiteral("Twitch");
        case Service::YouTube: return QStringLiteral("YouTube Live");
        case Service::Custom:  return QStringLiteral("Custom RTMP");
    }
    return QString();
}

QString StreamSettings::rtmpUrl() const {
    const QString tpl = (service == Service::Custom) ? customUrl : templateFor(service);
    if (streamKey.isEmpty())
        return tpl;   // leave {key} in place so callers can detect an unconfigured key
    QString url = tpl;
    url.replace(QStringLiteral("{key}"), streamKey);
    return url;
}

StreamSettings StreamSettings::load() {
    QSettings s;
    StreamSettings o;
    o.service     = serviceFromString(
        s.value(QStringLiteral("stream/service"), serviceToString(o.service)).toString());
    o.customUrl   = s.value(QStringLiteral("stream/customUrl"),   o.customUrl  ).toString();
    o.bitrateKbps = s.value(QStringLiteral("stream/bitrateKbps"), o.bitrateKbps).toInt();
    o.keyframeSec = s.value(QStringLiteral("stream/keyframeSec"), o.keyframeSec).toInt();
    o.title       = s.value(QStringLiteral("stream/title"),    o.title).toString();
    o.category    = s.value(QStringLiteral("stream/category"), o.category).toString();
    o.tags        = s.value(QStringLiteral("stream/tags"),     o.tags).toStringList();
    o.streamKey   = loadStreamKey();
    return o;
}

void StreamSettings::save() const {
    QSettings s;
    s.setValue(QStringLiteral("stream/service"),     serviceToString(service));
    s.setValue(QStringLiteral("stream/customUrl"),   customUrl);
    s.setValue(QStringLiteral("stream/bitrateKbps"), bitrateKbps);
    s.setValue(QStringLiteral("stream/keyframeSec"), keyframeSec);
    s.setValue(QStringLiteral("stream/title"),    title);
    s.setValue(QStringLiteral("stream/category"), category);
    s.setValue(QStringLiteral("stream/tags"),     tags);
    saveStreamKey(streamKey);
}
