#pragma once
#include <QString>
#include <QStringList>

// App-wide RTMP streaming settings.
//
// The stream key is sensitive and stored encrypted via the Windows Credential
// Manager (CredWriteW / CredReadW) — NOT in plain QSettings. Everything else
// (service choice, bitrate, custom URL template) lives in QSettings under
// "stream/*".
//
// rtmpUrl() expands the service-specific template against the key and returns
// the full URL ffmpeg should push to.
struct StreamSettings {
    enum class Service { Twitch, YouTube, Custom };

    Service service       = Service::Twitch;
    QString streamKey;                            // loaded from CredRead at load(); never saved to QSettings
    QString customUrl     = QStringLiteral("rtmp://your-server.example.com/live/{key}");  // template for Service::Custom
    int     bitrateKbps   = 4500;                 // 1080p60 sweet spot for Twitch
    int     keyframeSec   = 2;                    // Twitch/YouTube hard requirement: ≤4s

    // Broadcast metadata shown in the Streaming Studio (persisted, not yet sent
    // to any platform API — that integration is future work).
    QString     title;
    QString     category;
    QStringList tags;

    // Round-trip through QSettings (service/customUrl/bitrate/keyframe) and
    // Credential Manager (streamKey).
    static StreamSettings load();
    void                  save() const;

    // Returns the full RTMP URL with the stream key substituted.
    // For Service::Custom, replaces "{key}" in customUrl.
    QString rtmpUrl() const;

    // Returns the static service URL template (without the key substituted).
    static QString templateFor(Service s);

    // Human-readable service name for the dialog.
    static QString displayName(Service s);
};
