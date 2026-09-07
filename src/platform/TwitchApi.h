#pragma once

// The two Twitch calls this app needs: fetch the stream key so the user never
// pastes it, and push the title and category when going live so the metadata
// the Streaming workspace already collects finally reaches the channel.
//
// Every call goes through TwitchAuth::withAccessToken, so an expired token is
// refreshed before the request rather than after a 401.

#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;
class TwitchAuth;

class TwitchApi : public QObject {
    Q_OBJECT
public:
    // The scopes these calls need. Requested together at sign-in, because
    // Twitch grants scopes per authorization and asking again later means
    // sending the user back through the flow.
    static QStringList requiredScopes();

    explicit TwitchApi(TwitchAuth* auth, QObject* parent = nullptr);

    // Overrides the api.twitch.tv base for testing against a local stand-in.
    void setApiBase(const QString& baseUrl);

    // The broadcaster id of the signed-in user, cached after the first call
    // since it never changes for an account.
    void fetchUserId(std::function<void(bool ok, QString idOrError)> done);

    // The channel's stream key. Requires channel:read:stream_key.
    void fetchStreamKey(std::function<void(bool ok, QString keyOrError)> done);

    // Sets the stream title, and the category when categoryName is non-empty
    // and resolves to a game. Requires channel:manage:broadcast.
    void updateChannel(const QString& title, const QString& categoryName,
                       std::function<void(bool ok, QString error)> done);

    // ---- Pure response parsing, exposed for testing -----------------------

    // Helix wraps everything in {"data":[...]}. These return an empty string
    // and set *error when the payload is an error or an unexpected shape.
    static QString parseUserId(const QByteArray& json, QString* error);
    static QString parseStreamKey(const QByteArray& json, QString* error);
    static QString parseGameId(const QByteArray& json, QString* error);
    static QString parseHelixError(const QByteArray& json);

private:
    void resolveGameId(const QString& name, std::function<void(QString gameId)> done);

    TwitchAuth* m_auth = nullptr;
    QNetworkAccessManager* m_net = nullptr;
    QString m_apiBase;
    QString m_userId;
};
